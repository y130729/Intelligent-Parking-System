#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CI 配置本身的不变量检查。

没有 PyYAML（也不想为一个文件引依赖），所以这里不解析 YAML 语法，
只盯住"改一行就悄悄失效"的那几处：矩阵漏了 Windows、某个 run 步骤
写成了 bash 专有语法、README 的徽章指向了不存在的 workflow。

语法错误由 GitHub 在推送时报出来；这里负责的是语义漂移。
"""

import os
import re
import unittest

import common

CI_PATH = os.path.join(common.REPO_ROOT, ".github", "workflows", "ci.yml")


class CiFileTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(os.path.exists(CI_PATH), "CI 工作流文件不见了：.github/workflows/ci.yml")
        self.text = common.read(CI_PATH)
        self.lines = self.text.splitlines()

    def test_no_tabs_and_lf_endings(self):
        """YAML 不允许制表符缩进；CRLF 混入会让 diff 和部分工具报错。"""
        for i, line in enumerate(self.lines, 1):
            self.assertNotIn("\t", line, "第 %d 行有制表符，YAML 只能用空格缩进" % i)
        self.assertNotIn("\r", self.text, "ci.yml 出现了 CRLF，仓库统一用 LF")

    def test_triggers_cover_push_and_pr(self):
        self.assertRegex(self.text, r"(?m)^on:")
        self.assertIn("push:", self.text)
        self.assertIn("pull_request:", self.text)

    def test_matrix_covers_both_platforms_and_pythons(self):
        for os_name in ("ubuntu-latest", "windows-latest"):
            self.assertIn(os_name, self.text, "CI 矩阵里缺 %s" % os_name)
        for version in ('"3.9"', '"3.12"'):
            self.assertIn(version, self.text, "CI 矩阵里缺 Python %s" % version)
        self.assertIn("fail-fast: false", self.text,
                      "少了 fail-fast: false，一个平台挂了会隐藏另一个平台的结果")

    def test_run_steps_are_plain_python(self):
        """CI 只有 python 命令：不装依赖、不写 bash 专有语法，Windows 才能跑。"""
        runs = re.findall(r"(?m)^\s*run:\s*(.+)$", self.text)
        self.assertTrue(runs, "ci.yml 里没有任何 run: 步骤")
        for cmd in runs:
            self.assertTrue(cmd.startswith("python "), "run 步骤不是 python 命令：%s" % cmd)
        for banned in ("&&", "||", "$(", "| "):
            self.assertNotIn(banned, " ".join(runs),
                             "run 步骤里用了 shell 专有语法 %r，Windows/macOS 行为不一致" % banned)

    def test_readme_badge_points_to_this_workflow(self):
        readme = common.read(common.README_PATH)
        match = re.search(r"actions/workflows/([\w.-]+)/badge\.svg", readme)
        self.assertIsNotNone(match, "README 顶部找不到 CI 徽章")
        badge_file = match.group(1)
        self.assertTrue(os.path.exists(os.path.join(common.REPO_ROOT, ".github", "workflows", badge_file)),
                        "README 徽章指向 %s，但这个文件不存在" % badge_file)


if __name__ == "__main__":
    unittest.main()
