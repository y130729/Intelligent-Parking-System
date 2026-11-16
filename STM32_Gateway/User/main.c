#include "stm32f10x.h"
#include "misc.h"
#include "system_tick.h"
#include "dwt.h"
#include "USART.h"
#include "Serial.h"
#include "OLED.h"
#include "MFRC522.h"
#include "Infrared.h"
#include "Servo.h"
#include "project_config.h"
#include "state_machine.h"
#include "link.h"
#include "watchdog.h"
#include "fault_log.h"
#include <stdio.h>
#include <string.h>

void System_Init(void);
void OLED_DisplayRemainingSpots(void);
void ProcessUltrasonicData(void);
void ProcessPendingSpotUpdate(void);
void OLED_DisplayPlateNumber(const char *plate);
void ReportIsrTiming(void);

uint8_t spot_occupied[MAX_SPOT_ID] = {0};
uint8_t ultrasonic_new_data_flag = 0;

/* EXTI 中断耗时统计（DWT 周期数）
 * 中断里不做任何 IO，只有一次 GPIO 读 + 一次置标志，
 * 具体多快由 ReportIsrTiming() 在主循环里打出来。 */
static volatile uint32_t infrared_isr_cycles_max = 0;

int main(void)
{
    System_Init();
    
    while (1)
    {
        StateMachine_Process();
        USART_Process();                    // 处理调试串口命令（环形缓冲取行）
        OLED_DisplayRemainingSpots();
        Link_Process();                     // 收树莓派命令 + ACK 超时重传
        Serial_Process();                   // 处理CC2530超声波数据（环形缓冲取行）
        ProcessUltrasonicData();

        ProcessPendingSpotUpdate();         // 排空"车位变化位图"
        ReportIsrTiming();                  // 刷新 ISR 最坏耗时（只在破纪录时打印）

        Watchdog_Feed();                    // 只要主循环还在转，就不会被 IWDG 复位
        delay_ms(10);
    }
}

void System_Init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SysTick_Init();
    DWT_Init();
    /* 尽早打开看门狗：初始化阶段卡死同样能自愈。
       注意 delay_ms 会在长延时期间喂狗，正常初始化不会被误复位。 */
    Watchdog_Init();
    USART_Init_Config();
    Serial_Init();
    OLED_Init();
    OLED_Clear();
    MFRC522_Init();
    Infrared_Init();
    Servo_Init();

    /* 若上次是异常复位，这里会把现场打印出来（并清除记录） */
    FaultLog_ReportOnBoot();

    OLED_ShowString(1, 1, "Smart Parking");
    OLED_ShowString(2, 1, "System v1.0");
    OLED_ShowString(3, 1, "Initializing...");
    delay_ms(1000);

    StateMachine_Init();
    Link_Init();

    OLED_ShowString(3, 1, "System Ready   ");
    StateMachine_UpdateRemainingSpots(TOTAL_SPOTS);
    OLED_DisplayPlateNumber(NULL);  // 预留第4行显示车牌号（占位，收到树莓派车牌后刷新）
    USART_Printf("System Init OK\r\n");
    USART_Printf("Total spots: %d\r\n", TOTAL_SPOTS);
    USART_Printf("CC2530 Coordinator connected via USART3\r\n");
}

void OLED_DisplayRemainingSpots(void)
{
    static char spot_str[24];
    static uint8_t last_available = 0xFF;
    
    uint8_t occupied_count = 0;
    uint8_t i;
    uint8_t available_spots;

    for (i = 0; i < MAX_SPOT_ID; i++)
    {
        if (spot_occupied[i])
        {
            occupied_count++;
        }
    }
    
    available_spots = MAX_SPOT_ID - occupied_count;
    
    if (last_available != available_spots)
    {
        snprintf(spot_str, sizeof(spot_str), "Spots: %d/20      ", available_spots);
        OLED_ShowString(2, 1, spot_str);
        last_available = available_spots;
    }
}

