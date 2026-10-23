#!/usr/bin/env bash
# oMLX Mate 发布构建（可复现）
#   pio run -e sticks3 → esptool merge_bin（bootloader + 分区表 + boot_app0 + app）
#   → tools/check_merged.py 结构校验 → releases/ + sha256
# 用法: bash tools/make_release.sh [版本号]
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-$(sed -n 's/^#define FW_VERSION "\(.*\)"/\1/p' include/config.h | head -1)}"
[ -n "$VER" ] || { echo "✗ 取不到版本号"; exit 1; }
echo "▸ oMLX Mate v$VER 构建中…"
pio run -e sticks3
B=".pio/build/sticks3"
BOOT_APP0="$(find "$HOME/.platformio/packages" -path "*framework-arduinoespressif32/tools/partitions/boot_app0.bin" | head -1)"
[ -n "$BOOT_APP0" ] || { echo "✗ 找不到 boot_app0.bin"; exit 1; }
mkdir -p releases
OUT="releases/omlx-mate-v${VER}-merged.bin"
rm -f "$OUT" "$OUT.sha256"
echo "▸ 合并镜像…"
esptool --chip esp32s3 merge_bin -o "$OUT" \
  --flash_mode qio --flash_freq 80m --flash_size 8MB \
  0x0 "$B/bootloader.bin" \
  0x8000 "$B/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$B/firmware.bin"
echo "▸ 结构校验："
python3 tools/check_merged.py "$OUT"
shasum -a 256 "$OUT" | tee "$OUT.sha256"
echo "✓ 产物：$OUT"
