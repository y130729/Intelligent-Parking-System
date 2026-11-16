#include "pi_link.h"
#include "ring_buffer.h"
#include "uart_tx.h"
#include "USART.h"
#include "system_tick.h"
#include "OLED.h"
#include "state_machine.h"
#include "link.h"
#include <string.h>
#include <stdio.h>

/* 环形缓冲容量（2 的幂）。单行最长 PI_RX_BUF_SIZE，这里可排队多条命令，
   避免主循环繁忙时"缓冲覆盖丢指令"。

   取 512 而不是 256 的原因：加卡/清库要擦 Flash，F1 在擦除期间不能从 Flash 取指，
   CPU 停 20~80ms（115200 下最多 920 字节）。USART3 那条链路已经改成 DMA + 2KB
   环形缓冲，但 USART2 仍走 RXNE 中断 + 这个环形缓冲——中断只能把字节搬进这里，
   256 字节只够 22ms，正好卡在擦除窗口里。512 字节 = 44ms，配合"一次只发一条命令、
   等 ACK"的树莓派侧节奏足够；真要连续灌数据，这条链路还是得换 DMA。 */
#define PI_RX_RING_SIZE   512

static uint8_t  pi_rx_ring_buf[PI_RX_RING_SIZE];
static RingBuffer pi_rx_ring;

static uint8_t  pi_tx_buf[PI_TX_RING_SIZE];
static UartTx   pi_tx;

static PiLinkStats pi_stats;

/* --- 发送侧：序号 / 待确认帧 --- */
static uint8_t  pi_tx_seq = 0;
static char     pi_pending[LINK_FRAME_MAX_LINE + 2];
static uint16_t pi_pending_len = 0;
static uint8_t  pi_pending_seq = 0;
static uint8_t  pi_pending_active = 0;
static uint8_t  pi_pending_retries = 0;
static uint32_t pi_pending_deadline = 0;

/* --- 接收侧：重复帧检测 --- */
static uint8_t  pi_last_seq = 0;
static uint8_t  pi_has_last_seq = 0;

void PI_UART_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB1PeriphClockCmd(PI_UART_CLK, ENABLE);
    RCC_APB2PeriphClockCmd(PI_UART_GPIO_CLK, ENABLE);

    GPIO_InitStructure.GPIO_Pin = PI_UART_TX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(PI_UART_TX_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = PI_UART_RX_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(PI_UART_RX_PORT, &GPIO_InitStructure);

    USART_InitStructure.USART_BaudRate = PI_UART_BAUDRATE;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_InitStructure.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;
    USART_Init(PI_UARTx, &USART_InitStructure);

    USART_ITConfig(PI_UARTx, USART_IT_RXNE, ENABLE);
    USART_ITConfig(PI_UARTx, USART_IT_TXE, DISABLE);

    NVIC_InitStructure.NVIC_IRQChannel = PI_UART_IRQ;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    USART_Cmd(PI_UARTx, ENABLE);

    RingBuf_Init(&pi_rx_ring, pi_rx_ring_buf, PI_RX_RING_SIZE);
    UartTx_Init(&pi_tx, PI_UARTx, pi_tx_buf, PI_TX_RING_SIZE);

    memset(&pi_stats, 0, sizeof(pi_stats));
}

const PiLinkStats *PI_UART_Stats(void)
{
    return &pi_stats;
}

/* 阻塞发一个字节：仅用于关机前/异常时的兜底，业务路径不要用 */
void PI_UART_SendByte(uint8_t byte)
{
    while (USART_GetFlagStatus(PI_UARTx, USART_FLAG_TXE) == RESET);
    USART_SendData(PI_UARTx, byte);
}

void PI_UART_SendString(const char *str)
{
    if (str == NULL) return;
    while (*str)
    {
        PI_UART_SendByte((uint8_t)*str);
        str++;
    }
}

/* USART2 中断：RX 入环形缓冲；TX 由 UartTx 的 TXE 状态机出队。 */
void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(PI_UARTx, USART_IT_RXNE) != RESET)
    {
        uint8_t ch = (uint8_t)USART_ReceiveData(PI_UARTx);
        RingBuf_Write(&pi_rx_ring, ch);
    }
    if (USART_GetITStatus(PI_UARTx, USART_IT_TXE) != RESET)
    {
        UartTx_OnTxE(&pi_tx);
    }
}

