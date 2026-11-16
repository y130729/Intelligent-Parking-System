/**************************************************************************************************
  Filename:       ParkingNode.c
  Revised:        $Date: 2024-01-01 $
  Revision:       $Revision: 1.0 $

  Description:    CC2530 Parking Node Application - Ultrasonic Sensor Based Parking Spot Detection.
                  This application uses HC-SR04 ultrasonic sensor to detect parking spot occupancy
                  and sends data to the coordinator via ZigBee network.


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

/*********************************************************************
  CC2530 Parking Node Application
  
  This application is designed for parking spot detection using:
  - HC-SR04 ultrasonic sensor for distance measurement
  - ZigBee wireless communication to coordinator
  - Periodic distance reporting (every 100ms)
  
  Data format sent to coordinator:
  - "PXX:XXXcm" (9 bytes) - P=Parking, XX=Spot number, XXX=Distance in cm
  - "PXX:err" (7 bytes) - Error case (distance > 1000cm)
  
  Hardware configuration:
  - P0.4: Trig pin (output) - Trigger ultrasonic sensor
  - P0.5: Echo pin (input) - Receive echo signal
*********************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include "OSAL.h"
#include "ZGlobals.h"
#include "AF.h"
#include "aps_groups.h"
#include "ZDApp.h"

#include "ParkingNode.h"

#include "OnBoard.h"

/* HAL */
#include "hal_led.h"
#include "hal_key.h"
#include "MT_UART.h"
#include "MT_APP.h"
#include "MT.h"


/*********************************************************************
 * MACROS
 */

#define TRIG_PIN P0_4    // Ultrasonic sensor trigger pin
#define ECHO_PIN P0_5    // Ultrasonic sensor echo pin

/*********************************************************************
 * CONSTANTS
 */

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */

// Application specific Cluster IDs
const cId_t ParkingNode_ClusterList[PARKINGNODE_MAX_CLUSTERS] =
{
  PARKINGNODE_PERIODIC_CLUSTERID,
  PARKINGNODE_FLASH_CLUSTERID
};

const SimpleDescriptionFormat_t ParkingNode_SimpleDesc =
{
  PARKINGNODE_ENDPOINT,              //  int Endpoint;
  PARKINGNODE_PROFID,                //  uint16 AppProfId[2];
  PARKINGNODE_DEVICEID,              //  uint16 AppDeviceId[2];
  PARKINGNODE_DEVICE_VERSION,        //  int   AppDevVer:4;
  PARKINGNODE_FLAGS,                 //  int   AppFlags:4;
  PARKINGNODE_MAX_CLUSTERS,          //  uint8  AppNumInClusters;
  (cId_t *)ParkingNode_ClusterList,  //  uint8 *pAppInClusterList;
  PARKINGNODE_MAX_CLUSTERS,          //  uint8  AppNumOutClusters;
  (cId_t *)ParkingNode_ClusterList   //  uint8 *pAppOutClusterList;
};

// Endpoint/Interface description
endPointDesc_t ParkingNode_epDesc;

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */
uint8 ParkingNode_TaskID;    // Task ID for internal task/event processing
devStates_t ParkingNode_NwkState;
uint8 ParkingNode_TransID;   // Unique message ID (counter)

afAddrType_t ParkingNode_Periodic_DstAddr;  // Broadcast address
afAddrType_t ParkingNode_Flash_DstAddr;     // Group address
afAddrType_t ParkingNode_P2P_DstAddr;       // Point-to-point address (coordinator)

aps_Group_t ParkingNode_Group;

uint8 ParkingNodePeriodicCounter = 0;
uint8 ParkingNodeFlashCounter = 0;
uint8 TxBuff[15] = {0};
uint8 ParkingSpotNum = PARKINGNODE_SPOT_NUM;  // Current device's parking spot number

/*********************************************************************
 * LOCAL FUNCTIONS
 */
