Raspberry Pi (Zero WH) - Parking Gateway
project: ~/parking_gateway        (部署目录与用户名按现场填写，不写进版本库)

Current files (deployed on Pi):
  plate_gateway.py      integrated gateway: MQTT cloud + UART + camera OCR
                        + vehicle registry + plate/card auto-open (OR) + event log
  stm32_protocol.py     STM32 line protocol + framed codec (CRC-8 / SEQ / ACK)
  passage.py            single-passage session (TTL) + credential resolution
  plate_recognition.py  OCR pipeline (patched, uses plate_matcher)
  plate_matcher.py      plate fuzzy matcher (weighted edit distance, reads vehicles.json)
  plate_config.py       config loading + rpicam-still command builder
  vehicles.json         vehicle registry: plate + bound cards + owner (authoritative)
  events.jsonl          passage event log (runtime, auto-created)
  cloud_config.json     Huawei IoTDA MQTT connection config (NO password inside)
  cloud_secret.json     device password, chmod 600, NOT in git
                        (template: cloud_secret.example.json)
  plate_camera.py       manual capture -> recognize tool
  huawei_iotda_product_model.json  IoTDA product model export (reference)
  deploy/               systemd unit + install.sh
  tests/                unit tests (122, no camera / serial / network needed)

test/     sample images: test.jpg -> T234J5, test2.jpg -> EM456U, test3.jpg -> K4Z567
archive/  superseded/historical: pi_cloud.py (old MQTT-only), patch_plate_recognition.py,
          plate_recognition_prepatch.py, README_20260818.txt

Run on Pi (managed by systemd, NOT nohup):
  sudo systemctl status plate-gateway
  journalctl -u plate-gateway -f
  sudo systemctl restart plate-gateway

Foreground debug:
  cd ~/parking_gateway && source .venv/bin/activate && python3 plate_gateway.py
  python3 -m unittest discover -s tests -t .

Trigger (simulates STM32 signal):
  echo capture > /tmp/plate_cmd
