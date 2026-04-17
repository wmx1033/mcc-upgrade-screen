#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdint.h>

void Bridge_Init(void);
void Bridge_Process(void);
void Bridge_OnRxComplete(void *uart_instance, uint8_t byte);
void Bridge_OnTxComplete(void *uart_instance);

#endif /* BRIDGE_H */
