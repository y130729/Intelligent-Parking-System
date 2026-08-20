#ifndef __ESP8266_H
#define	__ESP8266_H

#include "stm32f10x.h"
#include <stdio.h>
#include <stdbool.h>
#include "project_config.h"
#include "USART.h"
#include "system_tick.h"



#if defined ( __CC_ARM   )
#pragma anon_unions
#endif



/******************************* ESP8266 模式类型定义 ***************************/
typedef enum{
	STA,
  AP,
  STA_AP  
} ENUM_Net_ModeTypeDef;

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_INIT,
    WIFI_STATE_JOINING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_MQTT_CONNECTING,
    WIFI_STATE_MQTT_CONNECTED,
    WIFI_STATE_FAILED
} WIFI_StateTypeDef;

extern WIFI_StateTypeDef wifi_state;


typedef enum{
	 enumTCP,
	 enumUDP,
} ENUM_NetPro_TypeDef;
	

typedef enum{
	Multiple_ID_0 = 0,
	Multiple_ID_1 = 1,
	Multiple_ID_2 = 2,
	Multiple_ID_3 = 3,
	Multiple_ID_4 = 4,
	Single_ID_0 = 5,
} ENUM_ID_NO_TypeDef;
	

typedef enum{
	OPEN = 0,
	WEP = 1,
	WPA_PSK = 2,
	WPA2_PSK = 3,
	WPA_WPA2_PSK = 4,
} ENUM_AP_PsdMode_TypeDef;



/******************************* ESP8266 外部全局变量 ***************************/
#define RX_BUF_MAX_LEN     1024                                     

extern struct  STRUCT_USARTx_Fram                                  //声明接收数据帧的结构体变量
{
	char  Data_RX_BUF [ RX_BUF_MAX_LEN ];
	
  union {
    __IO u16 InfAll;
    struct {
		  __IO u16 FramLength       :15;                               // 14:0 
		  __IO u16 FramFinishFlag   :1;                                // 15 
	  } InfBit;
  }; 
	
} strEsp8266_Fram_Record;



/******************************** ESP8266 引脚定义 ***********************************/
#define      macESP8266_CH_PD_APBxClock_FUN                   RCC_APB2PeriphClockCmd
#define      macESP8266_CH_PD_CLK                             RCC_APB2Periph_GPIOB  
#define      macESP8266_CH_PD_PORT                            GPIOB
#define      macESP8266_CH_PD_PIN                             GPIO_Pin_14

#define      macESP8266_RST_APBxClock_FUN                     RCC_APB2PeriphClockCmd
#define      macESP8266_RST_CLK                               RCC_APB2Periph_GPIOB
#define      macESP8266_RST_PORT                              GPIOB
#define      macESP8266_RST_PIN                               GPIO_Pin_15

 

#define      macESP8266_USART_BAUD_RATE                       115200

#define      macESP8266_USARTx                                USART2
#define      macESP8266_USART_APBxClock_FUN                   RCC_APB1PeriphClockCmd
#define      macESP8266_USART_CLK                             RCC_APB1Periph_USART2
#define      macESP8266_USART_GPIO_APBxClock_FUN              RCC_APB2PeriphClockCmd
#define      macESP8266_USART_GPIO_CLK                        RCC_APB2Periph_GPIOA    
#define      macESP8266_USART_TX_PORT                         GPIOA   
#define      macESP8266_USART_TX_PIN                          GPIO_Pin_2  // Default USART2 TX pin
#define      macESP8266_USART_RX_PORT                         GPIOA
#define      macESP8266_USART_RX_PIN                          GPIO_Pin_3  // Default USART2 RX pin
#define      macESP8266_USART_IRQ                             USART2_IRQn
#define      macESP8266_USART_INT_FUN                         USART2_IRQHandler


/* ESP8266 Macros */
#define     macESP8266_Usart(fmt, ...)              ESP8266_USART_Printf(fmt, ##__VA_ARGS__)

