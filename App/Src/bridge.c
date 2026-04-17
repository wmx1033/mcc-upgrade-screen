#include "bridge.h"

#include <stdbool.h>
#include <stddef.h>

#include "ring_buffer.h"
#include "usart.h"

#define BRIDGE_RING_BUFFER_SIZE (16U * 1024U)

typedef struct
{
  UART_HandleTypeDef *tx_uart;
  RingBuffer *tx_rb;
  volatile bool tx_busy;
  volatile size_t tx_pending_len;
} BridgeChannel;

static uint8_t g_pc_to_screen_storage[BRIDGE_RING_BUFFER_SIZE];
static uint8_t g_screen_to_pc_storage[BRIDGE_RING_BUFFER_SIZE];
static RingBuffer g_pc_to_screen_rb;
static RingBuffer g_screen_to_pc_rb;
static BridgeChannel g_from_pc;
static BridgeChannel g_from_screen;
static uint8_t g_usart1_rx_byte;
static uint8_t g_usart6_rx_byte;

static void Bridge_StartRx(UART_HandleTypeDef *huart);
static void Bridge_TryStartTx(BridgeChannel *channel);
static BridgeChannel *Bridge_FromUartInstance(void *uart_instance);

void Bridge_Init(void)
{
  RingBuffer_Init(&g_pc_to_screen_rb, g_pc_to_screen_storage, sizeof(g_pc_to_screen_storage));
  RingBuffer_Init(&g_screen_to_pc_rb, g_screen_to_pc_storage, sizeof(g_screen_to_pc_storage));

  g_from_pc.tx_uart = &huart6;
  g_from_pc.tx_rb = &g_pc_to_screen_rb;
  g_from_pc.tx_busy = false;
  g_from_pc.tx_pending_len = 0U;

  g_from_screen.tx_uart = &huart1;
  g_from_screen.tx_rb = &g_screen_to_pc_rb;
  g_from_screen.tx_busy = false;
  g_from_screen.tx_pending_len = 0U;

  Bridge_StartRx(&huart1);
  Bridge_StartRx(&huart6);
}

void Bridge_Process(void)
{
  Bridge_TryStartTx(&g_from_pc);
  Bridge_TryStartTx(&g_from_screen);
}

void Bridge_OnRxComplete(void *uart_instance, uint8_t byte)
{
  BridgeChannel *channel = Bridge_FromUartInstance(uart_instance);
  if (channel == NULL)
  {
    return;
  }

  (void)RingBuffer_PushByte(channel->tx_rb, byte);
  Bridge_TryStartTx(channel);
}

void Bridge_OnTxComplete(void *uart_instance)
{
  BridgeChannel *channel = Bridge_FromUartInstance(uart_instance);
  if (channel == NULL)
  {
    return;
  }

  if (channel->tx_pending_len > 0U)
  {
    RingBuffer_Drop(channel->tx_rb, channel->tx_pending_len);
  }

  channel->tx_pending_len = 0U;
  channel->tx_busy = false;
  Bridge_TryStartTx(channel);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    Bridge_OnRxComplete(huart->Instance, g_usart1_rx_byte);
    Bridge_StartRx(huart);
    return;
  }

  if (huart->Instance == USART6)
  {
    Bridge_OnRxComplete(huart->Instance, g_usart6_rx_byte);
    Bridge_StartRx(huart);
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  Bridge_OnTxComplete(huart->Instance);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart->Instance == USART1) || (huart->Instance == USART6))
  {
    Bridge_StartRx(huart);
  }
}

static void Bridge_StartRx(UART_HandleTypeDef *huart)
{
  uint8_t *rx_byte = NULL;
  if (huart->Instance == USART1)
  {
    rx_byte = &g_usart1_rx_byte;
  }
  else if (huart->Instance == USART6)
  {
    rx_byte = &g_usart6_rx_byte;
  }

  if (rx_byte == NULL)
  {
    return;
  }

  if (HAL_UART_Receive_IT(huart, rx_byte, 1U) != HAL_OK)
  {
    Error_Handler();
  }
}

static void Bridge_TryStartTx(BridgeChannel *channel)
{
  uint8_t *tx_ptr = NULL;
  size_t tx_len;

  if (channel->tx_busy)
  {
    return;
  }

  tx_len = RingBuffer_PeekLinear(channel->tx_rb, &tx_ptr);
  if ((tx_len == 0U) || (tx_ptr == NULL))
  {
    return;
  }

  if (tx_len > 0xFFFFU)
  {
    tx_len = 0xFFFFU;
  }

  channel->tx_busy = true;
  channel->tx_pending_len = tx_len;

  if (HAL_UART_Transmit_DMA(channel->tx_uart, tx_ptr, (uint16_t)tx_len) != HAL_OK)
  {
    channel->tx_pending_len = 0U;
    channel->tx_busy = false;
  }
}

static BridgeChannel *Bridge_FromUartInstance(void *uart_instance)
{
  if (uart_instance == USART1)
  {
    return &g_from_pc;
  }

  if (uart_instance == USART6)
  {
    return &g_from_screen;
  }

  return NULL;
}
