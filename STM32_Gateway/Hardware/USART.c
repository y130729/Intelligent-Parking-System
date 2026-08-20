#include "USART.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "stm32f10x_gpio.h"
#include "stm32f10x_usart.h"
#include "stm32f10x_rcc.h"
#include "misc.h"
#include "flash_storage.h"

static char usart_buffer[1024];
static char command_buffer[128];
static uint8_t command_index = 0;

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

void USART1_IRQHandler(void)
{
    if(USART_GetITStatus(USART1, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = USART_ReceiveData(USART1);
        
        if (ch == '\r' || ch == '\n')
        {
            if (command_index > 0)
            {
                command_buffer[command_index] = '\0';
                
                uint8_t card_id[4];
                
                if (strncmp(command_buffer, "addcard ", 8) == 0)
                {
                    if (sscanf(command_buffer + 8, "%02X%02X%02X%02X", 
                               &card_id[0], &card_id[1], &card_id[2], &card_id[3]) == 4)
                    {
                        USART_Printf("Adding card: %02X%02X%02X%02X\r\n", 
                                    card_id[0], card_id[1], card_id[2], card_id[3]);
                        FlashStorage_AddCard(card_id);
                    }
                    else
                    {
                        USART_Printf("Invalid card format. Use: addcard AABBCCDD\r\n");
                    }
                }
                else if (strncmp(command_buffer, "delcard ", 8) == 0)
                {
                    if (sscanf(command_buffer + 8, "%02X%02X%02X%02X", 
                               &card_id[0], &card_id[1], &card_id[2], &card_id[3]) == 4)
                    {
                        USART_Printf("Removing card: %02X%02X%02X%02X\r\n", 
                                    card_id[0], card_id[1], card_id[2], card_id[3]);
                        FlashStorage_RemoveCard(card_id);
                    }
                    else
                    {
                        USART_Printf("Invalid card format. Use: delcard AABBCCDD\r\n");
                    }
                }
                else if (strcmp(command_buffer, "listcards") == 0)
                {
                    FlashStorage_PrintWhitelist();
                }
                else
                {
                    USART_Printf("Unknown command: %s\r\n", command_buffer);
                    USART_Printf("Available commands:\r\n");
                    USART_Printf("  addcard AABBCCDD  - Add a card to whitelist\r\n");
                    USART_Printf("  delcard AABBCCDD  - Remove a card from whitelist\r\n");
                    USART_Printf("  listcards         - List all cards in whitelist\r\n");
                }
                
                command_index = 0;
            }
        }
        else if (command_index < sizeof(command_buffer) - 1)
        {
            command_buffer[command_index++] = ch;
        }
    }
}
