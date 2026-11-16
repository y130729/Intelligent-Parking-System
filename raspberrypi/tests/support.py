#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""测试桩（paho-mqtt / pyserial / OpenCV）+ 两个假对象。

PC 上没有装 paho-mqtt / pyserial / OpenCV，而 plate_gateway 在 import
阶段就会去 import 它们。桩只提供被调用到的接口，让纯逻辑（帧编解码、
行缓冲、会话、线程模型）能在没有硬件的机器上直接跑单测。
"""

import sys
import time
import types

import plate_format


class FakeClient(object):
    """记录 publish 调用；可选地模拟"网络很慢 / 直接失败"。"""

    def __init__(self, publish_delay=0.0, publish_raises=None):
        self.published = []
        self.publish_delay = publish_delay
        self.publish_raises = publish_raises
        self.subscribed = []

    def publish(self, topic, payload=None, *a, **kw):
        if self.publish_raises:
            raise self.publish_raises
        if self.publish_delay:
            time.sleep(self.publish_delay)
        self.published.append((topic, payload))
        return types.SimpleNamespace(wait_for_publish=lambda *a, **k: True)

    def subscribe(self, topic, qos=0):
        self.subscribed.append((topic, qos))


class FakeSerial(object):
    def __init__(self):
        self.written = []

    def write(self, data):
        self.written.append(data.decode("utf-8"))

    def flush(self):
        pass

    def close(self):
        pass

    def lines(self):
        return [l.strip() for l in self.written]


def install_stubs():
    if "paho.mqtt.client" not in sys.modules:
        paho = types.ModuleType("paho")
        paho_mqtt = types.ModuleType("paho.mqtt")
        paho_client = types.ModuleType("paho.mqtt.client")
        paho_client.Client = FakeClient
        paho_client.CallbackAPIVersion = types.SimpleNamespace(VERSION1=1)
        paho.mqtt = paho_mqtt
        paho_mqtt.client = paho_client
        sys.modules["paho"] = paho
        sys.modules["paho.mqtt"] = paho_mqtt
        sys.modules["paho.mqtt.client"] = paho_client

    if "serial" not in sys.modules:
        serial_mod = types.ModuleType("serial")
        serial_mod.Serial = FakeSerial
        serial_mod.SerialException = Exception
        sys.modules["serial"] = serial_mod

    if "plate_recognition" not in sys.modules:
        pr = types.ModuleType("plate_recognition")
        pr.cv2 = types.SimpleNamespace(imread=lambda p: None)
        pr.find_plate_candidates = lambda img: []
        pr.ocr_plate_crop = lambda img, c: ("", None, None)
        pr.post_process = plate_format.post_process     # 用真实的规范化逻辑
        sys.modules["plate_recognition"] = pr
