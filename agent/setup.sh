#!/bin/bash
# oMLX Mate — 由 StickS3 提供的安装脚本
# 用法: curl -sL http://omlx-mate-xxxx.local/setup.sh | bash
set -euo pipefail

MATE_URL="__MATE_URL__"
MATE_TOKEN="__MATE_TOKEN__"
MATE_NAME="__MATE_NAME__"
DEST="$HOME/.omlx-mate"
PLIST="$HOME/Library/LaunchAgents/com.omlx.mate.plist"

echo ""
echo "  oMLX Mate · Mac 端安装"
echo "  ─────────────────────────────"
echo "  设备: $MATE_NAME"
echo "  地址: $MATE_URL"
echo ""

if [ "$(uname -s)" != "Darwin" ]; then
  echo "  ✗ 这个脚本只支持 macOS。"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "  ✗ 没有找到 python3。"
  echo "    请先安装命令行工具（会弹出系统安装窗）:"
  echo "      xcode-select --install"
  echo "    或安装 Homebrew 版: brew install python"
  exit 1
fi

PY_VER=$(python3 -c 'import sys;print("%d.%d"%sys.version_info[:2])')
echo "  ✓ python3 $PY_VER"

if [ ! -d "$HOME/.omlx" ]; then
  echo "  ⚠ 没有找到 ~/.omlx，请先启动一次 oMLX（它会创建用量数据库）。"
  echo "    仍然继续安装，Mate 会在 oMLX 启动后自动出数。"
fi

mkdir -p "$DEST"

echo "  → 从设备下载 agent…"
if ! curl -fsSL --max-time 20 "$MATE_URL/agent.py" -o "$DEST/agent.py"; then
  echo "  ✗ 下载失败。请确认 Mac 和 StickS3 在同一个 Wi-Fi。"
  exit 1
fi
chmod +x "$DEST/agent.py"

echo "  → 登记设备…"
python3 "$DEST/agent.py" --add-device "$MATE_URL" "$MATE_TOKEN" "$MATE_NAME" >/dev/null

echo "  → 写入 LaunchAgent…"
python3 "$DEST/agent.py" --write-plist >/dev/null

echo "  → 启动后台服务…"
launchctl bootout "gui/$(id -u)/com.omlx.mate" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null || launchctl load "$PLIST" 2>/dev/null || true
sleep 2

echo "  → 首次推送…"
python3 "$DEST/agent.py" --once || true

URL=""
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if [ -f "$DEST/url.txt" ]; then URL=$(cat "$DEST/url.txt"); break; fi
  sleep 0.6
done

echo ""
echo "  ✅ 安装完成"
echo "  ─────────────────────────────"
if [ -n "$URL" ]; then
  echo "  控制台: $URL"
  open "$URL" 2>/dev/null || true
else
  echo "  控制台地址见: $DEST/url.txt"
fi
echo "  设备屏幕会在几秒内开始显示用量。"
echo ""
echo "  卸载: curl -sL $MATE_URL/uninstall.sh | bash"
echo ""
