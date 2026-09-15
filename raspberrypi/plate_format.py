#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""中国车牌结构规则。

本模块只做纯字符串判断，不依赖图像库/OCR，因此可以直接单元测试
（见 tests/test_plate_format.py）。

背景：OCR 白名单是 A-Z0-9，省份简称（汉字）不参与识别，所以进入本模块的
字符串是"发牌机关代号字母 + 序号"，共 6 位（燃油）或 7 位（新能源）：

    燃油        X 12345            机构字母 + 5 位序号
    新能源小型  X D12345 / X F12345  序号首位为 D/F
    新能源大型  X 12345D / X 12345F  序号末位为 D/F

判定原则：
  * 只使用"结构合法 + 误读修复"，不使用针对某一块测试车牌的字符替换表；
  * I、O 在中国车牌中一律不出现（易与 1、0 混淆），统一修复为 1、0；
  * 第 0 位必须是字母（发牌机关代号）。若 OCR 在此位给出数字，本模块不会
    猜测它对应哪个字母——那属于"候选车牌消歧"，由车辆库匹配
    （plate_matcher）结合库内车牌决定，避免凭空改字造成误判。
"""

import re

# 发牌机关代号可用字母：A-Z 去掉 I、O
LETTERS = set("ABCDEFGHJKLMNPQRSTUVWXYZ")
DIGITS = set("0123456789")
# 序号可用字符：数字 + 字母（同样不含 I、O）
ALNUM = DIGITS | LETTERS

# 新能源号牌标识字母
NEW_ENERGY_MARKS = ("D", "F")

# 合法长度：燃油 6 位，新能源 7 位
VALID_LENGTHS = (6, 7)

# 车牌中不存在的字符 -> 最接近的合法字符（唯一保留的"修复"规则，
# 因为 I/O 在车牌上确实不允许出现，属于确定性修复而非猜测）
ILLEGAL_REPAIR = {"I": "1", "O": "0"}

# 可选的兜底替换表（默认空）。只在"找不到任何结构合法窗口"时才应用，
# 用于吸收某台相机/字体的固定误读（例如 {"1": "T"}）。默认空表示
# 交由车辆库匹配来消歧。
FALLBACK_SUBSTITUTIONS = {}


def set_fallback_substitutions(mapping):
    """配置兜底替换表（可选）。mapping: {误读字符: 期望字符}。"""
    global FALLBACK_SUBSTITUTIONS
    FALLBACK_SUBSTITUTIONS = dict(mapping or {})


def normalize(text):
    """去掉所有非字母数字字符并转大写。"""
    return re.sub(r"[^A-Za-z0-9]", "", text or "").upper()


def repair(text):
    """应用确定性误读修复（I->1、O->0）。"""
    return "".join(ILLEGAL_REPAIR.get(ch, ch) for ch in text)


def apply_fallback(text):
    """应用可选的兜底替换表。"""
    return "".join(FALLBACK_SUBSTITUTIONS.get(ch, ch) for ch in text)


def is_new_energy(plate):
    """是否为（结构上的）新能源号牌：7 位且首位或末位序号为 D/F。"""
    if len(plate) != 7:
        return False
    return plate[1] in NEW_ENERGY_MARKS or plate[6] in NEW_ENERGY_MARKS


def is_valid(plate):
    """结构是否合法（不含省份汉字）。"""
    if len(plate) not in VALID_LENGTHS:
        return False
    if plate[0] not in LETTERS:
        return False
    return all(ch in ALNUM for ch in plate[1:])


def score(plate):
    """结构打分：越高越像一块中国车牌。不做任何具体字符的加权。"""
    if not plate:
        return -99
    sc = 0
    if len(plate) in VALID_LENGTHS:
        sc += 2
    else:
        sc -= 4
    sc += 3 if plate[0] in LETTERS else -3
    rest = plate[1:]
    if rest.isdigit():
        sc += 2                      # 纯数字序号最常见
    if len(plate) == 7:
        sc += 2 if is_new_energy(plate) else -2
    if any(ch in ("I", "O") for ch in plate):
        sc -= 2                      # 车牌不含 I/O
    return sc


def windows(text):
    """产出所有候选窗口（只产出合法长度，避免无谓枚举）。"""
    s = normalize(text)
    for ln in VALID_LENGTHS:
        if len(s) < ln:
            continue
        for i in range(0, len(s) - ln + 1):
            yield s[i:i + ln]


def post_process(text):
    """OCR 文本 -> 最可能的 6/7 位车牌串（不含省份汉字）。

    流程：归一化 -> 逐窗口修复 -> 结构打分选优。
    """
    s = normalize(text)
    if not s:
        return ""

    best_plate = ""
    best_key = None
    for win in windows(s):
        cand = repair(win)
        key = (
            1 if is_valid(cand) else 0,          # 结构合法优先
            score(cand),                          # 结构分
            len(cand),                            # 同分时取更完整的窗口（7 位优于 6 位）
        )
        if best_key is None or key > best_key:
            best_key, best_plate = key, cand

    if best_key is not None and best_key[0]:
        return best_plate

    # 没有任何结构合法窗口：退回兜底替换表（默认空），再退回整串修复结果
    fallback = apply_fallback(s)
    if fallback != s:
        for win in windows(fallback):
            cand = repair(win)
            if is_valid(cand):
                return cand

    if len(s) <= max(VALID_LENGTHS):
        return repair(s)
    return best_plate or repair(s)
