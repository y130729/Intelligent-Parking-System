#include "esp8266.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include "stm32f10x_gpio.h"
#include "stm32f10x_rcc.h"
#include "stm32f10x_usart.h"
#include "misc.h"
#include "Servo.h"
#include "project_config.h"
#include "mqtt_report.h"

volatile uint8_t ucTcpClosedFlag = 0;
volatile uint8_t wifi_rx_flag = 0;
WIFI_StateTypeDef wifi_state = WIFI_STATE_IDLE;

// 用于延迟命令响应的变量
char pending_request_id[64] = "unknown";
int pending_result_code = 0;
char pending_result_desc[32] = "failed";
uint8_t pending_command_response = 0;
uint16_t servo_close_delay = 0; // 用于延迟关闭道闸
uint8_t command_processing = 0; // 防止命令重复处理

// 用于存储待处理的MQTT命令
char pending_mqtt_command[1024] = {0};
uint8_t pending_mqtt_command_flag = 0;

char cStr [ 1500 ] = { 0 };
static void                   ESP8266_GPIO_Config                 ( void );
static void                   ESP8266_USART_Config                ( void );
static void                   ESP8266_USART_NVIC_Configuration    ( void );
struct  STRUCT_USARTx_Fram strEsp8266_Fram_Record = { 0 };

static uint32_t wifi_retry_delay = 0;
static uint8_t wifi_retry_count = 0;


/**
  * @brief  ESP8266 作为STA TCP客户端模式配置
  * @param  无
  * @retval 无
  */
void ESP8266_StaTcpClient ( void )
{
//	uint8_t ucStatus;
//	char cStr [ 100 ] = { 0 };
//  USART_Printf ( "\r\n正在配置 ESP8266 ......\r\n" );

	macESP8266_CH_ENABLE();
	
	//ESP8266_AT_Test ();
	
	ESP8266_Net_Mode_Choose ( STA );

  while ( ! ESP8266_JoinAP ( macUser_ESP8266_ApSsid, macUser_ESP8266_ApPwd ) );	
	// USART_Printf("\r\n=== ESP8266 IP Address Query ===\r\n");
	// USART_Printf("Querying ESP8266 IP address...\r\n");
	
	// 发送CIFSR命令获取IP地址
	if(ESP8266_Cmd ( "AT+CIFSR", "OK", 0, 1000 )) {
		// 解析响应中的IP地址
		// CIFSR响应格式通常是: +CIFSR:STAIP,"192.168.x.x"
		char* response = strEsp8266_Fram_Record.Data_RX_BUF;
		char* ip_start = strstr(response, "\"");
		if(ip_start) {
			ip_start++; // 跳过第一个引号
			char* ip_end = strstr(ip_start, "\"");
			if(ip_end) {
				*ip_end = '\0'; // 临时结束字符串
				// USART_Printf("ESP8266 IP Address: %s\r\n", ip_start);
				// USART_Printf("ESP8266 is ready for TCP connections on port 8288\r\n");
			}
		} else {
			// USART_Printf("ESP8266 Response: %s\r\n", response);
		}
	} else {
		// USART_Printf("Failed to get IP address\r\n");
	}
	
	// USART_Printf("=================================\r\n\r\n");
	ESP8266_Cmd ( "AT+CIPMUX=1", "OK", 0, 1000 );
	
	
	ESP8266_Cmd ( "AT+CIPSERVER=1,8288", "OK", 0, 1000 );
	//ESP8266_Cmd("AT+CIPSTART=\"TCP\",\"192.168.1.1\",8000","OK",0,1000);
	
	// USART_Printf( "\r\n ESP8266 success\r\n" ); 
	
	
	
}

/**
  * @brief  ESP8266初始化函数
  * @param  无
  * @retval 无
  */
void ESP8266_Init ( void )
{
	ESP8266_GPIO_Config();
	ESP8266_USART_Config();
	
	macESP8266_CH_DISABLE();
	delay_ms(100);
	
	macESP8266_RST_LOW_LEVEL();
	delay_ms(100);
	
	macESP8266_CH_ENABLE();
	delay_ms(100);
	
	macESP8266_RST_HIGH_LEVEL();
	
	delay_ms(1000);
	
	strEsp8266_Fram_Record.InfBit.FramLength = 0;
	memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, RX_BUF_MAX_LEN);
	
	wifi_state = WIFI_STATE_INIT;
	wifi_retry_delay = 0;
	wifi_retry_count = 0;
}


/**
  * @brief  ESP8266专用USART发送函数 - 优化版本
  * @param  format: 格式字符串
  * @param  ...: 可变参数
  * @retval 无
  * @note   确保使用正确的USART2端口，每个字符发送后充分等待发送完成
  */
void ESP8266_USART_Printf(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    ESP8266_USART_SendString(buffer);
}

/**
  * @brief  ESP8266专用字符串发送函数
  * @param  str: 要发送的字符串
  * @retval 无
  * @note   确保使用USART2，每个字符发送后等待TXE标志，字符串发送完成后等待TC标志
  */