void ParkingNode_HandleKeys( uint8 shift, uint8 keys );
void ParkingNode_MessageMSGCB( afIncomingMSGPacket_t *pckt );
void ParkingNode_SendPeriodicMessage( void );
void ParkingNode_SendFlashMessage( uint16 flashTime );
void ParkingNode_SendDistanceData( void );

/*********************************************************************
 * NETWORK LAYER CALLBACKS
 */

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @fn      ParkingNode_Init
 *
 * @brief   Initialization function for the Parking Node Task.
 *          This is called during initialization and contains
 *          application specific initialization (hardware setup,
 *          table initialization, power up notification).
 *
 * @param   task_id - the ID assigned by OSAL.
 *
 * @return  none
 */
void ParkingNode_Init( uint8 task_id )
{ 
  ParkingNode_TaskID = task_id;
  ParkingNode_NwkState = DEV_INIT;
  ParkingNode_TransID = 0;
  
  // UART initialization
  MT_UartInit();
  MT_UartRegisterTaskID(task_id);

  // Configure ultrasonic sensor pins
  P0DIR |= 0x10;    // P0.4 (Trig) as output
  P0DIR &= ~0x20;   // P0.5 (Echo) as input
  
#if defined ( BUILD_ALL_DEVICES )
  // Check jumper for coordinator/router selection
  if ( readCoordinatorJumper() )
    zgDeviceLogicalType = ZG_DEVICETYPE_COORDINATOR;
  else
    zgDeviceLogicalType = ZG_DEVICETYPE_ROUTER;
#endif // BUILD_ALL_DEVICES

#if defined ( HOLD_AUTO_START )
  ZDOInitDevice(0);
#endif

  // Setup periodic message destination address - Broadcast
  ParkingNode_Periodic_DstAddr.addrMode = (afAddrMode_t)AddrBroadcast;
  ParkingNode_Periodic_DstAddr.endPoint = PARKINGNODE_ENDPOINT;
  ParkingNode_Periodic_DstAddr.addr.shortAddr = 0xFFFF;

  // Setup flash command destination address - Group 1
  ParkingNode_Flash_DstAddr.addrMode = (afAddrMode_t)afAddrGroup;
  ParkingNode_Flash_DstAddr.endPoint = PARKINGNODE_ENDPOINT;
  ParkingNode_Flash_DstAddr.addr.shortAddr = PARKINGNODE_FLASH_GROUP;
  
  // Setup point-to-point address - Coordinator
  ParkingNode_P2P_DstAddr.addrMode = (afAddrMode_t)Addr16Bit;
  ParkingNode_P2P_DstAddr.endPoint = PARKINGNODE_ENDPOINT;
  ParkingNode_P2P_DstAddr.addr.shortAddr = 0x0000;

  // Fill out the endpoint description
  ParkingNode_epDesc.endPoint = PARKINGNODE_ENDPOINT;
  ParkingNode_epDesc.task_id = &ParkingNode_TaskID;
  ParkingNode_epDesc.simpleDesc = (SimpleDescriptionFormat_t *)&ParkingNode_SimpleDesc;
  ParkingNode_epDesc.latencyReq = noLatencyReqs;

  // Register the endpoint description with the AF
  afRegister( &ParkingNode_epDesc );

  // Register for all key events
  RegisterForKeys( ParkingNode_TaskID );

  // By default, all devices start out in Group 1
  ParkingNode_Group.ID = 0x0001;
  osal_memcpy( ParkingNode_Group.name, "Group 1", 7 );
  aps_AddGroup( PARKINGNODE_ENDPOINT, &ParkingNode_Group );
}

/*********************************************************************
 * @fn      ParkingNode_ProcessEvent
 *
 * @brief   Parking Node Application Task event processor.
 *          This function is called to process all events for the task.
 *          Events include timers, messages and any other user defined events.
 *
 * @param   task_id  - The OSAL assigned task ID.
 * @param   events - events to process (bit map).
 *
 * @return  none
 */
