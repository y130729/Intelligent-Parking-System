#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""STM32 <-> 树莓派 行协议解析与构造。

协议为 ASCII 文本行，以 \\r\\n 结尾，单行不超过 64 字节。

  STM32 -> 树莓派
    SPOTS:<n>              剩余车位数
    SPOT:<id>:<0|1>        车位 <id> 空闲/占用
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