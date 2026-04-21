#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
  BRIDGE_FAULT_NONE = 0,
  BRIDGE_FAULT_RX_OVERFLOW_USART1_TO_USART6 = 1,
  BRIDGE_FAULT_RX_OVERFLOW_USART6_TO_USART1 = 2,
  BRIDGE_FAULT_RX_RESTART_FAILED = 3
} BridgeFaultCode;

typedef struct
{
  uint32_t usart1_rx_bytes;
  uint32_t usart6_rx_bytes;
  uint32_t usart1_to_usart6_tx_bytes;
  uint32_t usart6_to_usart1_tx_bytes;
  uint32_t usart1_to_usart6_tx_start_count;
  uint32_t usart6_to_usart1_tx_start_count;
  uint32_t usart1_to_usart6_tx_complete_count;
  uint32_t usart6_to_usart1_tx_complete_count;
  uint32_t usart1_to_usart6_overflow_count;
  uint32_t usart6_to_usart1_overflow_count;
  uint32_t uart_error_count;
  uint32_t usart1_error_count;
  uint32_t usart6_error_count;
  uint32_t usart1_error_ore_count;
  uint32_t usart6_error_ore_count;
  uint32_t usart1_error_fe_count;
  uint32_t usart6_error_fe_count;
  uint32_t usart1_error_ne_count;
  uint32_t usart6_error_ne_count;
  uint32_t usart1_error_pe_count;
  uint32_t usart6_error_pe_count;
  uint32_t usart1_error_dma_count;
  uint32_t usart6_error_dma_count;
  uint32_t usart1_error_rto_count;
  uint32_t usart6_error_rto_count;
  uint32_t usart1_error_other_count;
  uint32_t usart6_error_other_count;
  uint32_t usart1_last_error_code;
  uint32_t usart6_last_error_code;
  uint32_t usart1_rx_event_count;
  uint32_t usart6_rx_event_count;
  uint32_t usart1_last_rx_event_size;
  uint32_t usart6_last_rx_event_size;
  uint32_t usart1_rx_dma_start_ok_count;
  uint32_t usart6_rx_dma_start_ok_count;
  uint32_t usart1_rx_dma_start_fail_count;
  uint32_t usart6_rx_dma_start_fail_count;
  uint32_t rx_restart_recover_count;
  uint32_t rx_restart_fail_count;
  uint32_t tx_dma_fail_count;
  BridgeFaultCode fault_code;
} BridgeStats;

void Bridge_Init(void);
void Bridge_Process(void);
void Bridge_OnRxComplete(void *uart_instance, uint8_t byte);
void Bridge_OnTxComplete(void *uart_instance);
const BridgeStats *Bridge_GetStats(void);
bool Bridge_IsHealthy(void);

#endif /* BRIDGE_H */
