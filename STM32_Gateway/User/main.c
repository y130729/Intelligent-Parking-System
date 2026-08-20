#include "stm32f10x.h"
#include "misc.h"
#include "system_tick.h"
#include "USART.h"
#include "Serial.h"
#include "OLED.h"
#include "MFRC522.h"
#include "Infrared.h"
#include "Servo.h"
#include "esp8266.h"
#include "project_config.h"
#include "state_machine.h"
#include "mqtt_report.h"
#include "pi_link.h"
#include <stdio.h>
#include <string.h>

void System_Init(void);
void OLED_DisplayRemainingSpots(void);
void ProcessUltrasonicData(void);
void ProcessPendingSpotUpdate(void);
void OLED_DisplayPlateNumber(const char *plate);

uint8_t spot_occupied[MAX_SPOT_ID] = {0};
uint8_t ultrasonic_new_data_flag = 0;

int main(void)
{
    System_Init();
    
    while (1)
    {
        StateMachine_Process();
        OLED_DisplayRemainingSpots();
#if WIFI_ENABLE
        ESP8266_ProcessWiFiConnection();
        ESP8266_ProcessMQTTConnection();
        ESP8266_ProcessData();
#else
        PI_UART_Process();
#endif
        ProcessUltrasonicData();
#if WIFI_ENABLE
        MQTT_ProcessHeartbeat();
#endif
        
        // 处理延迟的车位状态MQTT上报
        ProcessPendingSpotUpdate();
        
        // 处理ESP8266命令响应
#if WIFI_ENABLE
        ESP8266_ProcessPendingTasks();
#endif
        
        delay_ms(10);
    }
}

void System_Init(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SysTick_Init();
    USART_Init_Config();
    Serial_Init();
    OLED_Init();
    OLED_Clear();
    MFRC522_Init();
    Infrared_Init();
    Servo_Init();
#if WIFI_ENABLE
    ESP8266_Init();
#endif
    
    OLED_ShowString(1, 1, "Smart Parking");
    OLED_ShowString(2, 1, "System v1.0");
    OLED_ShowString(3, 1, "Initializing...");
    delay_ms(2000);
    
    StateMachine_Init();
#if WIFI_ENABLE
    MQTT_Init();
#else
    PI_UART_Init();
#endif
    
    OLED_ShowString(3, 1, "System Ready   ");
#if WIFI_ENABLE
    /* cloud via ESP8266 */
#else
    PI_UART_SendString("LINK:OK\r\n");
    StateMachine_UpdateRemainingSpots(TOTAL_SPOTS);
#endif
    OLED_DisplayPlateNumber(NULL);  // 预留第4行显示车牌号（占位，收到树莓派车牌后刷新）
    USART_Printf("System Init OK\r\n");
    USART_Printf("Total spots: %d\r\n", TOTAL_SPOTS);
    USART_Printf("CC2530 Coordinator connected via USART3\r\n");
}

void OLED_DisplayRemainingSpots(void)
{
    static char spot_str[20];
    static uint8_t last_available = 0xFF;
    
    uint8_t occupied_count = 0;
    for (uint8_t i = 0; i < MAX_SPOT_ID; i++)
    {
        if (spot_occupied[i])
        {
            occupied_count++;
        }
    }
    
    uint8_t available_spots = MAX_SPOT_ID - occupied_count;
    
    if (last_available != available_spots)
    {
        sprintf(spot_str, "Spots: %d/20      ", available_spots);
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
    sprintf(plate_line, "Plate:%s", current_plate);
    OLED_ShowCNString(4, 1, plate_line);
}

void ProcessUltrasonicData(void)
{
    if (ultrasonic_new_data_flag)
    {
        ultrasonic_new_data_flag = 0;
        
        char message[100];
        uint8_t occupied_count = 0;
        char occupied_spots[50] = "";
        char free_spots[50] = "";
        
        for (uint8_t i = 0; i < MAX_SPOT_ID; i++)
        {
            if (spot_occupied[i])
            {
                occupied_count++;
                if (strlen(occupied_spots) > 0) strcat(occupied_spots, ",");
                char spot_str[4];
                sprintf(spot_str, "%d", i + 1);
                strcat(occupied_spots, spot_str);
            }
            else
            {
                if (strlen(free_spots) > 0) strcat(free_spots, ",");
                char spot_str[4];
                sprintf(spot_str, "%d", i + 1);
                strcat(free_spots, spot_str);
            }
        }
        
        sprintf(message, "OCCUPANCY:%d/%d|OCCUPIED:%s|FREE:%s", occupied_count, MAX_SPOT_ID, occupied_spots, free_spots);
        USART_Printf("WiFi: Sending occupancy info: %s\r\n", message);

    }
}

// 处理延迟的车位状态MQTT上报
extern uint8_t pending_spot_update;
extern uint8_t pending_spot_id;
extern uint8_t pending_spot_status;

void ProcessPendingSpotUpdate(void)
{
    if (pending_spot_update)
    {
        pending_spot_update = 0;
#if WIFI_ENABLE
        MQTT_ReportSpotStatus(pending_spot_id, pending_spot_status);
#else
        {
            char pi_buf[32];
            sprintf(pi_buf, "SPOT:%d:%d\r\n", pending_spot_id, pending_spot_status);
            PI_UART_SendString(pi_buf);
        }
#endif
        /* sync remaining spot count to Pi after each change */
        {
            uint8_t occupied_count = 0;
            for (uint8_t i = 0; i < MAX_SPOT_ID; i++)
            {
                if (spot_occupied[i])
                {
                    occupied_count++;
                }
            }
            StateMachine_UpdateRemainingSpots((uint8_t)(MAX_SPOT_ID - occupied_count));
        }
    }
}

void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line11) != RESET)
    {
        if (Infrared_Is_Blocked())
        {
            StateMachine_SetVehicleArrive();
            USART_Printf("Infrared triggered, vehicle arrived\r\n");
        }
        EXTI_ClearITPendingBit(EXTI_Line11);
    }
}
