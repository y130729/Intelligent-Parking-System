#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""一条命令跑完全部"不需要硬件"的检查。

为什么要有这个文件：README 里写四行命令，评审者只会照抄第一行。
把顺序、工作目录、退出码都固化下来，本地和 CI 跑的就一定是同一套。

  python tools/run_tests.py          # 全跑
  python tools/run_tests.py --list   # 只列步骤

四步都只依赖 Python 标准库：
  1. 树莓派纯逻辑单测（帧编解码、会话、网关线程模型，带硬件桩）
  2. 固件静态/一致性测试（静态规则、Flash 分区、协议向量、文档同步）
  3. 固件静态检查（5 条项目规约规则，可独立运行）
  4. 密钥扫描（防止把密码/私钥提交进仓库）
"""

import argparse
import os
import subprocess
import sys
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STEPS = [
    ("树莓派逻辑单测",
     [sys.executable, "-m", "unittest", "discover",
      "-s", os.path.join("raspberrypi", "tests"), "-t", "raspberrypi"]),
    ("固件静态/一致性测试",
     [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-t", "tests"]),
    ("固件静态检查",
     [sys.executable, os.path.join("tools", "fw_lint.py")]),
    ("密钥扫描",
     [sys.executable, os.path.join("tools", "check_secrets.py")]),
]


def main(argv=None):
    parser = argparse.ArgumentParser(description="跑完所有无硬件检查")
    parser.add_argument("--list", action="store_true", help="只打印步骤")
    args = parser.parse_args(argv)

    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    if args.list:
        for name, cmd in STEPS:
            print("%-24s %s" % (name, " ".join(cmd[1:])))
        return 0

    env = dict(os.environ)
    env.setdefault("PYTHONIOENCODING", "utf-8")
    env["PYTHONDONTWRITEBYTECODE"] = "1"

    results = []
    for name, cmd in STEPS:
        print("\n=== %s ===" % name, flush=True)
        started = time.time()
        proc = subprocess.run(cmd, cwd=REPO_ROOT, env=env)
        elapsed = time.time() - started
        results.append((name, proc.returncode, elapsed))
        if proc.returncode != 0:
            print("--- %s 失败（exit=%d）" % (name, proc.returncode), flush=True)

    print("\n=== 汇总 ===")
    failed = 0
    for name, code, elapsed in results:
        status = "PASS" if code == 0 else "FAIL"
        if code != 0:
            failed += 1
        print("%-24s %s  %.1fs" % (name, status, elapsed))

    print("\n%d/%d 通过" % (len(results) - failed, len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
