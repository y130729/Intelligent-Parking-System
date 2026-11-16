#ifndef __DWT_H
#define __DWT_H

#include "stm32f10x.h"

/* Cortex-M3 的 DWT 周期计数器：用来量"这段代码跑了多少 CPU 周期"。
 *
 * 存在意义：面试里被问"你的中断多快"时，能直接给数字（例如
 * "EXTI 中断最坏 1.8us @72MHz"），而不是"应该很快吧"。
 * 它也是排查"主循环为什么慢"的第一手工具，成本几乎为零。
 *
 * 注意：CYCCNT 是 32 位，72MHz 下约 59.6 秒回绕一次。
 * 因此只能量短片段（中断、函数），不能做长期计时——长期计时用 SysTick。
 */
void     DWT_Init(void);
uint32_t DWT_GetCycles(void);
uint32_t DWT_CyclesToUs(uint32_t cycles);

#endif /* __DWT_H */
