#ifndef __LINK_FRAME_H
#define __LINK_FRAME_H

#include <stdint.h>

/* STM32 <-> 树莓派 帧格式（文本外壳 + 完整性校验）
 *
 *     '#' VER SEQ(2) LEN(2) PAYLOAD '*' CRC(2)
 *      1   1    2      2      len      1    2      = 9 + len 字节
 *    例：  #1 05 0B "OPEN:card" '*' 3F
 *
 * - VER  ：协议版本，当前 1。接收方不接受未知版本（避免半升级状态互发错帧）
 * - SEQ  ：0..255 序号，用于"重复帧丢弃"（重传时同一个 SEQ 不会被重复执行）
 * - LEN  ：payload 字节数（上限 LINK_FRAME_MAX_PAYLOAD）
 * - CRC  ：CRC-8/ATM（多项式 0x07，初值 0x00），覆盖 VER..PAYLOAD
 *
 * 为什么用"文本外壳 + CRC"而不是纯二进制 + COBS：
 *   本链路的 payload 只有 10~20 字节、115200 波特率，二进制省不下几个字节；
 *   而现在排障时 `cat /dev/serial0` 就能直接看清 OPEN:card 这样的语义。
 *   一旦速率上到 460800 以上、或 payload 上到几百字节，再换二进制+COBS 才有意义。
 *
 * 本模块只做编解码，不碰硬件，可在 PC 上直接单元测试（见 tests/）。
 */

#define LINK_FRAME_VER          1
#define LINK_FRAME_MAX_PAYLOAD  63
#define LINK_FRAME_MAX_LINE     (LINK_FRAME_MAX_PAYLOAD + 10)

/* 解码结果 */
#define LINK_FRAME_LEGACY      (-1)   /* 不是帧（老固件/老脚本的裸文本） */
#define LINK_FRAME_INVALID      0     /* 是帧但校验/格式不通过 */
#define LINK_FRAME_OK           1

uint8_t LinkFrame_Crc8(const char *p, uint16_t n);

/* 成功返回写入 out 的字节数（不含结尾 NUL），失败返回 0 */
int LinkFrame_Encode(uint8_t seq, const char *payload, char *out, int out_size);

/* 返回值：
     LINK_FRAME_OK      —— 成功，seq 与 payload 均有效
     LINK_FRAME_LEGACY  —— 不是帧（老固件发的裸文本）
     LINK_FRAME_INVALID —— 是帧但格式/CRC 不通过 */
int LinkFrame_Decode(const char *line, uint8_t *seq, char *payload, int payload_size);

#endif /* __LINK_FRAME_H */
