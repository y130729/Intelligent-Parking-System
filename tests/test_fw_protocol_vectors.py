#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""链路帧的黄金样本 + 随机测试 + C 侧结构对表。

tests/vectors.json 是一份"协议样本"：同一组字节既可以被 Python 编解码器
校验，也可以（将来有 host 编译器时）直接喂给 C 实现。评审者能一眼看到
"帧长什么样"，而不是只看到一堆 assert。

三层检查：

  1. 黄金样本：固定输入必须产生固定字节，改协议就会红；
  2. 随机/变异测试：随便怎么改都不许抛异常、不许把坏帧当成好帧；
  3. 结构对表：C 源码里的帧边界算式（6 + len + 3）和魔数必须还在。

第 3 条是给"已经修过一次的 bug"上的锁：当初 C 侧少算了一个 '#',
导致自己发出去的每一帧都被自己判成坏帧，现象是"串口全哑"。
"""

import json
import os
import random
import re
import unittest

import common
import stm32_protocol as sp


def load_vectors():
    with open(common.VECTORS_JSON, encoding="utf-8") as fh:
        return json.load(fh)


class GoldenVectorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = load_vectors()

    def test_manifest_matches_code(self):
        self.assertEqual(self.doc["version"], sp.FRAME_VER)
        self.assertEqual(self.doc["max_payload"], sp.FRAME_MAX_PAYLOAD)

    def test_readme_counts_the_same_frames(self):
        """README 里写了"6 条帧"。加样本忘了改文档，这里就会红。"""
        self.assertGreaterEqual(len(self.doc["vectors"]), 4,
                                "样本太少，覆盖不到 OPEN/CLOSE/中文/故障")
        readme = common.read(common.README_PATH)
        match = re.search(r"保存了\s*(\d+)\s*条帧", readme)
        self.assertIsNotNone(match, "README 里找不到 '保存了 N 条帧' 这句话")
        self.assertEqual(int(match.group(1)), len(self.doc["vectors"]),
                         "README 写的样本条数和 vectors.json 不一致")

    def test_encode_matches_golden_bytes(self):
        for vec in self.doc["vectors"]:
            frame = sp.encode_frame(vec["seq"], vec["payload"])
            self.assertIsNotNone(frame, vec["name"])
            self.assertEqual(bytes.fromhex(vec["hex"]), frame.encode("latin-1"),
                             "样本 %s 的字节和实现不一致" % vec["name"])

    def test_decode_roundtrip(self):
        for vec in self.doc["vectors"]:
            raw = bytes.fromhex(vec["hex"]).decode("latin-1")
            status, seq, payload = sp.decode_frame(raw)
            self.assertEqual(sp.FRAME_OK, status, vec["name"])
            self.assertEqual(vec["seq"], seq, vec["name"])
            self.assertEqual(vec["payload"], payload, vec["name"])

    def test_header_length_counts_utf8_bytes(self):
        # '京' 在 UTF-8 里是 3 字节；LEN 字段写的是字节数不是字符数
        for vec in self.doc["vectors"]:
            self.assertEqual(vec["len"], len(vec["payload"].encode("utf-8")))
            raw = bytes.fromhex(vec["hex"])
            self.assertEqual(vec["len"], int(raw[4:6], 16), vec["name"])

    def test_frame_is_nine_plus_payload(self):
        for vec in self.doc["vectors"]:
            self.assertEqual(9 + vec["len"], len(bytes.fromhex(vec["hex"])), vec["name"])

    def test_frames_fit_line_budget(self):
        for vec in self.doc["vectors"]:
            self.assertLessEqual(len(bytes.fromhex(vec["hex"])), sp.FRAME_MAX_LINE)


class RobustnessTest(unittest.TestCase):
    def test_single_bit_flip_is_always_rejected(self):
        """任何一位翻转都必须被 CRC 拦住，不能"解出另一个合法命令"。"""
        for vec in load_vectors()["vectors"]:
            raw = bytearray(bytes.fromhex(vec["hex"]))
            body = raw[6:6 + vec["len"]]
            for i in range(len(body)):
                for bit in (0x01, 0x80):
                    mutated = bytearray(raw)
                    mutated[6 + i] ^= bit
                    status, _, _ = sp.decode_frame(mutated.decode("latin-1"))
                    self.assertEqual(sp.FRAME_INVALID, status,
                                     "%s 的第 %d 字节翻位后没被拒绝" % (vec["name"], i))

    def test_random_payloads_roundtrip(self):
        rng = random.Random(20261117)
        alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:"
        for _ in range(2000):
            payload = "".join(rng.choice(alphabet)
                              for _ in range(rng.randint(1, sp.FRAME_MAX_PAYLOAD)))
            frame = sp.encode_frame(rng.randint(0, 0xFF), payload)
            self.assertIsNotNone(frame)
            status, _, decoded = sp.decode_frame(frame)
            self.assertEqual(sp.FRAME_OK, status)
            self.assertEqual(payload, decoded)

    def test_random_garbage_never_raises_and_never_says_ok(self):
        rng = random.Random(42)
        for _ in range(3000):
            blob = bytes(rng.randint(0, 255) for _ in range(rng.randint(0, 40)))
            text = blob.decode("latin-1")
            status, seq, payload = sp.decode_frame(text)   # 不许抛异常
            if status == sp.FRAME_OK:
                # 万一凑巧撞上合法帧，至少要能原样解出来
                self.assertIsNotNone(seq)
                self.assertIsInstance(payload, str)

    def test_truncated_frames_rejected(self):
        raw = bytes.fromhex(load_vectors()["vectors"][0]["hex"]).decode("latin-1")
        for cut in range(1, len(raw)):
            status, _, _ = sp.decode_frame(raw[:cut])
            self.assertNotEqual(sp.FRAME_OK, status, "截断到 %d 字节仍然被判为合法" % cut)


class FirmwareSourceTest(unittest.TestCase):
    """C 侧源码的结构性对表：常量、魔数、帧长算式。"""

    @classmethod
    def setUpClass(cls):
        cls.hdr = common.read_fw("Hardware", "link_frame.h")
        cls.src = common.read_fw("Hardware", "link_frame.c")

    def test_header_matches_python_model(self):
        self.assertEqual(sp.FRAME_VER,
                         common.define_int(self.hdr, "LINK_FRAME_VER"))
        self.assertEqual(sp.FRAME_MAX_PAYLOAD,
                         common.define_int(self.hdr, "LINK_FRAME_MAX_PAYLOAD"))

    def test_magic_bytes_are_hash_and_star(self):
        self.assertIn("'#'", self.src)
        self.assertIn("'*'", self.src)

    def test_crc_polynomial_is_crc8_atm(self):
        self.assertIn("0x07", self.src, "CRC-8 必须是 poly=0x07（CRC-8/ATM）")
        self.assertIn("0x00", self.src, "init 必须是 0x00，才能对上 Python 的 crc8")

    def test_frame_length_arithmetic_covers_the_hash(self):
        """9 = '#' + VER + SEQ(2) + LEN(2) + '*' + CRC(2)。

        这里曾经少算开头的 '#'，C 端起手就把自己发的帧判成坏帧。
        """
        compact = self.src.replace(" ", "")
        self.assertIn("6+len+3", compact,
                      "帧长校验必须写成 6 + len + 3（含开头 '#'），不要改成 5 + len + 3")
        self.assertIn("9", self.src)

    def test_max_line_covers_overhead(self):
        max_line = common.define_int(self.hdr, "LINK_FRAME_MAX_LINE")
        max_payload = common.define_int(self.hdr, "LINK_FRAME_MAX_PAYLOAD")
        self.assertGreaterEqual(max_line, 9 + max_payload)


if __name__ == "__main__":
    unittest.main()
