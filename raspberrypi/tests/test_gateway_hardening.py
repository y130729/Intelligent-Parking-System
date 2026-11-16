#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""现场硬化项的回归测试（串口行缓冲 / 发送加锁 / 帧入口 / 密钥 / TLS / 命令队列）。

每一条都对应一个真实踩过的坑：

  - readline() 超时返回半行，被当成完整命令解析（"BARRIER:OP"）；
  - 多线程同时写串口，两帧交错成半帧 + 半帧；
  - 对端重传导致同一条命令被执行两次；
  - 坏 CRC 的帧被当成有效命令；
  - 密码写在会被 git 跟踪的配置文件里；
  - CA 缺失时静默降级成 CERT_NONE；
  - MQTT 回调里等串口 5 秒，把 paho 网络线程堵死。
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
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import passage as psg
import stm32_protocol as sp
from tests.support import FakeClient, FakeSerial, install_stubs

install_stubs()
import plate_gateway as gw


class UartLineSplitTest(unittest.TestCase):
    def test_half_line_is_not_a_command(self):
        buf, lines, dropped = gw.split_uart_lines(b"", b"BARRIER:OP")
        self.assertEqual((buf, lines, dropped), (b"BARRIER:OP", [], 0))
        buf, lines, dropped = gw.split_uart_lines(buf, b"EN\r\n")
        self.assertEqual(lines, [b"BARRIER:OPEN\r\n"])
        self.assertEqual((buf, dropped), (b"", 0))

    def test_multiple_lines_in_one_chunk(self):
        buf, lines, _ = gw.split_uart_lines(b"", b"A:1\r\nB:2\r\nC:3\r")
        self.assertEqual(lines, [b"A:1\r\n", b"B:2\r\n"])
        self.assertEqual(buf, b"C:3\r")

    def test_bare_lf_also_splits(self):
        _, lines, _ = gw.split_uart_lines(b"", b"SPOT:1:1\nSPOT:1:0\n")
        self.assertEqual(len(lines), 2)

    def test_oversized_without_newline_is_dropped(self):
        buf, lines, dropped = gw.split_uart_lines(b"", b"x" * 600, limit=512)
        self.assertEqual((buf, lines, dropped), (b"", [], 1))

    def test_oversized_line_is_dropped(self):
        _, lines, dropped = gw.split_uart_lines(b"", b"x" * 600 + b"\n", limit=512)
        self.assertEqual((lines, dropped), ([], 1))

    def test_limit_boundary_kept(self):
        payload = b"x" * 511 + b"\n"
        _, lines, dropped = gw.split_uart_lines(b"", payload, limit=512)
        self.assertEqual((len(lines), dropped), (1, 0))


