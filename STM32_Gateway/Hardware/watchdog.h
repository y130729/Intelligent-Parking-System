#ifndef __WATCHDOG_H
#define __WATCHDOG_H

#include "stm32f10x.h"

/* 独立看门狗（IWDG）
 *
 * 为什么需要：本项目是裸机主循环，一旦程序死在某处（外设虚焊导致寄存器
 * 死等、指针跑飞、SPI 时序被打断……），现场表现就是"设备彻底没反应，
 * 只能断电重启"。加了 IWDG 之后，设备能自己恢复。
 *
 * 超时取 4 秒：
 *   - 必须大于最长的正常阻塞（错误提示 ERROR_DISPLAY_DELAY_MS = 3 秒）；
 *   - 又要足够短，让故障设备几秒内自愈。
 * 长延时（delay_ms）期间由 system_tick.c 按 WATCHDOG_FEED_INTERVAL_MS
 * 间隔喂狗 —— 注意喂狗条件依赖 SysTick 走时，若 SysTick 停了则不会喂，
 * 看门狗仍会复位，这是有意为之。
 */
#define WATCHDOG_TIMEOUT_MS        4000u
#define WATCHDOG_FEED_INTERVAL_MS  500u

void Watchdog_Init(void);
void Watchdog_Feed(void);

#endif /* __WATCHDOG_H */
