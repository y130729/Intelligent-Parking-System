#ifndef __STATE_MACHINE_H
#define __STATE_MACHINE_H

#include "stm32f10x.h"

typedef enum {
    STATE_IDLE = 0,
    STATE_WAITING_RFID,
    STATE_BARRIER_UP,
    STATE_BARRIER_DOWN
} SystemState;

extern SystemState current_state;
extern uint8_t remaining_spots;
extern uint8_t vehicle_arrive_flag;

void StateMachine_Init(void);
void StateMachine_Process(void);
void StateMachine_SetVehicleArrive(void);
void StateMachine_UpdateRemainingSpots(uint8_t count);

void StateMachine_RemoteOpen(void);
void StateMachine_RemoteClose(void);
#endif /* __STATE_MACHINE_H */
