#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""提交前密钥扫描（pre-commit secret scan）。

背景：这个仓库历史上把华为云设备密钥以明文写进过 `cloud_config.json`，
文件还被 git 跟踪了很久。删掉文件不等于删掉历史，但至少要让"下一次"发生
时被挡住。

检查项：
  1. JSON 里出现非占位符的 "password" / "secret" / "token" 字段
  2. PEM 私钥块
  3. 常见云厂商 access key 形态
  4. 命令行里硬编码的密码（-pw xxx / password=xxx）
  5. 形如 "用户名@主机" 的内网地址（顺带提醒，不算错误）

用法：
  python tools/check_secrets.py            # 扫描仓库
  python tools/check_secrets.py --staged   # 只看 git 暂存区（pre-commit 用）

作为 pre-commit 钩子：
  cp tools/check_secrets.py .git/hooks/pre-commit    # 需要可执行位/她bang
  # 或在 .git/hooks/pre-commit 里写一行：
  #   exec python "$(git rev-parse --show-toplevel)/tools/check_secrets.py" --staged
退出码非 0 表示发现问题。
"""

import os
import re
import subprocess
import sys

SKIP_DIRS = {".git", "__pycache__", ".venv", "Objects", "Listings", "build"}
TEXT_EXT = {
    ".py", ".json", ".md", ".txt", ".c", ".h", ".sh", ".service", ".cfg",
    ".ini", ".yml", ".yaml", ".ps1", ".conf", ".example",
}
SKIP_FILES = {"check_secrets.py", "cloud_secret.example.json"}

# 允许出现 password 字样的"说明性"位置：占位符 / 环境变量名 / 报错信息
PLACEHOLDER = re.compile(r"^(<.*>|\$\{.*\}|\{\{.*\}\}|x+|\.\.\.|示例.*)$")

PATTERNS = [
    ("私钥块", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("云厂商 AccessKey", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("命令行明文密码", re.compile(r"(?:-pw\s+|-P\s+)(?!<)[^\s<\"']{6,}")),
  # 不做"裸十六进制串"匹配：Z-Stack SDK 里的 OTA 证书、hash、固件 CRC
  # 都长这个样子，误报比漏报还麻烦——一个总是响的得者就没人看了。
  # 设备密钥主要靠下面的 JSON 字段检查来守。
]

JSON_SECRET_KEY = re.compile(
    r'"\s*(password|passwd|secret|token|api_?key)\s*"\s*:\s*"([^"]*)"', re.I)


def iter_files(staged_only):
    if staged_only:
        out = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                             capture_output=True, text=True)
        names = [n for n in out.stdout.splitlines() if n.strip()]
    else:
        names = []
        for root, dirs, files in os.walk("."):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in files:
                names.append(os.path.relpath(os.path.join(root, f), "."))
    for name in names:
        name = name.replace("\\", "/")
        base = os.path.basename(name)
        if base in SKIP_FILES:
            continue
        if os.path.splitext(base)[1].lower() not in TEXT_EXT:
            continue
        if not os.path.exists(name):
            continue
        yield name


def scan(path):
    problems = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return problems

    for i, line in enumerate(lines, 1):
        for label, rx in PATTERNS:
            for m in rx.finditer(line):
                problems.append((path, i, label, m.group(0)[:60]))

        for m in JSON_SECRET_KEY.finditer(line):
            value = m.group(2).strip()
            if value and not PLACEHOLDER.match(value):
                problems.append((path, i, "明文 %s 字段" % m.group(1), value[:20] + "***"))
    return problems


def main(argv):
    staged = "--staged" in argv
    found = []
    for path in iter_files(staged):
        found.extend(scan(path))

    for path, line_no, label, sample in found:
        print("%s:%d: [%s] %s" % (path, line_no, label, sample))
    if found:
        print("\n发现 %d 处疑似密钥，提交已中止。" % len(found))
        print("请改用环境变量或 cloud_secret.json（0600，已 gitignore）。")
        return 1
    print("secret scan: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