uint16 ParkingNode_ProcessEvent( uint8 task_id, uint16 events )
{
  afIncomingMSGPacket_t *MSGpkt;
  (void)task_id;  // Intentionally unreferenced parameter

  if ( events & SYS_EVENT_MSG )
  {
    MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive( ParkingNode_TaskID );
    while ( MSGpkt )
    {
      switch ( MSGpkt->hdr.event )
      {
        // Received when a key is pressed
        case KEY_CHANGE:
          ParkingNode_HandleKeys( ((keyChange_t *)MSGpkt)->state, ((keyChange_t *)MSGpkt)->keys );
          break;

        // Received when a message is received (OTA) for this endpoint
        case AF_INCOMING_MSG_CMD:
          ParkingNode_MessageMSGCB( MSGpkt );
          break;

        // Received whenever the device changes state in the network
        case ZDO_STATE_CHANGE:
          ParkingNode_NwkState = (devStates_t)(MSGpkt->hdr.status);
          if ( (ParkingNode_NwkState == DEV_ROUTER)
              || (ParkingNode_NwkState == DEV_END_DEVICE) )
          {
            // Start sending the periodic distance data
            osal_start_timerEx( ParkingNode_TaskID,
                              PARKINGNODE_SEND_PERIODIC_MSG_EVT,
                              PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT );

            /* 入网成功后再开狗，避免入网阶段被反复复位 */
            PARKINGNODE_WDT_START();
          }
          else
          {
            // Device is no longer in the network
          }
          break;

        default:
          break;
      }

      // Release the memory
      osal_msg_deallocate( (uint8 *)MSGpkt );

      // Next - if one is available
      MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive( ParkingNode_TaskID );
    }

    // return unprocessed events
    return (events ^ SYS_EVENT_MSG);
  }

  // Send distance data - generated by timer
  if ( events & PARKINGNODE_SEND_PERIODIC_MSG_EVT )
  {
    /* 喂狗：只要应用事件还能被处理，说明 OSAL 没死。
       一旦卡在某个无出口的等待里，这里就不会执行，1 秒后自动复位。 */
    PARKINGNODE_WDT_FEED();

    // Send the distance data to coordinator
    ParkingNode_SendDistanceData();

    // Setup to send message again in normal period (+ jitter)
    osal_start_timerEx( ParkingNode_TaskID, PARKINGNODE_SEND_PERIODIC_MSG_EVT,
        (PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT + (osal_rand() & 0x00FF)) );

    // return unprocessed events
    return (events ^ PARKINGNODE_SEND_PERIODIC_MSG_EVT);
  }

  // Discard unknown events
  return 0;
}

/*********************************************************************
 * Event Generation Functions
 */

/*********************************************************************
 * @fn      ParkingNode_HandleKeys
 *
 * @brief   Handles all key events for this device.
 *
 * @param   shift - true if in shift/alt.
 * @param   keys - bit field for key events.
 *
 * @return  none
 */
void ParkingNode_HandleKeys( uint8 shift, uint8 keys )
{
  (void)shift;  // Intentionally unreferenced parameter
  
  if ( keys & HAL_KEY_SW_1 )
  {
    // Send Flash Command to Group 1
    ParkingNode_SendFlashMessage( PARKINGNODE_FLASH_DURATION );
  }

  if ( keys & HAL_KEY_SW_2 )
  {
    // Toggle this device in and out of group 1
    aps_Group_t *grp;
    grp = aps_FindGroup( PARKINGNODE_ENDPOINT, PARKINGNODE_FLASH_GROUP );
    if ( grp )
    {
      // Remove from the group
      aps_RemoveGroup( PARKINGNODE_ENDPOINT, PARKINGNODE_FLASH_GROUP );
    }
    else
    {
      // Add to the flash group
      aps_AddGroup( PARKINGNODE_ENDPOINT, &ParkingNode_Group );
    }
  }
}

/*********************************************************************
 * LOCAL FUNCTIONS
 */

