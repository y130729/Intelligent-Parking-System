#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
车牌识别 - 树莓派 Zero WH 优化版
流程: HSV 蓝色掩码 -> 候选轮廓(宽高比≈3.15) -> minAreaRect 透视校正
      -> CLAHE -> Tesseract LSTM (psm 8 -> 13 -> 7, 白名单 A-Z0-9)
      -> 校正图 OCR 为空时回退识别蓝色包围盒裁剪
后处理: 在 6 位子串中按车牌特征打分选优(保护城市字母), 再修正 T/Z 误读
用法: python3 plate_recognition.py [--debug] <图片1> [图片2 ...]
"""
import os, re, sys, time
import plate_format
import plate_matcher
import cv2, numpy as np, pytesseract

WHITELIST = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
PSM_ORDER = (8, 13, 7)          # psm8/13 最稳, psm7 兜底
ASPECT_TARGET = 3.15            # 标准车牌宽高比
ASPECT_RANGE = (2.0, 4.0)       # 允许范围
# 候选面积/宽度按图像尺寸取比例，避免换分辨率后阈值失效：
#   2592x1944 下 MIN_AREA_RATIO 对应原来的 5000 像素
#   2592 宽下 MIN_WIDTH_RATIO 对应原来的 60 像素
MIN_AREA_RATIO = 0.00099
MIN_WIDTH_RATIO = 0.023
OCR_MAXW = 600


def clean_text(t):
    return re.sub(r"[^A-Z0-9]", "", (t or "").upper())


def find_plate_candidates(img):
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV)
    mask = cv2.inRange(hsv, (90, 60, 60), (135, 255, 255))
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((15, 7), np.uint8))
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    img_h, img_w = img.shape[:2]
    min_area = MIN_AREA_RATIO * img_w * img_h
    min_w = MIN_WIDTH_RATIO * img_w
    cands = []
    for c in contours:
        area = cv2.contourArea(c)
        if area < min_area:
            continue
        (_, _), (rw, rh), _ = cv2.minAreaRect(c)
        if rw < rh:
            rw, rh = rh, rw
        if rh <= 0:
            continue
        aspect = rw / rh
        if not (ASPECT_RANGE[0] <= aspect <= ASPECT_RANGE[1]):
            continue
        if rw < min_w or rh < min_w / 3.0:
            continue
        cands.append((abs(aspect - ASPECT_TARGET), -area, c))
    cands.sort(key=lambda t: (t[0], t[1]))
    return [c for _, _, c in cands[:2]]


def warp_plate(img, contour):
    rect = cv2.minAreaRect(contour)
    box = cv2.boxPoints(rect)
    pts = box.astype(np.float32)
    s = pts.sum(axis=1)
    d = np.diff(pts, axis=1).ravel()
    src = np.float32([pts[np.argmin(s)], pts[np.argmin(d)],
                      pts[np.argmax(s)], pts[np.argmax(d)]])
    (_, _), (rw, rh), _ = rect
    if rw < rh:
        rw, rh = rh, rw
    W, H = max(int(rw), 60), max(int(rh), 20)
    dst = np.float32([[0, 0], [W - 1, 0], [W - 1, H - 1], [0, H - 1]])
    M = cv2.getPerspectiveTransform(src, dst)
    return cv2.warpPerspective(img, M, (W, H), flags=cv2.INTER_CUBIC)


def preprocess(roi):
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY) if roi.ndim == 3 else roi
    gray = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(gray)
    h, w = gray.shape[:2]
    if w > OCR_MAXW:
        s = OCR_MAXW / w
        gray = cv2.resize(gray, (OCR_MAXW, max(1, int(h * s))),
                          interpolation=cv2.INTER_CUBIC)
    return gray


def ocr_line(proc, psms=PSM_ORDER):
    base = "--oem 1 -c tessedit_char_whitelist=%s" % WHITELIST
    for psm in psms:
        t0 = time.time()
        text = clean_text(pytesseract.image_to_string(
            proc, lang="eng", config="%s --psm %d" % (base, psm)))
        if text:
            return text, psm, time.time() - t0
    return "", None, None


def ocr_plate_crop(img, contour):
    """先识别透视校正图; 为空则回退识别轴对齐蓝色包围盒."""
    warp = warp_plate(img, contour)
    text, psm, dt = ocr_line(preprocess(warp))
    if text:
        return text, psm, dt
    x, y, w, h = cv2.boundingRect(contour)
    x0, y0 = max(0, x - int(w * 0.03)), max(0, y - int(h * 0.08))
    x1, y1 = min(img.shape[1], x + w + int(w * 0.03)), min(img.shape[0], y + h + int(h * 0.08))
    return ocr_line(preprocess(img[y0:y1, x0:x1]))


def post_process(text):
    """OCR 文本 -> 规范化的车牌串（6 位燃油 / 7 位新能源）。

    规则集中在 plate_format（中国车牌结构语义），不再硬编码
    "首位 1/7/8 改成 T"、"序号 7 改成 Z" 这类只对某一块测试车牌成立的
    替换——那会把合法车牌改坏。首位到底属于哪个字母属于"消歧"，
    交由车辆库匹配（plate_matcher）结合库内车牌决定。
    """
    return plate_format.post_process(text)


def main():
    db = plate_matcher.load_db()
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    debug = "--debug" in sys.argv
    if not args:
        raise SystemExit("用法: python3 plate_recognition.py [--debug] <图片1> [图片2 ...]")
    for path in args:
        img = cv2.imread(path)
        if img is None:
            print("[%s] 无法读取图片" % path)
            continue
        t_start = time.time()
        cands = find_plate_candidates(img)
        print("[%s] %dx%d 候选 %d 个" % (path, img.shape[1], img.shape[0], len(cands)))
        best = ""
        for i, c in enumerate(cands):
            if debug:
                base = os.path.splitext(os.path.basename(path))[0]
                cv2.imwrite("%s_cand%d.jpg" % (base, i), warp_plate(img, c))
            text, psm, dt = ocr_plate_crop(img, c)
            if text:
                fixed = post_process(text)
                full, score, hit = plate_matcher.fix_plate(fixed, db)
                if hit:
                    print("  -> plate_db match: %s (sim %.2f)" % (full, score))
                    fixed = full
                print("  候选%d: OCR=%s (psm %d, %.1fs) -> %s" % (i + 1, text, psm, dt, fixed))
                if len(text) >= 6 and len(fixed) == 6:
                    best = fixed
                    break
                if not best:
                    best = fixed
        print("  [最终] %s  总耗时 %.1fs" % (best or "(未识别)", time.time() - t_start))


if __name__ == "__main__":
    main()
