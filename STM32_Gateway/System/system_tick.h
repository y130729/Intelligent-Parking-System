#ifndef __SYSTEM_TICK_H
#define __SYSTEM_TICK_H

#include "stm32f10x.h"

void SysTick_Init(void);
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);
uint32_t GetTick(void);
void SysTick_Tick(void);

#endif /* __SYSTEM_TICK_H */