/*********************************************************************
 * @fn      ParkingNode_MessageMSGCB
 *
 * @brief   Data message processor callback. Processes incoming data
 *          from other devices based on cluster ID.
 *
 * @param   pkt - incoming message packet
 *
 * @return  none
 */
void ParkingNode_MessageMSGCB( afIncomingMSGPacket_t *pkt )
{
    uint16 flashTime;

    switch ( pkt->clusterId )
    {
        // Handle incoming distance data
        case PARKINGNODE_P2P_CLUSTERID:
            // Data format: PXX:XXXcm (9 bytes) or PXX:err (7 bytes)
            if(pkt->cmd.DataLength == 9 || pkt->cmd.DataLength == 7)
            {
                byte rf_buff[15] = {0};
                osal_memcpy(rf_buff, pkt->cmd.Data, pkt->cmd.DataLength);
                // Send to UART
                HalUARTWrite(0, rf_buff, pkt->cmd.DataLength); 
                HalUARTWrite(0, "\r\n", 2); 
            }
            break;    
            
        case PARKINGNODE_PERIODIC_CLUSTERID:
            break;

        case PARKINGNODE_FLASH_CLUSTERID:
            flashTime = BUILD_UINT16(pkt->cmd.Data[1], pkt->cmd.Data[2] );
            HalLedBlink( HAL_LED_4, 4, 50, (flashTime / 4) );
            break;
    }
}

/*********************************************************************
 * @fn      ParkingNode_SendPeriodicMessage
 *
 * @brief   Send the periodic message (broadcast).
 *
 * @return  none
 */
void ParkingNode_SendPeriodicMessage( void )
{
  if ( AF_DataRequest( &ParkingNode_Periodic_DstAddr, &ParkingNode_epDesc,
                       PARKINGNODE_PERIODIC_CLUSTERID,
                       1,
                       (uint8*)&ParkingNodePeriodicCounter,
                       &ParkingNode_TransID,
                       AF_DISCV_ROUTE,
                       AF_DEFAULT_RADIUS ) == afStatus_SUCCESS )
  {
  }
  else
  {
    // Error occurred in request to send
  }
}

/*********************************************************************
 * @fn      ParkingNode_SendFlashMessage
 *
 * @brief   Send the flash message to group 1.
 *
 * @param   flashTime - in milliseconds
 *
 * @return  none
 */
void ParkingNode_SendFlashMessage( uint16 flashTime )
{
  uint8 buffer[3];
  buffer[0] = (uint8)(ParkingNodeFlashCounter++);
  buffer[1] = LO_UINT16( flashTime );
  buffer[2] = HI_UINT16( flashTime );

  if ( AF_DataRequest( &ParkingNode_Flash_DstAddr, &ParkingNode_epDesc,
                       PARKINGNODE_FLASH_CLUSTERID,
                       3,
                       buffer,
                       &ParkingNode_TransID,
                       AF_DISCV_ROUTE,
                       AF_DEFAULT_RADIUS ) == afStatus_SUCCESS )
  {
  }
  else
  {
    // Error occurred in request to send
  }
}

/*********************************************************************
 * 超声波测距（带超时、滤波、故障上报）
 *
 * 这一段是整个节点最容易"把 OSAL 任务卡死"的地方，改动要点：
 *   1. 原来的 while(ECHO_PIN == 0) / while(ECHO_PIN == 1) 没有任何出口：
 *      传感器松脱、回波丢失 -> 任务永久阻塞 -> ZigBee 节点失联，
 *      只能靠人去现场断电。现在每个等待都有迭代上限。
 *   2. 5 次采样取中值（丢掉故障样本，至少 3 个有效才算通过），
 *      抑制单次误测；2cm 迟滞 + 连续 3 次一致才改判，抑制边界抖动。
 *   3. 无回波（ECHO 一直不拉高）= 传感器故障，单独上报 PXX:FAULT，
 *      不再随便报一个距离让上层误判成"空位"。
 *
 * 关于计时精度：本文件沿用原有的 Delay_10us() 作为 10us 时基
 * （原代码就是按 "每次循环 = 10us" 标定的）。更严谨的做法是用
 * Timer1 输入捕获直接量高电平宽度，但那会和 MAC 层的定时器冲突，
 * 需要重新分配定时器资源；在没有硬件的环境里改寄存器风险高于收益，
 * 因此这里先用"有界轮询 + 中值滤波"解决卡死和抖动这两个真问题。
 */

