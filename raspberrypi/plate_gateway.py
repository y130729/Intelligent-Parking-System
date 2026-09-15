#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Integrated gateway: Huawei Cloud MQTT + STM32 UART + camera plate recognition.

Serial protocol (line based, terminated by \r\n / \n) —— 详细定义见 stm32_protocol.py：

  STM32 -> Raspberry Pi:
    SPOTS:<n>             remaining spots count
    SPOT:<id>:<0|1>       spot id (1-based), 1=occupied
    CARD:<8hex>           RFID card id (4 bytes)
    BARRIER:OPEN[:<why>]  barrier opened, why = card|plate|remote （老固件不带 why）
    BARRIER:CLOSE         barrier closed
    CAPTURE               vehicle arrived -> take photo + recognize plate
    LINK:OK               STM32 booted

  Raspberry Pi -> STM32:
    PLATE:<plate>|NONE    recognized plate result
    OPEN[:<why>]|CLOSE[:<why>]  barrier control (plate match / cloud command)

线程模型（重要）：
    watch_serial  —— 只做"读串口 + 解析 + 入队"，绝不碰网络；
    publish_worker —— 唯一向 MQTT 发布属性的线程；
    capture_and_report —— 相机/OCR 子线程；
    MQTT loop —— paho 网络线程。
    这样云端链路抖动不会阻塞本地串口链路（历史上曾把 5 秒的
    wait_for_publish 放在串口线程里，网络一慢串口就丢数据）。

Fallback local trigger (testing without STM32):
    echo capture > /tmp/plate_cmd

Run（现场用 systemd，见 deploy/plate-gateway.service）:
    python3 plate_gateway.py