class SerialWriteTest(unittest.TestCase):
    def setUp(self):
        gw.TX_SEQ["next"] = 0
        gw.TX_STATS["frames"] = 0
        gw.TX_STATS["errors"] = 0

    def test_writes_one_framed_line(self):
        ser = FakeSerial()
        self.assertTrue(gw.send_serial(ser, "OPEN:card"))
        self.assertEqual(len(ser.written), 1)
        self.assertTrue(ser.written[0].endswith("\r\n"))
        self.assertEqual(sp.decode_frame(ser.written[0]), (sp.FRAME_OK, 0, "OPEN:card"))

    def test_seq_increments_and_wraps(self):
        ser = FakeSerial()
        for _ in range(3):
            gw.send_serial(ser, "X")
        gw.TX_SEQ["next"] = 255
        gw.send_serial(ser, "Y")
        seqs = [sp.decode_frame(l)[1] for l in ser.written]
        self.assertEqual(seqs, [0, 1, 2, 255])
        self.assertEqual(gw.TX_SEQ["next"], 0)

    def test_overlong_payload_is_refused(self):
        ser = FakeSerial()
        self.assertFalse(gw.send_serial(ser, "OPEN:" + "A" * 60))
        self.assertEqual(ser.written, [])

    def test_no_serial_is_not_fatal(self):
        self.assertFalse(gw.send_serial(None, "OPEN:card"))

    def test_concurrent_writers_never_interleave(self):
        """8 个线程同时发：每一行都必须能被完整解码出来。"""

        class GuardSerial(object):
            def __init__(self):
                self.busy = 0
                self.overlap = False
                self.chunks = []
                self.lock = threading.Lock()

            def write(self, data):
                with self.lock:
                    self.busy += 1
                    if self.busy > 1:
                        self.overlap = True
                time.sleep(0.0005)           # 模拟真实串口的分段写
                with self.lock:
                    self.chunks.append(data)
                    self.busy -= 1

            def flush(self):
                pass

        ser = GuardSerial()

        def worker(n):
            for i in range(10):
                gw.send_serial(ser, "SPOT:%d:%d" % (n, i % 2))

        threads = [threading.Thread(target=worker, args=(n,)) for n in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        self.assertFalse(ser.overlap, "写串口时出现了并发交叠")
        self.assertEqual(len(ser.chunks), 80)
        seqs = []
        for chunk in ser.chunks:
            status, seq, payload = sp.decode_frame(chunk)
            self.assertEqual(status, sp.FRAME_OK, chunk)
            self.assertTrue(payload.startswith("SPOT:"))
            seqs.append(seq)
        # 序号唯一：两个线程拿到同一个 SEQ 时，对端会按"重复帧"丢掉第二条
        self.assertEqual(len(set(seqs)), len(seqs), "出现了重复 SEQ")


class FrameIngressTest(unittest.TestCase):
    def setUp(self):
        fd, self.events_path = tempfile.mkstemp(suffix=".jsonl")
        os.close(fd)
        gw.EVENTS_PATH = self.events_path
        gw.STATE = {"remaining_spots": None, "barrier_state": None, "plate_number": None,
                    "last_card": None, "last_card_ts": None,
                    "remote_request_ts": None, "remote_operator": None}
        gw.SESSION = psg.PassageSession(ttl_s=30.0)
        gw.PUB_Q = queue.Queue(maxsize=32)
        gw.PENDING_CMD = {"event": threading.Event(), "name": None, "success": False}
        gw.RX_STATS.update({"frames": 0, "legacy": 0, "bad": 0, "duplicates": 0})
        gw.LAST_SEQ["seen"] = False
        gw.LAST_SEQ["value"] = None
        gw.load_vehicles = lambda: []
        gw.capture_and_report = lambda *a, **kw: None
        gw.TX_SEQ["next"] = 0
        self.cfg = {"device_id": "test-device"}
        self.ser = FakeSerial()

    def tearDown(self):
        try:
            os.unlink(self.events_path)
        except OSError:
            pass

    def queued(self):
        items = []
        while not gw.PUB_Q.empty():
            items.append(gw.PUB_Q.get_nowait())
        return items

    def events(self):
        with open(self.events_path, encoding="utf-8") as f:
            return [json.loads(l) for l in f if l.strip()]

    def feed(self, payload, seq=1, client=None):
        frame = sp.encode_frame(seq, payload) + "\r\n"
        gw.handle_stm32_line(self.ser, client or FakeClient(), self.cfg,
                             frame)

    def test_frame_is_acked(self):
        self.feed("SPOT:2:1", seq=0x2A)
        self.assertEqual(len(self.ser.written), 1)
        self.assertEqual(sp.decode_frame(self.ser.written[0]),
                         (sp.FRAME_OK, 0, "ACK:2A"))
        self.assertEqual(gw.RX_STATS["frames"], 1)
        self.assertEqual(len(self.queued()), 1)

    def test_duplicate_seq_executed_once(self):
        self.feed("SPOT:5:1", seq=7)
        self.feed("SPOT:5:1", seq=7)          # 对端重传
        self.assertEqual(gw.RX_STATS["frames"], 1)
        self.assertEqual(gw.RX_STATS["duplicates"], 1)
        self.assertEqual(len(self.queued()), 1)
        # 重复帧也要补 ACK，否则对端会一直重传下去
        self.assertEqual(len(self.ser.written), 2)

    def test_stm32_reboot_resets_seq_dedup(self):
        """STM32 复位后 SEQ 归零，不能被上一轮的记录判成重复帧。"""
        self.feed("SPOT:1:1", seq=0)
        self.feed("LINK:OK", seq=0)          # 同序号：重启公告必须能进来
        self.assertEqual(len(self.queued()), 1)   # 只有 SPOT 入队，LINK:OK 不入队
        self.feed("SPOT:1:0", seq=1)
        self.assertEqual(len(self.queued()), 1)

    def test_bad_crc_is_dropped(self):
        frame = sp.encode_frame(3, "SPOT:5:1")
        frame = frame[:-2] + ("00" if frame[-2:] != "00" else "01")
        gw.handle_stm32_line(self.ser, FakeClient(), self.cfg, frame + "\r\n")
        self.assertEqual(gw.RX_STATS["bad"], 1)
        self.assertEqual(self.queued(), [])
        self.assertEqual(self.ser.written, [])

    def test_ack_frames_are_not_acked_again(self):
        self.feed("ACK:01", seq=1)
        self.assertEqual(self.ser.written, [])
        self.assertEqual(gw.RX_STATS["frames"], 0)

    def test_legacy_text_still_works(self):
        gw.handle_stm32_line(self.ser, FakeClient(), self.cfg, "SPOTS:19\r\n")
        self.assertEqual(gw.RX_STATS["legacy"], 1)
        self.assertEqual(len(self.queued()), 1)

    def test_sensor_fault_is_reported_not_treated_as_empty(self):
        self.feed("SPOT:4:FAULT", seq=9)
        item = self.queued()[0]
        props = item[2]
        self.assertEqual(props, {"Parking_id": "4", "Parking_state": "fault"})
        self.assertEqual(self.events()[-1]["action"], "sensor_fault")


class SecretConfigTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.secret_path = os.path.join(self.tmpdir, "cloud_secret.json")
        self._old_secret = gw.SECRET_PATH
        self._old_cfg = gw.CFG_PATH
        gw.SECRET_PATH = self.secret_path
        os.environ.pop("PARKING_MQTT_PASSWORD", None)

    def tearDown(self):
        gw.SECRET_PATH = self._old_secret
        gw.CFG_PATH = self._old_cfg
        os.environ.pop("PARKING_MQTT_PASSWORD", None)
        for name in os.listdir(self.tmpdir):
            os.unlink(os.path.join(self.tmpdir, name))
        os.rmdir(self.tmpdir)

    def _write_secret(self, password, mode=0o600):
        with open(self.secret_path, "w", encoding="utf-8") as f:
            json.dump({"password": password}, f)
        os.chmod(self.secret_path, mode)

    def test_env_var_wins(self):
        os.environ["PARKING_MQTT_PASSWORD"] = "from-env"
        self._write_secret("from-file")
        self.assertEqual(gw._read_secret(), "from-env")

    def test_secret_file_used(self):
        self._write_secret("from-file")
        self.assertEqual(gw._read_secret(), "from-file")

    def test_missing_secret_fails_loudly(self):
        with self.assertRaises(RuntimeError) as ctx:
            gw._read_secret()
        self.assertIn("PARKING_MQTT_PASSWORD", str(ctx.exception))

    def test_loose_permissions_warn(self):
        self._write_secret("p", mode=0o644)
        with mock.patch.object(gw.os, "stat",
                               return_value=types.SimpleNamespace(st_mode=0o644)):
            self.assertEqual(gw._read_secret(), "p")

    def test_config_file_has_no_password(self):
        cfg_path = os.path.join(self.tmpdir, "cloud_config.json")
        with open(cfg_path, "w", encoding="utf-8") as f:
            json.dump({"hostname": "h", "port": 8883, "device_id": "d"}, f)
        gw.CFG_PATH = cfg_path
        os.environ["PARKING_MQTT_PASSWORD"] = "envpass"
        cfg = gw.load_config()
        self.assertEqual(cfg["password"], "envpass")
        self.assertNotIn("password", json.load(open(cfg_path, encoding="utf-8")))


class CaPathTest(unittest.TestCase):
    def test_relative_path_is_resolved_against_code_dir(self):
        with self.assertRaises(RuntimeError) as ctx:
            gw.resolve_ca_path({"ca_cert": "no_such_ca.pem"})
        self.assertIn(os.path.join(gw.BASE, "no_such_ca.pem"), str(ctx.exception))

    def test_missing_ca_refuses_to_start(self):
        with self.assertRaises(RuntimeError):
            gw.resolve_ca_path({"ca_cert": "/no/such/ca.pem"})
        with self.assertRaises(RuntimeError):
            gw.resolve_ca_path({})

    def test_existing_ca_is_returned(self):
        fd, path = tempfile.mkstemp()
        os.close(fd)
        try:
            self.assertEqual(gw.resolve_ca_path({"ca_cert": path}), path)
        finally:
            os.unlink(path)


class CommandQueueTest(unittest.TestCase):
    class Msg(object):
        def __init__(self, topic, payload):
            self.topic = topic
            self.payload = payload

    def setUp(self):
        self.cfg = {"device_id": "test-device", "_serial": None}
        gw.CMD_Q = queue.Queue(maxsize=2)
        gw.STATE = {"remaining_spots": 12, "barrier_state": "down", "plate_number": "京A00001",
                    "last_card": None, "last_card_ts": None,
                    "remote_request_ts": None, "remote_operator": None}

    def test_callback_only_enqueues(self):
        msg = self.Msg("$oc/devices/d/sys/commands/request_id=r1",
                       b'{"command_name":"open_barrier"}')
        t0 = time.time()
        gw.on_message(None, None, msg)
        self.assertLess(time.time() - t0, 0.05)
        self.assertEqual(gw.CMD_Q.qsize(), 1)

    def test_non_command_topics_are_ignored(self):
        gw.on_message(None, None, self.Msg("$oc/devices/d/sys/messages/down", b"{}"))
        self.assertEqual(gw.CMD_Q.qsize(), 0)

    def test_bad_json_is_ignored(self):
        gw.on_message(None, None, self.Msg("$oc/devices/d/sys/commands/request_id=r1",
                                           b"not json"))
        self.assertEqual(gw.CMD_Q.qsize(), 0)

    def test_full_queue_does_not_raise(self):
        gw.CMD_Q.put(("a", {}))
        gw.CMD_Q.put(("b", {}))
        gw.on_message(None, None, self.Msg("$oc/devices/d/sys/commands/request_id=r9",
                                           b'{"command_name":"open_barrier"}'))
        self.assertEqual(gw.CMD_Q.qsize(), 2)

    def test_query_status_answers_from_state(self):
        client = FakeClient()
        gw.execute_command(client, self.cfg, "$oc/devices/d/sys/commands/request_id=abc",
                           {"command_name": "query_status"})
        topic, body = client.published[-1]
        self.assertTrue(topic.endswith("request_id=abc"))
        payload = json.loads(body)
        self.assertEqual(payload["paras"]["remaining_spots"], 12)
        self.assertEqual(payload["paras"]["barrier_state"], "down")


if __name__ == "__main__":
    unittest.main()
