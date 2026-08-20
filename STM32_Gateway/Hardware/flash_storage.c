#include "flash_storage.h"
#include "USART.h"
#include <string.h>

static CardWhitelist whitelist;

void FlashStorage_Init(void)
{
    FlashStorage_ReadWhitelist(&whitelist);
    
    if (whitelist.card_count == 0xFF || whitelist.card_count > MAX_WHITELIST_CARDS)
    {
        USART_Printf("FlashStorage: No valid whitelist found, initializing with default cards\r\n");
        whitelist.card_count = 0;
        
        uint8_t default_card[CARD_ID_LENGTH] = {0x75, 0x06, 0xD5, 0x06};
        FlashStorage_AddCard(default_card);
        
        FlashStorage_WriteWhitelist(&whitelist);
    }
    
    USART_Printf("FlashStorage: Initialized with %d cards\r\n", whitelist.card_count);
}

void FlashStorage_WriteWhitelist(CardWhitelist *wl)
{
    FLASH_Status status;
    
    status = FLASH_ErasePage(FLASH_SECTOR_ADDR);
    if (status != FLASH_COMPLETE)
    {
        USART_Printf("FlashStorage: Erase failed, status: %d\r\n", status);
        return;
    }
    
    uint32_t *src = (uint32_t *)wl;
    uint32_t *dst = (uint32_t *)FLASH_SECTOR_ADDR;
    uint32_t size = sizeof(CardWhitelist) / sizeof(uint32_t);
    
    for (uint32_t i = 0; i < size; i++)
    {
        status = FLASH_ProgramWord(FLASH_SECTOR_ADDR + i * 4, src[i]);
        if (status != FLASH_COMPLETE)
        {
            USART_Printf("FlashStorage: Program failed at offset %d, status: %d\r\n", i * 4, status);
            return;
        }
    }
    
    USART_Printf("FlashStorage: Whitelist written successfully\r\n");
}

void FlashStorage_ReadWhitelist(CardWhitelist *wl)
{
    uint32_t *src = (uint32_t *)FLASH_SECTOR_ADDR;
    uint32_t *dst = (uint32_t *)wl;
    uint32_t size = sizeof(CardWhitelist) / sizeof(uint32_t);
    
    for (uint32_t i = 0; i < size; i++)
    {
        dst[i] = src[i];
    }
}

uint8_t FlashStorage_AddCard(uint8_t *card_id)
{
    if (whitelist.card_count >= MAX_WHITELIST_CARDS)
    {
        USART_Printf("FlashStorage: Whitelist is full\r\n");
        return 0;
    }
    
    for (uint8_t i = 0; i < whitelist.card_count; i++)
    {
        if (memcmp(whitelist.cards[i], card_id, CARD_ID_LENGTH) == 0)
        {
            USART_Printf("FlashStorage: Card already exists\r\n");
            return 0;
        }
    }
    
    memcpy(whitelist.cards[whitelist.card_count], card_id, CARD_ID_LENGTH);
    whitelist.card_count++;
    
    FlashStorage_WriteWhitelist(&whitelist);
    
    USART_Printf("FlashStorage: Card added successfully (total: %d)\r\n", whitelist.card_count);
    return 1;
}

uint8_t FlashStorage_RemoveCard(uint8_t *card_id)
{
    for (uint8_t i = 0; i < whitelist.card_count; i++)
    {
        if (memcmp(whitelist.cards[i], card_id, CARD_ID_LENGTH) == 0)
        {
            for (uint8_t j = i; j < whitelist.card_count - 1; j++)
            {
                memcpy(whitelist.cards[j], whitelist.cards[j + 1], CARD_ID_LENGTH);
            }
            whitelist.card_count--;
            
            FlashStorage_WriteWhitelist(&whitelist);
            
            USART_Printf("FlashStorage: Card removed successfully (total: %d)\r\n", whitelist.card_count);
            return 1;
        }
    }
    
    USART_Printf("FlashStorage: Card not found\r\n");
    return 0;
}

uint8_t FlashStorage_IsCardValid(uint8_t *card_id)
{
    for (uint8_t i = 0; i < whitelist.card_count; i++)
    {
        if (memcmp(whitelist.cards[i], card_id, CARD_ID_LENGTH) == 0)
        {
            return 1;
        }
    }
    return 0;
}

void FlashStorage_PrintWhitelist(void)
{
    USART_Printf("FlashStorage: Whitelist (%d cards):\r\n", whitelist.card_count);
    for (uint8_t i = 0; i < whitelist.card_count; i++)
    {
        USART_Printf("  [%d] %02X%02X%02X%02X\r\n", i + 1, 
                     whitelist.cards[i][0], whitelist.cards[i][1],
                     whitelist.cards[i][2], whitelist.cards[i][3]);
    }
}