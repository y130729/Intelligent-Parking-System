#ifndef __MQTT_REPORT_H
#define __MQTT_REPORT_H

#include "stm32f10x.h"

void MQTT_Init(void);
void MQTT_ReportProperty(uint8_t remaining_spots);
void MQTT_ReportCardID(uint8_t *card_id);
void MQTT_ReportBarrierState(char *state);
void MQTT_ReportSpotStatus(uint8_t spot_id, uint8_t status);
void MQTT_SendHeartbeat(void);
void MQTT_ProcessHeartbeat(void);
void MQTT_ReportCommandResponse(char *request_id, int result_code, char *result_desc);

#endif /* __MQTT_REPORT_H */
