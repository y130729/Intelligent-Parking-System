#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""中国车牌结构规则单元测试（纯逻辑，不需要相机/OCR）。

运行：
    python -m unittest discover -s raspberrypi/tests -t raspberrypi
或：
    python raspberrypi/tests/test_plate_format.py
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import plate_format as pf


class TestNormalizeAndRepair(unittest.TestCase):
    def test_normalize_strips_separators_and_case(self):
        self.assertEqual(pf.normalize("冀 T234J5"), "T234J5")
        self.assertEqual(pf.normalize("  t234j5 "), "T234J5")
        self.assertEqual(pf.normalize("京A·12345"), "A12345")
        self.assertEqual(pf.normalize(None), "")

    def test_repair_removes_illegal_letters(self):
        self.assertEqual(pf.repair("T2I4J5"), "T214J5")
        self.assertEqual(pf.repair("TO3456"), "T03456")
        self.assertEqual(pf.repair("T234J5"), "T234J5")


class TestValidity(unittest.TestCase):
    def test_accepts_structural_plates(self):
        self.assertTrue(pf.is_valid("T234J5"))
        self.assertTrue(pf.is_valid("A12345"))
        self.assertTrue(pf.is_valid("TD12345"))

    def test_rejects_bad_structure(self):
        self.assertFalse(pf.is_valid("1234J5"))    # 第 0 位必须是字母
        self.assertFalse(pf.is_valid("T234J"))     # 长度不足
        self.assertFalse(pf.is_valid("T23IJ5"))    # I 不出现在车牌中
        self.assertFalse(pf.is_valid(""))
        self.assertFalse(pf.is_valid("T2345678"))

    def test_new_energy_marker(self):
        self.assertTrue(pf.is_new_energy("TD12345"))
        self.assertTrue(pf.is_new_energy("T12345D"))
        self.assertTrue(pf.is_new_energy("TF12345"))
        self.assertFalse(pf.is_new_energy("T12345J"))
        self.assertFalse(pf.is_new_energy("T234J5"))

    def test_score_prefers_structural_plates(self):
        self.assertGreater(pf.score("T234J5"), pf.score("1234J5"))
        self.assertGreater(pf.score("TD12345"), pf.score("T12345J"))


class TestPostProcess(unittest.TestCase):
    def tearDown(self):
        pf.set_fallback_substitutions({})

    def test_clean_input_passthrough(self):
        self.assertEqual(pf.post_process("T234J5"), "T234J5")
        self.assertEqual(pf.post_process("冀T234J5"), "T234J5")
        self.assertEqual(pf.post_process("京A·12345"), "A12345")
        self.assertEqual(pf.post_process(""), "")

    def test_repairs_illegal_letters(self):
        self.assertEqual(pf.post_process("T2I4J5"), "T214J5")

    def test_drops_leading_noise(self):
        self.assertEqual(pf.post_process("8T234J5"), "T234J5")

    def test_new_energy_kept_in_full(self):
        self.assertEqual(pf.post_process("TD12345"), "TD12345")

    def test_no_longer_forces_z_substitution(self):
        """回归：旧实现把序号中的 7 强改成 Z，会改坏合法车牌。"""
        self.assertEqual(pf.post_process("T27345"), "T27345")
        self.assertEqual(pf.post_process("T234J5"), "T234J5")

    def test_does_not_invent_letters_for_position_zero(self):
        """回归：旧实现把首位的 1/7/8 强改成 T，属于凭空猜字。"""
        self.assertEqual(pf.post_process("123456"), "123456")

    def test_fallback_table_is_opt_in(self):
        self.assertEqual(pf.post_process("1234J5"), "1234J5")
        pf.set_fallback_substitutions({"1": "T"})
        result = pf.post_process("1234J5")
        self.assertTrue(pf.is_valid(result))
        self.assertTrue(result.startswith("T"))

    def test_picks_best_window_from_noisy_string(self):
        self.assertEqual(pf.post_process("XYZTD12345ABC"), "TD12345")


if __name__ == "__main__":
    unittest.main(verbosity=2)
