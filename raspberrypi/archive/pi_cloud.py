#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Raspberry Pi -> Huawei Cloud IoTDA connection script (MQTTS).

Reads cloud_config.json in the same directory, then:
  - subscribes to the command topic and auto-replies result=success
  - reports a test property on connect if "test_report" is present in config

Usage:
  python3 pi_cloud.py                                   # foreground / debug
  nohup python3 pi_cloud.py >/tmp/pi_cloud.log 2>&1 &   # background
"""
import json
import os
import ssl
import time

import paho.mqtt.client as mqtt

BASE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(BASE, "cloud_config.json")

CMD_SUB_TOPIC = "$oc/devices/{}/sys/commands/#"
CMD_RESP_TOPIC = "$oc/devices/{}/sys/commands/response/request_id={}"
PROP_REPORT_TOPIC = "$oc/devices/{}/sys/properties/report"


def load_config():
    with open(CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def report_property(client, device_id, service_id, properties):
    """Report a property, e.g. report_property(c, dev, "PlateRecognition", {"plate_number": "T234J5"})."""
    payload = {"services": [{"service_id": service_id, "properties": properties}]}
    topic = PROP_REPORT_TOPIC.format(device_id)
    client.publish(topic, json.dumps(payload, ensure_ascii=False))
    print("[IoTDA] property reported: %s -> %s" % (service_id, properties), flush=True)


def on_connect(client, userdata, flags, rc):
    if rc != 0:
        print("[IoTDA] connect failed, rc=%d" % rc, flush=True)
        return
    cfg = userdata
    device_id = cfg["device_id"]
    sub_topic = CMD_SUB_TOPIC.format(device_id)
    client.subscribe(sub_topic, qos=0)
    print("[IoTDA] connected, subscribed: %s" % sub_topic, flush=True)
    tr = cfg.get("test_report")
    if tr:
        report_property(client, device_id, tr["service_id"], tr["properties"])


def on_disconnect(client, userdata, rc):
    print("[IoTDA] disconnected rc=%s, auto-reconnecting..." % rc, flush=True)


def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except Exception:
        print("[IoTDA] non-JSON msg: %s" % msg.payload[:200], flush=True)
        return
    if "request_id=" in topic:
        request_id = topic.rsplit("request_id=", 1)[-1]
        cmd_name = payload.get("command_name", "")
        service_id = payload.get("service_id", "")
        paras = payload.get("paras", {})
        print("[IoTDA] command: %s/%s paras=%s" % (service_id, cmd_name, paras), flush=True)
        resp = {"result_code": 0, "response_name": cmd_name,
                "paras": {"result": "success"}}
        resp_topic = CMD_RESP_TOPIC.format(userdata["device_id"], request_id)
        client.publish(resp_topic, json.dumps(resp, ensure_ascii=False))
    else:
        print("[IoTDA] msg: %s -> %s" % (topic, msg.payload[:200]), flush=True)


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
            print("[IoTDA] WARN: CA cert missing, skipping server cert verify", flush=True)
    client.user_data_set(cfg)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    print("[IoTDA] connecting %s:%d (device=%s)" % (cfg["hostname"], cfg["port"], cfg["device_id"]), flush=True)
    while True:
        try:
            client.connect(cfg["hostname"], cfg["port"], keepalive=60)
            break
        except Exception as e:
            print("[IoTDA] connect failed: %s, retry in 5s" % e, flush=True)
            time.sleep(5)
    client.loop_forever()


if __name__ == "__main__":
    main()