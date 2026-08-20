#include "system_tick.h"

static volatile uint32_t sys_tick_ms = 0;

void SysTick_Init(void)
{
    if(SysTick_Config(SystemCoreClock / 1000))
    {
        while(1);
    }
    NVIC_SetPriority(SysTick_IRQn, 0x00);
}

void delay_ms(uint32_t ms)
{
    uint32_t start = sys_tick_ms;
    while((sys_tick_ms - start) < ms);
}

void delay_us(uint32_t us)
{
    uint32_t i;
    for(i = 0; i < us * 8; i++);
}

uint32_t GetTick(void)
{
    return sys_tick_ms;
}

void SysTick_Tick(void)
{
    sys_tick_ms++;
}
