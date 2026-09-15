#include "watchdog.h"

/* LSI 约 40kHz，预分频 64 -> 625Hz
   重装值 = 625 * 4s = 2500 -> 超时 4.0 秒 */
#define WATCHDOG_PRESCALER  IWDG_Prescaler_64
#define WATCHDOG_RELOAD     2500u

void Watchdog_Init(void)
{
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(WATCHDOG_PRESCALER);
    IWDG_SetReload(WATCHDOG_RELOAD);
    IWDG_ReloadCounter();
    IWDG_Enable();          /* 使能后无法关闭，只能靠喂狗维持 */
}

void Watchdog_Feed(void)
{
    IWDG_ReloadCounter();
}
