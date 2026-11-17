#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成 tests/vectors.json（协议黄金样本）。

**只在协议有意变更时运行**，并且必须同时改 STM32 的 link_frame.c。
这个脚本存在的意义是：让"样本从哪来"可追溯，而不是有人手工抄一串十六进制。

用法：python tools/gen_protocol_vectors.py
"""

import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "raspberrypi"))

import stm32_protocol as sp   # noqa: E402

OUT = os.path.join(REPO_ROOT, "tests", "vectors.json")

CASES = [
    ("open_card", 0x05, "OPEN:card"),
    ("open_plate", 0x06, "OPEN:plate"),
    ("close_remote", 0x07, "CLOSE:remote"),
    ("plate_chinese", 0x2A, "PLATE:京A00001"),
    ("spot_fault", 0xFF, "SPOT:7:FAULT"),
    ("link_ok", 0x00, "LINK:OK"),
]


def main():
    vectors = []
    for name, seq, payload in CASES:
        frame = sp.encode_frame(seq, payload)
        if frame is None:
            raise SystemExit("编码失败：%s" % payload)
        raw = frame.encode("latin-1")
        vectors.append({
            "name": name,
            "seq": seq,
            "payload": payload,
            "len": len(payload.encode("utf-8")),
            "hex": raw.hex().upper(),
            "ascii": "".join(chr(b) if 32 <= b < 127 else "." for b in raw),
        })

    doc = {
        "spec": ("'#' VER SEQ(2 hex) LEN(2 hex) PAYLOAD '*' CRC(2 hex)，"
                 "CRC-8/ATM(poly=0x07, init=0x00) 覆盖 VER..PAYLOAD"),
        "version": sp.FRAME_VER,
        "max_payload": sp.FRAME_MAX_PAYLOAD,
        "_comment": "由 tools/gen_protocol_vectors.py 生成，勿手工编辑",
        "vectors": vectors,
    }
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(doc, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print("写入 %s（%d 条）" % (OUT, len(vectors)))


if __name__ == "__main__":
    main()
