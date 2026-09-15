#!/usr/bin/env bash
# 把 plate_gateway.py 装成 systemd 服务（开机自启 + 崩溃自动重启 + 日志进 journald）。
#
# 用法：
#     sudo ./install.sh [APP_ROOT] [SERVICE_USER]
#     APP_ROOT      代码根目录（内含 raspberrypi/），默认 /opt/parking
#     SERVICE_USER  运行用户，默认当前登录用户
#
# 例：
#     sudo ./raspberrypi/deploy/install.sh /opt/parking pi
#
# 先决条件（树莓派 Zero 上 OpenCV 用 apt 装，pip 编译不动）：
#     sudo apt install -y python3-venv python3-opencv tesseract-ocr
#
# 注意：本脚本会建新 venv 并 pip install，**需要外网**。
# 如果这台树莓派没有外网（现场机器常见），不要用本脚本，
# 改为复用已有 venv 做原地升级 —— 步骤见 Readme「原地升级」一节。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_ROOT="${1:-/opt/parking}"
SERVICE_USER="${2:-$(id -un)}"
VENV_DIR="$APP_ROOT/.venv"
GATEWAY="$APP_ROOT/raspberrypi/plate_gateway.py"
UNIT_NAME="plate-gateway.service"
UNIT_SRC="$SCRIPT_DIR/$UNIT_NAME"
UNIT_DST="/etc/systemd/system/$UNIT_NAME"

if [[ $EUID -ne 0 ]]; then
    echo "请用 sudo 运行（要写 /etc/systemd/system）" >&2
    exit 1
fi

if [[ ! -f "$GATEWAY" ]]; then
    echo "找不到 $GATEWAY" >&2
    echo "请先把代码复制到 $APP_ROOT（例如：sudo cp -r raspberrypi $APP_ROOT/）" >&2
    exit 1
fi

if [[ ! -f "$APP_ROOT/raspberrypi/cloud_config.json" ]]; then
    echo "缺少 cloud_config.json（华为云设备凭据），请先放好再安装" >&2
    exit 1
fi

if ! id "$SERVICE_USER" >/dev/null 2>&1; then
    echo "用户 $SERVICE_USER 不存在" >&2
    exit 1
fi

echo "==> 检查依赖"
for pkg in python3-venv python3-opencv tesseract-ocr; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "    警告：未检测到 $pkg，建议 sudo apt install -y $pkg"
    fi
done
command -v rpicam-still >/dev/null 2>&1 || echo "    警告：未找到 rpicam-still（相机工具）"

echo "==> 创建虚拟环境 $VENV_DIR"
if [[ ! -x "$VENV_DIR/bin/python3" ]]; then
    # --system-site-packages：复用 apt 装的 cv2
    sudo -u "$SERVICE_USER" python3 -m venv --system-site-packages "$VENV_DIR"
fi
sudo -u "$SERVICE_USER" "$VENV_DIR/bin/pip" install --quiet --upgrade pip
sudo -u "$SERVICE_USER" "$VENV_DIR/bin/pip" install --quiet -r "$APP_ROOT/raspberrypi/requirements.txt"

echo "==> 写入 $UNIT_DST"
sed -e "s|^User=.*|User=$SERVICE_USER|" \
    -e "s|^Group=.*|Group=$(id -gn "$SERVICE_USER")|" \
    -e "s|^WorkingDirectory=.*|WorkingDirectory=$APP_ROOT/raspberrypi|" \
    -e "s|^ExecStart=.*|ExecStart=$VENV_DIR/bin/python3 $GATEWAY|" \
    -e "s|^ReadWritePaths=.*|ReadWritePaths=$APP_ROOT/raspberrypi /tmp|" \
    "$UNIT_SRC" > "$UNIT_DST"
chmod 644 "$UNIT_DST"

echo "==> 启用并启动"
systemctl daemon-reload
systemctl enable --now "$UNIT_NAME"

sleep 2
systemctl --no-pager --full status "$UNIT_NAME" || true
echo
echo "查看日志： journalctl -u $UNIT_NAME -f"
echo "重启服务： sudo systemctl restart $UNIT_NAME"
