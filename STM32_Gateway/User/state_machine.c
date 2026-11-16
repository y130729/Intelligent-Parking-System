#include "state_machine.h"
#include "OLED.h"
#include "MFRC522.h"
#include "Servo.h"
#include "Infrared.h"
#include "barrier.h"
#include "project_config.h"
#include "system_tick.h"
#include "USART.h"
#include "link.h"
#include "flash_storage.h"
#include <string.h>
#include <stdio.h>

SystemState current_state = STATE_IDLE;
uint8_t remaining_spots = TOTAL_SPOTS;
volatile uint8_t vehicle_arrive_flag = 0;

/* 当前状态的截止时间（SysTick 毫秒）。0 表示"无超时"。 */
static uint32_t state_deadline = 0;
static uint8_t  anticrush_retry = 0;

/* --- 小工具 ------------------------------------------------------ */

static void goto_state(SystemState next, uint32_t delay_ms)
{
    current_state = next;
    state_deadline = (delay_ms == 0) ? 0 : (GetTick() + delay_ms);
}

/* 到期判断：用"减法 + 有符号比较"，SysTick 回绕（49.7 天）也正确 */
static uint8_t deadline_reached(void)
{
    if (state_deadline == 0) return 0;
    return ((int32_t)(GetTick() - state_deadline) >= 0) ? 1 : 0;
}

static void show_error(const char *msg, uint32_t ms)
{
    OLED_ShowString(3, 1, "                ");
    OLED_ShowString(3, 1, (char *)msg);   /* OLED 只读，不修改内容 */
    goto_state(STATE_ERROR, ms);
}

/* 开闸统一入口：舵机动作与状态迁移只出现在这一处，
   避免"刷卡开闸"和"远程开闸"两条路径的行为慢慢走偏。 */
static void start_open(LinkReason reason)
{
    Servo_SetUp();                 /* 只设角度，不阻塞 1.5s */
    anticrush_retry = 0;
    OLED_ShowString(3, 1, "Entry Allowed   ");
    USART_Printf("Opening barrier (%s)\r\n", Link_ReasonToString(reason));
    /* 立即回报，让树莓派的 5s 命令等待能拿到应答；
       舵机行程在 STATE_OPENING 里以 1500ms 的截止时间体现。 */
    Link_ReportBarrierUp(reason);
    goto_state(STATE_OPENING, BARRIER_MOVE_MS);
}

static void start_close(void)
{
    Servo_SetDown();
    OLED_ShowString(3, 1, "Closing Barrier ");
    USART_Printf("Closing barrier\r\n");
    Link_ReportBarrierDown();
    goto_state(STATE_CLOSING, BARRIER_MOVE_MS);
}

/* --- 对外接口 ---------------------------------------------------- */

void StateMachine_Init(void)
{
    remaining_spots = TOTAL_SPOTS;
    vehicle_arrive_flag = 0;
    anticrush_retry = 0;
    Barrier_Init();
    FlashStorage_Init();
    goto_state(STATE_IDLE, 0);
}

void StateMachine_SetVehicleArrive(void)
{
    /* ISR 里只做这一件事：置标志。
       原来这里还会 USART_Printf，115200 下几十字节要阻塞 2~3ms，
       期间所有低优先级中断（含 USART 接收）都被压住。 */
    vehicle_arrive_flag = 1;
}

void StateMachine_UpdateRemainingSpots(uint8_t count)
{
    remaining_spots = count;
    Link_ReportRemainingSpots(remaining_spots);
}

void StateMachine_RemoteOpen(LinkReason reason)
{
    start_open(reason);
}

void StateMachine_RemoteClose(void)
{
    if (Barrier_HasFault() || !Barrier_CanClose())
    {
        /* 防砸优先于远程指令：闸下还有车就不落杆 */
        show_error("Vehicle Below!  ", ERROR_DISPLAY_DELAY_MS);
        USART_Printf("Remote close refused: path not clear\r\n");
        goto_state(STATE_FAULT, BARRIER_ANTICRUSH_HOLD_MS);
        return;
    }
    Barrier_ClearFault();
    start_close();
}

/* --- 主循环 ------------------------------------------------------ */