void ESP8266_USART_SendString(const char *str)
{
    if(str == NULL) return;
    
    while(*str)
    {
        // 等待发送缓冲区为空
        while(USART_GetFlagStatus(macESP8266_USARTx, USART_FLAG_TXE) == RESET);
        // 发送字符
        USART_SendData(macESP8266_USARTx, *str);
        str++;
    }
    
    // 等待整个字符串发送完成（最后一个字符的发送完成）
    while(USART_GetFlagStatus(macESP8266_USARTx, USART_FLAG_TC) == RESET);
}

/**
  * @brief  ESP8266专用字符发送函数
  * @param  ch: 要发送的字符
  * @retval 无
  * @note   确保使用USART2并充分等待发送完成
  */
void ESP8266_USART_SendByte(uint8_t ch)
{
    // 等待发送缓冲区为空
    while(USART_GetFlagStatus(macESP8266_USARTx, USART_FLAG_TXE) == RESET);
    // 发送字符
    USART_SendData(macESP8266_USARTx, ch);
    // 对于单字符发送，不需要等待TC，只需等待TXE即可
    // 这样可以提高发送效率，同时保证数据完整性
}


/**
  * @brief  初始化ESP8266模块的GPIO配置
  * @param  无
  * @retval 无
  */
static void ESP8266_GPIO_Config ( void )
{
	GPIO_InitTypeDef GPIO_InitStructure;

	macESP8266_CH_PD_APBxClock_FUN(macESP8266_CH_PD_CLK, ENABLE);
									   
	GPIO_InitStructure.GPIO_Pin = macESP8266_CH_PD_PIN;	
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;   
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz; 
	GPIO_Init(macESP8266_CH_PD_PORT, &GPIO_InitStructure);	 

	macESP8266_RST_APBxClock_FUN(macESP8266_RST_CLK, ENABLE);
									   
	GPIO_InitStructure.GPIO_Pin = macESP8266_RST_PIN;	
	GPIO_Init(macESP8266_RST_PORT, &GPIO_InitStructure);	 
}


/**
  * @brief  初始化ESP8266模块的USART配置
  * @param  无
  * @retval 无
  */
static void ESP8266_USART_Config ( void )
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;
	
	macESP8266_USART_APBxClock_FUN(macESP8266_USART_CLK, ENABLE);
	macESP8266_USART_GPIO_APBxClock_FUN(macESP8266_USART_GPIO_CLK, ENABLE);
	
	GPIO_InitStructure.GPIO_Pin = macESP8266_USART_TX_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(macESP8266_USART_TX_PORT, &GPIO_InitStructure);  
	
	GPIO_InitStructure.GPIO_Pin = macESP8266_USART_RX_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(macESP8266_USART_RX_PORT, &GPIO_InitStructure);
	
	USART_InitStructure.USART_BaudRate = macESP8266_USART_BAUD_RATE;
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	USART_InitStructure.USART_Parity = USART_Parity_No;
	USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	USART_Init(macESP8266_USARTx, &USART_InitStructure);
	
	USART_ITConfig(macESP8266_USARTx, USART_IT_RXNE, ENABLE);
	USART_ITConfig(macESP8266_USARTx, USART_IT_IDLE, ENABLE);
	
	ESP8266_USART_NVIC_Configuration();
	
	USART_Cmd(macESP8266_USARTx, ENABLE);
}


/**
  * @brief  配置 ESP8266 USART 的 NVIC 中断
  * @param  无
  * @retval 无
  */
static void ESP8266_USART_NVIC_Configuration(void)
{
	NVIC_InitTypeDef NVIC_InitStructure; 

	/* Enable the USART2 Interrupt */
	NVIC_InitStructure.NVIC_IRQChannel = macESP8266_USART_IRQ;	 
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);

}


void ESP8266_Rst(void)
{
	macESP8266_RST_LOW_LEVEL();
	delay_ms(500);
	macESP8266_RST_HIGH_LEVEL();
}


/*
 * 函数名称：ESP8266_Cmd
 * 功能描述：向WF-ESP8266模块发送AT指令
 * 输入参数：cmd - 要发送的指令
 *          reply1, reply2 - 期望的响应，可以为NULL，若期望响应为二者逻辑或关系
 *          waittime - 等待响应的时间
 * 输出参数：无
 * 返 回 值：1 - 指令发送成功
 *          0 - 指令发送失败
 * 调用说明：被外部函数调用
 */
bool ESP8266_Cmd(char *cmd, char *reply1, char *reply2, u32 waittime)
{	    
	strEsp8266_Fram_Record.InfBit.FramLength = 0;

	macESP8266_Usart("%s\r\n", cmd);

	if ((reply1 == 0) && (reply2 == 0))
		return true;
	
	delay_ms(waittime);
	
	strEsp8266_Fram_Record.Data_RX_BUF[strEsp8266_Fram_Record.InfBit.FramLength] = '\0';
  
	if ((reply1 != 0) && (reply2 != 0))
	{
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, reply1) || strstr(strEsp8266_Fram_Record.Data_RX_BUF, reply2))
			return true;
	}
 	
	else if (reply1 != 0)
	{
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, reply1))
			return true;
	}
	
	else if (reply2 != 0)
	{
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, reply2))
			return true;
	}
	
	return false;
}




