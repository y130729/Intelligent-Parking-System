#include "stm32f10x.h"                  // Device header
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "USART.h"
#include "mqtt_report.h"

#define PARKING_THRESHOLD_CM 20
#define MAX_SPOTS 20

extern uint8_t spot_occupied[MAX_SPOTS];
extern uint8_t ultrasonic_new_data_flag;

// 用于延迟MQTT上报的变量
uint8_t pending_spot_id = 0;
uint8_t pending_spot_status = 0;
uint8_t pending_spot_update = 0;

/**
  * 函    数：串口初始化 - USART3连接CC2530协调器
  * 参    数：无
  * 返 回 值：无
  */
void Serial_Init(void)
{
	/*开启时钟*/
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);	//开启USART3的时钟
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);	//开启GPIOB的时钟
	
	/*GPIO初始化*/
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);					//将PB10引脚初始化为复用推挽输出(TX)
	
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOB, &GPIO_InitStructure);					//将PB11引脚初始化为上拉输入(RX)
	
	/*USART初始化*/
	USART_InitTypeDef USART_InitStructure;					//定义结构体变量
	USART_InitStructure.USART_BaudRate = 115200;			//波特率，与CC2530协调器匹配
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;	//硬件流控制，不需要
	USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;	//模式，发送模式和接收模式均选择
	USART_InitStructure.USART_Parity = USART_Parity_No;		//奇偶校验，不需要
	USART_InitStructure.USART_StopBits = USART_StopBits_1;	//停止位，选择1位
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;		//字长，选择8位
	USART_Init(USART3, &USART_InitStructure);				//将结构体变量交给USART_Init，配置USART3
	
	/*中断输出配置*/
	USART_ITConfig(USART3, USART_IT_RXNE, ENABLE);
	
	/*NVIC配置*/
	NVIC_InitTypeDef NVIC_InitStructure;					//定义结构体变量
	NVIC_InitStructure.NVIC_IRQChannel = USART3_IRQn;		//选择配置NVIC的USART3线
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;			//指定NVIC线路使能
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;		//指定NVIC线路的抢占优先级为1
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;		//指定NVIC线路的响应优先级为1
	NVIC_Init(&NVIC_InitStructure);							//将结构体变量交给NVIC_Init，配置NVIC外设
	
	/*USART使能*/
	USART_Cmd(USART3, ENABLE);								//使能USART3，串口开始运行
	
	USART_Printf("CC2530 UART initialized (USART3)\r\n");
}

/**
  * 函    数：串口发送一个字节
  * 参    数：Byte 要发送的一个字节
  * 返 回 值：无
  */
void Serial_SendByte(uint8_t Byte)
{
	USART_SendData(USART3, Byte);		//将字节数据写入数据寄存器，写入后USART自动生成时序波形
	while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);	//等待发送完成
	/*下次写入数据寄存器会自动清除发送完成标志位，故此循环后，无需清除标志位*/
}

/**
  * 函    数：串口发送一个数组
  * 参    数：Array 要发送数组的首地址
  * 参    数：Length 要发送数组的长度
  * 返 回 值：无
  */
void Serial_SendArray(uint8_t *Array, uint16_t Length)
{
	uint16_t i;
	for (i = 0; i < Length; i ++)		//遍历数组
	{
		Serial_SendByte(Array[i]);		//依次调用Serial_SendByte发送每个字节数据
	}
}

/**
  * 函    数：串口发送一个字符串
  * 参    数：String 要发送字符串的首地址
  * 返 回 值：无
  */
void Serial_SendString(char *String)
{
	uint8_t i;
	for (i = 0; String[i] != '\0'; i ++)//遍历字符数组（字符串），遇到字符串结束标志位后停止
	{
		Serial_SendByte(String[i]);		//依次调用Serial_SendByte发送每个字节数据
	}
}

/**
  * 函    数：次方函数（内部使用）
  * 返 回 值：返回值等于X的Y次方
  */
uint32_t Serial_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;	//设置结果初值为1
	while (Y --)			//执行Y次
	{
		Result *= X;		//将X累乘到结果
	}
	return Result;
}

