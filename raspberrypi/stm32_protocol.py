#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""STM32 <-> 树莓派 行协议解析与构造。

协议为 ASCII 文本行，以 \\r\\n 结尾，单行不超过 64 字节。

  STM32 -> 树莓派
    SPOTS:<n>              剩余车位数
    SPOT:<id>:<0|1>        车位 <id> 空闲/占用
    SPOT:<id>:FAULT        车位 <id> 传感器故障（回波超时/无回波）
                           必须单独报，不能报成 "空位"——否则云端会把
                           坏传感器报出来的空位派给下一辆车
    CARD:<8hex>            读到的 RFID 卡号
    BARRIER:OPEN[:<reason>] 栏杆已抬起，reason = card|plate|remote
    BARRIER:CLOSE          栏杆已落下
    LINK:OK                STM32 启动完成
    CAPTURE                车辆到位，请拍照识别

  树莓派 -> STM32
    PLATE:<车牌> | PLATE:NONE
    OPEN[:<reason>] | CLOSE[:<reason>]
    LINK:OK                （保留）

reason 字段是新固件加入的，老固件只发 BARRIER:OPEN。解析时两种都兼容，
凭证来源由此得到明确答案，不再依赖时间窗口猜测。
"""

BARRIER_UP = "up"
BARRIER_DOWN = "down"

VALID_REASONS = ("card", "plate", "remote")


def _pay(topic, line, sep=":"):
    return line[len(topic) + len(sep):]


def parse_line(line):
    """解析一行 STM32 上报，返回 dict（kind 字段标识类型）。"""
    raw = (line or "").strip()
    if not raw:
        return {"kind": "empty", "raw": raw}

    if raw.startswith("SPOTS:"):
        try:
            return {"kind": "spots", "remaining": int(raw[len("SPOTS:"):])}
        except ValueError:
            return {"kind": "unknown", "raw": raw}

    if raw.startswith("SPOT:"):
        parts = raw.split(":")
        if len(parts) < 3:
            return {"kind": "unknown", "raw": raw}
        try:
            spot_id = int(parts[1])
        except ValueError:
            return {"kind": "unknown", "raw": raw}
        status = parts[2].strip().lower()
        if status in ("fault", "error", "sensor_fault"):
            return {"kind": "spot_fault", "spot_id": spot_id}
        if status in ("1", "occupied", "occupy", "true"):
            occupied = True
        elif status in ("0", "empty", "free", "false"):
            occupied = False
        else:
            return {"kind": "unknown", "raw": raw}
        return {"kind": "spot", "spot_id": spot_id, "occupied": occupied}

    if raw.startswith("CARD:"):
        return {"kind": "card", "card": _pay("CARD", raw).strip().upper()}

    if raw.startswith("BARRIER:"):
        parts = raw.split(":")
        state = parts[1].strip().lower() if len(parts) > 1 else ""
        reason = parts[2].strip().lower() if len(parts) > 2 else ""
        if reason not in VALID_REASONS:
            reason = ""
        if state == "open":
            return {"kind": "barrier", "state": BARRIER_UP, "reason": reason}
        if state == "close":
            return {"kind": "barrier", "state": BARRIER_DOWN, "reason": reason}
        return {"kind": "unknown", "raw": raw}

    if raw == "LINK:OK":
        return {"kind": "link_ok"}
    if raw == "CAPTURE":
        return {"kind": "capture"}

    return {"kind": "unknown", "raw": raw}


def build_open(reason="remote"):
    return "OPEN:%s" % reason


def build_close(reason="remote"):
    return "CLOSE:%s" % reason


def build_plate(plate):
    plate = (plate or "").strip()
    return "PLATE:%s" % (plate if plate else "NONE")
# ---------------------------------------------------------------------------
# 帧格式（与 STM32 侧 Hardware/link_frame.c 一一对应）
#
#     '#' VER SEQ(2) LEN(2) PAYLOAD '*' CRC(2)
#
# 为什么加这一层：裸文本没有完整性保护——一个被 EMI 打乱的字节会让
# "SPOT:12:1" 变成 "SPOT:12:7"，而且序列号缺失导致重传无法去重。
# 加 CRC 后至少能"发现问题"，加 SEQ 后重传可以做到幂等。
# 外层仍然是可读文本：现场 `cat /dev/serial0` 能直接看懂语义。
# ---------------------------------------------------------------------------

FRAME_VER = 1
FRAME_MAX_PAYLOAD = 63
FRAME_MAX_LINE = FRAME_MAX_PAYLOAD + 10

FRAME_OK = 1
FRAME_INVALID = 0
FRAME_LEGACY = -1


def crc8(data):
    """CRC-8/ATM（多项式 0x07，初值 0x00），与 STM32 的 LinkFrame_Crc8 一致。

    标准校验向量：crc8(b"123456789") == 0xF4。
    """
    if isinstance(data, str):
        data = data.encode("latin-1")
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def _line_bytes(line):
    """把读到的行还原成原始字节。

    串口是真·字节流。latin-1 是一一映射，不会丢字节；这里若统一按
    UTF-8 解码，含中文的 PAYLOAD 会被解成 str，回编码后 CRC 就对不上。
    """
    if isinstance(line, bytes):
        return line
    try:
        return line.encode("latin-1")
    except UnicodeEncodeError:
        return line.encode("utf-8")


def _payload_text(data):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("latin-1")


def encode_frame(seq, payload):
    """payload(str) -> 帧字符串（不含 CRLF），失败返回 None。

    返回的是"字节透明"的 latin-1 视图：payload 先按 UTF-8 编码，再逐
    字节映射成字符，发送端 encode("latin-1") 即可还原原始字节。
    """
    data = payload.encode("utf-8")
    if not data or len(data) > FRAME_MAX_PAYLOAD:
        return None
    seq &= 0xFF
    head = ("#%d%02X%02X" % (FRAME_VER, seq, len(data))).encode("ascii")
    body = head + data
    return (body + ("*%02X" % crc8(body[1:])).encode("ascii")).decode("latin-1")


def decode_frame(line):
    """解码一行 -> (status, seq, payload)。

    status = FRAME_OK / FRAME_INVALID / FRAME_LEGACY
    """
    if not line:
        return FRAME_LEGACY, None, line

    raw = _line_bytes(line).rstrip(b"\r\n")
    if not raw.startswith(b"#"):
        return FRAME_LEGACY, None, _payload_text(raw)
    if len(raw) < 10:
        return FRAME_INVALID, None, ""
    try:
        ver = int(raw[1:2], 16)
        seq = int(raw[2:4], 16)
        length = int(raw[4:6], 16)
    except ValueError:
        return FRAME_INVALID, None, ""
    if ver != FRAME_VER or length <= 0 or length > FRAME_MAX_PAYLOAD:
        return FRAME_INVALID, None, ""
    if len(raw) != 6 + length + 3:      # '#' VER SEQ(2) LEN(2) + payload + '*' + CRC(2)
        return FRAME_INVALID, None, ""
    if raw[6 + length:7 + length] != b"*":
        return FRAME_INVALID, None, ""
    try:
        want = int(raw[7 + length:9 + length], 16)
    except ValueError:
        return FRAME_INVALID, None, ""
    if crc8(raw[1:6 + length]) != want:
        return FRAME_INVALID, None, ""

    return FRAME_OK, seq, _payload_text(raw[6:6 + length])
