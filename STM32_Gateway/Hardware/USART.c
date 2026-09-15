#include "USART.h"
#include "ring_buffer.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "stm32f10x_gpio.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_rcc.h"
#include "misc.h"
#include "flash_storage.h"

static char usart_buffer[1024];

/* 调试命令环形缓冲（2 的幂）：ISR 写入，主循环 USART_Process() 取行处理 */
#define USART_RX_RING_SIZE  256
#define USART_RX_LINE_SIZE  128

static uint8_t usart_rx_ring_buf[USART_RX_RING_SIZE];
static RingBuffer usart_rx_ring;

void USART_Init_Config(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(DEBUG_USART_GPIO_CLK | DEBUG_USART_CLK, ENABLE);

    GPIO_InitStructure.GPIO_Pin = DEBUG_USART_TX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(DEBUG_USART_TX_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = DEBUG_USART_RX_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(DEBUG_USART_RX_GPIO_PORT, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = DEBUG_USART_BAUDRATE;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_Init(DEBUG_USARTx, &USART_InitStructure);

    USART_ITConfig(DEBUG_USARTx, USART_IT_RXNE, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = DEBUG_USART_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(DEBUG_USARTx, ENABLE);
    RingBuf_Init(&usart_rx_ring, usart_rx_ring_buf, USART_RX_RING_SIZE);
}

void USART_SendByte(uint8_t byte)
{
    while(USART_GetFlagStatus(DEBUG_USARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(DEBUG_USARTx, byte);
}

void USART_SendString(char *str)
{
    while(*str)
    {
        USART_SendByte(*str++);
    }
}

void USART_Printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(usart_buffer, sizeof(usart_buffer), format, args);
    va_end(args);
    USART_SendString(usart_buffer);
}

int fputc(int ch, FILE *f)
{
    USART_SendData(DEBUG_USARTx, (uint8_t)ch);
    while(USART_GetFlagStatus(DEBUG_USARTx, USART_FLAG_TXE) == RESET);
    return ch;
}

/* USART1 RX interrupt: 只把收到的字节写入环形缓冲，
   行提取与命令处理由主循环 USART_Process() 完成。 */
void USART1_IRQHandler(void)
{
    if(USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = USART_ReceiveData(USART1);
        RingBuf_Write(&usart_rx_ring, ch);
    }
}

/* 处理一条调试命令（addcard/delcard/listcards）。 */
static void USART_HandleCommand(char *command)
{
    /* sscanf 的 %X 目标必须是 unsigned int*，直接传 uint8_t* 会多写 3 字节，
       因此先解析到临时变量再收窄赋值。 */
    unsigned int parsed[4];
    uint8_t card_id[4];
    uint8_t i;

    if (strncmp(command, "addcard ", 8) == 0)
    {
        if (sscanf(command + 8, "%02X%02X%02X%02X",
                   &parsed[0], &parsed[1], &parsed[2], &parsed[3]) == 4)
        {
            for (i = 0; i < 4; i++) { card_id[i] = (uint8_t)parsed[i]; }
            USART_Printf("Adding card: %02X%02X%02X%02X\r\n",
                        card_id[0], card_id[1], card_id[2], card_id[3]);
            FlashStorage_AddCard(card_id);
        }
        else
        {
            USART_Printf("Invalid card format. Use: addcard AABBCCDD\r\n");
        }
    }
    else if (strncmp(command, "delcard ", 8) == 0)
    {
        if (sscanf(command + 8, "%02X%02X%02X%02X",
                   &parsed[0], &parsed[1], &parsed[2], &parsed[3]) == 4)
        {
            for (i = 0; i < 4; i++) { card_id[i] = (uint8_t)parsed[i]; }
            USART_Printf("Removing card: %02X%02X%02X%02X\r\n",
                        card_id[0], card_id[1], card_id[2], card_id[3]);
            FlashStorage_RemoveCard(card_id);
        }
        else
        {
            USART_Printf("Invalid card format. Use: delcard AABBCCDD\r\n");
        }
    }
    else if (strcmp(command, "listcards") == 0)
    {
        FlashStorage_PrintWhitelist();
    }
    else
    {
        USART_Printf("Unknown command: %s\r\n", command);
        USART_Printf("Available commands:\r\n");
        USART_Printf("  addcard AABBCCDD  - Add a card to whitelist\r\n");
        USART_Printf("  delcard AABBCCDD  - Remove a card from whitelist\r\n");
        USART_Printf("  listcards         - List all cards in whitelist\r\n");
    }
}

/* 主循环调用：从环形缓冲取字节并按 \r\n 切行处理调试命令。 */
void USART_Process(void)
{
    static char line[USART_RX_LINE_SIZE];
    static uint16_t line_len = 0;
    uint8_t ch;

    while (RingBuf_Read(&usart_rx_ring, &ch))
    {
        if (ch == '\r' || ch == '\n')
        {
            if (line_len > 0)
            {
                line[line_len] = '\0';
                USART_HandleCommand(line);
                line_len = 0;
            }
        }
        else if (line_len < USART_RX_LINE_SIZE - 1)
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