/* 统一出口：track=1 时把这一帧记为"待确认"，超时由 PI_UART_Tick 重传。
   ACK 帧自己不再等确认，否则两边会互相 ACK 到死。 */
static void send_frame(const char *payload, uint8_t track)
{
    char frame[LINK_FRAME_MAX_LINE + 2];
    uint8_t seq = pi_tx_seq++;
    int n;

    n = LinkFrame_Encode(seq, payload, frame, sizeof(frame) - 2);
    if (n <= 0)
    {
        return;
    }
    frame[n++] = '\r';
    frame[n++] = '\n';

    if (!UartTx_PutBytes(&pi_tx, (const uint8_t *)frame, (uint16_t)n))
    {
        pi_stats.tx_overflow++;
    }
    pi_stats.tx_frames++;

    if (track)
    {
        /* 只保留最近一帧待确认（见 Readme「重传策略」）。
           事件是稀疏的（每次刷卡/抬杆一条），单槽足够覆盖真实场景。 */
        memcpy(pi_pending, frame, (size_t)n);
        pi_pending_len = (uint16_t)n;
        pi_pending_seq = seq;
        pi_pending_active = 1;
        pi_pending_retries = 0;
        pi_pending_deadline = GetTick() + PI_ACK_TIMEOUT_MS;
    }
}

void PI_UART_SendFramed(const char *payload)
{
    uint8_t track = 1;
    if (payload == NULL) return;
    /* "ACK:" 开头的帧不再要求对端确认 */
    if (strncmp(payload, "ACK:", 4) == 0) track = 0;
    send_frame(payload, track);
}

void PI_UART_Tick(void)
{
    if (!pi_pending_active) return;
    if ((int32_t)(GetTick() - pi_pending_deadline) < 0) return;

    if (pi_pending_retries >= PI_MAX_RETRY)
    {
        pi_pending_active = 0;
        pi_stats.tx_giveup++;
        USART_Printf("Link: frame %02X unacked, give up after %d retries\r\n",
                     pi_pending_seq, (int)PI_MAX_RETRY);
        return;
    }

    pi_pending_retries++;
    pi_stats.tx_retries++;
    UartTx_PutBytes(&pi_tx, (const uint8_t *)pi_pending, pi_pending_len);
    pi_pending_deadline = GetTick() + PI_ACK_TIMEOUT_MS;
}

static void note_ack(const char *payload)
{
    /* "ACK:XX" */
    int hi, lo, val;
    if ((int)strlen(payload) < 6) return;
    hi = payload[4];
    lo = payload[5];
    hi = (hi >= '0' && hi <= '9') ? hi - '0'
       : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
       : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10 : -1;
    lo = (lo >= '0' && lo <= '9') ? lo - '0'
       : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
       : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10 : -1;
    if (hi < 0 || lo < 0) return;
    val = (hi << 4) | lo;
    if (pi_pending_active && (uint8_t)val == pi_pending_seq)
    {
        pi_pending_active = 0;
    }
}

/* 处理一条来自树莓派的命令。
   注意：这里只做"校验 + 分发"，不在这里阻塞等待任何东西。 */
