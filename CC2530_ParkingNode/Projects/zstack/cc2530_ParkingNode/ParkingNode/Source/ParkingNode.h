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