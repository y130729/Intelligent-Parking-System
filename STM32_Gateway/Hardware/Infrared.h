#ifndef __INFRARED_H
#define __INFRARED_H

#include "stm32f10x.h"

#define INFRARED_GPIO_PORT      GPIOA
#define INFRARED_GPIO_PIN       GPIO_Pin_11
#define INFRARED_GPIO_CLK       RCC_APB2Periph_GPIOA
#define INFRARED_EXTI_PORT      GPIO_PortSourceGPIOA
#define INFRARED_EXTI_PIN       GPIO_PinSource11
#define INFRARED_EXTI_LINE      EXTI_Line11
#define INFRARED_EXTI_IRQ       EXTI15_10_IRQn

typedef enum {
    INFRARED_STATUS_CLEAR = 0,
    INFRARED_STATUS_BLOCKED = 1
} Infrared_StatusTypeDef;

// 红外传感器计数模式相关函数
void Infrared_Reset_Counter(void);
uint16_t Infrared_Get_Counter(void);
uint8_t Infrared_Has_New_Obstruction(void);
void Infrared_Clear_New_Obstruction_Flag(void);

void Infrared_Init(void);
Infrared_StatusTypeDef Infrared_Get_Status(void);
void Infrared_EXTI_Config(void);
uint8_t Infrared_Is_Blocked(void);

#endif
