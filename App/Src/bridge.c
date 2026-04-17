#include "bridge.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ring_buffer.h"
#include "usart.h"

#define BRIDGE_RING_BUFFER_SIZE (16U * 1024U)
#define BRIDGE_RX_DMA_BUFFER_SIZE 8192U
#define BRIDGE_RX_RESTART_MAX_TRIES 3U
#define BRIDGE_SCREEN_POWER_CYCLE_MS 300U
#define BRIDGE_SCREEN_BOOT_WAIT_MS 800U

typedef struct
{
  UART_HandleTypeDef *rx_uart;
  UART_HandleTypeDef *tx_uart;
  RingBuffer *tx_rb;
  uint8_t *rx_dma_buffer;
  uint16_t rx_dma_buffer_size;
  uint16_t rx_last_pos;
  BridgeFaultCode overflow_fault;
  uint32_t *rx_bytes_counter;
  uint32_t *tx_bytes_counter;
  uint32_t *tx_start_counter;
  uint32_t *tx_complete_counter;
  uint32_t *overflow_counter;
  volatile bool tx_busy;
  volatile size_t tx_pending_len;
} BridgeChannel;

static uint8_t g_pc_to_screen_storage[BRIDGE_RING_BUFFER_SIZE];
static uint8_t g_screen_to_pc_storage[BRIDGE_RING_BUFFER_SIZE];
static uint8_t g_usart1_rx_dma_buffer[BRIDGE_RX_DMA_BUFFER_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));
static uint8_t g_usart6_rx_dma_buffer[BRIDGE_RX_DMA_BUFFER_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));
static RingBuffer g_pc_to_screen_rb;
static RingBuffer g_screen_to_pc_rb;
static BridgeChannel g_from_pc;
static BridgeChannel g_from_screen;
static BridgeStats g_bridge_stats;

static void Bridge_StartRx(BridgeChannel *channel);
static void Bridge_HandleRxData(BridgeChannel *channel, uint16_t start, uint16_t length);
static bool Bridge_PushRxByte(BridgeChannel *channel, uint8_t byte);
static void Bridge_TryStartTx(BridgeChannel *channel);
static BridgeChannel *Bridge_ChannelFromRxInstance(void *uart_instance);
static BridgeChannel *Bridge_ChannelFromTxInstance(void *uart_instance);
static uint32_t Bridge_EnterCritical(void);
static void Bridge_ExitCritical(uint32_t primask);
static void Bridge_SetFault(BridgeFaultCode fault_code);
static void Bridge_PrepareUartRxRestart(UART_HandleTypeDef *huart);
static void Bridge_RecordUartError(UART_HandleTypeDef *huart);

