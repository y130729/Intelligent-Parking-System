#include "stm32f10x.h"                  // Device header
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "USART.h"

#define PARKING_THRESHOLD_CM 20
#define MAX_SPOTS 20

/* USART3 接收走 DMA 环形缓冲，不再用 RXNE 中断。
 *
 * 为什么必须换：加卡时要擦 1KB Flash 页。F103 在擦除期间**不能从 Flash 取指**，
 * CPU 整个停住 20~40ms（115200 下等于 230~460 字节）。原来 64 字节的环形
 * 缓冲 5.5ms 就满，于是一次"加卡"必然丢一帧车位上报——而且丢的是哪一帧
 * 取决于谁在那一刻上报，排查时极难复现。
 * DMA 是独立总线主设备，不取指、不依赖 CPU，擦除期间照样把字节写进 SRAM。
 *
 * 容量换算（115200-N-8-1 ≈ 11.5KB/s，即 1 字节 / 87us）：
 *   - 单槽白名单提交：擦 1 页 20~40ms + 写 27 个字 ≈ 45ms 最坏 -> 约 520 字节；
 *   - FlashStorage_EraseAll()：连擦 A/B 两页 40~80ms -> 约 920 字节；
 *   再叠加"多节点入网/重连后集中上报"的突发，1KB 只剩 1.1 倍余量，太紧。
 *   取 2KB = 满速 178ms，覆盖两页擦除最坏值仍有 2 倍余量。
 *   代价是 2KB SRAM（F103C8 共 20KB，本工程 RAM 占用约 7KB，可接受）。 */
#define SERIAL_RX_DMA_SIZE    2048
#define SERIAL_RX_DMA_CHANNEL DMA1_Channel3      /* F103: USART3_RX = DMA1_CH3 */
#define SERIAL_RX_LINE_SIZE   32

static uint8_t  serial_rx_buf[SERIAL_RX_DMA_SIZE];
static uint16_t serial_rx_tail = 0;

/* DMA 写指针（0..SIZE-1）：CNDTR 在循环模式下从 SIZE 递减到 0 再重装 */
static uint16_t serial_rx_write_pos(void)
{
	uint16_t remain = DMA_GetCurrDataCounter(SERIAL_RX_DMA_CHANNEL);
	return (uint16_t)((SERIAL_RX_DMA_SIZE - remain) % SERIAL_RX_DMA_SIZE);
}

/* 取一个已收到的字节；没有新数据返回 0。
 * 注意：环形 DMA 无法精确统计"被覆盖"的字节数（没有回绕中断可依赖），
 * 所以这里只保证"不读到半截数据"，容量靠上面的换算留足。 */
static uint8_t serial_rx_read(uint8_t *ch)
{
	uint16_t head = serial_rx_write_pos();

	if (head == serial_rx_tail) return 0;
	*ch = serial_rx_buf[serial_rx_tail];
	serial_rx_tail = (uint16_t)((serial_rx_tail + 1u) % SERIAL_RX_DMA_SIZE);
	return 1;
}

static void serial_rx_dma_init(void)
{
	DMA_InitTypeDef dma;

	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

	DMA_DeInit(SERIAL_RX_DMA_CHANNEL);
	dma.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DR;
	dma.DMA_MemoryBaseAddr     = (uint32_t)serial_rx_buf;
	dma.DMA_DIR                = DMA_DIR_PeripheralSRC;
	dma.DMA_BufferSize         = SERIAL_RX_DMA_SIZE;
	dma.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
	dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
	dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
	dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
	dma.DMA_Mode               = DMA_Mode_Circular;
	dma.DMA_Priority           = DMA_Priority_High;
	dma.DMA_M2M                = DMA_M2M_Disable;
	DMA_Init(SERIAL_RX_DMA_CHANNEL, &dma);
	DMA_Cmd(SERIAL_RX_DMA_CHANNEL, ENABLE);
}

extern uint8_t spot_occupied[MAX_SPOTS];
extern uint8_t ultrasonic_new_data_flag;

/* 车位变化"脏位图"。
 *
 * 原来是 pending_spot_id/status/update 三个变量凑成的"单槽"：
 * 同一轮里多个节点都变化时，后写的会把前面的覆盖掉，只剩一条上报。
 * 改成位图后，每个车位一位，天然按车位合并（同一车位多次变化只报最终态），
 * 又不会丢别的车位。20 个车位用一个 uint32_t 就够。
 *
 * 这些变量只在主循环上下文读写（ISR 只往环形缓冲写字节），
 * 因此不需要关中断保护。 */