/**
  * 函    数：串口发送数字
  * 参    数：Number 要发送的数字，范围：0~4294967295
  * 参    数：Length 要发送数字的长度，范围：0~10
  * 返 回 值：无
  */
void Serial_SendNumber(uint32_t Number, uint8_t Length)
{
	uint8_t i;
	for (i = 0; i < Length; i ++)		//根据数字长度遍历数字的每一位
	{
		Serial_SendByte(Number / Serial_Pow(10, Length - i - 1) % 10 + '0');	//依次调用Serial_SendByte发送每位数字
	}
}

/**
  * 函    数：自己封装的prinf函数
  * 参    数：format 格式化字符串
  * 参    数：... 可变的参数列表
  * 返 回 值：无
  */
void Serial_Printf(char *format, ...)
{
	char String[100];				//定义字符数组
	va_list arg;					//定义可变参数列表数据类型的变量arg
	va_start(arg, format);			//从format开始，接收参数列表到arg变量
	vsprintf(String, format, arg);	//使用vsprintf打印格式化字符串和参数列表到字符数组中
	va_end(arg);					//结束变量arg
	Serial_SendString(String);		//串口发送字符数组（字符串）
}

static uint8_t rx_buffer[32];
static uint8_t rx_index = 0;

/**
  * 函    数：USART3中断函数 - 处理CC2530发送的超声波数据
  * 参    数：无
  * 返 回 值：无
  * 注意事项：此函数为中断函数，无需调用，中断触发后自动执行
  *           解析格式："PX:XXXcm" 其中P为标识位，X为车位号(1-20)，XXX为距离数值(0-999cm)
  *           例如 "P1:15cm" 表示车位1距离15cm，距离小于20cm判定为有车
  */
void USART3_IRQHandler(void)
{
	if (USART_GetITStatus(USART3, USART_IT_RXNE) == SET)	//判断是否是USART3的接收事件触发的中断
	{
		uint8_t RxData = USART_ReceiveData(USART3);			//读取CC2530发送的数据
		
		if (RxData == '\n' || RxData == '\r')
		{
			if (rx_index > 0)
			{
				rx_buffer[rx_index] = '\0';
				
				uint8_t spot_id = 0;
				uint16_t distance = 0;
				
				// 解析带车位号的格式: "PX:XXXcm"
				if (rx_buffer[0] == 'P' && rx_buffer[3] == ':')
				{
					// 解析车位号 (1-2位)
					if (rx_buffer[1] >= '0' && rx_buffer[1] <= '9')
					{
						spot_id = (rx_buffer[1] - '0') * 10;
						if (rx_buffer[2] >= '0' && rx_buffer[2] <= '9')
						{
							spot_id += (rx_buffer[2] - '0');
						}
						else if (rx_buffer[2] == ':')
						{
							spot_id = (rx_buffer[1] - '0');
						}
					}
					
					if (spot_id > 0 && spot_id <= MAX_SPOTS)
					{
						// 解析距离 (格式: XXXcm)
						if (sscanf((char*)&rx_buffer[4], "%hucm", &distance) == 1)
						{
							uint8_t spot_index = spot_id - 1;
							uint8_t prev_state = spot_occupied[spot_index];
							spot_occupied[spot_index] = (distance < PARKING_THRESHOLD_CM) ? 1 : 0;
							
							if (spot_occupied[spot_index] != prev_state)
							{
								ultrasonic_new_data_flag = 1;
								if (spot_occupied[spot_index])
								{
									USART_Printf("Spot %d occupied (distance: %dcm)\r\n", spot_id, distance);
								}
								else
								{
									USART_Printf("Spot %d free (distance: %dcm)\r\n", spot_id, distance);
								}
								// 不要在中断中调用MQTT发布，设置标志位让主循环处理
								pending_spot_id = spot_id;
								pending_spot_status = spot_occupied[spot_index];
								pending_spot_update = 1;
							}
						}
					}
				}
				rx_index = 0;
			}
		}
		else if (rx_index < sizeof(rx_buffer) - 1)
		{
			rx_buffer[rx_index++] = RxData;
		}
		
		USART_ClearITPendingBit(USART3, USART_IT_RXNE);		//清除标志位
	}
}