void Bridge_Init(void)
{
  memset(&g_bridge_stats, 0, sizeof(g_bridge_stats));
  RingBuffer_Init(&g_pc_to_screen_rb, g_pc_to_screen_storage, sizeof(g_pc_to_screen_storage));
  RingBuffer_Init(&g_screen_to_pc_rb, g_screen_to_pc_storage, sizeof(g_screen_to_pc_storage));

  g_from_pc.rx_uart = &huart1;
  g_from_pc.tx_uart = &huart6;
  g_from_pc.tx_rb = &g_pc_to_screen_rb;
  g_from_pc.rx_dma_buffer = g_usart1_rx_dma_buffer;
  g_from_pc.rx_dma_buffer_size = (uint16_t)sizeof(g_usart1_rx_dma_buffer);
  g_from_pc.overflow_fault = BRIDGE_FAULT_RX_OVERFLOW_USART1_TO_USART6;
  g_from_pc.rx_bytes_counter = &g_bridge_stats.usart1_rx_bytes;
  g_from_pc.tx_bytes_counter = &g_bridge_stats.usart1_to_usart6_tx_bytes;
  g_from_pc.tx_start_counter = &g_bridge_stats.usart1_to_usart6_tx_start_count;
  g_from_pc.tx_complete_counter = &g_bridge_stats.usart1_to_usart6_tx_complete_count;
  g_from_pc.overflow_counter = &g_bridge_stats.usart1_to_usart6_overflow_count;
  g_from_pc.tx_busy = false;
  g_from_pc.tx_pending_len = 0U;
  g_from_pc.rx_last_pos = 0U;

  g_from_screen.rx_uart = &huart6;
  g_from_screen.tx_uart = &huart1;
  g_from_screen.tx_rb = &g_screen_to_pc_rb;
  g_from_screen.rx_dma_buffer = g_usart6_rx_dma_buffer;
  g_from_screen.rx_dma_buffer_size = (uint16_t)sizeof(g_usart6_rx_dma_buffer);
  g_from_screen.overflow_fault = BRIDGE_FAULT_RX_OVERFLOW_USART6_TO_USART1;
  g_from_screen.rx_bytes_counter = &g_bridge_stats.usart6_rx_bytes;
  g_from_screen.tx_bytes_counter = &g_bridge_stats.usart6_to_usart1_tx_bytes;
  g_from_screen.tx_start_counter = &g_bridge_stats.usart6_to_usart1_tx_start_count;
  g_from_screen.tx_complete_counter = &g_bridge_stats.usart6_to_usart1_tx_complete_count;
  g_from_screen.overflow_counter = &g_bridge_stats.usart6_to_usart1_overflow_count;
  g_from_screen.tx_busy = false;
  g_from_screen.tx_pending_len = 0U;
  g_from_screen.rx_last_pos = 0U;

  /* Debug aid: force screen power cycle before bridge starts. */
  HAL_GPIO_WritePin(SCREEN_ENABLE_GPIO_Port, SCREEN_ENABLE_Pin, GPIO_PIN_RESET);
  HAL_Delay(BRIDGE_SCREEN_POWER_CYCLE_MS);
  HAL_GPIO_WritePin(SCREEN_ENABLE_GPIO_Port, SCREEN_ENABLE_Pin, GPIO_PIN_SET);
  HAL_Delay(BRIDGE_SCREEN_BOOT_WAIT_MS);

  if ((g_from_pc.rx_uart->hdmarx != NULL) && (g_from_pc.rx_uart->hdmarx->Init.Mode != DMA_CIRCULAR))
  {
    g_from_pc.rx_uart->hdmarx->Init.Mode = DMA_CIRCULAR;
    if (HAL_DMA_Init(g_from_pc.rx_uart->hdmarx) != HAL_OK)
    {
      Bridge_SetFault(BRIDGE_FAULT_RX_RESTART_FAILED);
      return;
    }
  }

  if ((g_from_screen.rx_uart->hdmarx != NULL) && (g_from_screen.rx_uart->hdmarx->Init.Mode != DMA_CIRCULAR))
  {
    g_from_screen.rx_uart->hdmarx->Init.Mode = DMA_CIRCULAR;
    if (HAL_DMA_Init(g_from_screen.rx_uart->hdmarx) != HAL_OK)
    {
      Bridge_SetFault(BRIDGE_FAULT_RX_RESTART_FAILED);
      return;
    }
  }

  Bridge_StartRx(&g_from_pc);
  Bridge_StartRx(&g_from_screen);
}

void Bridge_Process(void)
{
  if (!Bridge_IsHealthy())
  {
    return;
  }

  Bridge_TryStartTx(&g_from_pc);
  Bridge_TryStartTx(&g_from_screen);
}

void Bridge_OnRxComplete(void *uart_instance, uint8_t byte)
{
  BridgeChannel *channel = Bridge_ChannelFromRxInstance(uart_instance);
  if (channel == NULL)
  {
    return;
  }

  (void)Bridge_PushRxByte(channel, byte);
}

void Bridge_OnTxComplete(void *uart_instance)
{
  uint32_t primask;
  BridgeChannel *channel = Bridge_ChannelFromTxInstance(uart_instance);
  if (channel == NULL)
  {
    return;
  }

  primask = Bridge_EnterCritical();
  if (channel->tx_pending_len > 0U)
  {
    *channel->tx_bytes_counter += (uint32_t)channel->tx_pending_len;
    (*channel->tx_complete_counter)++;
    RingBuffer_Drop(channel->tx_rb, channel->tx_pending_len);
  }

  channel->tx_pending_len = 0U;
  channel->tx_busy = false;
  Bridge_ExitCritical(primask);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  (void)huart;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  Bridge_OnTxComplete(huart->Instance);
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  BridgeChannel *channel = Bridge_ChannelFromRxInstance(huart->Instance);
  uint16_t current_pos;
  uint16_t chunk_len;

  if (channel == NULL)
  {
    return;
  }

  if (huart->Instance == USART1)
  {
    g_bridge_stats.usart1_rx_event_count++;
    g_bridge_stats.usart1_last_rx_event_size = Size;
  }
  else if (huart->Instance == USART6)
  {
    g_bridge_stats.usart6_rx_event_count++;
    g_bridge_stats.usart6_last_rx_event_size = Size;
  }

  current_pos = Size;
  if (current_pos > channel->rx_dma_buffer_size)
  {
    current_pos = channel->rx_dma_buffer_size;
  }

  if (current_pos >= channel->rx_last_pos)
  {
    chunk_len = current_pos - channel->rx_last_pos;
    if (chunk_len > 0U)
    {
      Bridge_HandleRxData(channel, channel->rx_last_pos, chunk_len);
    }
  }
  else
  {
    chunk_len = channel->rx_dma_buffer_size - channel->rx_last_pos;
    if (chunk_len > 0U)
    {
      Bridge_HandleRxData(channel, channel->rx_last_pos, chunk_len);
    }
    if (current_pos > 0U)
    {
      Bridge_HandleRxData(channel, 0U, current_pos);
    }
  }

  channel->rx_last_pos = current_pos;
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  BridgeChannel *channel = Bridge_ChannelFromRxInstance(huart->Instance);
  if (channel != NULL)
  {
    Bridge_RecordUartError(huart);
    g_bridge_stats.uart_error_count++;
    Bridge_StartRx(channel);
  }
}

