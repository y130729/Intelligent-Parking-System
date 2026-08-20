#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""拍照 -> 车牌识别 -> 打印结果 (树莓派 Zero WH)
依赖: rpicam-still(已可用) + plate_recognition.py(同目录)
用法: source .venv/bin/activate && python3 plate_camera.py [输出图片]
"""
import os, subprocess, sys, time

import plate_recognition as pr

CAM_FILE = "/tmp/plate_cam.jpg"
CAM_W, CAM_H = 2592, 1944   # 与测试图同分辨率


def capture(out_path):
    cmd = ["rpicam-still", "-o", out_path, "--width", str(CAM_W),
           "--height", str(CAM_H), "--timeout", "3000", "-n"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    if not os.path.exists(out_path):
        raise SystemExit("拍照失败: %s" % ((r.stderr or r.stdout) or "")[-500:])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else CAM_FILE
    print("拍照中...", flush=True)
    t0 = time.time()
    capture(out)
    print("已拍照: %s (%.1fs)" % (out, time.time() - t0), flush=True)

    img = pr.cv2.imread(out)
    if img is None:
        raise SystemExit("无法读取拍摄图片: %s" % out)
    print("图片尺寸: %dx%d" % (img.shape[1], img.shape[0]), flush=True)

    t1 = time.time()
    cands = pr.find_plate_candidates(img)
    print("候选 %d 个" % len(cands), flush=True)
    best = ""
    for i, c in enumerate(cands):
        text, psm, dt = pr.ocr_plate_crop(img, c)
        if text:
            fixed = pr.post_process(text)
            print("  候选%d: OCR=%s (psm %d, %.1fs) -> %s" % (i + 1, text, psm, dt, fixed), flush=True)
            if len(text) >= 6 and len(fixed) == 6:
                best = fixed
                break
            if not best:
                best = fixed
    print("识别结果: %s  识别耗时 %.1fs" % (best or "(未识别)", time.time() - t1), flush=True)


if __name__ == "__main__":
    main()
