Raspberry Pi (Zero WH) - Parking Gateway
Host: 10.172.83.46  user: zny  project: ~/parking_gateway
Backup consolidated: 2026-08-19

Current files (deployed on Pi):
  plate_gateway.py      integrated gateway: MQTT cloud + FIFO trigger + camera OCR
                        + vehicle registry + plate/card auto-open (OR) + event log
  plate_recognition.py  OCR pipeline (patched, uses plate_matcher)
  plate_matcher.py      plate fuzzy matcher (weighted edit distance, reads vehicles.json)
  vehicles.json         vehicle registry: plate + bound cards + owner (authoritative)
  events.jsonl          passage event log (runtime, auto-created)
  cloud_config.json     Huawei IoTDA MQTT connection config (MQTTS 8883)
  plate_camera.py       manual capture -> recognize tool
  huawei_iotda_product_model.json  IoTDA product model export (reference)

test/     sample images: test.jpg -> T234J5, test2.jpg -> EM456U, test3.jpg -> K4Z567
archive/  superseded/historical: pi_cloud.py (old MQTT-only), patch_plate_recognition.py,
          plate_recognition_prepatch.py, README_20260818.txt

Run on Pi:
  cd ~/parking_gateway && source .venv/bin/activate
  nohup python3 plate_gateway.py >/tmp/gateway.log 2>&1 &

Trigger (simulates STM32 signal):
  echo capture > /tmp/plate_cmd