static void Bridge_StartRx(BridgeChannel *channel)
{
  HAL_StatusTypeDef status;
  uint32_t try_count;

  if (!Bridge_IsHealthy())
  {
    return;
  }

  for (try_count = 0U; try_count < BRIDGE_RX_RESTART_MAX_TRIES; try_count++)
  {
    status = HAL_UARTEx_ReceiveToIdle_DMA(channel->rx_uart, channel->rx_dma_buffer, channel->rx_dma_buffer_size);
    if (status == HAL_OK)
    {
      if (channel->rx_uart->Instance == USART1)
      {
        g_bridge_stats.usart1_rx_dma_start_ok_count++;
      }
      else if (channel->rx_uart->Instance == USART6)
      {
        g_bridge_stats.usart6_rx_dma_start_ok_count++;
      }

      if (channel->rx_uart->hdmarx != NULL)
      {
        __HAL_DMA_DISABLE_IT(channel->rx_uart->hdmarx, DMA_IT_HT);
      }
      channel->rx_last_pos = 0U;

      if (try_count > 0U)
      {
        g_bridge_stats.rx_restart_recover_count++;
      }
      return;
    }

    if (channel->rx_uart->Instance == USART1)
    {
      g_bridge_stats.usart1_rx_dma_start_fail_count++;
    }
    else if (channel->rx_uart->Instance == USART6)
    {
      g_bridge_stats.usart6_rx_dma_start_fail_count++;
    }

    Bridge_PrepareUartRxRestart(channel->rx_uart);
  }

  g_bridge_stats.rx_restart_fail_count++;
  Bridge_SetFault(BRIDGE_FAULT_RX_RESTART_FAILED);
}

static void Bridge_HandleRxData(BridgeChannel *channel, uint16_t start, uint16_t length)
{
  uint16_t index;
  uint32_t accepted = 0U;

  if (start >= channel->rx_dma_buffer_size)
  {
    return;
  }

  if ((uint32_t)start + (uint32_t)length > channel->rx_dma_buffer_size)
  {
    length = channel->rx_dma_buffer_size - start;
  }

  for (index = 0U; index < length; index++)
  {
    if (!Bridge_PushRxByte(channel, channel->rx_dma_buffer[start + index]))
    {
      break;
    }
    accepted++;
  }

  *channel->rx_bytes_counter += accepted;
}

static bool Bridge_PushRxByte(BridgeChannel *channel, uint8_t byte)
{
  if (!Bridge_IsHealthy())
  {
    return false;
  }

  if (!RingBuffer_PushByte(channel->tx_rb, byte))
  {
    (*channel->overflow_counter)++;
    Bridge_SetFault(channel->overflow_fault);
    return false;
  }

  return true;
}

static void Bridge_TryStartTx(BridgeChannel *channel)
{
  uint32_t primask;
  uint8_t *tx_ptr = NULL;
  size_t tx_len;

  if (!Bridge_IsHealthy())
  {
    return;
  }

  primask = Bridge_EnterCritical();
  if (channel->tx_busy)
  {
    Bridge_ExitCritical(primask);
    return;
  }

  tx_len = RingBuffer_PeekLinear(channel->tx_rb, &tx_ptr);
  if ((tx_len == 0U) || (tx_ptr == NULL))
  {
    Bridge_ExitCritical(primask);
    return;
  }

  if (tx_len > 0xFFFFU)
  {
    tx_len = 0xFFFFU;
  }

  channel->tx_busy = true;
  channel->tx_pending_len = tx_len;
  Bridge_ExitCritical(primask);

  if (HAL_UART_Transmit_IT(channel->tx_uart, tx_ptr, (uint16_t)tx_len) != HAL_OK)
  {
    primask = Bridge_EnterCritical();
    channel->tx_pending_len = 0U;
    channel->tx_busy = false;
    Bridge_ExitCritical(primask);
    g_bridge_stats.tx_dma_fail_count++;
    return;
  }

  (*channel->tx_start_counter)++;
}

