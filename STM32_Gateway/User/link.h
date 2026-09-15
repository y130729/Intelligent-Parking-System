#ifndef __LINK_H
#define __LINK_H

#include "stm32f10x.h"

/* 应用层链路协议（STM32 <-> 树莓派）
 *
 * 分层：state_machine / main（应用）
 *         -> link（协议：拼/解协议字符串，本文件）
 *           -> pi_link（驱动：USART2 字节收发 + 行提取）
 *
 * 目的：业务代码里不再出现传输细节。原来每处动作都写成
 *   #if WIFI_ENABLE
 *       ESP8266_SendToClient("BARRIER:OPEN");
 *   #else
 *       PI_UART_SendString("BARRIER:OPEN\r\n");
 *   #endif
 * 业务逻辑被传输层开关切成了两半；现在统一成 Link_xxx()。
 *
 * 协议定义见 Readme「串口协议」一节，与树莓派侧 stm32_protocol.py 对应。
 */

/* 开闸凭证来源，需与树莓派侧 passage.py 的取值一致 */
#define LINK_REASON_CARD    "card"
#define LINK_REASON_PLATE   "plate"
#define LINK_REASON_REMOTE  "remote"

/* 初始化链路并发 LINK:OK */
void Link_Init(void);

/* 主循环调用：处理树莓派下发的命令 */
void Link_Process(void);

/* 上报类（STM32 -> 树莓派） */
void Link_ReportRemainingSpots(uint8_t remaining);
void Link_ReportSpot(uint8_t spot_id, uint8_t occupied);
void Link_ReportCard(const uint8_t *card_id);
void Link_RequestCapture(void);
void Link_ReportBarrierUp(const char *reason);
void Link_ReportBarrierDown(void);

#endif /* __LINK_H */
