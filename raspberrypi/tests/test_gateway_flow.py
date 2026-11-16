#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""网关主流程测试（不需要相机 / 网络 / 硬件）。

用桩模块替换 paho-mqtt / pyserial / plate_recognition，验证三件事：

  1. 串口线程只做"解析 + 入队"，无论如何都不会在网络 I/O 上阻塞
     （回归：旧实现把 wait_for_publish(5) 放在串口线程里）；
  2. 通行会话能开启、能在超时后失效、首次命中的凭证生效；
  3. 开闸事件记录了明确的可审计字段（credential / operator / card / plate / owner）。

运行：python -m unittest discover -s raspberrypi/tests -t raspberrypi
"""

import json
import os
import queue
import sys
import tempfile
import threading
import time
import types
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import plate_format
import passage as psg


from tests.support import FakeClient, FakeSerial, install_stubs

install_stubs()
import plate_gateway as gw


class GatewayFlowTest(unittest.TestCase):
    def setUp(self):
        fd, self.events_path = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        gw.EVENTS_PATH = self.events_path
        gw.STATE = {"remaining_spots": None, "barrier_state": None, "plate_number": None,
                    "last_card": None, "last_card_ts": None,
                    "remote_request_ts": None, "remote_operator": None}
        gw.SESSION = psg.PassageSession(ttl_s=30.0)
        gw.PUB_Q = queue.Queue(maxsize=8)
        gw.PENDING_CMD = {"event": threading.Event(), "name": None, "success": False}
        gw.load_vehicles = lambda: [{"plate": "冀T234J5",
                                     "cards": ["7506D506"],
                                     "owner": "张三"}]
        # 不真的去调 rpicam-still：只记录"有没有被触发"
        self.capture_calls = []
        gw.capture_and_report = lambda *a, **kw: self.capture_calls.append(1)
        self.cfg = {"device_id": "test-device"}
        self.ser = FakeSerial()

    def tearDown(self):
        try:
            os.unlink(self.events_path)
        except OSError:
            pass

    # ---- 工具 -----------------------------------------------------
    def feed(self, line, client=None):
        gw.handle_stm32_line(self.ser, client or FakeClient(), self.cfg, line)

    def events(self):
        with open(self.events_path, encoding="utf-8") as f:
            return [json.loads(l) for l in f if l.strip()]

    def queued(self):
        items = []
        while not gw.PUB_Q.empty():
            items.append(gw.PUB_Q.get_nowait())
        return items

    # ---- 1. 串口线程不得碰网络 -------------------------------------
    def test_serial_thread_never_publishes_directly(self):
        """回归：串口线程不允许直接 publish（哪怕网络会阻塞/报错）。"""
        client = FakeClient(publish_raises=AssertionError("串口线程不得调用网络"))
        for line in ("SPOTS:17", "SPOT:3:1", "CARD:7506D506",
                     "BARRIER:OPEN:card", "LINK:OK"):
            self.feed(line, client)
        self.assertEqual(client.published, [])
        self.assertGreater(len(self.queued()), 0)

    def test_serial_thread_is_fast_even_if_network_is_slow(self):
        client = FakeClient(publish_delay=5.0)
        t0 = time.time()
        for _ in range(50):
            self.feed("SPOT:3:1", client)
        self.assertLess(time.time() - t0, 0.5)

    def test_publish_worker_drains_queue(self):
        client = FakeClient()
        self.feed("SPOT:3:1", client)
        worker = threading.Thread(target=gw.publish_worker, args=(client, self.cfg),
                                  daemon=True)
        worker.start()
        deadline = time.time() + 2.0
        while not client.published and time.time() < deadline:
            time.sleep(0.01)
        self.assertEqual(len(client.published), 1)
        payload = json.loads(client.published[0][1])
        self.assertEqual(payload["services"][0]["properties"],
                         {"Parking_id": "3", "Parking_state": "occupied"})

    def test_queue_overflow_drops_oldest(self):
        for i in range(1, 12):                 # 队列容量只有 8
            self.feed("SPOT:%d:1" % i)
        items = self.queued()
        self.assertLessEqual(len(items), 8)
        self.assertIn("11", items[-1][2]["Parking_id"])   # 最新的一定保留

    # ---- 2. 通行会话 ----------------------------------------------
    def test_capture_starts_passage_session(self):
        self.assertFalse(gw.SESSION.is_active())
        self.feed("CAPTURE")
        self.assertTrue(gw.SESSION.is_active())
        deadline = time.time() + 1.0
        while not self.capture_calls and time.time() < deadline:
            time.sleep(0.01)
        self.assertEqual(len(self.capture_calls), 1)

    def test_session_expires_and_next_credential_is_recorded(self):
        gw.SESSION = psg.PassageSession(ttl_s=0.05)
        self.feed("CAPTURE")
        time.sleep(0.12)
        self.assertFalse(gw.SESSION.is_active())
        self.feed("CARD:7506D506")
        self.feed("BARRIER:OPEN:card")     # 会话已过期 -> 不改会话，只记流水
        self.assertFalse(gw.SESSION.snapshot()["opened"])
        self.assertEqual(self.events()[-1]["action"], "barrier_open_no_session")

    def test_barrier_close_ends_session(self):
        self.feed("CAPTURE")
        self.feed("BARRIER:OPEN:card")
        self.feed("BARRIER:CLOSE")
        self.assertFalse(gw.SESSION.is_active())

    def test_first_credential_wins(self):
        self.feed("CAPTURE")
        self.feed("BARRIER:OPEN:plate")
        self.assertTrue(gw.SESSION.try_open(psg.CREDENTIAL_REMOTE) is False)

    # ---- 3. 审计字段 ----------------------------------------------
    def test_card_open_records_card_credential(self):
        self.feed("CAPTURE")            # 车到位
        self.feed("CARD:7506D506")      # 刷卡
        self.feed("BARRIER:OPEN:card")
        ev = self.events()[-1]
        self.assertEqual(ev["credential"], "card")
        self.assertEqual(ev["card"], "7506D506")
        self.assertEqual(ev["owner"], "张三")
        self.assertEqual(ev["action"], "barrier_open")

    def test_remote_open_records_operator(self):
        """回归：远程开闸不再被记成 card_or_remote。"""
        gw.STATE["remote_request_ts"] = time.monotonic()
        gw.STATE["remote_operator"] = "web-admin"
        self.feed("BARRIER:OPEN:remote")
        ev = self.events()[-1]
        self.assertEqual(ev["credential"], "remote")
        self.assertEqual(ev["operator"], "web-admin")

    def test_legacy_firmware_without_reason(self):
        """老固件只发 BARRIER:OPEN，用"刚刷过卡"兜底判定为 card。"""
        self.feed("CARD:7506D506")
        self.feed("BARRIER:OPEN")
        self.assertEqual(self.events()[-1]["credential"], "card")

    # ---- 4. 协议兼容 ----------------------------------------------
    def test_unknown_line_is_ignored(self):
        self.feed("WiFi: hello")
        self.assertEqual(self.events(), [])
        self.assertEqual(self.queued(), [])

    def test_spot_words_accepted(self):
        self.feed("SPOT:5:occupied")
        self.assertEqual(self.queued()[-1][2]["Parking_state"], "occupied")

    def test_plate_command_sent_with_reason(self):
        gw.SESSION.start()
        gw._note_barrier_open(FakeClient(), self.cfg, "", time.monotonic())
        self.assertEqual(gw.SESSION.snapshot()["credential"], psg.CREDENTIAL_UNKNOWN)
        self.assertEqual(self.ser.lines(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
