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


SECRET_PATH = os.path.join(BASE, "cloud_secret.json")


def _read_secret():
    """取设备密钥，优先级：环境变量 > cloud_secret.json。

    为什么要外置：把密码写在 cloud_config.json 里，这个文件一旦进了
    git（本仓库历史上就进过）就等于把设备凭据公开了。现在配置文件里
    只剩"连哪里"，"凭什么连"放在仓库之外。
    """
    env = os.environ.get("PARKING_MQTT_PASSWORD")
    if env:
        return env
    if os.path.exists(SECRET_PATH):
        st = os.stat(SECRET_PATH)
        if st.st_mode & 0o077:
            print("[GW] WARN: %s 权限过宽（应为 0600）" % SECRET_PATH, flush=True)
        with open(SECRET_PATH, encoding="utf-8") as f:
            return json.load(f)["password"]
    raise RuntimeError(
        "缺少 MQTT 密码：请设置环境变量 PARKING_MQTT_PASSWORD，"
        "或创建 %s（chmod 600），内容形如 {\"password\": \"...\"}" % SECRET_PATH)


def load_config():
    with open(CFG_PATH, encoding="utf-8") as f:
        cfg = json.load(f)
    cfg["password"] = _read_secret()
    return cfg


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


# 串口写是共享资源：dispatcher 线程发 OPEN/CLOSE，拍照线程发 PLATE，
# FIFO 线程也可能发。pyserial 的 write 不是原子的，两帧交错就会得到
# 半帧 + 半帧——对方按行解析时两帧全废。所有写都必须拿这把锁。
SERIAL_LOCK = threading.Lock()
TX_SEQ = {"next": 0}
TX_STATS = {"frames": 0, "errors": 0}


def send_serial(ser, line):
    """发送一条 payload（自动加帧头/CRC/序号），线程安全。"""
    if ser is None:
        print("[GW] UART not available, drop: %s" % line, flush=True)
        return False
    # 序号必须在锁内取。原来"锁外读 SEQ -> 写 -> 锁外 +1"是有竞态的：
    # 两个线程可能拿到同一个序号，而对端按 SEQ 判重，第二条会被当成
    # "重传"直接丢掉——丢的正好可能是一条 OPEN/CLOSE。
    with SERIAL_LOCK:
        seq = TX_SEQ["next"]
        frame = sp.encode_frame(seq, line)
        if frame is None:
            print("[GW] UART frame too long, drop: %s" % line, flush=True)
            return False
        try:
            ser.write((frame + "\r\n").encode("latin-1"))
            ser.flush()
        except Exception as e:
            TX_STATS["errors"] += 1
            print("[GW] UART write error: %s" % e, flush=True)
            return False
        TX_SEQ["next"] = (seq + 1) & 0xFF
        TX_STATS["frames"] += 1
        return True


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


# 收帧统计与重复帧检测（对端重传时同一条不会被执行两次）
RX_STATS = {"frames": 0, "legacy": 0, "bad": 0, "duplicates": 0}
LAST_SEQ = {"seen": False, "value": None}


def _ack(ser, seq):
    send_serial(ser, "ACK:%02X" % seq)


def handle_stm32_line(ser, client, cfg, line):
    status, seq, payload = sp.decode_frame(line)
    if status == sp.FRAME_INVALID:
        RX_STATS["bad"] += 1
        print("[GW] UART bad frame dropped: %r" % line.strip(), flush=True)
        return
    if status == sp.FRAME_OK:
        if payload.startswith("ACK:"):
            return                      # ACK 不再回 ACK，避免两端互相确认到死
        if payload.startswith("LINK:OK") and LAST_SEQ["seen"]:
            # STM32 重启后序号从 0 重新开始。不清掉旧记录的话，
            # 重启后的第一条事件有可能正好撞上上一轮的最后一个 SEQ，
            # 被当成"重传"丢掉——设备复位一次，丢一条刷卡记录。
            LAST_SEQ["seen"] = False
            LAST_SEQ["value"] = None
        if LAST_SEQ["seen"] and seq == LAST_SEQ["value"]:
            RX_STATS["duplicates"] += 1
            _ack(ser, seq)              # 重复帧：补一个 ACK，但不再执行
            return
        LAST_SEQ["seen"] = True
        LAST_SEQ["value"] = seq
        RX_STATS["frames"] += 1
        _ack(ser, seq)
        line = payload
    else:
        RX_STATS["legacy"] += 1         # 老固件发的裸文本

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

    elif kind == "spot_fault":
        # 传感器故障单独上报：不能退化成 "occupied=False"，
        # 否则云端看到的是"车位空了"，调度会把它派给下一辆车。
        queue_property(client, device_id, "Spot_state",
                       {"Parking_id": str(msg["spot_id"]), "Parking_state": "fault"})
        append_event({"ts": time.strftime("%Y-%m-%d %H:%M:%S"),
                      "action": "sensor_fault", "spot_id": msg["spot_id"]})

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