void StateMachine_Process(void)
{
    uint8_t card_id[MAX_CARD_ID_LENGTH];
    uint8_t status;

    switch (current_state)
    {
        case STATE_IDLE:
            if (vehicle_arrive_flag)
            {
                vehicle_arrive_flag = 0;
                OLED_ShowString(3, 1, "Please Swipe Card");
                USART_Printf("Vehicle arrived, waiting for RFID...\r\n");
                Link_RequestCapture();
                goto_state(STATE_WAITING_RFID, WAIT_CARD_TIMEOUT_MS);
            }
            break;

        case STATE_WAITING_RFID:
            /* 车走了：立即回空闲，不要继续等一张不会来的卡 */
            if (!Infrared_Is_Blocked())
            {
                USART_Printf("Vehicle left, returning to idle\r\n");
                OLED_ShowString(3, 1, "Idle            ");
                goto_state(STATE_IDLE, 0);
                break;
            }

            if (deadline_reached())
            {
                USART_Printf("Wait for card timed out\r\n");
                show_error("Timeout         ", ERROR_DISPLAY_DELAY_MS);
                break;
            }

            status = MFRC522_Request(PICC_REQIDL, card_id);
            if (status != MI_OK)
            {
                MFRC522_Reset();
                MFRC522_AntennaOff();
                MFRC522_AntennaOn();
                break;
            }

            status = MFRC522_Anticoll(card_id);
            if (status != MI_OK)
            {
                break;
            }

            {
                char card_str[20];
                USART_Printf("Card detected: %02X%02X%02X%02X\r\n",
                             card_id[0], card_id[1], card_id[2], card_id[3]);
                snprintf(card_str, sizeof(card_str), "Card: %02X%02X%02X%02X",
                         card_id[0], card_id[1], card_id[2], card_id[3]);
                OLED_ShowString(3, 1, card_str);
            }

            Link_ReportCard(card_id);

            if (FlashStorage_IsCardValid(card_id))
            {
                if (remaining_spots > 0)
                {
                    start_open(LINK_REASON_CARD);
                }
                else
                {
                    USART_Printf("Parking lot full!\r\n");
                    show_error("Parking Full!   ", ERROR_DISPLAY_DELAY_MS);
                }
            }
            else
            {
                USART_Printf("Invalid card!\r\n");
                show_error("Invalid Card!   ", ERROR_DISPLAY_DELAY_MS);
            }
            MFRC522_Halt();
            break;

        case STATE_OPENING:
            if (deadline_reached())
            {
                goto_state(STATE_OPEN, BARRIER_UP_DELAY_MS);
            }
            break;

        case STATE_OPEN:
            if (!deadline_reached()) break;

            /* 落杆前必须先确认闸下没车 —— 这是整个项目唯一的防砸手段 */
            if (Barrier_CanClose())
            {
                anticrush_retry = 0;
                start_close();
            }
            else
            {
                anticrush_retry++;
                if (anticrush_retry > BARRIER_ANTICRUSH_MAX_RETRY)
                {
                    /* 一直有车挡着：保持开闸等人来处理，不要硬落 */
                    USART_Printf("Barrier: anti-crush timeout, hold open\r\n");
                    OLED_ShowString(3, 1, "SAFETY FAULT    ");
                    goto_state(STATE_FAULT, 0);
                }
                else
                {
                    USART_Printf("Barrier: vehicle below, hold open\r\n");
                    OLED_ShowString(3, 1, "Vehicle Below!  ");
                    goto_state(STATE_OPEN, BARRIER_ANTICRUSH_HOLD_MS);
                }
            }
            break;

        case STATE_CLOSING:
            if (deadline_reached())
            {
                OLED_ShowString(3, 1, "Idle            ");
                goto_state(STATE_IDLE, 0);
            }
            break;

        case STATE_ERROR:
            if (deadline_reached())
            {
                OLED_ShowString(3, 1, "Idle            ");
                goto_state(STATE_IDLE, 0);
            }
            break;

        case STATE_FAULT:
            /* 保持开闸。障碍物消失后自动恢复，也可以由远程 CLOSE 清故障。 */
            if (deadline_reached() && !Infrared_Is_Blocked())
            {
                Barrier_ClearFault();
                anticrush_retry = 0;
                USART_Printf("Barrier: fault cleared, back to idle\r\n");
                OLED_ShowString(3, 1, "Idle            ");
                goto_state(STATE_IDLE, 0);
            }
            break;
    }
}
