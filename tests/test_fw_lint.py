#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""静态检查器自身的测试。

一个只会说"我全绿"的检查器没有说服力——读者无法判断它是真在检查，
还是根本没匹配到任何东西。所以这里先用**故意写错的代码片段**验证每条
规则确实能抓到，再对真实工程断言 0 findings。
"""

import os
import unittest

import common
from tools import fw_lint


class RuleSelfTest(unittest.TestCase):
    """喂坏代码，确认每条规则都真的会响。"""

    def lint(self, code):
        return fw_lint.lint_source("<test>", code)

    def codes(self, code):
        return [i.rule for i in self.lint(code)]

    # ---- FW001 -------------------------------------------------------
    def test_fw001_printf_in_isr(self):
        code = (
            "void EXTI0_IRQHandler(void)\n{\n"
            "    USART_Printf(\"edge!\\r\\n\");\n"
            "}\n")
        self.assertIn("FW001", self.codes(code))

    def test_fw001_ok_in_main_loop(self):
        code = "void main_loop(void)\n{\n    USART_Printf(\"ok\");\n}\n"
        self.assertNotIn("FW001", self.codes(code))

    # ---- FW002 -------------------------------------------------------
    def test_fw002_empty_spin_on_register(self):
        code = (
            "void rc522_reset(void)\n{\n"
            "    while (Read_MFRC522(CommandReg) & 0x10);\n"
            "}\n")
        self.assertIn("FW002", self.codes(code))

    def test_fw002_ok_when_bounded(self):
        code = (
            "void rc522_reset(void)\n{\n"
            "    uint16_t guard = 0;\n"
            "    while ((Read_MFRC522(CommandReg) & 0x10) && guard++ < 1000u);\n"
            "}\n")
        self.assertNotIn("FW002", self.codes(code))

    def test_fw002_ok_for_do_while_tail(self):
        code = (
            "void card_wait(void)\n{\n"
            "    uint8_t i = 0xFF;\n"
            "    do\n    {\n        i--;\n    }\n"
            "    while ((i != 0) && !(n & 0x01));\n"
            "}\n")
        self.assertNotIn("FW002", self.codes(code))

    def test_fw002_ok_for_uart_txe(self):
        # TXE/TC 由硬件在 1 个字符时间内自清，不会因为外设故障永久挂住
        code = (
            "void uart_put(uint8_t b)\n{\n"
            "    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);\n"
            "    USART_SendData(USART1, b);\n"
            "}\n")
        self.assertNotIn("FW002", self.codes(code))

    # ---- FW003 -------------------------------------------------------
    def test_fw003_sprintf(self):
        code = (
            "void ack(char *dst)\n{\n"
            "    sprintf(dst, \"ACK:%02X\", 5);\n}\n")
        self.assertIn("FW003", self.codes(code))

    def test_fw003_ok_for_snprintf(self):
        code = (
            "void ack(char *dst, int n)\n{\n"
            "    snprintf(dst, (size_t)n, \"ACK:%02X\", 5);\n}\n")
        self.assertNotIn("FW003", self.codes(code))

    # ---- FW004 -------------------------------------------------------
    def test_fw004_isr_writes_non_volatile_flag(self):
        code = (
            "uint8_t vehicle_arrive_flag;\n"
            "void EXTI15_10_IRQHandler(void)\n{\n"
            "    vehicle_arrive_flag = 1;\n"
            "}\n")
        self.assertIn("FW004", self.codes(code))

    def test_fw004_ok_when_volatile(self):
        code = (
            "volatile uint8_t vehicle_arrive_flag;\n"
            "void EXTI15_10_IRQHandler(void)\n{\n"
            "    vehicle_arrive_flag = 1;\n"
            "}\n")
        self.assertNotIn("FW004", self.codes(code))

    def test_fw004_finds_declaration_in_other_file(self):
        # 变量声明在别的头文件里，也要能对上（真实工程的 vehicle_arrive_flag）
        code = (
            "void EXTI15_10_IRQHandler(void)\n{\n"
            "    vehicle_arrive_flag = 1;\n"
            "}\n")
        index = {"vehicle_arrive_flag": {"line": 1, "volatile": False}}
        issues = fw_lint.lint_source("<test>", code, index)
        self.assertIn("FW004", [i.rule for i in issues])

    # ---- FW005 -------------------------------------------------------
    def test_fw005_flash_write_without_unlock(self):
        code = (
            "void save(void)\n{\n"
            "    FLASH_ErasePage(0x0800F000u);\n"
            "    FLASH_ProgramWord(0x0800F000u, 0u);\n"
            "}\n")
        self.assertIn("FW005", self.codes(code))

    def test_fw005_ok_with_unlock(self):
        code = (
            "void save(void)\n{\n"
            "    FLASH_Unlock();\n"
            "    FLASH_ErasePage(0x0800F000u);\n"
            "    FLASH_Lock();\n"
            "}\n")
        self.assertNotIn("FW005", self.codes(code))

    def test_fw005_ok_when_caller_unlocks(self):
        # 薄封装：解锁在调用者里做，这是本项目 program_words 的写法
        code = (
            "static void program_word(uint32_t a, uint32_t v)\n{\n"
            "    FLASH_ProgramWord(a, v);\n"
            "}\n"
            "void save(void)\n{\n"
            "    FLASH_Unlock();\n"
            "    program_word(0x0800F000u, 0u);\n"
            "    FLASH_Lock();\n"
            "}\n")
        self.assertNotIn("FW005", self.codes(code))

    # ---- 通用机制 -----------------------------------------------------
    def test_comments_and_strings_are_ignored(self):
        code = (
            "void f(void)\n{\n"
            "    /* while (FLAG); 注释里的不算 */\n"
            "    printf(\"while (FLAG);\");\n"
            "}\n")
        self.assertNotIn("FW002", self.codes(code))

    def test_suppression_needs_reason(self):
        code = (
            "void f(void)\n{\n"
            "    /* lint:disable FW003 已知定长输出 */\n"
            "    sprintf(dst, \"%d\", 1);\n"
            "}\n")
        self.assertNotIn("FW003", self.codes(code))

    def test_suppression_is_scoped(self):
        # 豁免只影响紧邻的那一处，不会把整个文件关掉
        code = (
            "void f(void)\n{\n"
            "    /* lint:disable FW003 已知定长输出 */\n"
            "    sprintf(a, \"%d\", 1);\n"
            "    sprintf(b, \"%d\", 2);\n"
            "    sprintf(c, \"%d\", 3);\n"
            "}\n")
        self.assertEqual(self.codes(code).count("FW003"), 2)


class RealTreeTest(unittest.TestCase):
    """真实工程必须 0 findings。"""

    def test_repository_is_clean(self):
        issues = fw_lint.lint_paths(fw_lint.DEFAULT_TARGETS)
        detail = "\n".join(str(i) for i in issues)
        self.assertEqual([], [str(i) for i in issues], "静态检查有发现：\n" + detail)

    def test_every_rule_is_documented(self):
        codes = {i.rule for i in fw_lint.lint_paths(fw_lint.DEFAULT_TARGETS)}
        self.assertTrue(codes.issubset(set(fw_lint.RULES)))

    def test_scanner_actually_sees_our_sources(self):
        """防止"规则写了但一个文件都没扫到"这种假绿。"""
        files = list(fw_lint.iter_sources(fw_lint.DEFAULT_TARGETS))
        names = {os.path.basename(f) for f in files}
        self.assertGreaterEqual(len(files), 20, "只扫到 %d 个文件，范围配置可能错了" % len(files))
        for expected in ("state_machine.c", "flash_storage.c", "link.c",
                         "pi_link.c", "Serial.c", "MFRC522.c", "ParkingNode.c"):
            self.assertIn(expected, names)
        self.assertFalse(any(os.sep + "Components" + os.sep in f for f in files),
                         "Z-Stack / 标准库的厂商代码不应进入扫描范围（会淹没真问题）")

    def test_targets_exist(self):
        for target in fw_lint.DEFAULT_TARGETS:
            self.assertTrue(os.path.isdir(target), target)


if __name__ == "__main__":
    unittest.main()
