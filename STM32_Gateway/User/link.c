#include "link.h"
#include "pi_link.h"
#include "USART.h"
#include <stdio.h>

void Link_Init(void)
{
    PI_UART_Init();
    PI_UART_SendString("LINK:OK\r\n");
    USART_Printf("Link: Raspberry Pi UART ready (USART2)\r\n");
}

void Link_Process(void)
{
    PI_UART_Process();
}

void Link_ReportRemainingSpots(uint8_t remaining)
{
    char buf[16];
    sprintf(buf, "SPOTS:%d\r\n", (int)remaining);
    PI_UART_SendString(buf);
}

void Link_ReportSpot(uint8_t spot_id, uint8_t occupied)
{
    char buf[20];
    sprintf(buf, "SPOT:%d:%d\r\n", (int)spot_id, occupied ? 1 : 0);
    PI_UART_SendString(buf);
}

void Link_ReportCard(const uint8_t *card_id)
{
    char buf[32];
    sprintf(buf, "CARD:%02X%02X%02X%02X\r\n",
            (unsigned int)card_id[0], (unsigned int)card_id[1],
            (unsigned int)card_id[2], (unsigned int)card_id[3]);
    PI_UART_SendString(buf);
}

void Link_RequestCapture(void)
{
    PI_UART_SendString("CAPTURE\r\n");
}

void Link_ReportBarrierUp(const char *reason)
{
    char buf[32];
    if (reason == NULL || reason[0] == '\0')
    {
        reason = LINK_REASON_REMOTE;
    }
    sprintf(buf, "BARRIER:OPEN:%s\r\n", reason);
    PI_UART_SendString(buf);
}

void Link_ReportBarrierDown(void)
{
    PI_UART_SendString("BARRIER:CLOSE\r\n");
}