/* 一次测量的结果 */
#define ULTRA_OK          0
#define ULTRA_NO_ECHO     1   /* ECHO 一直不拉高：传感器/接线故障 */
#define ULTRA_TIMEOUT     2   /* ECHO 高电平超时：视为超量程 */

/* 回波等待上限：约 30ms（≈5m，HC-SR04 量程 4m，足够） */
#define ULTRA_RISE_TIMEOUT_ITERS   3000
/* 高电平宽度上限：约 40ms（≈6.8m），超过就按超量程处理 */
#define ULTRA_ECHO_TIMEOUT_ITERS   4000

void Delay_us(void)    // 1 us delay
{
    MicroWait(1);   
}

void Delay_10us(void)  // ~10 us delay
{
  MicroWait(1);
  asm("NOP"); 
}

void Delay_20us(void)  // ~20 us delay
{
   MicroWait(10);
   MicroWait(10);
}

/* 看门狗宏定义见 ParkingNode.h（需在 ProcessEvent 之前可见） */

/*********************************************************************
 * @fn      Ultrasonic_ReadOnce
 *
 * @brief   测一次距离。全程有界，任何情况下都会返回。
 *
 * @param   distance_cm - 输出：距离（cm）
 *
 * @return  ULTRA_OK / ULTRA_NO_ECHO / ULTRA_TIMEOUT
 */
static uint8 Ultrasonic_ReadOnce(uint16 *distance_cm)
{
    uint16 rise_wait = 0;
    uint16 echo_iters = 0;

    /* 触发：20us 高脉冲 */
    TRIG_PIN = 1;
    Delay_20us();
    TRIG_PIN = 0;

    /* 等 ECHO 拉高（有上限） */
    while (ECHO_PIN == 0)
    {
        if (++rise_wait >= ULTRA_RISE_TIMEOUT_ITERS)
        {
            return ULTRA_NO_ECHO;
        }
        Delay_10us();
    }

    /* 量高电平宽度（有上限） */
    while (ECHO_PIN == 1)
    {
        if (++echo_iters >= ULTRA_ECHO_TIMEOUT_ITERS)
        {
            return ULTRA_TIMEOUT;
        }
        Delay_10us();
    }

    if (echo_iters < 2)
    {
        return ULTRA_NO_ECHO;      /* 太窄，视为噪声 */
    }

    /* 每次迭代按 10us 计，声速 343m/s -> 距离(cm) = us / 58 */
    *distance_cm = (uint16)(((uint32)echo_iters * 10u) / 58u);
    return ULTRA_OK;
}

