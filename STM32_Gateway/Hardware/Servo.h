#ifndef __SERVO_H
#define __SERVO_H

#include "stm32f10x.h"

#define SERVO_GPIO_PORT     GPIOA
#define SERVO_GPIO_PIN      GPIO_Pin_1
#define SERVO_GPIO_CLK      RCC_APB2Periph_GPIOA
#define SERVO_TIM           TIM2
#define SERVO_TIM_CLK       RCC_APB1Periph_TIM2
#define SERVO_TIM_CHANNEL   TIM_Channel_2
#define SERVO_PULSE_MIN     500
#define SERVO_PULSE_MAX     2500
#define SERVO_ANGLE_MIN     0
#define SERVO_ANGLE_MAX     180

#define SERVO_BARRIER_DOWN_ANGLE    0
#define SERVO_BARRIER_UP_ANGLE      90

void Servo_Init(void);
void Servo_SetAngle(uint16_t angle);
void Servo_Up(void);
void Servo_Down(void);
void Servo_SetUp(void);  // 非阻塞版本：只设置角度，不等待
void Servo_SetDown(void); // 非阻塞版本：只设置角度，不等待

#endif
