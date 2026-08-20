#include "mqtt_report.h"
#include "esp8266.h"
#include "project_config.h"
#include "USART.h"
#include <string.h>
#include <stdio.h>

extern WIFI_StateTypeDef wifi_state;

static uint32_t heartbeat_timer = 0;

void MQTT_Init(void)
{
    heartbeat_timer = 0;
}

void MQTT_ReportProperty(uint8_t remaining_spots)
{
    if (wifi_state != WIFI_STATE_MQTT_CONNECTED)
    {
        USART_Printf("MQTT: Not connected to Huawei Cloud, skipping property report\r\n");
        return;
    }
    
    char topic[128];
    char payload[256];
    
    sprintf(topic, "$oc/devices/%s/sys/properties/report", HUAWEI_MQTT_DEVICE_ID);
    sprintf(payload, "{\"services\":[{\"service_id\":\"Spot_state\",\"properties\":{\"Parking_state\":\"%s\",\"Parking_id\":\"1\",\"remaining_spots\":%d}}]}", 
            remaining_spots > 0 ? "empty" : "occupied", remaining_spots);
    
    USART_Printf("MQTT Property: %s -> %s\r\n", topic, payload);
    
    if (ESP8266_MQTTPublish(topic, payload))
    {
        USART_Printf("MQTT: Property reported successfully\r\n");
    }
    else
    {
        USART_Printf("MQTT: Failed to report property\r\n");
    }
}

void MQTT_ReportSpotStatus(uint8_t spot_id, uint8_t status)
{
    if (wifi_state != WIFI_STATE_MQTT_CONNECTED)
    {
        USART_Printf("MQTT: Not connected to Huawei Cloud, skipping spot status report\r\n");
        return;
    }
    
    char topic[128];
    char payload[256];
    
    sprintf(topic, "$oc/devices/%s/sys/properties/report", HUAWEI_MQTT_DEVICE_ID);
    // 按照产品模型: Parking_id是车位号, Parking_state是状态
    sprintf(payload, "{\"services\":[{\"service_id\":\"Spot_state\",\"properties\":{\"Parking_id\":\"%d\",\"Parking_state\":\"%s\"}}]}", 
            spot_id, status == 0 ? "empty" : "occupied");
    
    USART_Printf("MQTT Spot %d Status: %s -> %s\r\n", spot_id, topic, payload);
    
    if (ESP8266_MQTTPublish(topic, payload))
    {
        USART_Printf("MQTT: Spot %d status reported successfully\r\n", spot_id);
    }
    else
    {
        USART_Printf("MQTT: Failed to report spot %d status\r\n", spot_id);
    }
}

void MQTT_ReportCardID(uint8_t *card_id)
{
    if (wifi_state != WIFI_STATE_MQTT_CONNECTED)
    {
        USART_Printf("MQTT: Not connected to Huawei Cloud, skipping card ID report\r\n");
        return;
    }
    
    char topic[128];
    char payload[256];
    char card_str[20];
    
    sprintf(card_str, "%02X%02X%02X%02X", card_id[0], card_id[1], card_id[2], card_id[3]);
    
    sprintf(topic, "$oc/devices/%s/sys/properties/report", HUAWEI_MQTT_DEVICE_ID);
    sprintf(payload, "{\"services\":[{\"service_id\":\"Spot_state\",\"properties\":{\"card_id\":\"%s\"}}]}", card_str);
    
    USART_Printf("MQTT Card ID: %s -> %s\r\n", topic, payload);
    
    // 使用较短超时，避免阻塞状态机
    if (ESP8266_MQTTPublish(topic, payload))
    {
        USART_Printf("MQTT: Card ID reported successfully\r\n");
    }
    else
    {
        USART_Printf("MQTT: Failed to report card ID\r\n");
    }
}

void MQTT_ReportBarrierState(char *state)
{
    if (wifi_state != WIFI_STATE_MQTT_CONNECTED)
    {
        USART_Printf("MQTT: Not connected to Huawei Cloud, skipping barrier state report\r\n");
        return;
    }
    
    char topic[128];
    char payload[256];
    
    sprintf(topic, "$oc/devices/%s/sys/properties/report", HUAWEI_MQTT_DEVICE_ID);
    sprintf(payload, "{\"services\":[{\"service_id\":\"ParkingBarrier\",\"properties\":{\"barrier_state\":\"%s\"}}]}", state);
    
    USART_Printf("MQTT Barrier: %s -> %s\r\n", topic, payload);
    
    if (ESP8266_MQTTPublish(topic, payload))
    {
        USART_Printf("MQTT: Barrier state reported successfully\r\n");
    }
    else
    {
        USART_Printf("MQTT: Failed to report barrier state\r\n");
    }
}

void MQTT_SendHeartbeat(void)
{
    // 心跳功能保留，但不再通过事件上报
    USART_Printf("Heartbeat sent\r\n");
    heartbeat_timer = 0;
}

void MQTT_ReportCommandResponse(char *request_id, int result_code, char *result_desc)
{
    if (wifi_state != WIFI_STATE_MQTT_CONNECTED)
    {
        USART_Printf("MQTT: Not connected to Huawei Cloud, skipping command response\r\n");
        return;
    }
    
    char topic[256];
    char payload[256];
    
    // 正确的Topic格式: $oc/devices/{device_id}/sys/commands/response/request_id={uuid}
    sprintf(topic, "$oc/devices/%s/sys/commands/response/request_id=%s", HUAWEI_MQTT_DEVICE_ID, request_id);
    // 按照产品模型 ParkingBarrier 服务的 cmdResponses 格式
    sprintf(payload, "{\"cmdResponses\":[{\"result\":\"%s\"}]}", result_desc);
    
    USART_Printf("MQTT Command Response: %s -> %s\r\n", topic, payload);
    
    if (ESP8266_MQTTPublish(topic, payload))
    {
        USART_Printf("MQTT: Command response sent successfully\r\n");
    }
    else
    {
        USART_Printf("MQTT: Failed to send command response\r\n");
    }
}

void MQTT_ProcessHeartbeat(void)
{
    heartbeat_timer++;
    if (heartbeat_timer >= HEARTBEAT_INTERVAL_S * 1000)
    {
        MQTT_SendHeartbeat();
    }
}