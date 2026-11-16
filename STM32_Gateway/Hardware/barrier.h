#ifndef __BARRIER_H
#define __BARRIER_H

#include "stm32f10x.h"

/* 道闸安全逻辑（防砸 / 限位 / 电流）
 *
 * 现状（原型）：执行机构是 SG90 舵机，没有限位开关、没有电流检测，
 * 唯一能用的"防砸"信号是闸前的红外对射。
 *
 * 工程上必须区分"探测到什么"和"据此怎么动作"：
 *   - 红外被挡住 -> 绝不允许落杆（这是本项目唯一真实的防砸输入）
 *   - 限位开关   -> 只有装了才用，没装就按"舵机走完行程即到位"处理
 *   - 电机电流   -> 只有装了才用，堵转即判故障并保持开闸
 *
 * 三个能力都用编译开关控制，默认按现有硬件（只有红外）配置。
 * 换成真实道闸时，把两个开关改成 1 并接好线即可，业务层不用改。
 */

#define BARRIER_HAS_LIMIT_SWITCHES   0
#define BARRIER_HAS_CURRENT_SENSE    0

/* 落杆前确认安全的保持时间：检测到车辆仍在闸下时，保持开闸并重新计时 */
#define BARRIER_ANTICRUSH_HOLD_MS    2000
/* 连续这么多次"想落杆但被防砸挡住"就进故障态（保持开闸，等人处理） */
#define BARRIER_ANTICRUSH_MAX_RETRY  5

void    Barrier_Init(void);

/* 1 = 可以落杆；0 = 有车在闸下/故障，禁止落杆 */
uint8_t Barrier_CanClose(void);

/* 故障态：保持开闸，直到人工介入 */
uint8_t Barrier_HasFault(void);
void    Barrier_ClearFault(void);

/* 应急释放：切断舵机 PWM，闸杆靠自重落下（用于断电/故障逃生） */
void    Barrier_EmergencyRelease(void);

#endif /* __BARRIER_H */