static BridgeChannel *Bridge_ChannelFromRxInstance(void *uart_instance)
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

static BridgeChannel *Bridge_ChannelFromTxInstance(void *uart_instance)
{
  if (uart_instance == USART6)
  {
    return &g_from_pc;
  }

  if (uart_instance == USART1)
  {
    return &g_from_screen;
  }

  return NULL;
}

const BridgeStats *Bridge_GetStats(void)
{
  return &g_bridge_stats;
}

bool Bridge_IsHealthy(void)
{
  return g_bridge_stats.fault_code == BRIDGE_FAULT_NONE;
}

static uint32_t Bridge_EnterCritical(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void Bridge_ExitCritical(uint32_t primask)
{
  if ((primask & 1U) == 0U)
  {
    __enable_irq();
  }
}

static void Bridge_SetFault(BridgeFaultCode fault_code)
{
  uint32_t primask = Bridge_EnterCritical();
  if (g_bridge_stats.fault_code == BRIDGE_FAULT_NONE)
  {
    g_bridge_stats.fault_code = fault_code;
  }
  Bridge_ExitCritical(primask);
}

static void Bridge_PrepareUartRxRestart(UART_HandleTypeDef *huart)
{
  (void)HAL_UART_AbortReceive(huart);
  __HAL_UART_CLEAR_OREFLAG(huart);
  __HAL_UART_CLEAR_FEFLAG(huart);
  __HAL_UART_CLEAR_NEFLAG(huart);
  __HAL_UART_CLEAR_PEFLAG(huart);
}

static void Bridge_RecordUartError(UART_HandleTypeDef *huart)
{
  uint32_t error = huart->ErrorCode;

  if (huart->Instance == USART1)
  {
    g_bridge_stats.usart1_error_count++;
    g_bridge_stats.usart1_last_error_code = error;
    if ((error & HAL_UART_ERROR_ORE) != 0U)
    {
      g_bridge_stats.usart1_error_ore_count++;
    }
    if ((error & HAL_UART_ERROR_FE) != 0U)
    {
      g_bridge_stats.usart1_error_fe_count++;
    }
    if ((error & HAL_UART_ERROR_NE) != 0U)
    {
      g_bridge_stats.usart1_error_ne_count++;
    }
    if ((error & HAL_UART_ERROR_PE) != 0U)
    {
      g_bridge_stats.usart1_error_pe_count++;
    }
    if ((error & HAL_UART_ERROR_DMA) != 0U)
    {
      g_bridge_stats.usart1_error_dma_count++;
    }
    if ((error & HAL_UART_ERROR_RTO) != 0U)
    {
      g_bridge_stats.usart1_error_rto_count++;
    }
    if ((error & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE | HAL_UART_ERROR_NE |
                  HAL_UART_ERROR_PE | HAL_UART_ERROR_DMA | HAL_UART_ERROR_RTO)) == 0U)
    {
      g_bridge_stats.usart1_error_other_count++;
    }
    return;
  }

  if (huart->Instance == USART6)
  {
    g_bridge_stats.usart6_error_count++;
    g_bridge_stats.usart6_last_error_code = error;
    if ((error & HAL_UART_ERROR_ORE) != 0U)
    {
      g_bridge_stats.usart6_error_ore_count++;
    }
    if ((error & HAL_UART_ERROR_FE) != 0U)
    {
      g_bridge_stats.usart6_error_fe_count++;
    }
    if ((error & HAL_UART_ERROR_NE) != 0U)
    {
      g_bridge_stats.usart6_error_ne_count++;
    }
    if ((error & HAL_UART_ERROR_PE) != 0U)
    {
      g_bridge_stats.usart6_error_pe_count++;
    }
    if ((error & HAL_UART_ERROR_DMA) != 0U)
    {
      g_bridge_stats.usart6_error_dma_count++;
    }
    if ((error & HAL_UART_ERROR_RTO) != 0U)
    {
      g_bridge_stats.usart6_error_rto_count++;
    }
    if ((error & (HAL_UART_ERROR_ORE | HAL_UART_ERROR_FE | HAL_UART_ERROR_NE |
                  HAL_UART_ERROR_PE | HAL_UART_ERROR_DMA | HAL_UART_ERROR_RTO)) == 0U)
    {
      g_bridge_stats.usart6_error_other_count++;
    }
  }
}