void OLED_DisplayPlateNumber(const char *plate)
{
    static char current_plate[16] = "------";
    static char plate_line[24];

    if (plate != NULL)
    {
        strncpy(current_plate, plate, sizeof(current_plate) - 1);
        current_plate[sizeof(current_plate) - 1] = '\0';
    }
    OLED_ShowString(4, 1, "                ");
    snprintf(plate_line, sizeof(plate_line), "Plate:%s", current_plate);
    OLED_ShowCNString(4, 1, plate_line);
}

void ProcessUltrasonicData(void)
{
    if (ultrasonic_new_data_flag)
    {
        uint8_t occupied_count = 0;
        uint8_t i;

        ultrasonic_new_data_flag = 0;

        for (i = 0; i < MAX_SPOT_ID; i++)
        {
            if (spot_occupied[i]) occupied_count++;
        }
        /* 汇总行只写调试串口；逐车位状态已经由 Link_ReportSpot 发给树莓派 */
        USART_Printf("Occupancy: %d/%d\r\n", occupied_count, MAX_SPOT_ID);
    }
}

/* 排空"车位变化位图"。
 *
 * 原来是单槽 pending_spot_id/status：同一轮里多个节点变化，
 * 后写的会把前面的覆盖掉。现在按位图逐位上报，同一车位只报最终态。
 * 位图由 Serial_Process（同为主循环上下文）置位，因此这里读+清不需要关中断。 */
extern volatile uint32_t spot_dirty_mask;
extern uint8_t spot_fault[MAX_SPOT_ID];

void ProcessPendingSpotUpdate(void)
{
    uint32_t mask = spot_dirty_mask;
    uint8_t i;
    uint8_t occupied_count = 0;

    if (mask == 0) return;
    spot_dirty_mask &= ~mask;

    for (i = 0; i < MAX_SPOT_ID; i++)
    {
        if ((mask & (1UL << i)) == 0) continue;

        if (spot_fault[i])
        {
            /* 传感器故障要单独上报，绝不能当成"空位" */
            Link_ReportSensorFault((uint8_t)(i + 1));
        }
        else
        {
            Link_ReportSpot((uint8_t)(i + 1), spot_occupied[i]);
        }
    }

    for (i = 0; i < MAX_SPOT_ID; i++)
    {
        if (spot_occupied[i]) occupied_count++;
    }
    StateMachine_UpdateRemainingSpots((uint8_t)(MAX_SPOT_ID - occupied_count));
}

/* 中断里什么都不做，只置标志 + 清中断源。
 *
 * 原来这里直接 USART_Printf(...)：115200 下 "Infrared triggered, vehicle arrived\r\n"
 * 有 38 字节，约 3.3ms。这 3.3ms 里 USART3（CC2530 数据）的 RXNE 中断
 * 无法得到服务，而 CC2530 是 115200 连续发——直接从"偶发丢字节"变成"必丢"。
 * 现在中断体只有一次 GPIO 读和一次赋值，耗时用 DWT 量出来打在日志里。 */
void EXTI15_10_IRQHandler(void)
{
    uint32_t t0 = DWT_GetCycles();

    if (EXTI_GetITStatus(EXTI_Line11) != RESET)
    {
        if (Infrared_Is_Blocked())
        {
            StateMachine_SetVehicleArrive();
        }
        EXTI_ClearITPendingBit(EXTI_Line11);
    }

    {
        uint32_t dt = DWT_GetCycles() - t0;
        if (dt > infrared_isr_cycles_max)
        {
            infrared_isr_cycles_max = dt;
        }
    }
}

/* 把 ISR 的最坏耗时打出来（只在刷新最大值时打印，不刷屏）。
 * 这条日志就是"中断多快"的直接答案。 */
void ReportIsrTiming(void)
{
    static uint32_t reported = 0;
    uint32_t cur = infrared_isr_cycles_max;

    if (cur != reported && cur != 0)
    {
        reported = cur;
        USART_Printf("ISR: EXTI11 worst = %lu cycles (%lu us @72MHz)\r\n",
                     (unsigned long)cur,
                     (unsigned long)DWT_CyclesToUs(cur));
    }
}
