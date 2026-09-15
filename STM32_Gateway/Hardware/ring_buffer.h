#ifndef __RING_BUFFER_H
#define __RING_BUFFER_H

#include "stm32f10x.h"

/* 通用字节环形缓冲区（单生产者/单消费者）。
 *
 * 设计要点：
 *  - 容量 size 必须为 2 的幂，用掩码代替取模，读写指针更新都是 O(1)。
 *  - 判满采用"预留一格"：write_idx + 1 == read_idx 视为满，
 *    因此实际可用容量为 size - 1。
 *  - 典型用法：串口中断（生产者）只调用 RingBuf_Write，
 *    主循环（消费者）只调用 RingBuf_Read，两端各改各的指针，
 *    不需要加锁，也不存在共享计数被撕裂的问题。
 *  - 满时策略：丢弃新数据（保旧不保新），并累计 overflow_count 供排查。
 */
typedef struct {
    uint8_t *buffer;                  /* 存储区（调用方提供的静态数组） */
    uint16_t size;                    /* 容量（2 的幂） */
    uint16_t mask;                    /* size - 1 */
    volatile uint16_t read_idx;       /* 读指针，消费者维护 */
    volatile uint16_t write_idx;      /* 写指针，生产者维护 */
    volatile uint16_t overflow_count; /* 满时被丢弃的字节计数 */
} RingBuffer;

void RingBuf_Init(RingBuffer *rb, uint8_t *buf, uint16_t size);
void RingBuf_Reset(RingBuffer *rb);
uint8_t RingBuf_Write(RingBuffer *rb, uint8_t byte); /* 返回 1 成功，0 满丢弃 */
uint8_t RingBuf_Read(RingBuffer *rb, uint8_t *byte); /* 返回 1 成功，0 空 */
uint16_t RingBuf_Available(RingBuffer *rb);

#endif /* __RING_BUFFER_H */
