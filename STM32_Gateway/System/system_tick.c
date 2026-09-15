#include "system_tick.h"
#include "watchdog.h"

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
    uint32_t next_feed = start + WATCHDOG_FEED_INTERVAL_MS;

    while((sys_tick_ms - start) < ms)
    {
        /* 长延时（例如错误提示 3 秒）期间按固定间隔喂狗，避免正常流程被
           看门狗复位。注意这里用"走时"做条件：如果 SysTick 停了，
           sys_tick_ms 不再变化，本分支永远不会进入 -> 照样会复位，
           这正是我们想要的（时基坏了本身就是故障）。 */
        if(ms > WATCHDOG_FEED_INTERVAL_MS &&
           (int32_t)(sys_tick_ms - next_feed) >= 0)
        {
            Watchdog_Feed();
            next_feed += WATCHDOG_FEED_INTERVAL_MS;
        }
    }
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
