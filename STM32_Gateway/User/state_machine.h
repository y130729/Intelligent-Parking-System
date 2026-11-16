#ifndef __STATE_MACHINE_H
#define __STATE_MACHINE_H

#include "stm32f10x.h"
#include "link.h"

/* 道闸状态机
 *
 * 关键改动：所有"等待"都改成"看 SysTick 的截止时间"，函数立即返回。
 * 原来用 state_timer += 10 假设主循环严格 10ms 一轮，但主循环里
 * Servo_Up()/Servo_Down() 各阻塞 1.5s、错误提示阻塞 3s，
 * 于是"3 秒"其实可能是 6 秒甚至更久，抬落杆时序整个是漂的。
 */
typedef enum {
    STATE_IDLE = 0,        /* 空闲，等待车辆 */
    STATE_WAITING_RFID,    /* 车已到位，等刷卡/车牌凭证 */
    STATE_OPENING,         /* 舵机上行，等行程结束 */
    STATE_OPEN,            /* 开闸保持中 */
    STATE_CLOSING,         /* 舵机下行，等行程结束 */
    STATE_ERROR,           /* 显示错误信息（非阻塞），到时回 IDLE */
    STATE_FAULT            /* 安全故障：保持开闸，等人工/障碍解除 */
} SystemState;

extern SystemState current_state;
extern uint8_t remaining_spots;

/* volatile：在 EXTI 中断里置位、主循环里读，编译器不能把它缓存在寄存器里 */
extern volatile uint8_t vehicle_arrive_flag;

void StateMachine_Init(void);
void StateMachine_Process(void);
void StateMachine_SetVehicleArrive(void);          /* 仅 ISR 调用：只置标志 */
void StateMachine_UpdateRemainingSpots(uint8_t count);

/* reason: 开闸凭证来源（白名单枚举，避免任意字符串进下游） */
void StateMachine_RemoteOpen(LinkReason reason);
void StateMachine_RemoteClose(void);

#endif /* __STATE_MACHINE_H */
