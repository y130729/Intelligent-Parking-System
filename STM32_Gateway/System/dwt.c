#include "dwt.h"

/* CMSIS 的 core_cm3.h 版本较老时没有 DWT_Type，这里直接用寄存器地址，
   避免为了一个计数器去改厂商库。地址见 ARMv7-M 架构参考手册。 */
#define DWT_CTRL_REG    (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT_REG  (*(volatile uint32_t *)0xE0001004UL)
#define DEMCR_REG       (*(volatile uint32_t *)0xE000EDFCUL)

#define DEMCR_TRCENA    (1UL << 24)
#define DWT_CYCCNTENA   (1UL << 0)

void DWT_Init(void)
{
    /* DEMCR.TRCENA 必须打开，否则 DWT 计数器不走 */
    DEMCR_REG |= DEMCR_TRCENA;
    DWT_CYCCNT_REG = 0;
    DWT_CTRL_REG |= DWT_CYCCNTENA;
}

uint32_t DWT_GetCycles(void)
{
    return DWT_CYCCNT_REG;
}

uint32_t DWT_CyclesToUs(uint32_t cycles)
{
    /* SystemCoreClock = 72MHz -> 1us = 72 周期 */
    return cycles / (SystemCoreClock / 1000000u);
}
