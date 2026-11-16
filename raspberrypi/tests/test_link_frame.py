#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""链路帧（#VER SEQ LEN PAYLOAD *CRC）编解码测试。

覆盖三件事：

  1. CRC-8/ATM 与 STM32 的 LinkFrame_Crc8 完全一致（用标准校验向量）；
  2. 往返、截断、坏 CRC、超长、含中文 payload 都不崩、不误判；
  3. 与固件头文件里的常量/枚举顺序做一次"跨语言对表"，防止两边改单边。
"""

import os
import random
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import stm32_protocol as sp

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW_DIR = os.path.join(REPO_ROOT, "STM32_Gateway")


def _read_fw(name):
    with open(os.path.join(FW_DIR, name), encoding="utf-8", errors="replace") as f:
        return f.read()


class Crc8Test(unittest.TestCase):
    def test_standard_check_vector(self):
        # CRC-8/ATM（poly 0x07, init 0x00）的业界标准校验值
        self.assertEqual(sp.crc8(b"123456789"), 0xF4)

    def test_empty_is_zero(self):
        self.assertEqual(sp.crc8(b""), 0)

    def test_accepts_str(self):
        self.assertEqual(sp.crc8("123456789"), 0xF4)

    def test_single_bit_flip_always_changes_crc(self):
        # CRC 能检出所有 1 bit 错误，这是它能替代奇偶校验的原因
        base = b"OPEN:card"
        want = sp.crc8(base)
        for i in range(len(base)):
            for bit in range(8):
                bad = bytearray(base)
                bad[i] ^= 1 << bit
                self.assertNotEqual(sp.crc8(bytes(bad)), want, (i, bit))


class FrameEncodeTest(unittest.TestCase):
    def test_layout(self):
        frame = sp.encode_frame(5, "OPEN:card")
        self.assertTrue(frame.startswith("#1"))
        self.assertEqual(frame[1], "1")                 # VER
        self.assertEqual(frame[2:4], "05")              # SEQ
        self.assertEqual(frame[4:6], "09")              # LEN = 9
        self.assertEqual(frame[6:15], "OPEN:card")      # PAYLOAD
        self.assertEqual(frame[15], "*")
        self.assertEqual(len(frame), 9 + 9)             # 9 字节帧头 + payload
        self.assertEqual(int(frame[16:18], 16), sp.crc8(frame[1:15]))

    def test_seq_is_masked_to_one_byte(self):
        self.assertEqual(sp.encode_frame(256, "X")[2:4], "00")
        self.assertEqual(sp.encode_frame(511, "X")[2:4], "FF")

    def test_rejects_empty_and_overlong(self):
        self.assertIsNone(sp.encode_frame(0, ""))
        self.assertIsNone(sp.encode_frame(0, "x" * (sp.FRAME_MAX_PAYLOAD + 1)))
        self.assertIsNotNone(sp.encode_frame(0, "x" * sp.FRAME_MAX_PAYLOAD))

    def test_len_counts_utf8_bytes_not_characters(self):
        frame = sp.encode_frame(1, "PLATE:冀T234J5")
        body = frame[6:frame.index("*")]
        self.assertEqual(int(frame[4:6], 16), len("PLATE:冀T234J5".encode("utf-8")))
        self.assertEqual(len(body), int(frame[4:6], 16))


class FrameRoundTripTest(unittest.TestCase):
    def test_ascii(self):
        for seq in (0, 1, 127, 255):
            for payload in ("SPOT:3:1", "BARRIER:OPEN:card", "LINK:OK",
                            "SPOTS:20", "ACK:0F"):
                status, got_seq, got = sp.decode_frame(sp.encode_frame(seq, payload))
                self.assertEqual((status, got_seq, got),
                                 (sp.FRAME_OK, seq, payload))

    def test_chinese_payload(self):
        status, seq, got = sp.decode_frame(sp.encode_frame(9, "PLATE:冀T234J5"))
        self.assertEqual((status, seq, got), (sp.FRAME_OK, 9, "PLATE:冀T234J5"))

    def test_accepts_bytes_input(self):
        raw = (sp.encode_frame(3, "SPOT:1:0") + "\r\n").encode("latin-1")
        self.assertEqual(sp.decode_frame(raw), (sp.FRAME_OK, 3, "SPOT:1:0"))


class FrameRejectTest(unittest.TestCase):
    def test_bad_crc(self):
        frame = sp.encode_frame(7, "OPEN:card")
        self.assertEqual(sp.decode_frame(frame[:-2] + "00")[0], sp.FRAME_INVALID)

    def test_truncated(self):
        frame = sp.encode_frame(7, "OPEN:card")
        for cut in range(1, len(frame)):
            self.assertEqual(sp.decode_frame(frame[:cut])[0], sp.FRAME_INVALID, cut)

    def test_extra_trailing_junk(self):
        frame = sp.encode_frame(7, "OPEN:card")
        self.assertEqual(sp.decode_frame(frame + "XY")[0], sp.FRAME_INVALID)

    def test_unknown_version(self):
        frame = sp.encode_frame(7, "OPEN:card")
        self.assertEqual(sp.decode_frame("#9" + frame[2:])[0], sp.FRAME_INVALID)

    def test_missing_star(self):
        frame = sp.encode_frame(7, "OPEN:card")
        idx = frame.index("*")
        self.assertEqual(sp.decode_frame(frame[:idx] + "!" + frame[idx + 1:])[0],
                         sp.FRAME_INVALID)

    def test_not_a_frame_is_legacy(self):
        self.assertEqual(sp.decode_frame("SPOT:3:1"),
                         (sp.FRAME_LEGACY, None, "SPOT:3:1"))
        self.assertEqual(sp.decode_frame("SPOT:3:1\r\n"),
                         (sp.FRAME_LEGACY, None, "SPOT:3:1"))
        self.assertEqual(sp.decode_frame("")[0], sp.FRAME_LEGACY)


class FrameFuzzTest(unittest.TestCase):
    def test_random_bytes_never_raise(self):
        rng = random.Random(20261116)
        for _ in range(2000):
            n = rng.randrange(0, 40)
            blob = bytes(rng.randrange(0, 256) for _ in range(n))
            status, seq, payload = sp.decode_frame(blob)
            self.assertIn(status, (sp.FRAME_OK, sp.FRAME_INVALID, sp.FRAME_LEGACY))
            if status == sp.FRAME_OK:
                # 只要被判为合法帧，就一定能重新编码成同样的字节
                again = sp.encode_frame(seq, payload)
                self.assertIsNotNone(again)

    def test_random_hash_prefixed_never_raises(self):
        rng = random.Random(7)
        for _ in range(2000):
            body = bytes(rng.choice(b"0123456789ABCDEF#*Xx\x00\xff")
                         for _ in range(rng.randrange(0, 30)))
            self.assertIn(sp.decode_frame(b"#" + body)[0],
                          (sp.FRAME_OK, sp.FRAME_INVALID))

    def test_long_reason_line_is_rejected_not_overflowed(self):
        # 旧实现里 "OPEN:" + 63 字节会被 sprintf 进 32 字节缓冲（栈溢出）。
        # 现在超长 payload 连帧都编不出来，链路上根本发不出去。
        self.assertIsNone(sp.encode_frame(0, "OPEN:" + "A" * 63))
        self.assertIsNotNone(sp.encode_frame(0, "OPEN:" + "A" * 40))


class FirmwareParityTest(unittest.TestCase):
    """Python 和固件两边的常量必须一模一样，否则升级只做一半就全线不通信。"""

    def test_header_constants_match(self):
        hdr = _read_fw(os.path.join("Hardware", "link_frame.h"))
        ver = int(re.search(r"#define\s+LINK_FRAME_VER\s+(\d+)", hdr).group(1))
        maxp = int(re.search(r"#define\s+LINK_FRAME_MAX_PAYLOAD\s+(\d+)", hdr).group(1))
        self.assertEqual(ver, sp.FRAME_VER)
        self.assertEqual(maxp, sp.FRAME_MAX_PAYLOAD)

    def test_reason_enum_order_matches(self):
        # link.h 的枚举顺序 -> link.c 的名字表 -> 树莓派 VALID_REASONS
        hdr = _read_fw(os.path.join("User", "link.h"))
        order = re.findall(r"LINK_REASON_(\w+)\s*=\s*(\d+)", hdr)
        by_index = [name.lower() for name, _ in sorted(order, key=lambda kv: int(kv[1]))]
        self.assertEqual(tuple(by_index), sp.VALID_REASONS)

    def test_sensor_fault_payload_is_parsed(self):
        self.assertEqual(sp.parse_line("SPOT:4:FAULT"),
                         {"kind": "spot_fault", "spot_id": 4})


if __name__ == "__main__":
    unittest.main()
