#include "ring_buffer.h"

void RingBuffer_Init(RingBuffer *rb, uint8_t *storage, size_t size)
{
  rb->buffer = storage;
  rb->size = size;
  rb->head = 0U;
  rb->tail = 0U;
}

size_t RingBuffer_Size(const RingBuffer *rb)
{
  size_t head = rb->head;
  size_t tail = rb->tail;

  if (head >= tail)
  {
    return head - tail;
  }

  return rb->size - tail + head;
}

size_t RingBuffer_Free(const RingBuffer *rb)
{
  return (rb->size - 1U) - RingBuffer_Size(rb);
}

bool RingBuffer_PushByte(RingBuffer *rb, uint8_t byte)
{
  size_t next_head = rb->head + 1U;
  if (next_head >= rb->size)
  {
    next_head = 0U;
  }

  if (next_head == rb->tail)
  {
    return false;
  }

  rb->buffer[rb->head] = byte;
  rb->head = next_head;
  return true;
}

size_t RingBuffer_PeekLinear(const RingBuffer *rb, uint8_t **data_ptr)
{
  size_t head = rb->head;
  size_t tail = rb->tail;

  if (head == tail)
  {
    *data_ptr = NULL;
    return 0U;
  }

  *data_ptr = &rb->buffer[tail];
  if (head > tail)
  {
    return head - tail;
  }

  return rb->size - tail;
}

void RingBuffer_Drop(RingBuffer *rb, size_t length)
{
  size_t used = RingBuffer_Size(rb);
  if (length >= used)
  {
    rb->tail = rb->head;
    return;
  }

  size_t tail = rb->tail + length;
  if (tail >= rb->size)
  {
    tail %= rb->size;
  }
  rb->tail = tail;
}
