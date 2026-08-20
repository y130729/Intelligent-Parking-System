#ifndef __FLASH_STORAGE_H
#define __FLASH_STORAGE_H

#include "stm32f10x.h"

#define MAX_WHITELIST_CARDS 20
#define CARD_ID_LENGTH 4
#define FLASH_SECTOR_ADDR 0x08007000

typedef struct {
    uint8_t card_count;
    uint8_t cards[MAX_WHITELIST_CARDS][CARD_ID_LENGTH];
} CardWhitelist;

void FlashStorage_Init(void);
void FlashStorage_WriteWhitelist(CardWhitelist *whitelist);
void FlashStorage_ReadWhitelist(CardWhitelist *whitelist);
uint8_t FlashStorage_AddCard(uint8_t *card_id);
uint8_t FlashStorage_RemoveCard(uint8_t *card_id);
uint8_t FlashStorage_IsCardValid(uint8_t *card_id);
void FlashStorage_PrintWhitelist(void);

#endif /* __FLASH_STORAGE_H */