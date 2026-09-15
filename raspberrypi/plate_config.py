#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""配置加载与拍照命令构造。

被 plate_gateway.py（网关）与 plate_camera.py（单张测试工具）共用，
保证调试工具和线上服务用的是同一套拍照参数。

拍照参数集中在 cloud_config.json 的 "camera" 段，现场可以只改配置、
不改代码：

    "camera": {
        "width": 2592,          // 分辨率
        "height": 1944,
        "timeout_ms": 200,      // 曝光/白平衡收敛时间（原来的 3000ms 太长：
                                // 车在闸前不会静止等 3 秒，等它"稳定"车已经动了）
        "immediate": false,     // true 时跳过 AE/AWB 收敛直接抓拍（更快但更暗）
        "metering": "centre",   // 按"抓拍"而不是"摄影"测光
        "extra_args": []        // 需要时透传 rpicam 参数，例如 ["--gain","4"]
    }
"""

import copy
import json
import os

BASE = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(BASE, "cloud_config.json")

DEFAULT_CAMERA = {
    "width": 2592,
    "height": 1944,
    "timeout_ms": 200,
    "immediate": False,
    "metering": "centre",
    "extra_args": [],
    # 抓拍重试次数（原来 3 次 x 90s 超时，最坏情况占用近 5 分钟）
    "attempts": 2,
    # 单次 rpicam-still 的超时上限与重试间隔
    "command_timeout_s": 15,
    "retry_delay_s": 1,
}


def load_config(path=None):
    """读取 cloud_config.json。"""
    with open(path or CFG_PATH, encoding="utf-8") as f:
        return json.load(f)


def camera_settings(cfg=None):
    """取相机配置，缺失项回落到默认值。"""
    settings = copy.deepcopy(DEFAULT_CAMERA)
    settings.update((cfg or {}).get("camera") or {})
    return settings


def camera_command(out_path, cfg=None):
    """构造 rpicam-still 命令行。"""
    cam = camera_settings(cfg)
    cmd = [
        "rpicam-still",
        "-o", out_path,
        "-n",                                    # 无预览（无头运行）
        "--width", str(int(cam["width"])),
        "--height", str(int(cam["height"])),
        "--timeout", str(int(cam["timeout_ms"])),
    ]
    if cam.get("metering"):
        cmd += ["--metering", str(cam["metering"])]
    if cam.get("immediate"):
        cmd.append("--immediate")
    cmd += [str(a) for a in (cam.get("extra_args") or [])]
    return cmd
