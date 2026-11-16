#ifndef __FAULT_LOG_H
#define __FAULT_LOG_H

#include "stm32f10x.h"

/* 故障记录（HardFault / MemManage / BusFault / UsageFault）
 *
 * 原来的处理器是标准库给出的 while(1) 死循环：现场表现就是"设备没反应，
 * 只能断电重启"，而且一点线索都没留下。现在改为：把现场写进 Flash 保留页，
 * 然后复位；开机时打印出来并清掉（只打印一次，避免误以为反复出错）。
 *
 * 记录内容是 Cortex-M3 的故障状态寄存器与栈指针。这里**没有**保存出错时的
 * PC/LR 等压栈寄存器 —— 那需要在异常入口用汇编取 MSP/PSP。为保持
 * "armcc v5 / armclang / GCC 都能编译"，本模块刻意不含汇编；
 * CFSR + BFAR 通常已经足够定位问题（例如 PRECISERR + BFAR 就是访问地址）。
 */

/* Flash 分区（STM32F103C8T6，64KB，1KB/页）
 *   0x08000000 - 0x0800EFFF  应用程序（IROM1 = 0xF000，60KB）
 *   0x0800F000 - 0x0800F3FF  刷卡白名单槽 A（flash_storage.c）
 *   0x0800F400 - 0x0800F7FF  刷卡白名单槽 B
 *   0x0800FC00 - 0x0800FFFF  故障记录（本模块，最后一页）
 * 最后两段都在 Keil 的下载范围之外，所以升级固件不会擦掉白名单，也不会
 * 擦掉"上次为什么挂"的记录。若把 IROM1 放大到 0x10000，每次烧写都会
 * 顺带擦掉它们 —— 不要这么做。
 */
#define FAULT_LOG_ADDR  0x0800FC00u
#define FAULT_LOG_MAGIC 0x5041524Bu   /* 'PARK' */

typedef enum {
    FAULT_KIND_HARD = 1,
    FAULT_KIND_MEMMANAGE = 2,
    FAULT_KIND_BUS = 3,
    FAULT_KIND_USAGE = 4
} FaultKind;

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t kind;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;
    uint32_t mmfar;
    uint32_t msp;
    uint32_t psp;
} FaultRecord;

/* 由异常处理函数调用：记录现场 -> 复位（不返回） */
void FaultLog_Save(uint32_t kind);

/* 开机调用：若上次是异常复位，打印原因并清除记录 */
void FaultLog_ReportOnBoot(void);

#endif /* __FAULT_LOG_H */
