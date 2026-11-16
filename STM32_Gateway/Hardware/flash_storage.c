#include "flash_storage.h"
#include "USART.h"
#include <string.h>

#define WL_MAGIC      0x504B574Cu   /* 'PKWL' */
#define WL_VERSION    2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t seq;           /* 越大越新 */
    uint32_t payload_len;   /* CardWhitelist 的字节数 */
    uint32_t crc;           /* payload 的 CRC32 */
    uint32_t commit;        /* 最后一个写；等于 WL_COMMIT 才有效 */
} WhitelistHeader;

#define WL_COMMIT     0x434F4D54u   /* 'COMT' */

/* CardWhitelist = 1 + 20*4 = 81 字节，补齐到 4 的倍数再按字烧写 */
#define WL_PAYLOAD_LEN   ((uint32_t)sizeof(CardWhitelist))
#define WL_PAYLOAD_PAD   (((WL_PAYLOAD_LEN) + 3u) & ~3u)

typedef char wl_header_must_be_word_multiple
    [((sizeof(WhitelistHeader) & 3u) == 0) ? 1 : -1];

static CardWhitelist whitelist;
static uint32_t whitelist_seq = 0;
static uint32_t active_slot = FLASH_SLOT_A_ADDR;
static uint8_t  whitelist_valid = 0;

static uint8_t wl_write_buf[WL_PAYLOAD_PAD];