bool ESP8266_Net_Mode_Choose(ENUM_Net_ModeTypeDef enumMode)
{
	switch (enumMode)
	{
		case STA:
			return ESP8266_Cmd("AT+CWMODE=1", "OK", "no change", 2500); 
		
		case AP:
			return ESP8266_Cmd("AT+CWMODE=2", "OK", "no change", 2500); 
		
		case STA_AP:
			return ESP8266_Cmd("AT+CWMODE=3", "OK", "no change", 2500); 
		
		default:
			return false;
	}
}


bool ESP8266_JoinAP(char *pSSID, char *pPassWord)
{
	char cCmd[120];
	sprintf(cCmd, "AT+CWJAP=\"%s\",\"%s\"", pSSID, pPassWord);
	return ESP8266_Cmd(cCmd, "OK", NULL, 5000);
}


bool ESP8266_BuildAP(char *pSSID, char *pPassWord, ENUM_AP_PsdMode_TypeDef enunPsdMode)
{
	char cCmd[120];
	sprintf(cCmd, "AT+CWSAP=\"%s\",\"%s\",1,%d", pSSID, pPassWord, enunPsdMode);
	return ESP8266_Cmd(cCmd, "OK", 0, 1000);
}

bool ESP8266_Enable_MultipleId(FunctionalState enumEnUnvarnishTx)
{
	return ESP8266_Cmd("AT+CIPMUX=%d", "OK", 0, 500);
}


bool ESP8266_Link_Server(ENUM_NetPro_TypeDef enumE, char *ip, char *ComNum, ENUM_ID_NO_TypeDef id)
{
	char cStr[100] = {0}, cCmd[120];

	switch (enumE)
	{
		case enumTCP:
			sprintf(cStr, "\"%s\",\"%s\",%s", "TCP", ip, ComNum);
			break;
		
		case enumUDP:
			sprintf(cStr, "\"%s\",\"%s\",%s", "UDP", ip, ComNum);
			break;
		
		default:
			break;
	}

	if (id < 5)
		sprintf(cCmd, "AT+CIPSTART=%d,%s", id, cStr);
	else
		sprintf(cCmd, "AT+CIPSTART=%s", cStr);

	return ESP8266_Cmd(cCmd, "OK", "ALREAY CONNECT", 4000);
}

bool ESP8266_StartOrShutServer(FunctionalState enumMode, char *pPortNum, char *pTimeOver)
{
	char cCmd1[120], cCmd2[120];

	if (enumMode)
	{
		sprintf(cCmd1, "AT+CIPSERVER=%d,%s", 1, pPortNum);
		sprintf(cCmd2, "AT+CIPSTO=%s", pTimeOver);
		return (ESP8266_Cmd(cCmd1, "OK", 0, 500) && ESP8266_Cmd(cCmd2, "OK", 0, 500));
	}
	else
	{
		sprintf(cCmd1, "AT+CIPSERVER=%d,%s", 0, pPortNum);
		return ESP8266_Cmd(cCmd1, "OK", 0, 500);
	}
}

uint8_t ESP8266_Get_LinkStatus(void)
{
	if (ESP8266_Cmd("AT+CIPSTATUS", "OK", 0, 500))
	{
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "STATUS:2\r\n"))
			return 2;
		else if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "STATUS:3\r\n"))
			return 3;
		else if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "STATUS:4\r\n"))
			return 4;
	}
	return 0;
}

uint8_t ESP8266_Get_IdLinkStatus(void)
{
	uint8_t ucIdLinkStatus = 0x00;
	
	if (ESP8266_Cmd("AT+CIPSTATUS", "OK", 0, 500))
	{
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTATUS:0,"))
			ucIdLinkStatus |= 0x01;
		else 
			ucIdLinkStatus &= ~0x01;
		
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTATUS:1,"))
			ucIdLinkStatus |= 0x02;
		else 
			ucIdLinkStatus &= ~0x02;
		
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTATUS:2,"))
			ucIdLinkStatus |= 0x04;
		else 
			ucIdLinkStatus &= ~0x04;
		
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTATUS:3,"))
			ucIdLinkStatus |= 0x08;
		else 
			ucIdLinkStatus &= ~0x08;
		
		if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+CIPSTATUS:4,"))
			ucIdLinkStatus |= 0x10;
		else 
			ucIdLinkStatus &= ~0x10;	

	}
	
	return ucIdLinkStatus;
	
}


uint8_t ESP8266_Inquire_ApIp(char *pApIp, uint8_t ucArrayLength)
{
	char uc;
	char *pCh;
	
	ESP8266_Cmd("AT+CIFSR", "OK", 0, 500);
	
	pCh = strstr(strEsp8266_Fram_Record.Data_RX_BUF, "APIP,\"");
	if (pCh)
		pCh += 6;
	else
		return 0;
	
	for (uc = 0; uc < ucArrayLength; uc++)
	{
		pApIp[uc] = *(pCh + uc);
		if (pApIp[uc] == '\"')
		{
			pApIp[uc] = '\0';
			break;
		}
	}
	return 1;
}

