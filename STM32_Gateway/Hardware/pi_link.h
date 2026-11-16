#ifndef __PI_LINK_H
#define __PI_LINK_H

#include "stm32f10x.h"
#include "link_frame.h"

/* USART2 (PA2/PA3) is reused as the link to Raspberry Pi.
   (Was connected to ESP8266 for cloud access; networking now lives on the Pi.) */
#define PI_UARTx                    USART2
#define PI_UART_CLK                 RCC_APB1Periph_USART2
#define PI_UART_GPIO_CLK            RCC_APB2Periph_GPIOA
#define PI_UART_TX_PORT             GPIOA
#define PI_UART_TX_PIN              GPIO_Pin_2
#define PI_UART_RX_PORT             GPIOA
#define PI_UART_RX_PIN              GPIO_Pin_3
#define PI_UART_BAUDRATE            115200
#define PI_UART_IRQ                 USART2_IRQn

/* 单行缓冲：一帧最长 LINK_FRAME_MAX_LINE(73) + CRLF，留出余量 */
#define PI_RX_BUF_SIZE              96
#define PI_TX_RING_SIZE             512

/* 等 ACK 的超时与重传次数 */
#define PI_ACK_TIMEOUT_MS           300
#define PI_MAX_RETRY                2

void PI_UART_Init(void);
void PI_UART_SendByte(uint8_t byte);
void PI_UART_SendString(const char *str);      /* 裸文本，仅调试用 */
void PI_UART_SendFramed(const char *payload);   /* 带 CRC/SEQ，正常业务都用这个 */
void PI_UART_Process(void);                     /* 主循环：收帧 + 分发 */
void PI_UART_Tick(void);                        /* 主循环：ACK 超时重传 */

/* 统计（排障用） */
typedef struct {
    uint32_t rx_frames;
    uint32_t rx_legacy;      /* 收到不带帧头的裸文本（老固件） */
    uint32_t rx_bad_crc;     /* 帧格式/CRC 错 */
    uint32_t rx_duplicates;  /* 重传导致的重复帧，已丢弃 */
    uint32_t tx_frames;
    uint32_t tx_retries;
    uint32_t tx_giveup;
    uint32_t tx_overflow;    /* 发送队列溢出 */
} PiLinkStats;
const PiLinkStats *PI_UART_Stats(void);

#endif /* __PI_LINK_H */
