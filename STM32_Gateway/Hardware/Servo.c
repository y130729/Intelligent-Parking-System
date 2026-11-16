#include "Servo.h"
#include "system_tick.h"
#include "stm32f10x.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_tim.h"
#include "stm32f10x_rcc.h"

void Servo_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_OCInitTypeDef TIM_OCInitStructure;
    
    RCC_APB2PeriphClockCmd(SERVO_GPIO_CLK, ENABLE);
    RCC_APB1PeriphClockCmd(SERVO_TIM_CLK, ENABLE);
    
    GPIO_InitStructure.GPIO_Pin = SERVO_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(SERVO_GPIO_PORT, &GPIO_InitStructure);
    
    TIM_TimeBaseStructure.TIM_Period = 19999;
    TIM_TimeBaseStructure.TIM_Prescaler = 71;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(SERVO_TIM, &TIM_TimeBaseStructure);
    
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse = 500;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC2Init(SERVO_TIM, &TIM_OCInitStructure);
    
    TIM_OC2PreloadConfig(SERVO_TIM, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(SERVO_TIM, ENABLE);
    TIM_Cmd(SERVO_TIM, ENABLE);
    
    Servo_Down();
}

void Servo_SetAngle(uint16_t angle)
{
    uint16_t pulse;
    
    if(angle > SERVO_ANGLE_MAX)
    {
        angle = SERVO_ANGLE_MAX;
    }
    
    pulse = SERVO_PULSE_MIN + (angle * (SERVO_PULSE_MAX - SERVO_PULSE_MIN) / SERVO_ANGLE_MAX);
    
    TIM_SetCompare2(SERVO_TIM, pulse);
}

/* 只设角度、不等待。
   原来这里 delay_ms(1500)，等于每次抬/落杆把整个主循环停 1.5 秒：
   串口不读、红外不计、看门狗要靠 delay_ms 内部特殊处理才不误复位。
   行程时间改由状态机的 STATE_OPENING / STATE_CLOSING 截止时间表达。 */
void Servo_Up(void)
{
    Servo_SetAngle(SERVO_BARRIER_UP_ANGLE);
}

void Servo_Down(void)
{
    Servo_SetAngle(SERVO_BARRIER_DOWN_ANGLE);
}

// 非阻塞版本：只设置角度，不等待
void Servo_SetUp(void)
{
    Servo_SetAngle(SERVO_BARRIER_UP_ANGLE);
}

// 非阻塞版本：只设置角度，不等待
void Servo_SetDown(void)
{
    Servo_SetAngle(SERVO_BARRIER_DOWN_ANGLE);
}