void ESP8266_ExitUnvarnishSend(void)
{
	delay_ms(1000);
	macESP8266_Usart("+++");
	delay_ms(500);
}

bool ESP8266_SendString(FunctionalState enumEnUnvarnishTx, char *pStr, u32 ulStrLength, ENUM_ID_NO_TypeDef ucId)
{
	char cStr[20];
	bool bRet = false;
	
	if (enumEnUnvarnishTx)
	{
		macESP8266_Usart("%s", pStr);
		bRet = true;
	}
	else
	{
		if (ucId < 5)
			sprintf(cStr, "AT+CIPSEND=%d,%d", ucId, ulStrLength + 2);
		else
			sprintf(cStr, "AT+CIPSEND=%d", ulStrLength + 2);
		
		ESP8266_Cmd(cStr, "> ", 0, 1000);
		bRet = ESP8266_Cmd(pStr, "SEND OK", 0, 1000);
	}
	return bRet;
}


char *ESP8266_ReceiveString(FunctionalState enumEnUnvarnishTx)
{
	char *pRecStr = 0;
	
	strEsp8266_Fram_Record.InfBit.FramLength = 0;
	strEsp8266_Fram_Record.InfBit.FramFinishFlag = 0;
	
	while (!strEsp8266_Fram_Record.InfBit.FramFinishFlag);
	strEsp8266_Fram_Record.Data_RX_BUF[strEsp8266_Fram_Record.InfBit.FramLength] = '\0';
	
	if (enumEnUnvarnishTx)
		pRecStr = strEsp8266_Fram_Record.Data_RX_BUF;
	else if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+IPD"))
		pRecStr = strEsp8266_Fram_Record.Data_RX_BUF;

	return pRecStr;
}

#if WIFI_ENABLE
void USART2_IRQHandler(void)
{	
    uint8_t ucCh;

    if (USART_GetITStatus(macESP8266_USARTx, USART_IT_RXNE) != RESET)
    {
        ucCh = USART_ReceiveData(macESP8266_USARTx);
        if (strEsp8266_Fram_Record.InfBit.FramLength < (RX_BUF_MAX_LEN - 1))
        {
            strEsp8266_Fram_Record.Data_RX_BUF[strEsp8266_Fram_Record.InfBit.FramLength++] = ucCh;
        }
        USART_ClearITPendingBit(macESP8266_USARTx, USART_IT_RXNE);
    }
     
    if (USART_GetITStatus(macESP8266_USARTx, USART_IT_IDLE) == SET)
    {
        ucCh = USART_ReceiveData(macESP8266_USARTx);
        strEsp8266_Fram_Record.Data_RX_BUF[strEsp8266_Fram_Record.InfBit.FramLength] = '\0';
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+IPD"))
        {
            wifi_rx_flag = 1;
            USART_Printf("WiFi: Received data frame\r\n");
        }
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "CONNECT"))
        {
            USART_Printf("WiFi: Client connected\r\n");
        }
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "CLOSED"))
        {
            USART_Printf("WiFi: Client disconnected\r\n");
            ucTcpClosedFlag = 1;
        }
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTCONNECTED"))
        {
            USART_Printf("MQTT: Connected to Huawei Cloud\r\n");
        }
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED"))
        {
            USART_Printf("MQTT: Disconnected from Huawei Cloud\r\n");
            wifi_state = WIFI_STATE_CONNECTED;
        }
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTSUBRECV"))
        {
            // 检查是否已有待处理的命令，避免重复处理
            if (pending_mqtt_command_flag)
            {
                USART_Printf("MQTT: Command already pending, skip this message\r\n");
                strEsp8266_Fram_Record.InfBit.FramLength = 0;
                strEsp8266_Fram_Record.InfBit.FramFinishFlag = 1;
                USART_ClearITPendingBit(macESP8266_USARTx, USART_IT_IDLE);
                return;
            }
            
            USART_Printf("MQTT: Received subscribed message\r\n");
            USART_Printf("MQTT: Message: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
            
            // 将命令保存到待处理缓冲区，由主循环处理
            strncpy(pending_mqtt_command, strEsp8266_Fram_Record.Data_RX_BUF, sizeof(pending_mqtt_command) - 1);
            pending_mqtt_command[sizeof(pending_mqtt_command) - 1] = '\0';
            pending_mqtt_command_flag = 1;
            
            // 清空原始缓冲区
            strEsp8266_Fram_Record.InfBit.FramLength = 0;
            memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, sizeof(strEsp8266_Fram_Record.Data_RX_BUF));
        }
        
        strEsp8266_Fram_Record.InfBit.FramFinishFlag = 1;
        USART_ClearITPendingBit(macESP8266_USARTx, USART_IT_IDLE);
    }	
}

