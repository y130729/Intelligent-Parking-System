#include "state_machine.h"
#include "OLED.h"
#include "MFRC522.h"
#include "Servo.h"
#include "mqtt_report.h"
#include "Infrared.h"
#include "project_config.h"
#include "system_tick.h"
#include "USART.h"
#include "esp8266.h"
#include "flash_storage.h"
#include <string.h>
#include <stdio.h>

SystemState current_state = STATE_IDLE;
uint8_t remaining_spots = TOTAL_SPOTS;
uint8_t vehicle_arrive_flag = 0;
uint32_t state_timer = 0;

static uint8_t RFID_VerifyCard(uint8_t *card_id)
{
    delay_ms(10);  // 添加短延时，确保RC522数据稳定
    
    return FlashStorage_IsCardValid(card_id);
}

static void RFID_ResetAndRetry(void)
{
    MFRC522_Reset();
    MFRC522_AntennaOff();
    MFRC522_AntennaOn();
}

void StateMachine_Init(void)
{
    remaining_spots = TOTAL_SPOTS;
    current_state = STATE_IDLE;
    vehicle_arrive_flag = 0;
    state_timer = 0;
    
    FlashStorage_Init();
}

void StateMachine_SetVehicleArrive(void)
{
    vehicle_arrive_flag = 1;
}

void StateMachine_UpdateRemainingSpots(uint8_t count)
{
    remaining_spots = count;
    MQTT_ReportProperty(remaining_spots);
}

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
                current_state = STATE_WAITING_RFID;
                state_timer = 0;
                OLED_ShowString(3, 1, "Please Swipe Card");
                USART_Printf("Vehicle arrived, waiting for RFID...\r\n");
            }
            break;
            
        case STATE_WAITING_RFID:
            state_timer += 10;
            
            if (!Infrared_Is_Blocked())
            {
                USART_Printf("Vehicle left, returning to idle\r\n");
                current_state = STATE_IDLE;
                OLED_ShowString(3, 1, "Idle            ");
                state_timer = 0;
                break;
            }
            
            status = MFRC522_Request(PICC_REQIDL, card_id);
            if (status != MI_OK)
            {
                RFID_ResetAndRetry();
                break;
            }
            
            status = MFRC522_Anticoll(card_id);
            if (status != MI_OK)
            {
                break;
            }
            
            USART_Printf("Card detected: %02X%02X%02X%02X\r\n", 
                        card_id[0], card_id[1], card_id[2], card_id[3]);
            
            char card_str[20];
            sprintf(card_str, "Card: %02X%02X%02X%02X", 
                    card_id[0], card_id[1], card_id[2], card_id[3]);
            OLED_ShowString(3, 1, card_str);
            
            MQTT_ReportCardID(card_id);  // 只上报卡号
            
            char wifi_msg[30];
            sprintf(wifi_msg, "CARD:%02X%02X%02X%02X", 
                    card_id[0], card_id[1], card_id[2], card_id[3]);
            USART_Printf("WiFi: Sending card ID: %s\r\n", wifi_msg);
            ESP8266_SendToClient(wifi_msg);
            
            if (RFID_VerifyCard(card_id))
            {
                if (remaining_spots > 0)
                {
                    current_state = STATE_BARRIER_UP;
                    OLED_ShowString(3, 1, "Entry Allowed   ");
                    USART_Printf("Card valid, opening barrier...\r\n");
                    Servo_Up();

                }
                else
                {
                    OLED_ShowString(3, 1, "Parking Full!   ");
                    USART_Printf("Parking lot full!\r\n");
                    delay_ms(ERROR_DISPLAY_DELAY_MS);
                    current_state = STATE_IDLE;
                    OLED_ShowString(3, 1, "Idle            ");
                }
            }
            else
            {
                OLED_ShowString(3, 1, "Invalid Card!   ");
                USART_Printf("Invalid card!\r\n");
                delay_ms(ERROR_DISPLAY_DELAY_MS);
                current_state = STATE_IDLE;
                OLED_ShowString(3, 1, "Idle            ");
            }
            MFRC522_Halt();
            break;
            
        case STATE_BARRIER_UP:
            state_timer += 10;
            if (state_timer >= BARRIER_UP_DELAY_MS)
            {
                current_state = STATE_BARRIER_DOWN;
                state_timer = 0;
                Servo_Down();
                OLED_ShowString(3, 1, "Closing Barrier ");
            }
            break;
            
        case STATE_BARRIER_DOWN:
            state_timer += 10;
            if (state_timer >= BARRIER_DOWN_DELAY_MS)
            {
                current_state = STATE_IDLE;
                OLED_ShowString(3, 1, "Idle            ");
                USART_Printf("Barrier closed, back to idle\r\n");
            }
            break;
    }
}