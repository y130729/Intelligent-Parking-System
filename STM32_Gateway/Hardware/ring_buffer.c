#include "ring_buffer.h"

void RingBuf_Init(RingBuffer *rb, uint8_t *buf, uint16_t size)
{
    rb->buffer = buf;
    rb->size = size;
    rb->mask = size - 1;
    rb->read_idx = 0;
    rb->write_idx = 0;
    rb->overflow_count = 0;
}

void RingBuf_Reset(RingBuffer *rb)
{
    rb->read_idx = 0;
    rb->write_idx = 0;
    rb->overflow_count = 0;
}

uint8_t RingBuf_Write(RingBuffer *rb, uint8_t byte)
{
    uint16_t next = (rb->write_idx + 1) & rb->mask;

    if (next == rb->read_idx)
    {
        /* 缓冲满：丢弃新数据并计数，保证已有数据不被覆盖 */
        rb->overflow_count++;
        return 0;
    }

    rb->buffer[rb->write_idx] = byte;
    rb->write_idx = next;
    return 1;
}

uint8_t RingBuf_Read(RingBuffer *rb, uint8_t *byte)
{
    if (rb->read_idx == rb->write_idx)
    {
        return 0;
    }

    *byte = rb->buffer[rb->read_idx];
    rb->read_idx = (rb->read_idx + 1) & rb->mask;
    return 1;
}

uint16_t RingBuf_Available(RingBuffer *rb)
{
    return (uint16_t)((rb->write_idx - rb->read_idx) & rb->mask);
}