void ESP8266_AT_Test(void)
{
    uint8_t count = 0;
    
    USART_Printf("ESP8266 AT Test: Starting...\r\n");
    
    macESP8266_RST_HIGH_LEVEL();
    delay_ms(1000);
    
    while (count < 5)
    {
        USART_Printf("ESP8266 AT Test: Attempt %d...\r\n", count + 1);
        strEsp8266_Fram_Record.InfBit.FramLength = 0;
        macESP8266_Usart("AT\r\n");
        delay_ms(500);
        strEsp8266_Fram_Record.Data_RX_BUF[strEsp8266_Fram_Record.InfBit.FramLength] = '\0';
        
        if (strstr(strEsp8266_Fram_Record.Data_RX_BUF, "OK"))
        {
            USART_Printf("ESP8266 AT Test: SUCCESS!\r\n");
            return;
        }
        USART_Printf("ESP8266 AT Test: Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        ESP8266_Rst();
        delay_ms(1000);
        count++;
    }
    USART_Printf("ESP8266 AT Test: FAILED after 5 attempts!\r\n");
}

#endif

#if WIFI_ENABLE
void ESP8266_ProcessWiFiConnection(void)
{
    if (wifi_state == WIFI_STATE_IDLE || wifi_state == WIFI_STATE_CONNECTED)
    {
        return;
    }
    
    if (wifi_retry_delay > 0)
    {
        wifi_retry_delay -= 10;
        return;
    }
    
    if (wifi_state == WIFI_STATE_INIT)
    {
        USART_Printf("\r\n=== ESP8266 WiFi Connection ===\r\n");
        
        USART_Printf("Step 1: Testing AT commands...\r\n");
        ESP8266_AT_Test();
        
        USART_Printf("Step 2: Setting STA mode...\r\n");
        if (ESP8266_Net_Mode_Choose(STA))
        {
            USART_Printf("STA mode set successfully\r\n");
        }
        else
        {
            USART_Printf("Failed to set STA mode!\r\n");
            wifi_retry_delay = 3000;
            return;
        }
        
        USART_Printf("Step 3: Joining AP '%s'...\r\n", macUser_ESP8266_ApSsid);
        while (!ESP8266_JoinAP(macUser_ESP8266_ApSsid, macUser_ESP8266_ApPwd))
        {
            wifi_retry_count++;
            USART_Printf("WiFi: Connection attempt %d failed, retrying...\r\n", wifi_retry_count);
            delay_ms(1000);
        }
        USART_Printf("WiFi: Connected to AP successfully!\r\n");
        
        USART_Printf("Step 4: Getting IP address...\r\n");
        if (ESP8266_Cmd("AT+CIFSR", "OK", 0, 1000))
        {
            char* response = strEsp8266_Fram_Record.Data_RX_BUF;
            char* ip_start = strstr(response, "\"");
            if(ip_start) {
                ip_start++;
                char* ip_end = strstr(ip_start, "\"");
                if(ip_end) {
                    *ip_end = '\0';
                    USART_Printf("WiFi: IP Address: %s\r\n", ip_start);
                }
            }
        }
        else
        {
            USART_Printf("Failed to get IP address!\r\n");
        }
        
        USART_Printf("Step 5: Configuring TCP server...\r\n");
        
        USART_Printf("Setting CIPMUX=1...\r\n");
        if (ESP8266_Cmd("AT+CIPMUX=1", "OK", 0, 1000))
        {
            USART_Printf("CIPMUX set to 1 successfully\r\n");
        }
        else
        {
            USART_Printf("Failed to set CIPMUX!\r\n");
            wifi_retry_delay = 3000;
            return;
        }
        
        USART_Printf("Starting TCP server on port 8288...\r\n");
        if (ESP8266_Cmd("AT+CIPSERVER=1,8288", "OK", 0, 1000))
        {
            USART_Printf("TCP Server started successfully on port 8288\r\n");
        }
        else
        {
            USART_Printf("Failed to start TCP server!\r\n");
            wifi_retry_delay = 3000;
            return;
        }
        
        USART_Printf("=== ESP8266 WiFi Connection Complete ===\r\n");
        USART_Printf("Connect to TCP server at: %s:8288\r\n", strstr(strEsp8266_Fram_Record.Data_RX_BUF, "\"") ? strstr(strEsp8266_Fram_Record.Data_RX_BUF, "\"") + 1 : "unknown");
        
        wifi_state = WIFI_STATE_CONNECTED;
        wifi_retry_count = 0;
    }
}
#endif

void ESP8266_ReconnectWiFi(void)
{
    wifi_state = WIFI_STATE_INIT;
}

void ESP8266_ProcessData(void)
{
    if (wifi_rx_flag && strEsp8266_Fram_Record.InfBit.FramFinishFlag)
    {
        wifi_rx_flag = 0;
        strEsp8266_Fram_Record.InfBit.FramFinishFlag = 0;
        
        char* ipd_pos = strstr(strEsp8266_Fram_Record.Data_RX_BUF, "+IPD");
        if (ipd_pos)
        {
            char* data_start = strchr(ipd_pos, ':');
            if (data_start)
            {
                data_start++;
                USART_Printf("WiFi: Received data: %s\r\n", data_start);
                
                if (strstr(data_start, "open") || strstr(data_start, "OPEN"))
                {
                    USART_Printf("WiFi: Command 'open' received - Opening barrier\r\n");
                    // 使用非阻塞版本
                    Servo_SetUp();
                    // 设置延迟关闭标志，由主循环处理
                    servo_close_delay = BARRIER_UP_DELAY_MS;
                }
            }
        }
        
        strEsp8266_Fram_Record.InfBit.FramLength = 0;
    }
}

void ESP8266_SendToClient(char *data)
{
    if (wifi_state == WIFI_STATE_CONNECTED)
    {
        char cmd[100];
        sprintf(cmd, "AT+CIPSEND=0,%d", strlen(data));
        if (ESP8266_Cmd(cmd, ">", 0, 1000))
        {
            ESP8266_Cmd(data, "SEND OK", 0, 1000);
        }
    }
}

static uint32_t mqtt_retry_delay = 0;
static uint8_t mqtt_retry_count = 0;

bool ESP8266_MQTTConnect(void)
{
    USART_Printf("\r\n========================================\r\n");
    USART_Printf("MQTT: Starting Huawei Cloud connection...\r\n");
    USART_Printf("========================================\r\n");
    
    USART_Printf("\r\n=== Step 3: AT+MQTTUSERCFG ===\r\n");
    USART_Printf("Desc: Set MQTT login username and password\r\n");
    USART_Printf("Sending: AT+MQTTUSERCFG=0,1,\"NULL\",\"%s\",\"***\",0,0,\"\"\r\n", HUAWEI_MQTT_USERNAME);
    char usercfg_cmd[256];
    sprintf(usercfg_cmd, "AT+MQTTUSERCFG=0,1,\"NULL\",\"%s\",\"%s\",0,0,\"\"", 
            HUAWEI_MQTT_USERNAME, HUAWEI_MQTT_PASSWORD);
    if (!ESP8266_Cmd(usercfg_cmd, "OK", NULL, 2000))
    {
        USART_Printf("Result: FAILED\r\n");
        USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        return false;
    }
    USART_Printf("Result: OK\r\n");
    USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
    
    USART_Printf("\r\n=== Step 4: AT+MQTTCLIENTID ===\r\n");
    USART_Printf("Desc: Set MQTT ClientID\r\n");
    USART_Printf("Sending: AT+MQTTCLIENTID=0,\"%s\"\r\n", HUAWEI_MQTT_CLIENT_ID);
    char clientid_cmd[128];
    sprintf(clientid_cmd, "AT+MQTTCLIENTID=0,\"%s\"", HUAWEI_MQTT_CLIENT_ID);
    if (!ESP8266_Cmd(clientid_cmd, "OK", NULL, 1000))
    {
        USART_Printf("Result: FAILED\r\n");
        USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        return false;
    }
    USART_Printf("Result: OK\r\n");
    USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
    
    USART_Printf("\r\n=== Step 5: AT+MQTTCONN ===\r\n");
    USART_Printf("Desc: Set MQTT broker address and connect\r\n");
    USART_Printf("Sending: AT+MQTTCONN=0,\"%s\",1883,0\r\n", HUAWEI_MQTT_BROKER);
    USART_Printf("Note: Last parameter is 0 (non-SSL mode)\r\n");
    char conn_cmd[256];
    sprintf(conn_cmd, "AT+MQTTCONN=0,\"%s\",1883,0", HUAWEI_MQTT_BROKER);
    if (!ESP8266_Cmd(conn_cmd, "+MQTTCONNECTED", "OK", 5000))
    {
        USART_Printf("Result: FAILED\r\n");
        USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        return false;
    }
    USART_Printf("Result: OK\r\n");
    USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
    
    USART_Printf("\r\n=== Step 6: AT+MQTTSUB ===\r\n");
    USART_Printf("Desc: Subscribe to property report topic\r\n");
    char topic[128];
    sprintf(topic, "$oc/devices/%s/sys/properties/report", HUAWEI_MQTT_DEVICE_ID);
    USART_Printf("Sending: AT+MQTTSUB=0,\"%s\",1\r\n", topic);
    char sub_cmd[256];
    sprintf(sub_cmd, "AT+MQTTSUB=0,\"%s\",1", topic);
    if (!ESP8266_Cmd(sub_cmd, "OK", NULL, 2000))
    {
        USART_Printf("Result: FAILED\r\n");
        USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        return false;
    }
    USART_Printf("Result: OK\r\n");
    USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
    
    USART_Printf("\r\n========================================\r\n");
    USART_Printf("MQTT: Connected to Huawei Cloud successfully!\r\n");
    USART_Printf("========================================\r\n");
    
    return true;
}

bool ESP8266_MQTTSubscribe(const char* topic, uint8_t qos)
{
    USART_Printf("\r\n--- MQTT Subscribe ---\r\n");
    USART_Printf("Topic: %s\r\n", topic);
    USART_Printf("QoS: %d\r\n", qos);
    char cmd[256];
    sprintf(cmd, "AT+MQTTSUB=0,\"%s\",%d", topic, qos);
    USART_Printf("Sending: AT+MQTTSUB=0,\"%s\",%d\r\n", topic, qos);
    
    if (!ESP8266_Cmd(cmd, "OK", NULL, 2000))
    {
        USART_Printf("Result: FAILED\r\n");
        USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
        return false;
    }
    
    USART_Printf("Result: OK\r\n");
    USART_Printf("Response: %s\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
    return true;
}

bool ESP8266_MQTTPublish(const char* topic, const char* payload)
{
    // 清空缓冲区，避免残留数据干扰
    strEsp8266_Fram_Record.InfBit.FramLength = 0;
    memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, sizeof(strEsp8266_Fram_Record.Data_RX_BUF));
    
    // 转义payload中的双引号和逗号（按照成功案例格式）
    char escaped_payload[512] = {0};
    int i = 0, j = 0;
    while (payload[i] != '\0' && j < 500)
    {
        if (payload[i] == '"')
        {
            escaped_payload[j++] = '\\';
            escaped_payload[j++] = '"';
            // 如果下一个字符是逗号，也需要转义
            if (payload[i+1] == ',')
            {
                escaped_payload[j++] = '\\';
                escaped_payload[j++] = ',';
                i++;
            }
        }
        else
        {
            escaped_payload[j++] = payload[i];
        }
        i++;
    }
    escaped_payload[j] = '\0';
    
    // 构建完整命令
    char cmd[1024];
    sprintf(cmd, "AT+MQTTPUB=0,\"%s\",\"%s\",0,0", topic, escaped_payload);
    
    // 使用较短的超时时间，避免阻塞系统
    if (!ESP8266_Cmd(cmd, "OK", NULL, 500))
    {
        USART_Printf("MQTT Publish FAILED\r\n");
        return false;
    }
    
    // 清空缓冲区
    strEsp8266_Fram_Record.InfBit.FramLength = 0;
    memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, sizeof(strEsp8266_Fram_Record.Data_RX_BUF));
    
    USART_Printf("MQTT Publish OK\r\n");
    return true;
}

