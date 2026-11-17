#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""固件侧测试的公共工具：读文件、解析 #define、解析 Keil 工程。

这一层刻意做得很薄：只用正则读文本，不引入编译器、不引入 pycparser。
"评审者 clone 下来一条命令就能跑"比"解析得绝对精确"更重要。
"""

import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW_DIR = os.path.join(REPO_ROOT, "STM32_Gateway")
ZIGBEE_DIR = os.path.join(REPO_ROOT, "CC2530_ParkingNode")
ZIGBEE_SRC = os.path.join(ZIGBEE_DIR, "Projects", "zstack", "cc2530_ParkingNode",
                          "ParkingNode", "Source")
ZIGBEE_CFG = os.path.join(ZIGBEE_DIR, "Projects", "zstack", "Tools",
                          "CC2530DB", "f8wConfig.cfg")
README_PATH = os.path.join(REPO_ROOT, "Readme.md")
RPI_DIR = os.path.join(REPO_ROOT, "raspberrypi")
VECTORS_JSON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vectors.json")

# 让测试既能在仓库根目录跑，也能在 CI 的任意 cwd 跑
for _p in (REPO_ROOT, RPI_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)


def read(path, *parts):
    if parts:
        path = os.path.join(path, *parts)
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def read_fw(*parts):
    return read(os.path.join(FW_DIR, *parts))


def parse_int(literal):
    """把 C 整数常量转成 int：支持 0x 前缀和 u/U/l/L 后缀。"""
    text = literal.strip().rstrip("uUlL")
    if text.lower().startswith("0x"):
        return int(text, 16)
    return int(text, 0)


def defines(text):
    """抓 #define NAME value（函数式宏会被跳过）。"""
    out = {}
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n/]+)",
                         text, re.M):
        out[m.group(1)] = m.group(2).strip()
    return out


def define_int(text, name, _table=None, _depth=0):
    """解析 #define 的整数值，支持引用其它宏：LINK_FRAME_MAX_LINE = (MAX_PAYLOAD + 10)。

    只做"引用展开 + 数字求和"，不支持真正的算符优先级——够用就好，
    真需要表达式求值说明常量本身写得过于聪明了。
    """
    table = _table if _table is not None else defines(text)
    value = table.get(name)
    if value is None:
        raise KeyError(name)
    if _depth > 5:
        raise ValueError("宏引用太深，可能有环：%s" % name)

    def _expand(match):
        ident = match.group(0)
        if ident in table:
            return "(%d)" % define_int(text, ident, table, _depth + 1)
        return ident

    expr = re.sub(r"\b[A-Za-z_]\w*\b", _expand, value)
    pieces = re.findall(r"0[xX][0-9A-Fa-f]+[uUlL]*|\d+[uUlL]*", expr)
    if not pieces:
        raise ValueError("无法解析 %s=%s" % (name, value))
    return sum(parse_int(p) for p in pieces)


def _region(xml_name):
    text = read_fw("Project.uvprojx")
    block = re.search(r"<%s>(.*?)</%s>" % (xml_name, xml_name), text, re.S)
    if not block:
        raise AssertionError("Project.uvprojx 里找不到 %s" % xml_name)
    start = parse_int(re.search(r"<StartAddress>\s*([^<]+)</StartAddress>",
                                block.group(1)).group(1))
    size = parse_int(re.search(r"<Size>\s*([^<]+)</Size>", block.group(1)).group(1))
    return start, size


def irom1():
    """应用区（Keil 的 IROM 段，界面上叫 IROM1）。"""
    return _region("IROM")


def iram1():
    return _region("IRAM")


def cfg_defines(text):
    """解析 IAR/Z-Stack 的 -DNAME 与 -DNAME=value（f8wConfig.cfg 用这种写法）。"""
    out = {}
    for m in re.finditer(r"^[ \t]*-D([A-Za-z_]\w*)(?:=([^\s/]+))?", text, re.M):
        out[m.group(1)] = m.group(2) if m.group(2) is not None else "1"
    return out


def cfg_int(text, name):
    table = cfg_defines(text)
    if name not in table:
        raise KeyError(name)
    return parse_int(table[name])
