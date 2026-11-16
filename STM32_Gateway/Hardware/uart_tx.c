#include "uart_tx.h"
#include <stddef.h>
#include "system_tick.h"

static uint16_t tx_space(const UartTx *tx)
{
    uint16_t used = RingBuf_Available((RingBuffer *)&tx->ring);
    return (uint16_t)(tx->ring.size - 1u - used);
}

void UartTx_Init(UartTx *tx, USART_TypeDef *usart, uint8_t *buf, uint16_t size)
{
    tx->usart = usart;
    tx->dropped = 0;
    tx->sent = 0;
    RingBuf_Init(&tx->ring, buf, size);
    USART_ITConfig(usart, USART_IT_TXE, DISABLE);
}

/* 队列里一有字节就打开 TXE 中断，由硬件节奏驱动发送。 */
static void tx_kick(UartTx *tx)
{
    if (RingBuf_Available(&tx->ring) > 0)
    {
        USART_ITConfig(tx->usart, USART_IT_TXE, ENABLE);
    }
}

uint8_t UartTx_PutBytes(UartTx *tx, const uint8_t *p, uint16_t n)
{
    uint16_t i;
    uint8_t ok = 1;

    /* 先一次性判断空间，避免每个字节都做一次减法 */
    if (tx_space(tx) < n)
    {
        ok = 0;
        tx->dropped = (uint16_t)(tx->dropped + (n - tx_space(tx)));
        n = tx_space(tx);
    }
    for (i = 0; i < n; i++)
    {
        RingBuf_Write(&tx->ring, p[i]);
    }
    tx_kick(tx);
    return ok;
}

uint8_t UartTx_PutString(UartTx *tx, const char *s)
{
    uint16_t n = 0;
    const char *p = s;

    if (s == NULL) return 1;
    while (*p) { n++; p++; }
    return UartTx_PutBytes(tx, (const uint8_t *)s, n);
}

uint16_t UartTx_Pending(const UartTx *tx)
{
    return (uint16_t)RingBuf_Available((RingBuffer *)&tx->ring);
}

void UartTx_OnTxE(UartTx *tx)
{
    uint8_t b;
    if (RingBuf_Read(&tx->ring, &b))
    {
        USART_SendData(tx->usart, b);
        tx->sent++;
    }
    else
    {
        /* 队列空：关掉 TXE 中断，避免空转中断风暴 */
        USART_ITConfig(tx->usart, USART_IT_TXE, DISABLE);
    }
}

void UartTx_Flush(UartTx *tx, uint32_t timeout_ms)
{
    uint32_t start = GetTick();
    while (UartTx_Pending(tx) > 0)
    {
        if ((GetTick() - start) >= timeout_ms) break;
    }
}
