#ifndef __PROJECT_CONFIG_H
#define __PROJECT_CONFIG_H

/* System Configuration */
#define SYS_CLOCK_FREQ        72000000UL

/* 云端连接方式：统一由树莓派（MQTTS + TLS）承担。
   STM32 侧的 ESP8266/MQTT 实现已删除 —— 保留两套传输会让业务代码里到处
   出现 #if WIFI_ENABLE 分支，而且明文 MQTT 也不适合现场使用。
   STM32 与树莓派之间只走 USART2 上的文本行协议（见 User/link.c）。 */

/* Parking Lot Configuration */
#define TOTAL_SPOTS           20
#define MAX_SPOT_ID           20

/* RFID Configuration */
#define MAX_CARD_ID_LENGTH    4
/* 白名单容量/存储地址在 Hardware/flash_storage.h：
   MAX_WHITELIST_CARDS / FLASH_SECTOR_ADDR / CARD_ID_LENGTH */

/* Timeout Configuration
   全部是"截止时间"，由状态机看 SysTick 判断；状态机里不再有
   delay_ms(1500) 这种把主循环（连同串口接收）一起停住的写法。 */
#define BARRIER_UP_DELAY_MS    3000   /* 开闸保持时间 */
#define BARRIER_MOVE_MS        1500   /* 舵机从一端走到另一端的时间 */
#define ERROR_DISPLAY_DELAY_MS 3000   /* 错误提示显示时长 */
#define WAIT_CARD_TIMEOUT_MS   30000  /* 车辆到位后等待刷卡的最长时间 */

/* 华为云设备凭据已从固件中移除：云端连接由树莓派负责，
   凭据只保留在树莓派侧的本地配置文件里（不进版本库）。 */

/* Flash 分区（64KB，1KB/页）—— 改分区表请同步 Readme 与 fault_log.h
   0x08000000 - 0x0800EFFF  应用程序（IROM1 = 0xF000，60KB）
   0x0800F000 - 0x0800F7FF  刷卡白名单 A/B 双页（Hardware/flash_storage.c）
   0x0800FC00 - 0x0800FFFF  故障记录（System/fault_log.c，最后一页） */
#define FLASH_APP_LIMIT_ADDR  0x0800F000u

/* Ultrasonic Configuration */
#define PARKING_THRESHOLD_CM 20

/* Global Variables Declaration */
extern uint8_t spot_occupied[MAX_SPOT_ID];
extern uint8_t ultrasonic_new_data_flag;

#endif /* __PROJECT_CONFIG_H */
