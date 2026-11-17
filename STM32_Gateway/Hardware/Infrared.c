#include "Infrared.h"
#include "system_tick.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_exti.h"
#include "misc.h"

static volatile Infrared_StatusTypeDef infrared_status = INFRARED_STATUS_CLEAR;
static volatile uint16_t obstruction_counter = 0;
static volatile uint8_t new_obstruction_flag = 0;
static volatile uint8_t last_blocked_state = 0;

void Infrared_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    
    RCC_APB2PeriphClockCmd(INFRARED_GPIO_CLK, ENABLE);
    
    GPIO_InitStructure.GPIO_Pin = INFRARED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(INFRARED_GPIO_PORT, &GPIO_InitStructure);
    
    GPIO_SetBits(INFRARED_GPIO_PORT, INFRARED_GPIO_PIN);
    
    Infrared_EXTI_Config();
}

void Infrared_EXTI_Config(void)
{
    EXTI_InitTypeDef EXTI_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;
    
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
    
    GPIO_EXTILineConfig(INFRARED_EXTI_PORT, INFRARED_EXTI_PIN);
    
    EXTI_InitStructure.EXTI_Line = INFRARED_EXTI_LINE;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);
    
    NVIC_InitStructure.NVIC_IRQChannel = INFRARED_EXTI_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

Infrared_StatusTypeDef Infrared_Get_Status(void)
{
    return infrared_status;
}

void Infrared_Reset_Counter(void)
{
    obstruction_counter = 0;
    new_obstruction_flag = 0;
}

uint16_t Infrared_Get_Counter(void)
{
    return obstruction_counter;
}

uint8_t Infrared_Has_New_Obstruction(void)
{
    return new_obstruction_flag;
}

void Infrared_Clear_New_Obstruction_Flag(void)
{
    new_obstruction_flag = 0;
}

uint8_t Infrared_Is_Blocked(void)
{
    uint8_t pin_state = GPIO_ReadInputDataBit(INFRARED_GPIO_PORT, INFRARED_GPIO_PIN);
    
    if(pin_state == Bit_RESET)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}