void ESP8266_ProcessMQTTCommand(char *message)
{
    // 空指针保护
    if (message == NULL || message[0] == '\0')
    {
        USART_Printf("ProcessMQTTCommand: Empty message received\r\n");
        return;
    }
    
    // 华为云命令格式: +MQTTSUBRECV:0,"$oc/devices/xxx/sys/commands/request_id=uuid",len,{"command_name":"open_barrier",...}
    USART_Printf("Processing MQTT command...\r\n");
    USART_Printf("Raw message: %s\r\n", message);
    
    // 1. 找到+MQTTSUBRECV部分开始解析
    char *mqtt_recv_start = strstr(message, "+MQTTSUBRECV");
    if (!mqtt_recv_start)
    {
        USART_Printf("No +MQTTSUBRECV found in message\r\n");
        command_processing = 0; // 重置标志位
        return;
    }
    
    // 2. 从+MQTTSUBRECV开始提取request_id
    char request_id[64] = "unknown";
    char *req_id_start = strstr(mqtt_recv_start, "request_id=");
    if (req_id_start)
    {
        req_id_start += 11; // 跳过 "request_id="
        char *req_id_end = strchr(req_id_start, '"'); // 查找下一个双引号
        if (req_id_end)
        {
            int req_id_len = req_id_end - req_id_start;
            if (req_id_len > 0 && req_id_len < sizeof(request_id))
            {
                strncpy(request_id, req_id_start, req_id_len);
                request_id[req_id_len] = '\0';
            }
        }
    }
    USART_Printf("Request ID: %s\r\n", request_id);
    
    // 3. 提取JSON payload (从{到最后一个})
    char *payload_start = strchr(mqtt_recv_start, '{');
    char *payload_end = strrchr(mqtt_recv_start, '}');
    
    if (!payload_start || !payload_end)
    {
        USART_Printf("Invalid command format - no JSON payload\r\n");
        return;
    }
    
    int payload_len = payload_end - payload_start + 1;
    char payload[512];
    strncpy(payload, payload_start, payload_len);
    payload[payload_len] = '\0';
    
    USART_Printf("Command payload: %s\r\n", payload);
    
    // 4. 解析command_name
    char *cmd_name_start = strstr(payload, "\"command_name\":\"");
    if (!cmd_name_start)
    {
        USART_Printf("No command_name found\r\n");
        return;
    }
    
    cmd_name_start += 16; // 跳过 "\"command_name\":\"" (16个字符)
    USART_Printf("cmd_name_start points to: [%s]\r\n", cmd_name_start);
    
    char *cmd_name_end = strchr(cmd_name_start, '"');
    if (!cmd_name_end)
    {
        USART_Printf("Invalid command_name - no closing quote\r\n");
        return;
    }
    
    int cmd_name_len = cmd_name_end - cmd_name_start;
    USART_Printf("Command name length: %d\r\n", cmd_name_len);
    
    char command_name[64];
    strncpy(command_name, cmd_name_start, cmd_name_len);
    command_name[cmd_name_len] = '\0';
    
    USART_Printf("Command: [%s]\r\n", command_name);
    
    // 执行命令
    if (strcmp(command_name, "open_barrier") == 0)
    {
        USART_Printf("Executing: Open barrier\r\n");
        // 使用非阻塞版本，只设置角度不等待
        Servo_SetUp();
        // 设置延迟关闭标志，由主循环处理
        servo_close_delay = 2000; // 2秒后关闭
        // 设置命令响应，由主循环发送
        strncpy(pending_request_id, request_id, sizeof(pending_request_id)-1);
        pending_result_code = 0;
        strncpy(pending_result_desc, "success", sizeof(pending_result_desc)-1);
        pending_command_response = 1;
    }
    else if (strcmp(command_name, "close_barrier") == 0)
    {
        USART_Printf("Executing: Close barrier\r\n");
        // 使用非阻塞版本，只设置角度不等待
        Servo_SetDown();
        // 设置命令响应，由主循环发送
        strncpy(pending_request_id, request_id, sizeof(pending_request_id)-1);
        pending_result_code = 0;
        strncpy(pending_result_desc, "success", sizeof(pending_result_desc)-1);
        pending_command_response = 1;
    }
    else if (strcmp(command_name, "query_status") == 0)
    {
        USART_Printf("Executing: Query status\r\n");
        // 设置命令响应，由主循环发送
        strncpy(pending_request_id, request_id, sizeof(pending_request_id)-1);
        pending_result_code = 0;
        strncpy(pending_result_desc, "success", sizeof(pending_result_desc)-1);
        pending_command_response = 1;
    }
    else
    {
        USART_Printf("Unknown command: %s\r\n", command_name);
        // 设置命令响应，由主循环发送
        strncpy(pending_request_id, request_id, sizeof(pending_request_id)-1);
        pending_result_code = -1;
        strncpy(pending_result_desc, "failed", sizeof(pending_result_desc)-1);
        pending_command_response = 1;
    }
}