#define     macESP8266_CH_ENABLE()                  GPIO_SetBits(macESP8266_CH_PD_PORT, macESP8266_CH_PD_PIN)
#define     macESP8266_CH_DISABLE()                 GPIO_ResetBits(macESP8266_CH_PD_PORT, macESP8266_CH_PD_PIN)

#define     macESP8266_RST_HIGH_LEVEL()             GPIO_SetBits(macESP8266_RST_PORT, macESP8266_RST_PIN)
#define     macESP8266_RST_LOW_LEVEL()              GPIO_ResetBits(macESP8266_RST_PORT, macESP8266_RST_PIN)



/* ESP8266 Function Declarations */
void                     ESP8266_Init                        ( void );
void                     ESP8266_Rst                         ( void );
void                     ESP8266_AT_Test                     ( void );
bool                     ESP8266_Cmd                         ( char *cmd, char *reply1, char *reply2, u32 waittime );
bool                     ESP8266_Net_Mode_Choose             ( ENUM_Net_ModeTypeDef enumMode );
bool                     ESP8266_JoinAP                      ( char *pSSID, char *pPassWord );
bool                     ESP8266_BuildAP                     ( char *pSSID, char *pPassWord, ENUM_AP_PsdMode_TypeDef enunPsdMode );
bool                     ESP8266_Enable_MultipleId           ( FunctionalState enumEnUnvarnishTx );
bool                     ESP8266_Link_Server                 ( ENUM_NetPro_TypeDef enumE, char *ip, char *ComNum, ENUM_ID_NO_TypeDef id);
bool                     ESP8266_StartOrShutServer           ( FunctionalState enumMode, char *pPortNum, char *pTimeOver );
uint8_t                  ESP8266_Get_LinkStatus              ( void );
uint8_t                  ESP8266_Get_IdLinkStatus            ( void );
uint8_t                  ESP8266_Inquire_ApIp                ( char *pApIp, uint8_t ucArrayLength );
void                     ESP8266_ExitUnvarnishSend           ( void );
bool                     ESP8266_SendString                  ( FunctionalState enumEnUnvarnishTx, char *pStr, u32 ulStrLength, ENUM_ID_NO_TypeDef ucId );
char *                   ESP8266_ReceiveString               ( FunctionalState enumEnUnvarnishTx );

void                     ESP8266_USART_Printf                ( const char *format, ... );
void                     ESP8266_USART_SendString            ( const char *str );
void                     ESP8266_USART_SendByte              ( uint8_t ch );

void                     ESP8266_ProcessWiFiConnection        ( void );
void                     ESP8266_ReconnectWiFi                ( void );
void                     ESP8266_ProcessData                  ( void );
void                     ESP8266_SendToClient                 ( char *data );

/* Huawei Cloud MQTT Functions */
void                     ESP8266_ProcessMQTTConnection        ( void );
bool                     ESP8266_MQTTConnect                  ( void );
bool                     ESP8266_MQTTPublish                  ( const char* topic, const char* payload );
bool                     ESP8266_MQTTSubscribe                 ( const char* topic, uint8_t qos );
void                     ESP8266_ProcessMQTTCommand           ( char *message );
void                     ESP8266_ProcessPendingTasks          ( void );


/********************************** 用户需要设置的参数**********************************/
#define      macUser_ESP8266_ApSsid                       "iqoo11"                //要连接的热点名称
#define      macUser_ESP8266_ApPwd                        "zny20041102"           //要连接的热点密码

//#define      macUser_ESP8266_TcpServer_IP                 "192.168.0.11"      
//#define      macUser_ESP8266_TcpServer_Port               "8080"               


/********************************** 外部全局变量 ***************************************/
extern volatile uint8_t ucTcpClosedFlag;
extern volatile uint8_t wifi_rx_flag;



/********************************** 测试和示例函数 ***************************************/
void                     ESP8266_StaTcpClient  ( void );


#endif /* __ESP8266_H */




