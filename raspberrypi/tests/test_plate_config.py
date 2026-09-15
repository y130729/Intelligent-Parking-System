#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""拍照命令构造单元测试。"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import plate_config as pc


class TestCameraSettings(unittest.TestCase):
    def test_defaults_when_config_missing_camera_section(self):
        cam = pc.camera_settings({})
        self.assertEqual(cam["timeout_ms"], pc.DEFAULT_CAMERA["timeout_ms"])
        self.assertEqual(cam["width"], pc.DEFAULT_CAMERA["width"])

    def test_config_overrides_defaults(self):
        cam = pc.camera_settings({"camera": {"timeout_ms": 1000, "immediate": True}})
        self.assertEqual(cam["timeout_ms"], 1000)
        self.assertTrue(cam["immediate"])
        self.assertEqual(cam["width"], pc.DEFAULT_CAMERA["width"])

    def test_defaults_are_not_mutated(self):
        pc.camera_settings({"camera": {"width": 640}})
        self.assertEqual(pc.DEFAULT_CAMERA["width"], 2592)


class TestCameraCommand(unittest.TestCase):
    def test_builds_short_timeout_command(self):
        """回归：拍照不应再固定等 3000ms。"""
        cmd = pc.camera_command("/tmp/x.jpg", {})
        self.assertEqual(cmd[0], "rpicam-still")
        idx = cmd.index("--timeout")
        self.assertEqual(cmd[idx + 1], "200")
        self.assertIn("-n", cmd)

    def test_no_immediate_by_default(self):
        self.assertNotIn("--immediate", pc.camera_command("/tmp/x.jpg", {}))

    def test_immediate_and_metering_from_config(self):
        cfg = {"camera": {"immediate": True, "metering": "spot"}}
        cmd = pc.camera_command("/tmp/x.jpg", cfg)
        self.assertIn("--immediate", cmd)
        self.assertEqual(cmd[cmd.index("--metering") + 1], "spot")

    def test_extra_args_passthrough(self):
        cfg = {"camera": {"extra_args": ["--gain", "4"]}}
        cmd = pc.camera_command("/tmp/x.jpg", cfg)
        self.assertEqual(cmd[cmd.index("--gain") + 1], "4")

    def test_blank_metering_is_omitted(self):
        cmd = pc.camera_command("/tmp/x.jpg", {"camera": {"metering": ""}})
        self.assertNotIn("--metering", cmd)


if __name__ == "__main__":
    unittest.main(verbosity=2)
