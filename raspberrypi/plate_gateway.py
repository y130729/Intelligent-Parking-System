#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Integrated gateway: Huawei Cloud MQTT + STM32 UART + camera plate recognition.

Serial protocol (line based, terminated by \r\n / \n):

  STM32 -> Raspberry Pi:
    SPOTS:<n>            remaining spots count
    SPOT:<id>:<0|1>      spot id (1-based), 1=occupied
    CARD:<8hex>          RFID card id (4 bytes)
    BARRIER:OPEN|CLOSE   barrier opened / closed (also acks remote cmd)
    CAPTURE              vehicle arrived -> take photo + recognize plate

  Raspberry Pi -> STM32:
    PLATE:<plate>|NONE   recognized plate result
    OPEN|CLOSE           remote barrier control (from cloud commands)

Fallback local trigger (testing without STM32):
    echo capture > /tmp/plate_cmd

Run:
    nohup python3 plate_gateway.py >/tmp/gateway.log 2>&1 &
"""
import json
import os
import ssl
import subprocess
import threading
import time

import paho.mqtt.client as mqtt

try:
    import serial
except ImportError:
    serial = None

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

# Latest known values, used to answer query_status / command responses.
STATE = {"remaining_spots": None, "barrier_state": None, "plate_number": None}
PENDING_CMD = {"event": threading.Event(), "name": None, "success": False}


def load_config():
    with open(CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def report_property(client, device_id, service_id, properties):
    payload = {"services": [{"service_id": service_id, "properties": properties}]}
    topic = PROP_REPORT_TOPIC.format(device_id)
    info = client.publish(topic, json.dumps(payload, ensure_ascii=False))
    info.wait_for_publish(5)
    print("[GW] property reported: %s -> %s" % (service_id, properties), flush=True)


def send_serial(ser, line):
    if ser is None:
        print("[GW] UART not available, drop: %s" % line, flush=True)
        return
    try:
        ser.write((line + "\r\n").encode("utf-8"))
    except Exception as e:
        print("[GW] UART write error: %s" % e, flush=True)


def capture_photo(out_path):
    cmd = ["rpicam-still", "-o", out_path, "--width", str(CAM_W),
           "--height", str(CAM_H), "--timeout", "3000", "-n"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    if not os.path.exists(out_path):
        raise RuntimeError("capture failed: %s" % ((r.stderr or r.stdout) or "")[-300:])


def capture_and_report(ser, client, cfg):
    """Sub-thread: photo -> OCR -> plate DB match -> report + send back to STM32."""
    try:
        t0 = time.time()
        capture_photo(CAM_FILE)
        img = pr.cv2.imread(CAM_FILE)
        if img is None:
            print("[GW] cannot read image", flush=True)
            send_serial(ser, "PLATE:NONE")
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
            send_serial(ser, "PLATE:NONE")
            return
        report_property(client, cfg["device_id"], "PlateRecognition",
                        {"plate_number": best})
        STATE["plate_number"] = best
        send_serial(ser, "PLATE:%s" % best)
        print("[GW] done: %s (%.1fs)" % (best, time.time() - t0), flush=True)
    except Exception as e:
        print("[GW] capture error: %s" % e, flush=True)
        send_serial(ser, "PLATE:NONE")


def complete_command(cmd_name):
    if PENDING_CMD["name"] == cmd_name:
        PENDING_CMD["success"] = True
        PENDING_CMD["event"].set()


def handle_stm32_line(ser, client, cfg, line):
    line = line.strip()
    if not line:
        return
    print("[GW] UART: %s" % line, flush=True)
    if line.startswith("SPOTS:"):
        try:
            n = int(line.split(":")[1])
        except Exception:
            return
        STATE["remaining_spots"] = n
        report_property(client, cfg["device_id"], "Spot_state",
                        {"remaining_spots": n})
    elif line.startswith("SPOT:"):
        parts = line.split(":")
        if len(parts) < 3:
            return
        spot_id, status = parts[1], parts[2]
        occ = "occupied" if status == "1" else "empty"
        report_property(client, cfg["device_id"], "Spot_state",
                        {"Parking_id": spot_id, "Parking_state": occ})
    elif line.startswith("CARD:"):
        card = line[len("CARD:"):]
        report_property(client, cfg["device_id"], "Spot_state",
                        {"card_id": card})
    elif line == "BARRIER:OPEN":
        STATE["barrier_state"] = "up"
        report_property(client, cfg["device_id"], "ParkingBarrier",
                        {"barrier_state": "up"})
        complete_command("open_barrier")
    elif line == "BARRIER:CLOSE":
        STATE["barrier_state"] = "down"
        report_property(client, cfg["device_id"], "ParkingBarrier",
                        {"barrier_state": "down"})
        complete_command("close_barrier")
    elif line == "CAPTURE":
        threading.Thread(target=capture_and_report, args=(ser, client, cfg),
                         daemon=True).start()
    else:
        print("[GW] UART unknown line: %s" % line, flush=True)


def handle_barrier_command(ser, client, cfg, cmd_name, uart_cmd):
    """Forward OPEN/CLOSE to STM32 and wait for the BARRIER ack line."""
    result = "failed"
    barrier_state = STATE["barrier_state"] or "down"
    if ser is not None:
        PENDING_CMD["event"].clear()
        PENDING_CMD["name"] = cmd_name
        PENDING_CMD["success"] = False
        send_serial(ser, uart_cmd)
        PENDING_CMD["event"].wait(5.0)
        if PENDING_CMD["success"]:
            result = "success"
            barrier_state = STATE["barrier_state"] or barrier_state
        PENDING_CMD["name"] = None
    paras = {"result": result}
    if cmd_name == "open_barrier":
        paras["barrier_state"] = barrier_state
    return {"result_code": 0, "response_name": cmd_name, "paras": paras}


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print("[GW] connect failed, rc=%d" % rc, flush=True)
        return
    cfg = userdata
    client.subscribe(CMD_SUB_TOPIC.format(cfg["device_id"]), qos=0)
    print("[GW] connected, subscribed: %s" % CMD_SUB_TOPIC.format(cfg["device_id"]), flush=True)


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
        cfg = userdata
        ser = cfg.get("_serial")
        if cmd_name == "capture_photo":
            threading.Thread(target=capture_and_report, args=(ser, client, cfg),
                             daemon=True).start()
            resp = {"result_code": 0, "response_name": cmd_name,
                    "paras": {"result": "success"}}
        elif cmd_name == "open_barrier":
            resp = handle_barrier_command(ser, client, cfg, "open_barrier", "OPEN")
        elif cmd_name == "close_barrier":
            resp = handle_barrier_command(ser, client, cfg, "close_barrier", "CLOSE")
        elif cmd_name == "query_status":
            resp = {"result_code": 0, "response_name": "query_status", "paras": {
                "result": "success",
                "remaining_spots": STATE["remaining_spots"] if STATE["remaining_spots"] is not None else 0,
                "plate_number": STATE["plate_number"] or "",
                "barrier_state": STATE["barrier_state"] or "down"}}
        else:
            resp = {"result_code": 0, "response_name": cmd_name,
                    "paras": {"result": "success"}}
        client.publish(CMD_RESP_TOPIC.format(cfg["device_id"], request_id),
                       json.dumps(resp, ensure_ascii=False))
        print("[GW] command response sent: %s" % cmd_name, flush=True)
    else:
        print("[GW] msg: %s -> %s" % (topic, msg.payload[:200]), flush=True)


def watch_serial(client, cfg):
    port = cfg.get("serial_port", "/dev/serial0")
    baud = int(cfg.get("serial_baud", 115200))
    if serial is None:
        print("[GW] pyserial not installed, UART disabled", flush=True)
        return
    ser = None
    while True:
        try:
            if ser is None:
                ser = serial.Serial(port, baud, timeout=0.5)
                cfg["_serial"] = ser
                print("[GW] serial %s opened @%d" % (port, baud), flush=True)
            raw = ser.readline()
            if raw:
                handle_stm32_line(ser, client, cfg,
                                  raw.decode("utf-8", errors="replace"))
        except Exception as e:
            print("[GW] serial error: %s, retry in 5s" % e, flush=True)
            try:
                if ser is not None:
                    ser.close()
            except Exception:
                pass
            ser = None
            cfg["_serial"] = None
            time.sleep(5)


def watch_fifo(client, cfg):
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
            threading.Thread(target=capture_and_report,
                             args=(cfg.get("_serial"), client, cfg),
                             daemon=True).start()


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
    threading.Thread(target=watch_serial, args=(client, cfg), daemon=True).start()
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