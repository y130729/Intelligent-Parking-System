#include "fault_log.h"
#include "USART.h"
#include <string.h>

/* 位运算实现的 CRC32（多项式 0xEDB88320），用于判断故障记录是否写完整。
   不用查表版本：这里只在上电/复位路径跑一次，省 Flash 更划算。 */
static uint32_t FaultLog_Crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint8_t bit;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8u; bit++)
        {
            if (crc & 1u)
                crc = (crc >> 1) ^ 0xEDB88320u;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

void FaultLog_Save(uint32_t kind)
{
    FaultRecord rec;
    uint32_t *src = (uint32_t *)&rec;
    uint32_t words = sizeof(FaultRecord) / sizeof(uint32_t);
    uint32_t i;

    memset(&rec, 0, sizeof(rec));
    rec.magic = FAULT_LOG_MAGIC;
    rec.kind  = kind;
    rec.cfsr  = SCB->CFSR;
    rec.hfsr  = SCB->HFSR;
    rec.bfar  = SCB->BFAR;
    rec.mmfar = SCB->MMFAR;
    rec.msp   = __get_MSP();
    rec.psp   = __get_PSP();
    rec.crc   = FaultLog_Crc32((const uint8_t *)&rec, sizeof(rec));

    /* 故障状态下写 Flash：Flash 编程不需要中断，可以安全执行。
       写失败也不再重试 —— 反正马上就要复位了。 */
    FLASH_Unlock();
    if (FLASH_ErasePage(FAULT_LOG_ADDR) == FLASH_COMPLETE)
    {
        for (i = 0; i < words; i++)
        {
            if (FLASH_ProgramWord(FAULT_LOG_ADDR + i * 4u, src[i]) != FLASH_COMPLETE)
                break;
        }
    }
    FLASH_Lock();

    USART_Printf("\r\n*** FAULT kind=%u CFSR=%08X HFSR=%08X BFAR=%08X MMFAR=%08X ***\r\n",
                 (unsigned int)rec.kind, (unsigned int)rec.cfsr,
                 (unsigned int)rec.hfsr, (unsigned int)rec.bfar,
                 (unsigned int)rec.mmfar);
    USART_Printf("*** FS: watchdog will recover after reset ***\r\n");

    NVIC_SystemReset();
    while (1) { }
}

void FaultLog_ReportOnBoot(void)
{
    const FaultRecord *flash = (const FaultRecord *)FAULT_LOG_ADDR;
    FaultRecord copy;
    uint32_t crc;

    memcpy(&copy, flash, sizeof(copy));
    if (copy.magic != FAULT_LOG_MAGIC)
        return;                      /* 首次上电或区域被擦除过 */

    crc = copy.crc;
    copy.crc = 0;
    if (FaultLog_Crc32((const uint8_t *)&copy, sizeof(copy)) != crc)
    {
        USART_Printf("[FAULT] record CRC mismatch, ignored\r\n");
        return;
    }

    USART_Printf("[FAULT] last reset was an exception: kind=%u CFSR=%08X HFSR=%08X "
                 "BFAR=%08X MMFAR=%08X\r\n",
                 (unsigned int)copy.kind, (unsigned int)copy.cfsr,
                 (unsigned int)copy.hfsr, (unsigned int)copy.bfar,
                 (unsigned int)copy.mmfar);
    USART_Printf("[FAULT] hint: kind 1=HardFault 2=MemManage 3=BusFault 4=UsageFault; "
                 "CFSR bit 8-15 BUSFAULT, 16-23 USGFAULT\r\n");

    /* 打印过就清掉，避免每次开机都以为是刚出错 */
    FLASH_Unlock();
    FLASH_ErasePage(FAULT_LOG_ADDR);
    FLASH_Lock();
}
