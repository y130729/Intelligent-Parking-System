#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""车牌库匹配单元测试（纯逻辑）。"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import plate_matcher as pm
import plate_format as pf

DB = [
    {"plate": "冀T234J5", "cards": ["7506D506"], "owner": "张三"},
    {"plate": "鄂EM456U", "cards": [], "owner": "李四"},
    {"plate": "湘K4Z567", "cards": [], "owner": "王五"},
]


class TestMatcher(unittest.TestCase):
    def test_exact_match(self):
        full, score, hit = pm.fix_plate("T234J5", DB)
        self.assertTrue(hit)
        self.assertEqual(full, "冀T234J5")
        self.assertGreaterEqual(score, pm.MATCH_THRESHOLD)

    def test_single_char_ocr_error_still_matches(self):
        full, score, hit = pm.fix_plate("T23475", DB)   # J -> 7
        self.assertTrue(hit)
        self.assertEqual(full, "冀T234J5")

    def test_unknown_plate_does_not_match(self):
        full, score, hit = pm.fix_plate("A99999", DB)
        self.assertFalse(hit)

    def test_pipeline_from_raw_ocr_text(self):
        """plate_format -> plate_matcher 的完整链路（无需相机）。"""
        for raw in ("T234J5", "T2J4J5", "T23475", "8T234J5"):
            fixed = pf.post_process(raw)
            full, score, hit = pm.fix_plate(fixed, DB)
            self.assertTrue(hit, "raw=%s fixed=%s" % (raw, fixed))
            self.assertEqual(full, "冀T234J5")

    def test_pipeline_rejects_unregistered_plate(self):
        fixed = pf.post_process("A12345")
        full, score, hit = pm.fix_plate(fixed, DB)
        self.assertFalse(hit)
        self.assertEqual(full, "A12345")


if __name__ == "__main__":
    unittest.main(verbosity=2)
