#ifndef __FLASH_STORAGE_H
#define __FLASH_STORAGE_H

#include "stm32f10x.h"

#define MAX_WHITELIST_CARDS 20
#define CARD_ID_LENGTH 4

/* Flash 分区（64KB，1KB/页）
 *   0x08000000 - 0x0800EFFF  应用（IROM1 = 0xF000，60KB）
 *   0x0800F000 - 0x0800F3FF  白名单槽 A
 *   0x0800F400 - 0x0800F7FF  白名单槽 B
 *   0x0800FC00 - 0x0800FFFF  故障记录（fault_log.c）
 *
 * 白名单与故障记录都放在 Flash 尾部：应用区可用到 60KB，而这两组页
 * 都在下载范围之外，烧写固件不会擦掉已登记的卡。
 *
 * FLASH_LEGACY_ADDR 是早期无头部格式的位置。首次运行时若新槽为空而该
 * 处数据合法，会一次性迁移过来，避免升级后丢失已登记的卡。
 */
#define FLASH_SLOT_A_ADDR 0x0800F000u
#define FLASH_SLOT_B_ADDR 0x0800F400u
#define FLASH_LEGACY_ADDR 0x08007000u
#define FLASH_SECTOR_ADDR FLASH_SLOT_A_ADDR   /* 兼容旧名字 */

typedef struct {
    uint8_t card_count;
    uint8_t cards[MAX_WHITELIST_CARDS][CARD_ID_LENGTH];
} CardWhitelist;

/* 掉电原子性说明
 * -----------------
 * 原来"擦整页 -> 逐字写"的写法，中途掉电就会得到空表或半张表，
 * 而读取端没有任何校验，会把半张表当成有效数据用。
 *
 * 现在改成 A/B 双槽 + 头部（magic/version/seq/len/crc）+ 最后一个字的提交标志：
 *   1. 永远写"较旧的那个槽"（不是当前正在用的那个）；
 *   2. 先清 commit，再写 payload，读回校验 CRC；
 *   3. 最后才写 commit。
 * 任何一步掉电，该槽的 commit 都不是有效值，读取时会被判无效并回退到另一个槽，
 * 所以"旧的白名单"永远可用，不会出现半张表。
 *
 * 这不能替代 PVD（掉电中断）——它保证的是"不会读到坏数据"，
 * 而不是"这次写入一定成功"。写入失败会上报并保留旧数据。
 */

void    FlashStorage_Init(void);
void    FlashStorage_WriteWhitelist(CardWhitelist *whitelist);
void    FlashStorage_ReadWhitelist(CardWhitelist *whitelist);
uint8_t FlashStorage_AddCard(uint8_t *card_id);
uint8_t FlashStorage_RemoveCard(uint8_t *card_id);
uint8_t FlashStorage_IsCardValid(uint8_t *card_id);
void    FlashStorage_PrintWhitelist(void);
uint8_t FlashStorage_EraseAll(void);       /* 产测/换车：两槽都清空 */
uint8_t FlashStorage_CardCount(void);
uint32_t FlashStorage_Seq(void);           /* 当前有效槽的序号（排障用） */

#endif /* __FLASH_STORAGE_H */
