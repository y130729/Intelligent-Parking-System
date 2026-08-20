#include "pi_link.h"
#include "USART.h"
#include <string.h>
#include <stdio.h>

static uint8_t  pi_rx_buf[PI_RX_BUF_SIZE];
static uint8_t  pi_rx_len = 0;
static volatile uint8_t pi_rx_ready = 0;
static char     pi_rx_line[PI_RX_BUF_SIZE];

void PI_UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB1PeriphClockCmd(PI_UART_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(PI_UART_GPIO_CLK, ENABLE);

    GPIO_InitStructure.GPIO_Pin = PI_UART_TX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(PI_UART_TX_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = PI_UART_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(PI_UART_RX_PORT, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = PI_UART_BAUDRATE;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(PI_UARTx, &USART_InitStructure);

    USART_ITConfig(PI_UARTx, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = PI_UART_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(PI_UARTx, ENABLE);
}

void PI_UART_SendByte(uint8_t byte)
{
    while (USART_GetFlagStatus(PI_UARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(PI_UARTx, byte);
}

void PI_UART_SendString(const char *str)
{
    if (str == NULL) return;
    while (*str)
    {
        PI_UART_SendByte((uint8_t)*str);
        str++;
    }
}

/* USART2 RX interrupt: collect one line (terminated by LF/CR). */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(PI_UARTx, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(PI_UARTx);
        if (ch == '\n' || ch == '\r')
        {
            if (pi_rx_len > 0)
            {
                pi_rx_buf[pi_rx_len] = '\0';
                pi_rx_ready = 1;
            }
            pi_rx_len = 0;
        }
        else if (pi_rx_len < PI_RX_BUF_SIZE - 1)
        {
            pi_rx_buf[pi_rx_len++] = ch;
        }
        else
        {
            pi_rx_len = 0;
        }
    }
}

/* Called from main loop: handle a complete line from Raspberry Pi. */
void PI_UART_Process(void)
{
    if (!pi_rx_ready) return;
    pi_rx_ready = 0;

    strncpy(pi_rx_line, (char *)pi_rx_buf, PI_RX_BUF_SIZE - 1);
    pi_rx_line[PI_RX_BUF_SIZE - 1] = '\0';
    USART_Printf("PI_RX: %s\r\n", pi_rx_line);

    /* Reserved: parse commands from Raspberry Pi here, e.g.
       "PLATE:<plate>" to show result, "OPEN"/"CLOSE" for remote barrier. */
}
