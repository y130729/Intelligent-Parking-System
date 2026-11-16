#ifndef __LINK_H
#define __LINK_H

#include "stm32f10x.h"

/* 应用层链路协议（STM32 <-> 树莓派）
 *
 * 分层：state_machine / main（应用）
 *         -> link（业务报文：拼 payload、解析 reason，本文件）
 *           -> pi_link（传输：帧格式、CRC、序号、ACK、重传）
 *             -> uart_tx + 环形缓冲（驱动）
 *
 * 业务代码里不再出现传输细节，也不再有 #if WIFI_ENABLE 分支。
 *
 * 帧格式见 link_frame.h；payload 语义见 Readme「串口协议」。
 */

/* 开闸凭证来源 —— 白名单枚举。
 * 必须是枚举而不是裸字符串：reason 来自串口输入，用字符串+sprintf
 * 会被"OPEN:" + 63 字节任意内容撑爆 32 字节缓冲（栈溢出）。
 * 枚举把可能性收敛成 3 个值，任何别的输入都在解析层被拒收。
 * 取值需与树莓派侧 passage.py 一致。 */
typedef enum {
    LINK_REASON_CARD = 0,
    LINK_REASON_PLATE = 1,
    LINK_REASON_REMOTE = 2
} LinkReason;

const char  *Link_ReasonToString(LinkReason reason);
/* 解析失败返回 LINK_REASON_REMOTE 并把 *ok 置 0（调用方应拒收） */
LinkReason   Link_ReasonFromString(const char *s, uint8_t *ok);

void Link_Init(void);       /* 初始化链路并发 LINK:OK */
void Link_Process(void);    /* 主循环：收命令 + ACK 超时重传 */
void Link_Tick(void);

/* 上报类（STM32 -> 树莓派） */
void Link_ReportRemainingSpots(uint8_t remaining);
void Link_ReportSpot(uint8_t spot_id, uint8_t occupied);
void Link_ReportCard(const uint8_t *card_id);
void Link_RequestCapture(void);
void Link_ReportBarrierUp(LinkReason reason);
void Link_ReportBarrierDown(void);
void Link_ReportSensorFault(uint8_t spot_id);

#endif /* __LINK_H */
