#ifndef __PROJECT_CONFIG_H
#define __PROJECT_CONFIG_H

/* System Configuration */
#define SYS_CLOCK_FREQ        72000000UL
/* Network switch: 0 = offline mode (cloud handled by Raspberry Pi), 1 = ESP8266 WiFi on STM32 */
#define WIFI_ENABLE           0

/* Parking Lot Configuration */
#define TOTAL_SPOTS           20
#define MAX_SPOT_ID           1

/* RFID Configuration */
#define MAX_CARD_ID_LENGTH    4
#define VALID_CARD_COUNT      10

/* Timeout Configuration */
#define RFID_TIMEOUT_MS       5000
#define BARRIER_UP_DELAY_MS   3000
#define BARRIER_DOWN_DELAY_MS 1000
#define HEARTBEAT_INTERVAL_S  30
#define ERROR_DISPLAY_DELAY_MS 3000

/* Huawei Cloud MQTT Configuration */
#define HUAWEI_MQTT_BROKER    "21210fb4d9.st1.iotda-device.cn-south-1.myhuaweicloud.com"
#define HUAWEI_MQTT_PORT      "1883"
#define HUAWEI_MQTT_DEVICE_ID "6a02cb70c00ccb6d4b5364bb_HW_MQTT"
#define HUAWEI_MQTT_CLIENT_ID "6a02cb70c00ccb6d4b5364bb_HW_MQTT_0_1_2026061608"
#define HUAWEI_MQTT_USERNAME  "6a02cb70c00ccb6d4b5364bb_HW_MQTT"
#define HUAWEI_MQTT_PASSWORD  "29eaba1a0fa4eaf3c1de8eb89c677febbc73a1081aab6dc83189b02d38a9025e"

/* Topic Definitions */
#define MQTT_TOPIC_EVENT      "$oc/devices/%s/sys/events/up"
#define MQTT_TOPIC_PROPERTY   "$oc/devices/%s/sys/properties/report"
#define MQTT_TOPIC_COMMAND    "$oc/devices/%s/sys/commands/down"

/* CC2530 Communication Protocol */
#define CC2530_FRAME_HEADER   0xAA
#define CC2530_FRAME_TAIL     0xBB
#define CC2530_FRAME_MIN_LEN 5

/* Ultrasonic Configuration */
#define PARKING_THRESHOLD_CM 20

#endif /* __PROJECT_CONFIG_H */

/* Global Variables Declaration */
extern uint8_t spot_occupied[MAX_SPOT_ID];
extern uint8_t ultrasonic_new_data_flag;

