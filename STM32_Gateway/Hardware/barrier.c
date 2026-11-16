#include "barrier.h"
#include "Infrared.h"
#include "Servo.h"
#include "USART.h"

#if BARRIER_HAS_LIMIT_SWITCHES
/* 换成真实道闸时在这里填引脚定义 */
#define BARRIER_LIMIT_OPEN_PORT     GPIOA
#define BARRIER_LIMIT_OPEN_PIN      GPIO_Pin_12
#define BARRIER_LIMIT_CLOSE_PORT    GPIOA
#define BARRIER_LIMIT_CLOSE_PIN     GPIO_Pin_15
#endif

#if BARRIER_HAS_CURRENT_SENSE
/* 电机电流采样（ADC 通道），阈值按实际电机标定 */
#define BARRIER_CURRENT_FAULT_MA   1200
#endif

static uint8_t barrier_fault = 0;

void Barrier_Init(void)
{
    barrier_fault = 0;

#if BARRIER_HAS_LIMIT_SWITCHES
    {
        GPIO_InitTypeDef gpio;
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
        gpio.GPIO_Mode = GPIO_Mode_IPU;
        gpio.GPIO_Speed = GPIO_Speed_50MHz;
        gpio.GPIO_Pin = BARRIER_LIMIT_OPEN_PIN | BARRIER_LIMIT_CLOSE_PIN;
        GPIO_Init(GPIOA, &gpio);
    }
#endif
}

uint8_t Barrier_CanClose(void)
{
    /* 1) 唯一的真实防砸输入：红外对射被挡 = 车辆仍在闸下 */
    if (Infrared_Is_Blocked())
    {
        return 0;
    }

    /* 2) 显式故障（例如电流超限、连续防砸）一律禁止落杆 */
    if (barrier_fault)
    {
        return 0;
    }

    /* 3) 限位：如果装了"开到位"反馈但没到位，说明机构卡住，不能落杆 */
#if BARRIER_HAS_LIMIT_SWITCHES
    if (GPIO_ReadInputDataBit(BARRIER_LIMIT_OPEN_PORT, BARRIER_LIMIT_OPEN_PIN) == Bit_RESET &&
        GPIO_ReadInputDataBit(BARRIER_LIMIT_CLOSE_PORT, BARRIER_LIMIT_CLOSE_PIN) == Bit_RESET)
    {
        return 0;   /* 两个限位都没触发：机构在中间位置，状态不明 */
    }
#endif

    return 1;
}

uint8_t Barrier_HasFault(void)
{
    return barrier_fault;
}

void Barrier_ClearFault(void)
{
    barrier_fault = 0;
}

void Barrier_EmergencyRelease(void)
{
    /* 关掉 PWM 输出，舵机失去保持力矩，闸杆可手动抬起。
       真实道闸这里对应"松开抱闸/离合器"。 */
    TIM_Cmd(SERVO_TIM, DISABLE);
    USART_Printf("Barrier: emergency release (PWM off)\r\n");
}