"""
import json
import os
import queue
import ssl
import subprocess
import threading
import time

import paho.mqtt.client as mqtt

try:
    import serial
except ImportError:
    serial = None

import passage as psg
import plate_config as pc
import plate_matcher as pm
import plate_recognition as pr
import stm32_protocol as sp

BASE = pc.BASE
CFG_PATH = pc.CFG_PATH
FIFO_PATH = "/tmp/plate_cmd"
CAM_FILE = "/tmp/plate_cam.jpg"
VEHICLES_PATH = os.path.join(BASE, "vehicles.json")
EVENTS_PATH = os.path.join(BASE, "events.jsonl")

CMD_SUB_TOPIC = "$oc/devices/{}/sys/commands/#"
CMD_RESP_TOPIC = "$oc/devices/{}/sys/commands/response/request_id={}"
PROP_REPORT_TOPIC = "$oc/devices/{}/sys/properties/report"

# Latest known values, used to answer query_status / command responses.
STATE = {"remaining_spots": None, "barrier_state": None, "plate_number": None,
         "last_card": None, "last_card_ts": None,
         "remote_request_ts": None, "remote_operator": None}
PENDING_CMD = {"event": threading.Event(), "name": None, "success": False}
CAPTURE_STATE = {"lock": threading.Lock(), "pending": False}
# One-passage session: CAPTURE starts it; either credential (plate/card) opens once.
# 带 TTL，超时自动失效，避免"车走了会话还挂着"把下一辆车记成上一辆。
SESSION = psg.PassageSession()

# 属性上报队列：串口线程只入队，真正 publish 由 publish_worker 负责。
PUB_QUEUE_SIZE = 256
PUB_Q = queue.Queue(maxsize=PUB_QUEUE_SIZE)


def load_config():
    with open(CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def load_vehicles():
    """Load vehicle registry (vehicles.json)."""
    if not os.path.exists(VEHICLES_PATH):
        return []
    with open(VEHICLES_PATH, encoding="utf-8") as f:
        data = json.load(f)
    return data if isinstance(data, list) else data.get("vehicles", [])


def _ascii_upper(s):
    return "".join(ch for ch in (s or "")
                   if "A" <= ch <= "Z" or "a" <= ch <= "z" or "0" <= ch <= "9").upper()


def find_vehicle_by_plate(plate):
    ascii_plate = _ascii_upper(plate)
    for v in load_vehicles():
        if v.get("plate") == plate or _ascii_upper(v.get("plate", "")) == ascii_plate:
            return v
    return None


def find_vehicle_by_card(card):
    card = (card or "").upper()
    for v in load_vehicles():
        for c in v.get("cards", []):
            if str(c).upper() == card:
                return v
    return None


def append_event(event):
    try:
        with open(EVENTS_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(event, ensure_ascii=False) + "\n")
    except Exception as e:
        print("[GW] event log error: %s" % e, flush=True)


def arrival_start():
    """车辆到位：开启一次通行会话（带 TTL）。"""
    SESSION.start()


def _publish_property(client, device_id, service_id, properties):
    """真正发布属性。只允许在 publish_worker 线程里调用。"""
    payload = {"services": [{"service_id": service_id, "properties": properties}]}
    topic = PROP_REPORT_TOPIC.format(device_id)
    # 不做 wait_for_publish：等待云端确认会把网络抖动放大成本地链路阻塞
    client.publish(topic, json.dumps(payload, ensure_ascii=False))
    print("[GW] property reported: %s -> %s" % (service_id, properties), flush=True)


def queue_property(client, device_id, service_id, properties):
    """把属性上报入队后立即返回（串口线程专用，绝不阻塞在网络上）。

    队列满时丢弃最旧的一条：对现场来说"最新的状态"比"补齐旧状态"更重要。
    """
    item = (device_id, service_id, properties)
    try:
        PUB_Q.put_nowait(item)
    except queue.Full:
        try:
            PUB_Q.get_nowait()
            PUB_Q.put_nowait(item)
            print("[GW] publish queue full, dropped oldest", flush=True)
        except queue.Empty:
            pass


def publish_worker(client, cfg):
    """唯一的属性发布线程：消费 PUB_Q，允许在这里阻塞。"""
    while True:
        device_id, service_id, properties = PUB_Q.get()
        try:
            _publish_property(client, device_id, service_id, properties)
        except Exception as e:
            print("[GW] publish error: %s" % e, flush=True)
        finally:
            PUB_Q.task_done()


def send_serial(ser, line):
    if ser is None:
        print("[GW] UART not available, drop: %s" % line, flush=True)
        return
    try:
        ser.write((line + "\r\n").encode("utf-8"))
    except Exception as e:
        print("[GW] UART write error: %s" % e, flush=True)


def capture_photo(out_path, cfg=None):
    """抓拍一张。拍照参数来自配置（见 plate_config.camera_command）。

    超时上限从 90s 收到 15s、重试从 3 次收到 2 次：单次失败要快速降级到
    刷卡兜底，不能赌——原来最坏情况一次触发能占住 worker 将近 5 分钟。
    """
    cam = pc.camera_settings(cfg)
    cmd = pc.camera_command(out_path, cfg)
    last_err = ""
    attempts = max(1, int(cam.get("attempts", 2)))
    timeout_s = float(cam.get("command_timeout_s", 15))
    retry_delay_s = float(cam.get("retry_delay_s", 1))
    for attempt in range(attempts):
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
        except subprocess.TimeoutExpired:
            last_err = "timeout"
            time.sleep(retry_delay_s)
            continue
        if os.path.exists(out_path) and os.path.getsize(out_path) > 0:
            return
        last_err = ((r.stderr or r.stdout) or "")[-300:]
        if attempt < attempts - 1:
            time.sleep(retry_delay_s)
    raise RuntimeError("capture failed: %s" % last_err)


def _capture_and_report(ser, client, cfg):
    """Sub-thread: photo -> OCR -> plate DB match -> report + send back to STM32."""
    try:
        t0 = time.time()
        capture_photo(CAM_FILE, cfg)
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
        queue_property(client, cfg["device_id"], "PlateRecognition",
                       {"plate_number": best})
        STATE["plate_number"] = best
        veh = find_vehicle_by_plate(best)
        do_open = False
        if veh is not None:
            # 首次命中的凭证生效；会话过期/已开过闸时不会重复开门
            do_open = SESSION.try_open(psg.CREDENTIAL_PLATE)
        if do_open:
            send_serial(ser, sp.build_open(psg.CREDENTIAL_PLATE))
            time.sleep(0.5)  # give STM32 time to consume OPEN before PLATE follows
            append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                          "credential": psg.CREDENTIAL_PLATE, "card": "",
                          "plate": best, "owner": veh.get("owner", ""),
                          "action": "barrier_open_by_plate"})
            print("[GW] registered vehicle %s, auto OPEN" % best, flush=True)
        elif veh is None:
            append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                          "credential": psg.CREDENTIAL_PLATE, "card": "",
                          "plate": best, "owner": "",
                          "action": "plate_unknown"})
        else:
            # 车牌在库，但本次通行会话已经用过别的凭证（或已过期），不再开门
            append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                          "credential": psg.CREDENTIAL_PLATE, "card": "",
                          "plate": best, "owner": veh.get("owner", ""),
                          "action": "plate_match_no_open"})
        send_serial(ser, "PLATE:%s" % best)
        print("[GW] done: %s (%.1fs)" % (best, time.time() - t0), flush=True)
    except Exception as e:
        print("[GW] capture error: %s" % e, flush=True)
        send_serial(ser, "PLATE:NONE")



def capture_and_report(ser, client, cfg):
    """Serialize camera access: only one photo+OCR at a time."""
    if not CAPTURE_STATE["lock"].acquire(blocking=False):
        CAPTURE_STATE["pending"] = True
        print("[GW] capture busy, queued one pending trigger", flush=True)
        return
    try:
        _capture_and_report(ser, client, cfg)
    finally:
        CAPTURE_STATE["lock"].release()
        if CAPTURE_STATE["pending"]:
            CAPTURE_STATE["pending"] = False
            threading.Thread(target=capture_and_report, args=(ser, client, cfg),
                             daemon=True).start()


def complete_command(cmd_name):
    if PENDING_CMD["name"] == cmd_name:
        PENDING_CMD["success"] = True
        PENDING_CMD["event"].set()


def _note_barrier_open(client, cfg, reason, now):
    """开闸审计：判定凭证来源，关联本次通行会话，写事件日志。

    凭证来源优先取 STM32 明确上报的 reason（新固件），老固件则用
    "最近是否刷过卡 / 下发过远程命令"兜底，不再统一记成 card_or_remote。
    """
    last_card_age = None if STATE["last_card_ts"] is None else now - STATE["last_card_ts"]
    remote_age = None if STATE["remote_request_ts"] is None else now - STATE["remote_request_ts"]
    credential = psg.resolve_credential(reason, last_card_age, remote_age)
    operator = STATE["remote_operator"] if credential == psg.CREDENTIAL_REMOTE else None

    veh = None
    if credential == psg.CREDENTIAL_CARD and STATE["last_card"]:
        veh = find_vehicle_by_card(STATE["last_card"])

    # 首次命中的凭证开启会话；会话已过期则本次只记流水、不改会话
    first = SESSION.try_open(credential, operator=operator)
    append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                  "credential": credential,
                  "operator": operator,
                  "card": STATE["last_card"] or "",
                  "plate": STATE["plate_number"] or (veh or {}).get("plate", ""),
                  "owner": (veh or {}).get("owner", ""),
                  "action": "barrier_open" if first else "barrier_open_no_session"})
    if not first:
        print("[GW] barrier open without active passage session (credential=%s)"
              % credential, flush=True)


def handle_stm32_line(ser, client, cfg, line):
    msg = sp.parse_line(line)
    kind = msg["kind"]
    if kind == "empty":
        return
    print("[GW] UART: %s" % line.strip(), flush=True)
    device_id = cfg["device_id"]

    if kind == "spots":
        STATE["remaining_spots"] = msg["remaining"]
        queue_property(client, device_id, "Spot_state",
                       {"remaining_spots": msg["remaining"]})

    elif kind == "spot":
        occ = "occupied" if msg["occupied"] else "empty"
        queue_property(client, device_id, "Spot_state",
                       {"Parking_id": str(msg["spot_id"]), "Parking_state": occ})

    elif kind == "card":
        card = msg["card"]
        STATE["last_card"] = card
        STATE["last_card_ts"] = time.monotonic()
        queue_property(client, device_id, "Spot_state", {"card_id": card})
        veh = find_vehicle_by_card(card)
        append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                      "credential": psg.CREDENTIAL_CARD, "card": card,
                      "plate": veh.get("plate", "") if veh else "",
                      "owner": veh.get("owner", "") if veh else "",
                      "action": "card_hit" if veh else "card_unknown"})

    elif kind == "barrier":
        now = time.monotonic()
        up = msg["state"] == sp.BARRIER_UP
        STATE["barrier_state"] = "up" if up else "down"
        queue_property(client, device_id, "ParkingBarrier",
                       {"barrier_state": STATE["barrier_state"]})
        if up:
            complete_command("open_barrier")
            _note_barrier_open(client, cfg, msg["reason"], now)
        else:
            complete_command("close_barrier")
            SESSION.finish()          # 一次通行结束，会话到此为止

    elif kind == "link_ok":
        print("[GW] STM32 link OK", flush=True)

    elif kind == "capture":
        arrival_start()
        threading.Thread(target=capture_and_report, args=(ser, client, cfg),
                         daemon=True).start()

    else:
        print("[GW] UART unknown line: %s" % msg.get("raw", line), flush=True)


def handle_barrier_command(ser, client, cfg, cmd_name, uart_cmd, operator="cloud"):
    """Forward OPEN/CLOSE to STM32 and wait for the BARRIER ack line."""
    result = "failed"
    barrier_state = STATE["barrier_state"] or "down"
    if ser is not None:
        PENDING_CMD["event"].clear()
        PENDING_CMD["name"] = cmd_name
        PENDING_CMD["success"] = False
        # 记下"这是一次远程开闸"，供 BARRIER:OPEN 判定凭证来源与操作人
        STATE["remote_request_ts"] = time.monotonic()
        STATE["remote_operator"] = operator
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
            resp = handle_barrier_command(
                ser, client, cfg, "open_barrier",
                sp.build_open(psg.CREDENTIAL_REMOTE),
                operator=(paras.get("operator") or "cloud"))
        elif cmd_name == "close_barrier":
            resp = handle_barrier_command(
                ser, client, cfg, "close_barrier",
                sp.build_close(psg.CREDENTIAL_REMOTE),
                operator=(paras.get("operator") or "cloud"))
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
            arrival_start()
            threading.Thread(target=capture_and_report,
                             args=(cfg.get("_serial"), client, cfg),
                             daemon=True).start()


def main():
    global SESSION, PUB_Q, PUB_QUEUE_SIZE
    cfg = load_config()
    # 通行会话 TTL：超时自动失效，避免"车走了会话还挂着"
    SESSION = psg.PassageSession(ttl_s=float(cfg.get("passage_ttl_s", psg.DEFAULT_TTL_S)))
    # 上报队列：串口线程入队、单独线程发布
    PUB_QUEUE_SIZE = int(cfg.get("publish_queue_size", PUB_QUEUE_SIZE))
    PUB_Q = queue.Queue(maxsize=PUB_QUEUE_SIZE)
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
    threading.Thread(target=publish_worker, args=(client, cfg), daemon=True).start()
    threading.Thread(target=watch_serial, args=(client, cfg), daemon=True).start()
    threading.Thread(target=watch_fifo, args=(client, cfg), daemon=True).start()
    print("[GW] connecting %s:%d (device=%s)" % (cfg["hostname"], cfg["port"], cfg["device_id"]), flush=True)
    print("[GW] passage TTL=%.0fs, publish queue=%d" % (SESSION.ttl, PUB_QUEUE_SIZE), flush=True)
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