volatile uint32_t spot_dirty_mask = 0;
uint8_t spot_fault[MAX_SPOTS] = {0};

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
	
	/*接收配置：DMA 循环搬进 SRAM，CPU 忙（例如擦 Flash）时也不丢字节*/
	serial_rx_dma_init();
	USART_DMACmd(USART3, USART_DMAReq_Rx, ENABLE);
	
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
	vsnprintf(String, sizeof(String), format, arg);	//有界版本：格式串再长也不会写出 String[100]
	va_end(arg);					//结束变量arg
	Serial_SendString(String);		//串口发送字符数组（字符串）
}

/* 解析无符号十进制数，返回解析到的字符指针；失败返回 NULL。 */
static const char *parse_u16(const char *s, uint16_t *out)
{
	uint32_t v = 0;
	uint8_t digits = 0;

	while (*s >= '0' && *s <= '9')
	{
		v = v * 10 + (uint32_t)(*s - '0');
		if (v > 65535) return NULL;
		s++;
		digits++;
	}
	if (digits == 0) return NULL;
	*out = (uint16_t)v;
	return s;
}

/**
  * 函    数：解析一行 CC2530 数据
  * 参    数：line 以 '\0' 结尾的一行，格式 "PXX:XXXcm" / "PXX:FAULT"
  * 返 回 值：无
  *
  * 注意：所有下标访问前都先确认长度。原来直接访问 line[3]，
  * 短行（例如只收到 "P0"）会读到缓冲区里的残留数据。
  */
static void Serial_HandleLine(uint8_t *line)
{
	const char *p = (const char *)line;
	uint8_t  spot_id = 0;
	uint16_t spot_value = 0;
	const char *rest;

	if (p == NULL) return;
	if (strlen(p) < 5) return;     /* 最短合法的是 "P1:23cm" */
	if (p[0] != 'P') return;

	rest = parse_u16(&p[1], &spot_value);
	if (rest == NULL) return;
	if (spot_value == 0 || spot_value > MAX_SPOTS) return;
	spot_id = (uint8_t)spot_value;
	if (*rest != ':') return;
	if (spot_id == 0 || spot_id > MAX_SPOTS) return;
	rest++;

	{
		uint8_t  index = (uint8_t)(spot_id - 1);
		uint16_t value;

		/* 传感器故障：单独一路，绝不能当成"距离=0 -> 空位" */
		if (strcmp(rest, "FAULT") == 0)
		{
			if (!spot_fault[index])
			{
				spot_fault[index] = 1;
				spot_dirty_mask |= (1UL << index);
				ultrasonic_new_data_flag = 1;
				USART_Printf("Spot %d sensor FAULT\r\n", spot_id);
			}
			return;
		}

		rest = parse_u16(rest, &value);
		if (rest == NULL) return;
		if (strcmp(rest, "cm") != 0) return;

		if (spot_fault[index])
		{
			spot_fault[index] = 0;
			spot_dirty_mask |= (1UL << index);
		}

		{
			uint8_t prev_state = spot_occupied[index];
			spot_occupied[index] = (value < PARKING_THRESHOLD_CM) ? 1 : 0;

			if (spot_occupied[index] != prev_state)
			{
				ultrasonic_new_data_flag = 1;
				spot_dirty_mask |= (1UL << index);
				USART_Printf("Spot %d %s (distance: %dcm)\r\n",
				             spot_id,
				             spot_occupied[index] ? "occupied" : "free",
				             value);
			}
		}
	}
}

/**
  * 函    数：主循环调用，从环形缓冲取字节并按行处理 CC2530 数据
  * 参    数：无
  * 返 回 值：无
  */
void Serial_Process(void)
{
	static uint8_t line[SERIAL_RX_LINE_SIZE];
	static uint8_t line_len = 0;
	static uint8_t discarding = 0;
	uint8_t ch;

	while (serial_rx_read(&ch))
	{
		if (ch == '\n' || ch == '\r')
		{
			if (line_len > 0 && !discarding)
			{
				line[line_len] = '\0';
				Serial_HandleLine(line);
			}
			line_len = 0;
			discarding = 0;
		}
		else if (discarding)
		{
			/* 超长行：丢到行尾再重新同步 */
		}
		else if (line_len < SERIAL_RX_LINE_SIZE - 1)
		{
			line[line_len++] = ch;
		}
		else
		{
			discarding = 1;
			line_len = 0;
		}
	}
}
