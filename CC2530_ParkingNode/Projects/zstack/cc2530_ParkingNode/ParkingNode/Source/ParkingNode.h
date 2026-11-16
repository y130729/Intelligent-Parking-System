/**************************************************************************************************
  Filename:       ParkingNode.h
  Revised:        $Date: 2024-01-01 $
  Revision:       $Revision: 1.0 $

  Description:    CC2530 Parking Node Application definitions.
                  This file contains definitions for the parking spot detection node
                  using HC-SR04 ultrasonic sensor.


  Copyright 2007 Texas Instruments Incorporated. All rights reserved.

  IMPORTANT: Your use of this Software is limited to those specific rights
  granted under the terms of a software license agreement between the user
  who downloaded the software, his/her employer (which must be your employer)
  and Texas Instruments Incorporated (the "License").  You may not use this
  Software unless you agree to abide by the terms of the License. The License
  limits your use, and you acknowledge, that the Software may not be modified,
  copied or distributed unless embedded on a Texas Instruments microcontroller
  or used solely and exclusively in conjunction with a Texas Instruments radio
  frequency transceiver, which is integrated into your product.  Other than for
  the foregoing purpose, you may not use, reproduce, copy, prepare derivative
  works of, modify, distribute, perform, display or sell this Software and/or
  its documentation for any purpose.

  YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
  PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, 
  INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE, 
  NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
  TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
  NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
  LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
  INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
  OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
  OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
  (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

  Should you have any questions regarding your right to use this Software,
  contact Texas Instruments Incorporated at www.TI.com. 
**************************************************************************************************/

#ifndef PARKINGNODE_H
#define PARKINGNODE_H

#ifdef __cplusplus
extern "C"
{
#endif

/*********************************************************************
 * INCLUDES
 */
#include "ZComDef.h"
#include "ioCC2530.h"   /* WDCTL 等外设寄存器 */

/*********************************************************************
 * CONSTANTS
 */

// Endpoint and Profile definitions
#define PARKINGNODE_ENDPOINT           20

#define PARKINGNODE_PROFID             0x0F08
#define PARKINGNODE_DEVICEID           0x0001
#define PARKINGNODE_DEVICE_VERSION     0
#define PARKINGNODE_FLAGS              0

// Cluster definitions
#define PARKINGNODE_MAX_CLUSTERS       2
#define PARKINGNODE_PERIODIC_CLUSTERID 1
#define PARKINGNODE_FLASH_CLUSTERID    2
#define PARKINGNODE_COM_CLUSTERID      3
#define PARKINGNODE_P2P_CLUSTERID      4
  
// Send Message Timeout (milliseconds)
#define PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT   100

// Application Events (OSAL) - Bit weighted definitions
#define PARKINGNODE_SEND_PERIODIC_MSG_EVT       0x0001
  
// Group ID for Flash Command
#define PARKINGNODE_FLASH_GROUP                 0x0001
  
// Flash Command Duration (milliseconds)
#define PARKINGNODE_FLASH_DURATION              1000

// Parking Spot Number - Device's parking spot identifier
// Modify this value for each parking node device
#define PARKINGNODE_SPOT_NUM                    1

/* 超声波采样与滤波
 *
 * 单次测量会被车体形状/地面反射干扰，所以取多次的中值；
 * 只有中值还不够——阈值边界上会来回抖，所以再加 2cm 迟滞，
 * 并且要求连续 3 次一致才真正改判。
 *
 * 100ms 周期下，三次确认带来约 300ms 额外延迟：
 * 对「车停进车位」这种秒级事件完全可接受，换来的是不再抖动的状态。 */
#define PARKINGNODE_ULTRASONIC_SAMPLES          5
#define PARKINGNODE_ULTRASONIC_MIN_VALID        3
#define PARKINGNODE_ULTRASONIC_MAX_CM           400
#define PARKINGNODE_ULTRASONIC_HYST_CM          2
#define PARKINGNODE_ULTRASONIC_CONFIRM          3

/* 看门狗（CC2530 内置，WDCTL 寄存器）
 * MODE=00 看门狗模式；INT=00 超时 1s；CLR[7:4] 清计数器。
 * 清狗必须按手册写"两拍"：先 0xA 再 0x5（WDCTL = 0xA0 然后 0x50），
 * 单写 0xA0 不算清狗——一秒钟后照样复位。这种错误在台面上很难发现：
 * 节点看起来"能工作"，只是每隔一秒悄悄重启、重新入网。
 * 只在入网成功后启动：入网前 OSAL 可能长时间没有应用事件，
 * 提前开狗会造成「还没连上就复位」的启动循环。
 * 入网后若应用任务卡死，1 秒内自动复位重连。
 * 喂狗周期 = 100ms + 抖动(<=255ms) + 单次测量耗时(<=350ms)，最坏约 700ms < 1s。 */
#define PARKINGNODE_WDT_START()   do { WDCTL = 0x00; } while (0)
#define PARKINGNODE_WDT_FEED()    do { WDCTL = 0xA0; WDCTL = 0x50; } while (0)


/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * FUNCTIONS
 */

/*
 * Task Initialization for the Parking Node Application
 */
extern void ParkingNode_Init( uint8 task_id );

/*
 * Task Event Processor for the Parking Node Application
 */
extern UINT16 ParkingNode_ProcessEvent( uint8 task_id, uint16 events );

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* PARKINGNODE_H */
