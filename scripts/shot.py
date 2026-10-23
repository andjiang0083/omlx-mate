#!/usr/bin/env python3
"""从 StickS3 串口抓一帧画面 -> PNG.

设备端实现: uiDumpFrame() 先打印 "SHOT <w> <h> <bytes>", 再把 RGB565 帧缓冲
base64 后按行吐出, 以 "SHOT-END" 结束. 这里解码成 BMP 再用 macOS 自带 sips 转 PNG
(不依赖 Pillow).

用法: python3 scripts/shot.py [/dev/cu.usbmodem101] [out.png]
"""

import base64
import os
import re
import struct
import subprocess
import sys
import time

import serial


def read_frame(ser, timeout=25.0):
    end = time.time() + timeout
    raw = b""
    while time.time() < end:
        chunk = ser.read(65536)
        if chunk:
            raw += chunk
            if b"SHOT-END" in raw:
                break
        else:
            time.sleep(0.01)
    m = re.search(rb"SHOT (\d+) (\d+) (\d+)\r?\n", raw)
    if not m:
        raise RuntimeError("no SHOT header; tail=%r" % raw[-200:])
    w, h, n = (int(x) for x in m.groups())
    body = raw[m.end():]
    stop = body.find(b"SHOT-END")
    if stop >= 0:
        body = body[:stop]
    text = re.sub(rb"[^A-Za-z0-9+/=]", b"", body)
    payload = base64.b64decode(text)
    if len(payload) < n:
        raise RuntimeError(f"short frame: {len(payload)} < {n}")
    return w, h, payload[:n]


def write_bmp(path, w, h, rgb565):
    row_pad = (4 - (w * 3) % 4) % 4
    row_size = w * 3 + row_pad
    pixel_bytes = row_size * h
    file_size = 54 + pixel_bytes
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", file_size, 0, 0, 54))
        f.write(struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, pixel_bytes, 2835, 2835, 0, 0))
        for y in range(h - 1, -1, -1):  # BMP 自下而上
            row = bytearray()
            base = y * w * 2
            for x in range(w):
                v = (rgb565[base + x * 2] << 8) | rgb565[base + x * 2 + 1]  # 画布是大端 RGB565
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                row += bytes((b << 3 | b >> 2, g << 2 | g >> 4, r << 3 | r >> 2))
            row += b"\x00" * row_pad
            f.write(row)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem101"
    out = sys.argv[2] if len(sys.argv) > 2 else "shot.png"
    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.4)
    ser.reset_input_buffer()
    ser.write(b"SHOT\n")
    ser.flush()
    w, h, payload = read_frame(ser)
    ser.close()
    bmp = os.path.splitext(out)[0] + ".bmp"
    write_bmp(bmp, w, h, payload)
    subprocess.run(["sips", "-s", "format", "png", bmp, "--out", out],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.remove(bmp)
    print(f"{out}  {w}x{h}  {len(payload)} bytes")


if __name__ == "__main__":
    main()
