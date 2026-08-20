#ifndef __PI_LINK_H
#define __PI_LINK_H

#include "stm32f10x.h"

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

#define PI_RX_BUF_SIZE              64

void PI_UART_Init(void);
void PI_UART_SendByte(uint8_t byte);
void PI_UART_SendString(const char *str);
void PI_UART_Process(void);

#endif /* __PI_LINK_H */
