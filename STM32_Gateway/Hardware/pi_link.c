#include "pi_link.h"
#include "ring_buffer.h"
#include "USART.h"
#include "OLED.h"
#include "state_machine.h"
#include <string.h>
#include <stdio.h>

/* 环形缓冲容量（2 的幂）。单行最长 PI_RX_BUF_SIZE，这里可排队多条命令，
   避免主循环繁忙时"缓冲覆盖丢指令"。 */
#define PI_RX_RING_SIZE   128

static uint8_t  pi_rx_ring_buf[PI_RX_RING_SIZE];
static RingBuffer pi_rx_ring;

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

    RingBuf_Init(&pi_rx_ring, pi_rx_ring_buf, PI_RX_RING_SIZE);
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

/* USART2 RX interrupt: 只把收到的字节写入环形缓冲，
   行提取与处理由主循环 PI_UART_Process() 完成。 */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(PI_UARTx, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(PI_UARTx);
        RingBuf_Write(&pi_rx_ring, ch);
    }
}

/* 处理一行来自树莓派的命令。 */
static void PI_UART_HandleLine(char *line)
{
    USART_Printf("PI_RX: %s\r\n", line);

    /* Commands from Raspberry Pi:
       "PLATE:<plate>"  -> display recognized plate on OLED line 4
       "PLATE:NONE"     -> clear plate display
       "OPEN[:<reason>]"/"CLOSE[:<reason>]" -> barrier control
         reason = card | plate | remote，用来告诉 STM32 这次是谁开的闸；
         STM32 会把它原样回给树莓派，写入通行记录（审计需要）。
         不带 reason 的写法仍然兼容（按 remote 处理）。 */
    if (strncmp(line, "PLATE:", 6) == 0)
    {
        char *plate = &line[6];
        if (strcmp(plate, "NONE") == 0)
        {
            OLED_DisplayPlateNumber("------");
        }
        else
        {
            OLED_DisplayPlateNumber(plate);
        }
    }
    else if (strncmp(line, "OPEN", 4) == 0 && (line[4] == '\0' || line[4] == ':'))
    {
        const char *reason = (line[4] == ':') ? &line[5] : "";
        USART_Printf("Remote command: open barrier (%s)\r\n",
                     (reason[0] != '\0') ? reason : "remote");
        StateMachine_RemoteOpen(reason);
    }
    else if (strncmp(line, "CLOSE", 5) == 0 && (line[5] == '\0' || line[5] == ':'))
    {
        const char *reason = (line[5] == ':') ? &line[6] : "";
        USART_Printf("Remote command: close barrier (%s)\r\n",
                     (reason[0] != '\0') ? reason : "remote");
        StateMachine_RemoteClose();
    }
}

/* Called from main loop: pull bytes from ring buffer, cut lines on CR/LF. */
void PI_UART_Process(void)
{
    static char line[PI_RX_BUF_SIZE];
    static uint16_t line_len = 0;
    uint8_t ch;

    while (RingBuf_Read(&pi_rx_ring, &ch))
    {
        if (ch == '\n' || ch == '\r')
        {
            if (line_len > 0)
            {
                line[line_len] = '\0';
                PI_UART_HandleLine(line);
                line_len = 0;
            }
        }
        else if (line_len < PI_RX_BUF_SIZE - 1)
        {
            line[line_len++] = (char)ch;
        }
        else
        {
            /* 单行超长：丢弃该行，等待行结束重新同步 */
            line_len = 0;
        }
    }
}
