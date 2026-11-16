#include "link.h"
#include "pi_link.h"
#include "USART.h"
#include <stdio.h>
#include <string.h>

/* 编译期断言：报文缓冲必须装得下最长的 payload。
   这里最容易犯的错就是"加了一个字段，忘了算长度"。 */
#define LINK_PAYLOAD_MAX   LINK_FRAME_MAX_PAYLOAD
#define LINK_BUF_SIZE      (LINK_PAYLOAD_MAX + 1)

static const char *const REASON_NAMES[] = { "card", "plate", "remote" };

const char *Link_ReasonToString(LinkReason reason)
{
    if (reason > LINK_REASON_REMOTE) return "remote";
    return REASON_NAMES[reason];
}

LinkReason Link_ReasonFromString(const char *s, uint8_t *ok)
{
    uint8_t i;

    if (ok) *ok = 1;
    if (s == NULL || s[0] == '\0')
    {
        /* 老固件/老脚本不带 reason：按 remote 处理（保持兼容） */
        return LINK_REASON_REMOTE;
    }
    for (i = 0; i <= (uint8_t)LINK_REASON_REMOTE; i++)
    {
        if (strcmp(s, REASON_NAMES[i]) == 0)
        {
            return (LinkReason)i;
        }
    }
    if (ok) *ok = 0;
    return LINK_REASON_REMOTE;
}

void Link_Init(void)
{
    PI_UART_Init();
    PI_UART_SendFramed("LINK:OK");
    USART_Printf("Link: Raspberry Pi UART ready (USART2, framed v%d)\r\n",
                 LINK_FRAME_VER);
}

void Link_Process(void)
{
    PI_UART_Process();
    PI_UART_Tick();
}

void Link_Tick(void)
{
    PI_UART_Tick();
}

/* 所有上报都用 snprintf + 定长缓冲：宁可截断，也不越界。 */
void Link_ReportRemainingSpots(uint8_t remaining)
{
    char buf[LINK_BUF_SIZE];
    snprintf(buf, sizeof(buf), "SPOTS:%d", (int)remaining);
    PI_UART_SendFramed(buf);
}

void Link_ReportSpot(uint8_t spot_id, uint8_t occupied)
{
    char buf[LINK_BUF_SIZE];
    snprintf(buf, sizeof(buf), "SPOT:%d:%d", (int)spot_id, occupied ? 1 : 0);
    PI_UART_SendFramed(buf);
}

void Link_ReportCard(const uint8_t *card_id)
{
    char buf[LINK_BUF_SIZE];
    snprintf(buf, sizeof(buf), "CARD:%02X%02X%02X%02X",
             (unsigned int)card_id[0], (unsigned int)card_id[1],
             (unsigned int)card_id[2], (unsigned int)card_id[3]);
    PI_UART_SendFramed(buf);
}

void Link_RequestCapture(void)
{
    PI_UART_SendFramed("CAPTURE");
}

void Link_ReportBarrierUp(LinkReason reason)
{
    char buf[LINK_BUF_SIZE];
    snprintf(buf, sizeof(buf), "BARRIER:OPEN:%s", Link_ReasonToString(reason));
    PI_UART_SendFramed(buf);
}

void Link_ReportBarrierDown(void)
{
    PI_UART_SendFramed("BARRIER:CLOSE");
}

/* 车位传感器故障：单独一种报文，不要混进 SPOT（否则会被当成"距离超限=空位"） */
void Link_ReportSensorFault(uint8_t spot_id)
{
    char buf[LINK_BUF_SIZE];
    snprintf(buf, sizeof(buf), "SPOT:%d:FAULT", (int)spot_id);
    PI_UART_SendFramed(buf);
}
