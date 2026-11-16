#include "USART.h"
#include "ring_buffer.h"
#include "uart_tx.h"
#include "system_tick.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "stm32f10x_gpio.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_rcc.h"
#include "misc.h"
#include "flash_storage.h"

/* printf 缓冲：原来 1024 字节，而 STM32F103C8 只有 20KB RAM，
   同时它还是不可重入的（中断里再 printf 会踩同一块缓冲）。
   现在只保留 160 字节，并且输出走队列，不再是"格式化完再阻塞发送"。 */
static char usart_buffer[160];

/* 调试串口发送队列：TXE 中断按 87us/字节 的节奏出队 */
#define USART_TX_RING_SIZE  512
static uint8_t usart_tx_buf[USART_TX_RING_SIZE];
static UartTx  usart_tx;

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
    USART_ITConfig(DEBUG_USARTx, USART_IT_TXE, DISABLE);

    NVIC_InitStructure.NVIC_IRQChannel = DEBUG_USART_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(DEBUG_USARTx, ENABLE);
    RingBuf_Init(&usart_rx_ring, usart_rx_ring_buf, USART_RX_RING_SIZE);
    UartTx_Init(&usart_tx, DEBUG_USARTx, usart_tx_buf, USART_TX_RING_SIZE);
}

/* 阻塞发送一个字节：保留给"队列还没起来"的极早期调试和异常路径 */
void USART_SendByte(uint8_t byte)
{
    while(USART_GetFlagStatus(DEBUG_USARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(DEBUG_USARTx, byte);
}

void USART_SendString(char *str)
{
    if (str == NULL) return;
    UartTx_PutString(&usart_tx, str);
}

void USART_Printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(usart_buffer, sizeof(usart_buffer), format, args);
    va_end(args);
    UartTx_PutString(&usart_tx, usart_buffer);
}

void USART_Flush(uint32_t timeout_ms)
{
    UartTx_Flush(&usart_tx, timeout_ms);
}

uint16_t USART_TxDropped(void)
{
    return usart_tx.dropped;
}

int fputc(int ch, FILE *f)
{
    (void)f;
    {
        uint8_t b = (uint8_t)ch;
        UartTx_PutBytes(&usart_tx, &b, 1);
    }
    return ch;
}

/* USART1 中断：RX 入环形缓冲，TX 由 UartTx 的 TXE 状态机出队。 */
void USART1_IRQHandler(void)
{
    if(USART_GetITStatus(DEBUG_USARTx, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(DEBUG_USARTx);
        RingBuf_Write(&usart_rx_ring, ch);
    }
    if(USART_GetITStatus(DEBUG_USARTx, USART_IT_TXE) != RESET)
    {
        UartTx_OnTxE(&usart_tx);
    }
}

/* 处理一条调试命令（addcard/delcard/listcards/provision）。 */
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
    else if (strcmp(command, "erasedb") == 0)
    {
        /* 产测/换车：清空白名单。不再有"内置默认卡"后门，
           空库就是空库，必须显式写入。 */
        FlashStorage_EraseAll();
        USART_Printf("Whitelist erased. Use addcard to provision.\r\n");
    }
    else
    {
        USART_Printf("Unknown command: %s\r\n", command);
        USART_Printf("Available commands:\r\n");
        USART_Printf("  addcard AABBCCDD  - Add a card to whitelist\r\n");
        USART_Printf("  delcard AABBCCDD  - Remove a card from whitelist\r\n");
        USART_Printf("  listcards         - List all cards in whitelist\r\n");
        USART_Printf("  erasedb           - Erase whitelist (production)\r\n");
    }
}

/* 主循环调用：从环形缓冲取字节并按 \r\n 切行处理调试命令。 */
void USART_Process(void)
{
    static char line[USART_RX_LINE_SIZE];
    static uint16_t line_len = 0;
    static uint8_t discarding = 0;
    uint8_t ch;

    while (RingBuf_Read(&usart_rx_ring, &ch))
    {
        if (ch == '\r' || ch == '\n')
        {
            if (line_len > 0 && !discarding)
            {
                line[line_len] = '\0';
                USART_HandleCommand(line);
            }
            line_len = 0;
            discarding = 0;
        }
        else if (discarding)
        {
            /* 超长行：丢弃到行尾重新同步 */
        }
        else if (line_len < USART_RX_LINE_SIZE - 1)
        {
            line[line_len++] = (char)ch;
        }
        else
        {
            discarding = 1;
            line_len = 0;
        }
    }
}