# 云命令队列：paho 的 on_message 跑在它自己的网络线程里。
# 原来这个回调里直接等串口 ACK（最多 5 秒），期间 paho 收不到任何东西，
# keepalive 受影响、其他命令排队、断线重连也被拖慢。
# 现在回调只做"解析 + 入队"，真正的执行在 command_worker 里。
CMD_Q = queue.Queue(maxsize=64)


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        return
    if "request_id=" not in topic:
        print("[GW] msg: %s -> %s" % (topic, msg.payload[:200]), flush=True)
        return
    try:
        CMD_Q.put_nowait((topic, payload))
    except queue.Full:
        print("[GW] command queue full, drop: %s" % topic, flush=True)


def execute_command(client, cfg, topic, payload):
    """真正执行一条云命令（在 command_worker 线程里）。"""
    request_id = topic.rsplit("request_id=", 1)[-1]
    cmd_name = payload.get("command_name", "")
    service_id = payload.get("service_id", "")
    paras = payload.get("paras", {})
    print("[GW] command: %s/%s paras=%s" % (service_id, cmd_name, paras), flush=True)

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


def command_worker(client, cfg):
    """唯一的云命令执行线程：串行执行，天然避免两条命令抢串口。"""
    while True:
        topic, payload = CMD_Q.get()
        try:
            execute_command(client, cfg, topic, payload)
        except Exception as e:
            print("[GW] command error: %s" % e, flush=True)
        finally:
            CMD_Q.task_done()


UART_LINE_MAX = 512


def split_uart_lines(buf, chunk, limit=UART_LINE_MAX):
    """把新到的字节并入行缓冲，切出"完整行"。

    返回 (剩余缓冲, 完整行列表, 被丢弃的超长行数)。

    为什么必须攒到换行：readline() 超时会返回"半行"（没有 \\n）。半行直接
    交给解析器，等于把 "BARRIER:OP" 当成一条未知命令，而真正的
    "BARRIER:OPEN" 的下半截永远拼不回来——一次开闸就这么丢了。
    超长（超过 limit 还没有换行）说明链路在灌噪声，整段丢弃重新同步，
    免得缓冲区无限增长变成一个慢性的内存泄漏。
    """
    buf = buf + (chunk or b"")
    lines = []
    dropped = 0
    while True:
        nl = buf.find(b"\n")
        if nl < 0:
            break
        line, buf = buf[:nl + 1], buf[nl + 1:]
        if len(line) > limit:
            dropped += 1
            continue
        lines.append(line)
    if len(buf) > limit:
        dropped += 1
        buf = b""
    return buf, lines, dropped


def watch_serial(client, cfg):
    port = cfg.get("serial_port", "/dev/serial0")
    baud = int(cfg.get("serial_baud", 115200))
    if serial is None:
        print("[GW] pyserial not installed, UART disabled", flush=True)
        return
    ser = None
    rx_buf = b""
    while True:
        try:
            if ser is None:
                ser = serial.Serial(port, baud, timeout=0.5)
                cfg["_serial"] = ser
                print("[GW] serial %s opened @%d" % (port, baud), flush=True)
            raw = ser.readline()
            if not raw:
                continue
            rx_buf, lines, dropped = split_uart_lines(rx_buf, raw)
            if dropped:
                print("[GW] UART dropped %d oversized line(s)" % dropped, flush=True)
            for raw_line in lines:
                # 优先 UTF-8；不合法就退回 latin-1（字节透明，绝不丢字节），
                # 否则一个干扰字节就让整帧 CRC 对不上、白白丢掉一条命令。
                try:
                    line = raw_line.decode("utf-8")
                except UnicodeDecodeError:
                    line = raw_line.decode("latin-1")
                handle_stm32_line(ser, client, cfg, line)
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


def resolve_ca_path(cfg):
    """返回可用的 CA 证书路径；缺失直接抛错。

    绝不降级成 CERT_NONE：那等于把 MQTTS 变成"加密但不验身份"，中间人
    可以随便冒充 broker，设备密钥会被直接送出去。宁可不启动，也不静默降级。
    """
    ca = (cfg.get("ca_cert") or "").strip()
    if not ca:
        # 空字符串会被 os.path.join 拼成"代码目录"本身，exists() 还会返回
        # True——于是拿着一个目录去 tls_set，最后在握手阶段报一个跟证书
        # 毫无关系的错。这里显式拦掉。
        raise RuntimeError("cloud_config.json 未配置 ca_cert，拒绝以无校验方式连接云端。")
    if not os.path.isabs(ca):          # 绝对路径直接用；相对路径按代码目录解析
        ca = os.path.join(BASE, ca)
    if not os.path.exists(ca):
        raise RuntimeError("CA 证书缺失：%s。拒绝以无校验方式连接云端。" % ca)
    return ca


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
        client.tls_set(ca_certs=resolve_ca_path(cfg))
    client.user_data_set(cfg)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    threading.Thread(target=publish_worker, args=(client, cfg), daemon=True).start()
    threading.Thread(target=command_worker, args=(client, cfg), daemon=True).start()
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
