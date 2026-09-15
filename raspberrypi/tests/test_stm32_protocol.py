#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""STM32 <-> 树莓派 行协议解析/构造单元测试。"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import stm32_protocol as proto


class TestParseLine(unittest.TestCase):
    def test_spots(self):
        self.assertEqual(proto.parse_line("SPOTS:17\r\n"),
                         {"kind": "spots", "remaining": 17})

    def test_spots_bad_payload(self):
        self.assertEqual(proto.parse_line("SPOTS:abc")["kind"], "unknown")

    def test_spot_occupied_and_free(self):
        self.assertEqual(proto.parse_line("SPOT:3:1"),
                         {"kind": "spot", "spot_id": 3, "occupied": True})
        self.assertEqual(proto.parse_line("SPOT:12:0"),
                         {"kind": "spot", "spot_id": 12, "occupied": False})

    def test_spot_accepts_word_form(self):
        self.assertEqual(proto.parse_line("SPOT:3:occupied")["occupied"], True)
        self.assertEqual(proto.parse_line("SPOT:3:empty")["occupied"], False)

    def test_spot_malformed(self):
        self.assertEqual(proto.parse_line("SPOT:3")["kind"], "unknown")
        self.assertEqual(proto.parse_line("SPOT:x:1")["kind"], "unknown")
        self.assertEqual(proto.parse_line("SPOT:3:maybe")["kind"], "unknown")

    def test_card_is_uppercased(self):
        self.assertEqual(proto.parse_line("CARD:7506d506"),
                         {"kind": "card", "card": "7506D506"})

    def test_barrier_with_reason(self):
        self.assertEqual(proto.parse_line("BARRIER:OPEN:card"),
                         {"kind": "barrier", "state": "up", "reason": "card"})
        self.assertEqual(proto.parse_line("BARRIER:OPEN:plate"),
                         {"kind": "barrier", "state": "up", "reason": "plate"})
        self.assertEqual(proto.parse_line("BARRIER:OPEN:remote"),
                         {"kind": "barrier", "state": "up", "reason": "remote"})
        self.assertEqual(proto.parse_line("BARRIER:CLOSE"),
                         {"kind": "barrier", "state": "down", "reason": ""})

    def test_barrier_legacy_without_reason(self):
        """老固件只发 BARRIER:OPEN，必须继续兼容。"""
        self.assertEqual(proto.parse_line("BARRIER:OPEN"),
                         {"kind": "barrier", "state": "up", "reason": ""})

    def test_barrier_unknown_reason_is_cleared(self):
        self.assertEqual(proto.parse_line("BARRIER:OPEN:bogus")["reason"], "")
        self.assertEqual(proto.parse_line("BARRIER:WHATEVER")["kind"], "unknown")

    def test_link_and_capture(self):
        self.assertEqual(proto.parse_line("LINK:OK")["kind"], "link_ok")
        self.assertEqual(proto.parse_line("CAPTURE")["kind"], "capture")

    def test_whitespace_and_blank(self):
        self.assertEqual(proto.parse_line("  CAPTURE \r\n")["kind"], "capture")
        self.assertEqual(proto.parse_line("")["kind"], "empty")
        self.assertEqual(proto.parse_line(None)["kind"], "empty")

    def test_unknown_line(self):
        self.assertEqual(proto.parse_line("WiFi: hello")["kind"], "unknown")


class TestBuilders(unittest.TestCase):
    def test_open_close_carry_reason(self):
        self.assertEqual(proto.build_open("plate"), "OPEN:plate")
        self.assertEqual(proto.build_open("remote"), "OPEN:remote")
        self.assertEqual(proto.build_close("remote"), "CLOSE:remote")

    def test_plate_builder(self):
        self.assertEqual(proto.build_plate("A12345"), "PLATE:A12345")
        self.assertEqual(proto.build_plate(""), "PLATE:NONE")
        self.assertEqual(proto.build_plate(None), "PLATE:NONE")

    def test_roundtrip_with_parse(self):
        out = proto.parse_line(proto.build_open("card"))
        self.assertEqual(out, {"kind": "unknown", "raw": "OPEN:card"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