static void PI_UART_HandlePayload(const char *payload)
{
    const char *reason_str;
    uint8_t reason_ok = 1;
    LinkReason reason;

    if (strncmp(payload, "ACK:", 4) == 0)
    {
        note_ack(payload);
        return;
    }

    USART_Printf("PI_RX: %s\r\n", payload);

    /* "PLATE:<plate>" -> 第 4 行显示
       "PLATE:NONE"    -> 清空 */
    if (strncmp(payload, "PLATE:", 6) == 0)
    {
        const char *plate = &payload[6];
        if (strcmp(plate, "NONE") == 0)
        {
            OLED_DisplayPlateNumber("------");
        }
        else
        {
            /* 截断在 OLED 能显示的长度，避免下游缓冲区溢出 */
            char safe[16];
            strncpy(safe, plate, sizeof(safe) - 1);
            safe[sizeof(safe) - 1] = '\0';
            OLED_DisplayPlateNumber(safe);
        }
    }
    else if (strncmp(payload, "OPEN", 4) == 0 && (payload[4] == '\0' || payload[4] == ':'))
    {
        reason_str = (payload[4] == ':') ? &payload[5] : "";
        reason = Link_ReasonFromString(reason_str, &reason_ok);
        if (!reason_ok)
        {
            /* 白名单之外的 reason 一律拒绝，绝不把任意字符串往下传 */
            USART_Printf("Link: reject OPEN with unknown reason\r\n");
            return;
        }
        USART_Printf("Remote command: open barrier (%s)\r\n", Link_ReasonToString(reason));
        StateMachine_RemoteOpen(reason);
    }
    else if (strncmp(payload, "CLOSE", 5) == 0 && (payload[5] == '\0' || payload[5] == ':'))
    {
        reason_str = (payload[5] == ':') ? &payload[6] : "";
        reason = Link_ReasonFromString(reason_str, &reason_ok);
        if (!reason_ok)
        {
            USART_Printf("Link: reject CLOSE with unknown reason\r\n");
            return;
        }
        USART_Printf("Remote command: close barrier (%s)\r\n", Link_ReasonToString(reason));
        StateMachine_RemoteClose();
    }
    else
    {
        USART_Printf("Link: unknown payload '%s'\r\n", payload);
    }
}

/* 一行到达：先尝试按帧解析，坏帧丢弃；不是帧则按裸文本兼容处理。 */
static void PI_UART_HandleLine(const char *line)
{
    char payload[LINK_FRAME_MAX_PAYLOAD + 1];
    uint8_t seq = 0;
    int rc;

    if (line[0] == '\0') return;

    rc = LinkFrame_Decode(line, &seq, payload, sizeof(payload));

    if (rc == LINK_FRAME_LEGACY)
    {
        /* 老固件/老脚本发的裸文本：仍然接受，但计数以便排查混装 */
        pi_stats.rx_legacy++;
        strncpy(payload, line, sizeof(payload) - 1);
        payload[sizeof(payload) - 1] = '\0';
        PI_UART_HandlePayload(payload);
        return;
    }

    if (rc == LINK_FRAME_INVALID)
    {
        pi_stats.rx_bad_crc++;
        USART_Printf("Link: bad frame dropped\r\n");
        return;
    }

    /* 重复帧（对端重传）：ACK 一次但不再执行，保证命令幂等 */
    if (pi_has_last_seq && seq == pi_last_seq)
    {
        char ack[8];
        pi_stats.rx_duplicates++;
        sprintf(ack, "ACK:%02X", (unsigned int)seq);
        PI_UART_SendFramed(ack);   /* ACK 帧不 track，不会再引发 ACK */
        return;
    }
    pi_last_seq = seq;
    pi_has_last_seq = 1;
    pi_stats.rx_frames++;

    {
        char ack[8];
        sprintf(ack, "ACK:%02X", (unsigned int)seq);
        PI_UART_SendFramed(ack);
    }

    PI_UART_HandlePayload(payload);
}

/* 主循环调用：从环形缓冲取字节、按 CR/LF 切行。 */
void PI_UART_Process(void)
{
    static char line[PI_RX_BUF_SIZE];
    static uint16_t line_len = 0;
    static uint8_t discarding = 0;
    uint8_t ch;

    while (RingBuf_Read(&pi_rx_ring, &ch))
    {
        if (ch == '\n' || ch == '\r')
        {
            if (line_len > 0 && !discarding)
            {
                line[line_len] = '\0';
                PI_UART_HandleLine(line);
            }
            line_len = 0;
            discarding = 0;
        }
        else if (discarding)
        {
            /* 超长行：丢弃到行尾再重新同步 */
        }
        else if (line_len < PI_RX_BUF_SIZE - 1)
        {
            line[line_len++] = (char)ch;
        }
        else
        {
            pi_stats.rx_bad_crc++;
            discarding = 1;
            line_len = 0;
        }
    }
}