/* --- CRC32（多项式 0xEDB88320，不建表以省 Flash） ------------------ */
static uint32_t crc32_bytes(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint8_t  b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint32_t)p[i];
        for (b = 0; b < 8; b++)
        {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc = (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* --- 底层读写 ------------------------------------------------------ */

static uint8_t program_words(uint32_t addr, const void *data, uint32_t len)
{
    const uint32_t *src = (const uint32_t *)data;
    uint32_t words = (len + 3u) / 4u;
    uint32_t i;

    for (i = 0; i < words; i++)
    {
        if (FLASH_ProgramWord(addr + i * 4u, src[i]) != FLASH_COMPLETE)
        {
            return 0;
        }
    }
    return 1;
}

static uint8_t read_slot(uint32_t addr, WhitelistHeader *hdr, CardWhitelist *out, uint32_t *seq)
{
    const WhitelistHeader *h = (const WhitelistHeader *)addr;

    /* 直接读 Flash 里的头部（它是只读的，不需要先拷出来） */
    if (h->magic != WL_MAGIC) return 0;
    if (h->version != WL_VERSION) return 0;
    if (h->commit != WL_COMMIT) return 0;
    if (h->payload_len != WL_PAYLOAD_LEN) return 0;

    memcpy(out, (const void *)(addr + sizeof(WhitelistHeader)), WL_PAYLOAD_LEN);

    if (out->card_count > MAX_WHITELIST_CARDS) return 0;
    if (crc32_bytes((const uint8_t *)out, WL_PAYLOAD_LEN) != h->crc) return 0;

    if (hdr) *hdr = *h;
    if (seq) *seq = h->seq;
    return 1;
}

/* --- 对外接口 ------------------------------------------------------ */

void FlashStorage_Init(void)
{
    WhitelistHeader ha, hb;
    CardWhitelist   wa, wb;
    uint8_t va, vb;

    memset(&whitelist, 0, sizeof(whitelist));

    va = read_slot(FLASH_SLOT_A_ADDR, &ha, &wa, NULL);
    vb = read_slot(FLASH_SLOT_B_ADDR, &hb, &wb, NULL);

    if (va && vb)
    {
        /* 两槽都有效：取序号大的（新写的那个） */
        if (hb.seq >= ha.seq) { whitelist = wb; whitelist_seq = hb.seq; active_slot = FLASH_SLOT_B_ADDR; }
        else                   { whitelist = wa; whitelist_seq = ha.seq; active_slot = FLASH_SLOT_A_ADDR; }
        whitelist_valid = 1;
    }
    else if (va)
    {
        whitelist = wa; whitelist_seq = ha.seq; active_slot = FLASH_SLOT_A_ADDR; whitelist_valid = 1;
    }
    else if (vb)
    {
        whitelist = wb; whitelist_seq = hb.seq; active_slot = FLASH_SLOT_B_ADDR; whitelist_valid = 1;
    }

    if (!whitelist_valid)
    {
        /* 老固件把白名单裸放在 0x08007000（没有头部/CRC）。
           升级到新固件后如果直接当空库处理，车主的卡就"突然全没了"。
           这里做一次一次性迁移：只在能通过基本合理性检查时才导入。 */
        const CardWhitelist *legacy = (const CardWhitelist *)FLASH_LEGACY_ADDR;

        if (legacy->card_count > 0 && legacy->card_count <= MAX_WHITELIST_CARDS)
        {
            uint8_t i, plausible = 1;
            for (i = 0; i < legacy->card_count; i++)
            {
                uint8_t j;
                /* 全 0 / 全 FF 的卡号说明这页根本没写过或已擦除 */
                uint8_t any = 0, all_ff = 1;
                for (j = 0; j < CARD_ID_LENGTH; j++)
                {
                    if (legacy->cards[i][j] != 0x00) any = 1;
                    if (legacy->cards[i][j] != 0xFF) all_ff = 0;
                }
                if (!any || all_ff) { plausible = 0; break; }
            }

            if (plausible)
            {
                whitelist = *legacy;
                whitelist_seq = 0;
                whitelist_valid = 1;
                USART_Printf("FlashStorage: migrating %d cards from legacy slot "
                             "(0x08007000)\r\n", whitelist.card_count);
                FlashStorage_WriteWhitelist(&whitelist);
                return;
            }
        }

        /* 重要：这里不再"自动写入默认卡"。
           内置默认卡（例如 A1B2C3D4） 等于一个公开后门——任何人拿这张卡都能进。
           空库就是空库，必须由产测工装/管理员显式 addcard。 */
        whitelist.card_count = 0;
        whitelist_seq = 0;
        USART_Printf("FlashStorage: no valid whitelist, starting EMPTY "
                     "(provision with 'addcard AABBCCDD')\r\n");
    }

    USART_Printf("FlashStorage: %d cards, seq=%lu, slot=%c\r\n",
                 whitelist.card_count,
                 (unsigned long)whitelist_seq,
                 (active_slot == FLASH_SLOT_B_ADDR) ? 'B' : 'A');
}

uint32_t FlashStorage_Seq(void)
{
    return whitelist_seq;
}

uint8_t FlashStorage_CardCount(void)
{
    return whitelist.card_count;
}

/* 写到"当前没在用的那个槽"，成功后才切换 active_slot */
void FlashStorage_WriteWhitelist(CardWhitelist *wl)
{
    uint32_t target;
    WhitelistHeader hdr;
    uint32_t crc;
    uint8_t  ok;

    if (wl == NULL) return;

    target = (active_slot == FLASH_SLOT_A_ADDR) ? FLASH_SLOT_B_ADDR : FLASH_SLOT_A_ADDR;

    memset(wl_write_buf, 0, sizeof(wl_write_buf));
    memcpy(wl_write_buf, wl, WL_PAYLOAD_LEN);
    crc = crc32_bytes(wl_write_buf, WL_PAYLOAD_LEN);

    if (FLASH_ErasePage(target) != FLASH_COMPLETE)
    {
        USART_Printf("FlashStorage: erase failed, keep old whitelist\r\n");
        return;
    }

    hdr.magic = WL_MAGIC;
    hdr.version = WL_VERSION;
    hdr.seq = whitelist_seq + 1u;
    hdr.payload_len = WL_PAYLOAD_LEN;
    hdr.crc = crc;
    hdr.commit = 0;                 /* 先不提交 */

    if (!program_words(target, &hdr, sizeof(hdr)))
    {
        USART_Printf("FlashStorage: header program failed\r\n");
        return;
    }
    if (!program_words(target + sizeof(WhitelistHeader), wl_write_buf, WL_PAYLOAD_PAD))
    {
        USART_Printf("FlashStorage: payload program failed\r\n");
        return;
    }

    /* 读回校验：Flash 写失败在掉压时并不总会返回错误，必须自己验 */
    {
        CardWhitelist check;
        memcpy(&check, (const void *)(target + sizeof(WhitelistHeader)), WL_PAYLOAD_LEN);
        if (crc32_bytes((const uint8_t *)&check, WL_PAYLOAD_LEN) != crc ||
            memcmp(&check, wl, WL_PAYLOAD_LEN) != 0)
        {
            USART_Printf("FlashStorage: verify failed, whitelist NOT committed\r\n");
            return;
        }
    }

    /* 最后一步：写提交标志。写之前任何掉电都只会留下"无效槽" */
    ok = (FLASH_ProgramWord(target + (sizeof(WhitelistHeader) - 4u), WL_COMMIT) == FLASH_COMPLETE);
    if (!ok)
    {
        USART_Printf("FlashStorage: commit failed, whitelist NOT committed\r\n");
        return;
    }

    {
        const WhitelistHeader *h = (const WhitelistHeader *)target;
        if (h->commit != WL_COMMIT || h->seq != hdr.seq)
        {
            USART_Printf("FlashStorage: readback mismatch, keep old whitelist\r\n");
            return;
        }
    }

    active_slot = target;
    whitelist_seq = hdr.seq;
    whitelist_valid = 1;
    USART_Printf("FlashStorage: written to slot %c, seq=%lu\r\n",
                 (target == FLASH_SLOT_B_ADDR) ? 'B' : 'A',
                 (unsigned long)whitelist_seq);
}

void FlashStorage_ReadWhitelist(CardWhitelist *wl)
{
    if (wl == NULL) return;
    *wl = whitelist;
}

uint8_t FlashStorage_AddCard(uint8_t *card_id)
{
    uint8_t i;

    if (card_id == NULL) return 0;
    if (whitelist.card_count >= MAX_WHITELIST_CARDS)
    {
        USART_Printf("FlashStorage: Whitelist is full\r\n");
        return 0;
    }

    for (i = 0; i < whitelist.card_count; i++)
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

    USART_Printf("FlashStorage: Card added (total: %d)\r\n", whitelist.card_count);
    return 1;
}

uint8_t FlashStorage_RemoveCard(uint8_t *card_id)
{
    uint8_t i, j;

    if (card_id == NULL) return 0;

    for (i = 0; i < whitelist.card_count; i++)
    {
        if (memcmp(whitelist.cards[i], card_id, CARD_ID_LENGTH) == 0)
        {
            for (j = i; j + 1 < whitelist.card_count; j++)
            {
                memcpy(whitelist.cards[j], whitelist.cards[j + 1], CARD_ID_LENGTH);
            }
            memset(whitelist.cards[whitelist.card_count - 1], 0, CARD_ID_LENGTH);
            whitelist.card_count--;

            FlashStorage_WriteWhitelist(&whitelist);

            USART_Printf("FlashStorage: Card removed (total: %d)\r\n", whitelist.card_count);
            return 1;
        }
    }

    USART_Printf("FlashStorage: Card not found\r\n");
    return 0;
}

uint8_t FlashStorage_IsCardValid(uint8_t *card_id)
{
    uint8_t i;

    if (card_id == NULL) return 0;

    for (i = 0; i < whitelist.card_count; i++)
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
    uint8_t i;

    USART_Printf("FlashStorage: Whitelist (%d cards, seq=%lu):\r\n",
                 whitelist.card_count, (unsigned long)whitelist_seq);
    for (i = 0; i < whitelist.card_count; i++)
    {
        USART_Printf("  [%d] %02X%02X%02X%02X\r\n", i + 1,
                     whitelist.cards[i][0], whitelist.cards[i][1],
                     whitelist.cards[i][2], whitelist.cards[i][3]);
    }
}

uint8_t FlashStorage_EraseAll(void)
{
    if (FLASH_ErasePage(FLASH_SLOT_A_ADDR) != FLASH_COMPLETE) return 0;
    if (FLASH_ErasePage(FLASH_SLOT_B_ADDR) != FLASH_COMPLETE) return 0;

    memset(&whitelist, 0, sizeof(whitelist));
    whitelist_seq = 0;
    whitelist_valid = 0;
    active_slot = FLASH_SLOT_A_ADDR;
    USART_Printf("FlashStorage: all slots erased\r\n");
    return 1;
}