void ESP8266_ProcessMQTTConnection(void)
{
    if (wifi_state == WIFI_STATE_IDLE || 
        wifi_state == WIFI_STATE_MQTT_CONNECTED || 
        wifi_state == WIFI_STATE_MQTT_CONNECTING)
    {
        return;
    }
    
    if (mqtt_retry_delay > 0)
    {
        mqtt_retry_delay -= 10;
        return;
    }
    
    if (wifi_state == WIFI_STATE_CONNECTED)
    {
        wifi_state = WIFI_STATE_MQTT_CONNECTING;
        USART_Printf("\r\n=== Huawei Cloud MQTT Connection ===\r\n");
        
        if (ESP8266_MQTTConnect())
        {
            wifi_state = WIFI_STATE_MQTT_CONNECTED;
            mqtt_retry_count = 0;
            USART_Printf("=== Huawei Cloud MQTT Connection Complete ===\r\n");
        }
        else
        {
            mqtt_retry_count++;
            mqtt_retry_delay = 5000;
            wifi_state = WIFI_STATE_CONNECTED;
            USART_Printf("MQTT: Connection attempt %d failed, retrying in 5 seconds...\r\n", mqtt_retry_count);
        }
    }
}

// 处理延迟任务：命令响应和servo延迟关闭
void ESP8266_ProcessPendingTasks(void)
{
    // 处理待处理的MQTT命令（从中断移到主循环）
    if (pending_mqtt_command_flag)
    {
        pending_mqtt_command_flag = 0;
        
        // 在主循环中处理命令
        ESP8266_ProcessMQTTCommand(pending_mqtt_command);
        
        // 清空待处理命令缓冲区
        memset(pending_mqtt_command, 0, sizeof(pending_mqtt_command));
        
        // 命令处理完成，重置标志
        command_processing = 0;
    }
    
    // 处理servo延迟关闭
    if (servo_close_delay > 0)
    {
        servo_close_delay -= 10;
        if (servo_close_delay <= 0)
        {
            // 使用非阻塞版本，只设置角度不等待
            Servo_SetDown();
            USART_Printf("Servo closed after delay\r\n");
        }
    }
    
    // 处理延迟的命令响应
    if (pending_command_response)
    {
        pending_command_response = 0;
        
        // 清空缓冲区，避免残留数据干扰
        strEsp8266_Fram_Record.InfBit.FramLength = 0;
        memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, sizeof(strEsp8266_Fram_Record.Data_RX_BUF));
        
        // 发送命令响应
        MQTT_ReportCommandResponse(pending_request_id, pending_result_code, pending_result_desc);
        
        // 再次清空缓冲区
        strEsp8266_Fram_Record.InfBit.FramLength = 0;
        memset(strEsp8266_Fram_Record.Data_RX_BUF, 0, sizeof(strEsp8266_Fram_Record.Data_RX_BUF));
        
        // 允许处理下一个命令
        command_processing = 0;
    }
}


