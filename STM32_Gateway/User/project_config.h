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

/* Timeout Configuration */
#define BARRIER_UP_DELAY_MS   3000
#define BARRIER_DOWN_DELAY_MS 1000
#define ERROR_DISPLAY_DELAY_MS 3000

/* 华为云设备凭据已从固件中移除：云端连接由树莓派负责，
   凭据只保留在树莓派侧的本地配置文件里（不进版本库）。 */

/* Flash 分区（64KB，1KB/页）—— 改分区表请同步 Readme 与 fault_log.h
   0x08000000 - 0x08006FFF  应用程序（代码不得越过 0x08007000）
   0x08007000 - 0x080073FF  刷卡白名单（Hardware/flash_storage.c）
   0x0800FC00 - 0x0800FFFF  故障记录（System/fault_log.c，最后一页） */
#define FLASH_APP_LIMIT_ADDR  0x08007000u

/* Ultrasonic Configuration */
#define PARKING_THRESHOLD_CM 20

/* Global Variables Declaration */
extern uint8_t spot_occupied[MAX_SPOT_ID];
extern uint8_t ultrasonic_new_data_flag;

#endif /* __PROJECT_CONFIG_H */
