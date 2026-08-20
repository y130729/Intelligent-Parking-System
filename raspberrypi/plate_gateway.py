#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Integrated gateway: Huawei Cloud MQTT + camera plate recognition.

Trigger (simulating the STM32 signal for now):
    echo capture > /tmp/plate_cmd

On trigger: capture photo -> OCR -> plate DB match -> report plate_number.
To integrate the real STM32 later, replace watch_fifo() with a UART
listener that calls the same trigger path.

Usage:
    nohup python3 plate_gateway.py >/tmp/gateway.log 2>&1 &
"""
import json
import os
import ssl
import subprocess
import threading
import time

import paho.mqtt.client as mqtt

import plate_matcher as pm
import plate_recognition as pr

BASE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(BASE, "cloud_config.json")
FIFO_PATH = "/tmp/plate_cmd"
CAM_FILE = "/tmp/plate_cam.jpg"
CAM_W, CAM_H = 2592, 1944

CMD_SUB_TOPIC = "$oc/devices/{}/sys/commands/#"
CMD_RESP_TOPIC = "$oc/devices/{}/sys/commands/response/request_id={}"
PROP_REPORT_TOPIC = "$oc/devices/{}/sys/properties/report"


def load_config():
    with open(CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def report_property(client, device_id, service_id, properties):
    payload = {"services": [{"service_id": service_id, "properties": properties}]}
    topic = PROP_REPORT_TOPIC.format(device_id)
    info = client.publish(topic, json.dumps(payload, ensure_ascii=False))
    info.wait_for_publish(5)
    print("[GW] property reported: %s -> %s" % (service_id, properties), flush=True)


def capture_photo(out_path):
    cmd = ["rpicam-still", "-o", out_path, "--width", str(CAM_W),
           "--height", str(CAM_H), "--timeout", "3000", "-n"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    if not os.path.exists(out_path):
        raise RuntimeError("capture failed: %s" % ((r.stderr or r.stdout) or "")[-300:])


def capture_and_report(client, cfg):
    """Sub-thread: photo -> OCR -> plate DB match -> report to cloud."""
    try:
        t0 = time.time()
        capture_photo(CAM_FILE)
        img = pr.cv2.imread(CAM_FILE)
        if img is None:
            print("[GW] cannot read image", flush=True)
            return
        best = ""
        for c in pr.find_plate_candidates(img):
            text, psm, dt = pr.ocr_plate_crop(img, c)
            if not text:
                continue
            fixed = pr.post_process(text)
            full, score, hit = pm.fix_plate(fixed)
            print("[GW] OCR=%s -> fixed=%s, db=%s (sim %.2f, hit=%s)" %
                  (text, fixed, full, score, hit), flush=True)
            if hit:
                best = full
                break
            if not best and len(fixed) >= 6:
                best = fixed
        if len(best) < 6:
            print("[GW] no valid plate recognized", flush=True)
            return
        report_property(client, cfg["device_id"], "PlateRecognition",
                        {"plate_number": best})
        print("[GW] done: %s (%.1fs)" % (best, time.time() - t0), flush=True)
    except Exception as e:
        print("[GW] capture error: %s" % e, flush=True)


def watch_fifo(client, cfg):
    """Watch local FIFO; 'capture' triggers photo+recognition (STM32 stand-in)."""
    if not os.path.exists(FIFO_PATH):
        os.mkfifo(FIFO_PATH)
    print("[GW] watching %s  (trigger: echo capture > %s)" % (FIFO_PATH, FIFO_PATH), flush=True)
    while True:
        try:
            with open(FIFO_PATH) as f:
                cmd = f.read().strip()
        except Exception:
            time.sleep(1)
            continue
        if cmd == "capture":
            print("[GW] trigger received: capture", flush=True)
            threading.Thread(target=capture_and_report, args=(client, cfg), daemon=True).start()


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print("[GW] connect failed, rc=%d" % rc, flush=True)
        return
    cfg = userdata
    device_id = cfg["device_id"]
    client.subscribe(CMD_SUB_TOPIC.format(device_id), qos=0)
    print("[GW] connected, subscribed: %s" % CMD_SUB_TOPIC.format(device_id), flush=True)


def on_disconnect(client, userdata, rc):
    print("[GW] disconnected rc=%s, auto-reconnecting..." % rc, flush=True)


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        return
    if "request_id=" in topic:
        request_id = topic.rsplit("request_id=", 1)[-1]
        cmd_name = payload.get("command_name", "")
        service_id = payload.get("service_id", "")
        paras = payload.get("paras", {})
        print("[GW] command: %s/%s paras=%s" % (service_id, cmd_name, paras), flush=True)
        if cmd_name == "capture_photo":  # reserved for cloud-command trigger
            threading.Thread(target=capture_and_report, args=(client, userdata), daemon=True).start()
        resp = {"result_code": 0, "response_name": cmd_name,
                "paras": {"result": "success"}}
        client.publish(CMD_RESP_TOPIC.format(userdata["device_id"], request_id),
                       json.dumps(resp, ensure_ascii=False))
    else:
        print("[GW] msg: %s -> %s" % (topic, msg.payload[:200]), flush=True)


def main():
    cfg = load_config()
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cfg["client_id"],
                             protocol=mqtt.MQTTv311)
    except AttributeError:  # paho-mqtt 1.x
        client = mqtt.Client(client_id=cfg["client_id"], protocol=mqtt.MQTTv311)
    client.username_pw_set(cfg["username"], cfg["password"])
    if cfg["port"] == 8883:
        ca = os.path.join(BASE, cfg.get("ca_cert", ""))
        if os.path.exists(ca):
            client.tls_set(ca_certs=ca)
        else:
            client.tls_set(cert_reqs=ssl.CERT_NONE)
            print("[GW] WARN: CA cert missing, skip verify", flush=True)
    client.user_data_set(cfg)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    threading.Thread(target=watch_fifo, args=(client, cfg), daemon=True).start()
    print("[GW] connecting %s:%d (device=%s)" % (cfg["hostname"], cfg["port"], cfg["device_id"]), flush=True)
    while True:
        try:
            client.connect(cfg["hostname"], cfg["port"], keepalive=60)
            break
        except Exception as e:
            print("[GW] connect failed: %s, retry in 5s" % e, flush=True)
            time.sleep(5)
    client.loop_forever()


if __name__ == "__main__":
    main()