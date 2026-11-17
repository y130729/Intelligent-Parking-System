#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""固件静态检查（无编译器、无硬件、零依赖）。

为什么自己写而不用 cppcheck/clang-tidy：检查要能在任何一台机器上直接跑
起来。要求"一条命令、不装任何东西"比"规则更多"更重要。规则只挑两类：

  1. 这个项目真实踩过的坑（ISR 里 printf、无出口的 while、没解锁的 Flash、
     ISR 共享变量没 volatile、sprintf 打到定长缓冲）；
  2. 换个 MCU/换个人重写也一定会再踩的坑。

所以这里不是通用 C 检查器，而是"项目规约的自动化版本"。每条规则都对应
README 或代码注释里记录过的故障模式，报错信息里带出处，读者能看懂为什么。

用法：
  python tools/fw_lint.py                # 扫默认目录
  python tools/fw_lint.py path1 path2    # 扫指定目录/文件
  python tools/fw_lint.py --json         # 机器可读（CI 存 artifact 用）
  python tools/fw_lint.py --list-rules   # 列出规则

在代码里豁免某条规则（必须写理由）：
  /* lint:disable FW005 由调用者统一解锁 */
豁免只作用于"下一行"或同一行，不会悄悄全局生效。
"""

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 只扫"我们写的"代码：Z-Stack / 标准外设库那些厂商代码扫了也是几百条噪音。
DEFAULT_TARGETS = [
    os.path.join(REPO_ROOT, "STM32_Gateway", "Hardware"),
    os.path.join(REPO_ROOT, "STM32_Gateway", "User"),
    os.path.join(REPO_ROOT, "STM32_Gateway", "System"),
    os.path.join(REPO_ROOT, "CC2530_ParkingNode", "Projects", "zstack",
                 "cc2530_ParkingNode", "ParkingNode", "Source"),
]

RULES = {
    "FW001": "中断服务函数里调用阻塞或不可重入的函数",
    "FW002": "寄存器/标志轮询写成 while (cond); 没有超时（UART TXE/TC 除外，由硬件保证在 1 个字符时间内自清）",
    "FW003": "使用无长度限制的字符串函数（sprintf/strcpy/strcat/gets）",
    "FW004": "ISR 里修改的全局变量没有 volatile",
    "FW005": "擦写 Flash 的函数没有 FLASH_Unlock()（F1 复位后 Flash 是锁定的）",
}

ISR_RE = re.compile(r"^[A-Za-z_]\w*_(?:IRQ)?Handler$")

# 在中断里会长时间阻塞或者会重入同一块静态缓冲的调用
BLOCKING_CALLS = (
    "USART_Printf", "Serial_Printf", "printf", "sprintf", "vsprintf",
    "delay_ms", "Delay_ms", "HAL_Delay", "malloc", "calloc", "free",
)

FLASH_WRITE_CALLS = ("FLASH_ErasePage", "FLASH_ProgramWord",
                     "FLASH_ProgramHalfWord", "FLASH_ProgramOptionByteData")

UNSAFE_STRING_FUNCS = ("sprintf", "vsprintf", "strcpy", "strcat", "gets")

DECL_TYPES = (
    r"void|char|short|int|long|float|double|unsigned|signed|"
    r"u?int8_t|u?int16_t|u?int32_t|uint8|uint16|uint32|BOOL|bool|size_t"
)

FUNC_RE = re.compile(
    r"^[ \t]*(?:static[ \t]+|inline[ \t]+|extern[ \t]+)*"
    r"(?:const[ \t]+|volatile[ \t]+)*"
    r"(?:" + DECL_TYPES + r")\b[^;{}()]*?"
    r"\b([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t\r\n]*\{",
    re.M,
)

DECL_RE = re.compile(
    r"^(?:static[ \t]+|extern[ \t]+)*(?P<vol>volatile[ \t]+)?"
    r"(?:const[ \t]+)?(?:" + DECL_TYPES + r")\b"
    r"(?P<rest>[^;(){}]*);[ \t]*$",
    re.M,
)


class Issue(object):
    __slots__ = ("rule", "path", "line", "message")

    def __init__(self, rule, path, line, message):
        self.rule = rule
        self.path = path
        self.line = line
        self.message = message

    def as_dict(self):
        return {"rule": self.rule, "file": self.path, "line": self.line,
                "message": self.message}

    def __str__(self):
        rel = os.path.relpath(self.path, REPO_ROOT) if os.path.isabs(self.path) else self.path
        return "%s:%d: %s %s" % (rel, self.line, self.rule, self.message)


def _blank(match):
    """把注释/字符串替换成等长空白，保留换行，保证行号不变。"""
    text = match.group(0)
    return "".join("\n" if ch == "\n" else " " for ch in text)


def strip_comments(text):
    """去掉注释与字符串字面量的内容，但保持长度和行号。

    必须保留长度：后面要用同一套下标去原文里取行号。字符串也要抹掉，
    否则 printf("while(1);") 会被当成真代码。
    """
    pattern = re.compile(
        r"/\*.*?\*/"          # 块注释
        r"|//[^\n]*"          # 行注释
        r'|"(?:\\.|[^"\\\n])*"'   # 字符串
        r"|'(?:\\.|[^'\\\n])*'",  # 字符
        re.S,
    )
    return pattern.sub(_blank, text)


def _match_paren(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _match_brace_backwards(text, close_idx):
    depth = 0
    for i in range(close_idx, -1, -1):
        ch = text[i]
        if ch == "}":
            depth += 1
        elif ch == "{":
            depth -= 1
            if depth == 0:
                return i
    return -1


def _is_do_while_tail(text, while_pos):
    """'while' 前面紧挨着的 '}' 如果属于 do{}，那这条 while 是循环尾巴，不是空转。"""
    i = while_pos - 1
    while i >= 0 and text[i] in " \t\r\n":
        i -= 1
    if i < 0 or text[i] != "}":
        return False
    open_idx = _match_brace_backwards(text, i)
    if open_idx < 0:
        return False
    head = text[:open_idx].rstrip()
    return head.endswith("do")


def _match_brace(text, open_idx):
    depth = 0
    for i in range(open_idx, len(text)):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(text) - 1


def find_functions(stripped):
    out = []
    for m in FUNC_RE.finditer(stripped):
        brace = stripped.find("{", m.end() - 1)
        if brace < 0:
            continue
        end = _match_brace(stripped, brace)
        out.append({
            "name": m.group(1),
            "start": m.start(),
            "body_start": brace,
            "body_end": end,
            "line": stripped.count("\n", 0, m.start()) + 1,
        })
    return out


def find_file_scope_vars(stripped):
    """文件作用域变量：必须顶格声明（缩进的都是局部变量）。"""
    out = {}
    for m in DECL_RE.finditer(stripped):
        line = stripped.count("\n", 0, m.start()) + 1
        rest = m.group("rest")
        for ident in re.finditer(r"([A-Za-z_]\w*)[ \t]*(?:\[|=|,|$)", rest):
            out[ident.group(1)] = {"line": line, "volatile": bool(m.group("vol"))}
    return out


def _suppressed(original, rule, pos):
    """看同一行或上一行有没有 lint:disable。

    必须在**未抹掉注释**的原文里找：strip_comments 会把指令本身一起抹掉。
    """
    start = original.rfind("\n", 0, max(0, pos - 1))
    window_start = original.rfind("\n", 0, max(0, start - 1))
    chunk = original[max(0, window_start):pos + 1]
    for token in re.findall(r"lint:disable\s+([A-Z0-9, ]+)", chunk):
        codes = [c.strip() for c in token.replace(",", " ").split()]
        if rule in codes or "all" in codes:
            return True
    return False


def _line_of(stripped, pos):
    return stripped.count("\n", 0, pos) + 1


def lint_source(path, text, decl_index=None):
    """对单个文件的源码做检查，返回 Issue 列表。纯函数，测试可以喂字符串。"""
    stripped = strip_comments(text)
    issues = []
    funcs = find_functions(stripped)
    decls = find_file_scope_vars(stripped)

    # ---- FW001: 中断里调阻塞/不可重入函数 ------------------------------
    for fn in funcs:
        if not ISR_RE.match(fn["name"]):
            continue
        body = stripped[fn["body_start"]:fn["body_end"]]
        for call in BLOCKING_CALLS:
            hit = re.search(r"\b%s\s*\(" % re.escape(call), body)
            if not hit:
                continue
            pos = fn["body_start"] + hit.start()
            if _suppressed(text, "FW001", pos):
                continue
            issues.append(Issue(
                "FW001", path, _line_of(stripped, pos),
                "中断 %s 里调用了 %s()；ISR 只置标志/入队，打印和延时放回主循环"
                % (fn["name"], call)))

    # ---- FW002: while (寄存器); 无超时 ---------------------------------
    for m in re.finditer(r"\bwhile\s*\(", stripped):
        open_idx = m.end() - 1
        close_idx = _match_paren(stripped, open_idx)
        if close_idx < 0:
            continue
        if not re.match(r"\s*;", stripped[close_idx + 1:close_idx + 8]):
            continue                      # 有循环体，不是空转
        if _is_do_while_tail(stripped, m.start()):
            continue                      # do { ... } while (cond); 正常写法
        cond = stripped[open_idx + 1:close_idx].strip()
        if cond in ("0", "1", "true", "false"):
            continue                      # while(1); 是显式的死等，按设计意图保留
        if "++" in cond or "--" in cond:
            continue                      # 计数器写在条件里，如 while (x && n++ < 1000);
        if "USART_FLAG_TXE" in cond or "USART_FLAG_TC" in cond:
            continue                      # 发送寄存器空/发送完成：硬件保证 1 字符内自清
        if _suppressed(text, "FW002", m.start()):
            continue
        issues.append(Issue(
            "FW002", path, _line_of(stripped, m.start()),
            "while (%s); 没有超时；外设应答类等待必须带计数上限（MFRC522 虚焊"
            "会把主循环永久卡死）" % cond[:40]))

    # ---- FW003: 无长度限制的字符串函数 ---------------------------------
    for m in re.finditer(r"\b(%s)\s*\(" % "|".join(UNSAFE_STRING_FUNCS), stripped):
        if _suppressed(text, "FW003", m.start()):
            continue
        issues.append(Issue(
            "FW003", path, _line_of(stripped, m.start()),
            "%s() 没有长度上限；远程/串口来的字符串请用 snprintf" % m.group(1)))

    # ---- FW004: ISR 写全局但没有 volatile ------------------------------
    lookup = dict(decls)
    if decl_index:
        for name, info in decl_index.items():
            lookup.setdefault(name, info)
    for fn in funcs:
        if not ISR_RE.match(fn["name"]):
            continue
        body = stripped[fn["body_start"]:fn["body_end"]]
        for m in re.finditer(r"\b([A-Za-z_]\w*)[ \t]*(?:=(?!=)|\+\+|--|\|=|&=|\+=|-=)",
                             body):
            name = m.group(1)
            info = lookup.get(name)
            if not info or info["volatile"]:
                continue
            pos = fn["body_start"] + m.start()
            if _suppressed(text, "FW004", pos):
                continue
            issues.append(Issue(
                "FW004", path, _line_of(stripped, pos),
                "中断 %s 里写 %s，但声明处没有 volatile；编译器会把主循环里的读"
                "提到循环外，标志永远看不到" % (fn["name"], name)))

    # ---- FW005: 擦写 Flash 没解锁 --------------------------------------
    unlock_funcs = set()
    write_funcs = []
    for fn in funcs:
        body = stripped[fn["body_start"]:fn["body_end"]]
        if "FLASH_Unlock" in body:
            unlock_funcs.add(fn["name"])
        if any("%s(" % c in body for c in FLASH_WRITE_CALLS):
            write_funcs.append(fn)
    for fn in write_funcs:
        if fn["name"] in unlock_funcs:
            continue
        # 允许"薄封装"：调用者已经解锁也算合规（program_words 就是这种）
        callers_ok = any(
            fn["name"] + "(" in stripped[g["body_start"]:g["body_end"]]
            and g["name"] in unlock_funcs
            for g in funcs)
        if callers_ok:
            continue
        if _suppressed(text, "FW005", fn["start"]):
            continue
        issues.append(Issue(
            "FW005", path, fn["line"],
            "%s() 里擦/写 Flash 但没有 FLASH_Unlock()；F1 复位后 FLASH_CR 是锁"
            "定的，写操作会直接返回错误（不写坏，但永远写不进去）" % fn["name"]))

    return issues


def iter_sources(paths):
    for path in paths:
        if os.path.isfile(path):
            if path.endswith((".c", ".h")):
                yield path
            continue
        for dirpath, dirnames, filenames in os.walk(path):
            dirnames[:] = [d for d in dirnames if d not in (".git", "Obj", "List", "Exe")]
            for name in sorted(filenames):
                if name.endswith(".c"):
                    yield os.path.join(dirpath, name)


def build_decl_index(paths):
    """跨文件建一张"变量 -> 是否 volatile"的表。

    典型场景：vehicle_arrive_flag 声明在 state_machine.h，却在 main.c 的
    EXTI 中断里被写。只看单个 .c 是查不出来的。
    """
    index = {}
    for path in iter_sources(paths):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                stripped = strip_comments(fh.read())
        except OSError:
            continue
        for name, info in find_file_scope_vars(stripped).items():
            index.setdefault(name, info)
            if info["volatile"]:
                index[name] = info
    return index


def lint_paths(paths, decl_index=None):
    if decl_index is None:
        decl_index = build_decl_index(paths)
    issues = []
    for path in iter_sources(paths):
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
        except OSError as exc:
            issues.append(Issue("FW000", path, 0, "读不到文件：%s" % exc))
            continue
        issues.extend(lint_source(path, text, decl_index))
    return issues


def main(argv=None):
    parser = argparse.ArgumentParser(description="固件静态检查（零依赖）")
    parser.add_argument("paths", nargs="*", help="要扫描的目录或 .c/.h 文件")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    parser.add_argument("--output", metavar="PATH", help="把结果写入文件（与 --json 搭配，避免依赖 shell 重定向编码）")
    parser.add_argument("--list-rules", action="store_true", help="列出规则")
    parser.add_argument("--quiet", action="store_true", help="只在有发现时输出")
    args = parser.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    if args.list_rules:
        for code in sorted(RULES):
            print("%s  %s" % (code, RULES[code]))
        return 0

    targets = args.paths or DEFAULT_TARGETS
    for target in targets:
        if not os.path.exists(target):
            sys.stderr.write("路径不存在：%s\n" % target)
            return 2

    issues = lint_paths(targets)

    if args.json:
        payload = json.dumps([i.as_dict() for i in issues], ensure_ascii=False, indent=2)
        if args.output:
            with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(payload + "\n")
        else:
            print(payload)
    else:
        for issue in issues:
            print(issue)
        if issues:
            print("\n共 %d 条：%s" % (
                len(issues),
                "、".join(sorted({i.rule for i in issues}))))
        elif not args.quiet:
            print("fw_lint: 0 findings（%d 条规则）" % len(RULES))

    return 1 if issues else 0


if __name__ == "__main__":
    sys.exit(main())
