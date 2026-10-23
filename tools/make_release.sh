#!/usr/bin/env bash
# oMLX Mate 发布构建（可复现）
#   pio run -e <env> → esptool merge_bin（bootloader + 分区表 + boot_app0 + app）
#   → tools/check_merged.py 结构校验 → DIO 头部断言 → releases/ + sha256
# 用法: bash tools/make_release.sh [版本号] [env]
#   env: sticks3（默认）| cardputer
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:-$(sed -n 's/^#define FW_VERSION "\(.*\)"/\1/p' include/config.h | head -1)}"
ENV="${2:-sticks3}"
[ -n "$VER" ] || { echo "✗ 取不到版本号"; exit 1; }
echo "▸ oMLX Mate v$VER 构建中（env=${ENV}）…"
pio run -e "$ENV"
B=".pio/build/$ENV"
BOOT_APP0="$(find "$HOME/.platformio/packages" -path "*framework-arduinoespressif32/tools/partitions/boot_app0.bin" | head -1)"
[ -n "$BOOT_APP0" ] || { echo "✗ 找不到 boot_app0.bin"; exit 1; }
mkdir -p releases
SUFFIX=""
[ "$ENV" = "sticks3" ] || SUFFIX="-$ENV"
OUT="releases/omlx-mate-v${VER}${SUFFIX}-merged.bin"
rm -f "$OUT" "$OUT.sha256"
echo "▸ 合并镜像…"
esptool --chip esp32s3 merge_bin -o "$OUT" \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0 "$B/bootloader.bin" \
  0x8000 "$B/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$B/firmware.bin"
echo "▸ 结构校验："
python3 tools/check_merged.py "$OUT"
python3 - "$OUT" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read(8)
assert d[0] == 0xe9 and d[2] == 0x02, "头部不是 DIO merged（e9 03 02 3f）→ M5Burner 会黑屏"
print("  ✓ flash_mode = DIO (0x02)")
PY
shasum -a 256 "$OUT" | tee "$OUT.sha256"
echo "✓ 产物：$OUT"
