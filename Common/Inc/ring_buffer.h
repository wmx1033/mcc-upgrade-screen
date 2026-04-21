#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint8_t *buffer;
  size_t size;
  volatile size_t head;
  volatile size_t tail;
} RingBuffer;

void RingBuffer_Init(RingBuffer *rb, uint8_t *storage, size_t size);
bool RingBuffer_PushByte(RingBuffer *rb, uint8_t byte);
size_t RingBuffer_Size(const RingBuffer *rb);
size_t RingBuffer_Free(const RingBuffer *rb);
size_t RingBuffer_PeekLinear(const RingBuffer *rb, uint8_t **data_ptr);
void RingBuffer_Drop(RingBuffer *rb, size_t length);

#endif /* RING_BUFFER_H */
