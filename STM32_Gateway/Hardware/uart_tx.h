#ifndef __UART_TX_H
#define __UART_TX_H

#include "stm32f10x.h"
#include "ring_buffer.h"

/* 非阻塞串口发送：字节先进环形队列，TXE 中断里出队写 DR。
 *
 * 为什么不让调用方直接 while(TXE==RESET)：115200 下一个字节 ~87us，
 * 30 字节的日志就是 2.6ms。原来这条路径还被 EXTI 中断调用过，
 * 等于每来一次红外就阻塞中断 2~3ms，期间所有别的中断都被压住。
 *
 * 为什么用 TXE 中断而不是 DMA：
 *   - 三个 UART 都要用，DMA1 通道要和 ADC/SPI 抢，优先级还得分；
 *   - 单字节中断在 115200 下每 87us 一次，CPU 占用可忽略；
 *   - 代码量小、行为可预测，便于在没有硬件的环境下审阅。
 *   DMA 的优势要到 460800 以上或大量突发数据才明显。
 *
 * 队列满时丢新字节并计数（dropped），绝不阻塞调用方。
 */

typedef struct {
    USART_TypeDef *usart;
    RingBuffer     ring;
    volatile uint16_t dropped;   /* 队列满被丢弃的字节数 */
    volatile uint16_t sent;      /* 已发送字节数，用于统计 */
} UartTx;

void     UartTx_Init(UartTx *tx, USART_TypeDef *usart, uint8_t *buf, uint16_t size);
uint8_t  UartTx_PutString(UartTx *tx, const char *s);      /* 1=全部入队 */
uint8_t  UartTx_PutBytes(UartTx *tx, const uint8_t *p, uint16_t n);
uint16_t UartTx_Pending(const UartTx *tx);
void     UartTx_OnTxE(UartTx *tx);                          /* 仅由 TXE 中断调用 */
void     UartTx_Flush(UartTx *tx, uint32_t timeout_ms);     /* 阻塞等待排空（关机/测试用） */

#endif /* __UART_TX_H */