/* 中值：对最多 5 个样本做插入排序，返回中位数 */
static uint16 Ultrasonic_Median(uint16 *v, uint8 n)
{
    uint8 i, j;
    for (i = 1; i < n; i++)
    {
        uint16 key = v[i];
        j = i;
        while (j > 0 && v[j - 1] > key)
        {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = key;
    }
    return v[n / 2];
}

/* 滤波器状态 */
static uint16 reported_cm = 0xFFFF;   /* 0xFFFF = 尚未有有效值 */
static uint16 candidate_cm = 0;
static uint8  candidate_count = 0;
static uint8  fault_count = 0;
static uint8  fault_reported = 0;

/*********************************************************************
 * @fn      ParkingNode_SendDistanceData
 *
 * @brief   采样 -> 滤波 -> 上报。数据格式：
 *            "PXX:XXXcm"   正常距离
 *            "PXX:FAULT"   传感器故障（无回波）
 *
 *          注意帧长度必须与实际内容一致：原来错误帧只有 7 字节，
 *          却被当成 9 字节发出去，STM32 侧的 sscanf("%hucm") 解析失败，
 *          错误被静默吞掉。现在长度按 strlen 计算。
 */
void ParkingNode_SendDistanceData( void )
{
    uint16 samples[PARKINGNODE_ULTRASONIC_SAMPLES];
    uint8  valid = 0;
    uint8  i;
    uint8  len;

    /* 1) 采样 N 次，只保留成功的那几个 */
    for (i = 0; i < PARKINGNODE_ULTRASONIC_SAMPLES; i++)
    {
        uint16 d = 0;
        if (Ultrasonic_ReadOnce(&d) == ULTRA_OK && d <= PARKINGNODE_ULTRASONIC_MAX_CM)
        {
            samples[valid++] = d;
        }
    }

    /* 2) 有效样本不足 -> 判故障；连续 3 次才上报，避免偶发丢波刷屏 */
    if (valid < PARKINGNODE_ULTRASONIC_MIN_VALID)
    {
        if (fault_count < 0xFF) fault_count++;
        if (fault_count >= PARKINGNODE_ULTRASONIC_CONFIRM && !fault_reported)
        {
            fault_reported = 1;
            TxBuff[0] = 'P';
            TxBuff[1] = (uint8)('0' + (ParkingSpotNum / 10));
            TxBuff[2] = (uint8)('0' + (ParkingSpotNum % 10));
            TxBuff[3] = ':';
            osal_memcpy(&TxBuff[4], "FAULT", 5);
            len = 9;
            (void)AF_DataRequest( &ParkingNode_P2P_DstAddr, &ParkingNode_epDesc,
                                  PARKINGNODE_P2P_CLUSTERID,
                                  len, TxBuff, &ParkingNode_TransID,
                                  AF_DISCV_ROUTE, AF_DEFAULT_RADIUS );
        }
        return;
    }

    fault_count = 0;
    fault_reported = 0;

    {
        uint16 median = Ultrasonic_Median(samples, valid);

        /* 3) 2cm 迟滞 + 连续 3 次一致才改判 */
        if (reported_cm == 0xFFFF)
        {
            reported_cm = median;
            candidate_count = 0;
        }
        else if (median + PARKINGNODE_ULTRASONIC_HYST_CM < reported_cm ||
                 median > reported_cm + PARKINGNODE_ULTRASONIC_HYST_CM)
        {
            if (candidate_count == 0 || median == candidate_cm)
            {
                candidate_cm = median;
                candidate_count++;
            }
            else
            {
                candidate_cm = median;
                candidate_count = 1;
            }

            if (candidate_count >= PARKINGNODE_ULTRASONIC_CONFIRM)
            {
                reported_cm = median;
                candidate_count = 0;
            }
        }
        else
        {
            candidate_count = 0;
        }
    }

    /* 4) 上报稳定后的距离（饱和到 999，保持 3 位宽度） */
    {
        uint16 out = reported_cm;
        if (out > 999) out = 999;

        TxBuff[0] = 'P';
        TxBuff[1] = (uint8)('0' + (ParkingSpotNum / 10));
        TxBuff[2] = (uint8)('0' + (ParkingSpotNum % 10));
        TxBuff[3] = ':';
        TxBuff[4] = (uint8)('0' + (out / 100));
        TxBuff[5] = (uint8)('0' + ((out / 10) % 10));
        TxBuff[6] = (uint8)('0' + (out % 10));
        TxBuff[7] = 'c';
        TxBuff[8] = 'm';
        len = 9;

        (void)AF_DataRequest( &ParkingNode_P2P_DstAddr, &ParkingNode_epDesc,
                              PARKINGNODE_P2P_CLUSTERID,
                              len, TxBuff, &ParkingNode_TransID,
                              AF_DISCV_ROUTE, AF_DEFAULT_RADIUS );
    }
}

/*********************************************************************
*********************************************************************/