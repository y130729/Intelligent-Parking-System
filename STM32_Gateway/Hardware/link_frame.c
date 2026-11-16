#include "link_frame.h"
#include <string.h>

static const char HEX[] = "0123456789ABCDEF";

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint8_t LinkFrame_Crc8(const char *p, uint16_t n)
{
    uint8_t crc = 0x00;
    uint16_t i;
    uint8_t b;

    for (i = 0; i < n; i++)
    {
        crc ^= (uint8_t)p[i];
        for (b = 0; b < 8; b++)
        {
            if (crc & 0x80) crc = (uint8_t)((crc << 1) ^ 0x07);
            else            crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

int LinkFrame_Encode(uint8_t seq, const char *payload, char *out, int out_size)
{
    int len;
    int i;
    int pos = 0;
    uint8_t crc;

    if (payload == NULL || out == NULL) return 0;
    len = (int)strlen(payload);
    if (len <= 0 || len > LINK_FRAME_MAX_PAYLOAD) return 0;
    if (out_size < len + 10) return 0;

    out[pos++] = '#';
    out[pos++] = (char)('0' + LINK_FRAME_VER);
    out[pos++] = HEX[(seq >> 4) & 0x0F];
    out[pos++] = HEX[seq & 0x0F];
    out[pos++] = HEX[(len >> 4) & 0x0F];
    out[pos++] = HEX[len & 0x0F];
    for (i = 0; i < len; i++) out[pos++] = payload[i];

    /* CRC 覆盖 VER..PAYLOAD，即 out[1..pos-1] */
    crc = LinkFrame_Crc8(&out[1], (uint16_t)(pos - 1));

    out[pos++] = '*';
    out[pos++] = HEX[(crc >> 4) & 0x0F];
    out[pos++] = HEX[crc & 0x0F];
    out[pos] = '\0';
    return pos;
}

int LinkFrame_Decode(const char *line, uint8_t *seq, char *payload, int payload_size)
{
    int len;
    int i;
    int ver;
    int s_hi, s_lo, l_hi, l_lo, c_hi, c_lo;
    uint8_t crc;

    if (line == NULL || line[0] != '#') return LINK_FRAME_LEGACY;

    ver  = hex_val(line[1]);
    s_hi = hex_val(line[2]);
    s_lo = hex_val(line[3]);
    l_hi = hex_val(line[4]);
    l_lo = hex_val(line[5]);
    if (ver < 0 || s_hi < 0 || s_lo < 0 || l_hi < 0 || l_lo < 0) return LINK_FRAME_INVALID;

    len = (l_hi << 4) | l_lo;
    if (len <= 0 || len > LINK_FRAME_MAX_PAYLOAD) return LINK_FRAME_INVALID;
    if (ver != LINK_FRAME_VER) return LINK_FRAME_INVALID;

    /* 需要 '#' + VER+SEQ+LEN(5) + payload(len) + '*' + CRC(2) = 9 + len
       注意：这里一定要算上开头的 '#'。少数一个字节，就会把自己发出去的每一帧
       都判成非法帧——两端都安装好了却一直不通信，而且不会有任何报错。 */
    if ((int)strlen(line) != 6 + len + 3) return LINK_FRAME_INVALID;
    if (line[6 + len] != '*') return LINK_FRAME_INVALID;

    c_hi = hex_val(line[7 + len]);
    c_lo = hex_val(line[8 + len]);
    if (c_hi < 0 || c_lo < 0) return LINK_FRAME_INVALID;

    crc = LinkFrame_Crc8(&line[1], (uint16_t)(5 + len));
    if (crc != (uint8_t)((c_hi << 4) | c_lo)) return LINK_FRAME_INVALID;

    if (payload != NULL && payload_size > len)
    {
        for (i = 0; i < len; i++) payload[i] = line[6 + i];
        payload[len] = '\0';
    }
    if (seq != NULL) *seq = (uint8_t)((s_hi << 4) | s_lo);
    return LINK_FRAME_OK;
}
