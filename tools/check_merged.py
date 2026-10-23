#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_merged.py — ESP32 固件镜像结构校验器（只读，不联网，仅标准库）

用途
----
M5Burner 商店上传的是「整包写 0x0 (eraseAll)」的 merged / factory 镜像。
如果某个文件其实只是 app-only（被误命名成 -merged），烧上去会覆盖 bootloader
导致黑屏。本脚本对镜像做结构分析，判定 VALID / INVALID：

  * merged/factory 整包 = bootloader + 分区表(partition table) + app
  * 文件头是 ESP app 镜像（0xE9）且「前 0x20000 字节里找不到分区表魔数」
    → 判定 INVALID（疑似 app-only 被误命名为 merged）

判定规则（不依赖单一硬编码偏移，按魔数探测）
---------------------------------------------
1. 文件 < 0x10004 字节 → INVALID，原因「文件过小/疑似截断」。
2. 分区表魔数 = AA 50（小端 0x50AA），必须出现在对齐偏移
   {0x8000, 0x9000, 0xC000, 0x10000} 上才算数（文件别处偶然出现 AA50 不算）。
   * S3/C3：bootloader@0x0，分区表@0x8000，app@0x10000
   * P4：bootloader@0x2000（0x0~0x2000 为 0xFF），分区表@0x8000，app@0x10000
   * 大 flash(16MB/xiaozhi)：分区表@0x8000，app@0x20000
   找不到对齐的分区表魔数 → INVALID（疑似 app-only）。
3. bootloader = <0x10000 区间内、页对齐(4KB)偏移处第一个 ESP app 镜像头
   （首字节 0xE9，第 2 字节段数在 [1,64]）。
   * S3/C3 → 0x0；P4（头部为 FF）→ 0x2000。
4. app = 分区表之后、页对齐偏移处，同时满足：ESP app 镜像头(0xE9+合法段数)
   且紧跟其后的 +0x20 处是 app 描述符魔数 ABCD5432（小端字节序 32 54 CD AB）。
   * app@0x10000 → 描述符在 0x10020；app@0x20000 → 描述符在 0x20020。
   * 用「镜像头+描述符」双重校验，避免把别处偶然的 AA50/3254CDAB 误判为 app。
   找不到 → INVALID（有分区表但无有效 app）。

用法
----
    python3 check_merged.py                 # 扫描当前目录
                                            #   下所有 *merged*.bin / *factory*.bin
                                            #   （跳过 .pio、node_modules、.git）
    python3 check_merged.py <目录或文件>...   # 只扫指定目标

退出码：全部 VALID → 0；存在 INVALID（或参数错误/无文件）→ 1。
"""

import os
import sys

# ---------------------------------------------------------------------------
# 常量（魔数与对齐偏移，均为探测锚点而非「唯一」硬编码位置）
# ---------------------------------------------------------------------------
DEFAULT_ROOT = os.getcwd()
SKIP_DIRS = {".pio", "node_modules", ".git"}

PT_MAGIC_OFFSETS = (0x8000, 0x9000, 0xC000, 0x10000)   # 分区表魔数允许的对齐偏移
PT_MAGIC = b"\xAA\x50"                                  # 分区表魔数（小端 0x50AA）
DESC_MAGIC = b"\x32\x54\xCD\xAB"                        # app 描述符魔数（小端 ABCD5432）
MIN_SIZE = 0x10004                                       # < 此大小判「过小/截断」
BOOT_SCAN_MAX = 0x10000                                  # bootloader 探测上限（页对齐）
PAGE = 0x1000                                            # flash 页大小


# ---------------------------------------------------------------------------
# 结构探测（全部只读，不修改任何 .bin）
# ---------------------------------------------------------------------------
def is_esp_image(data, off):
    """off 处是否为合法 ESP app 镜像头：0xE9 + 段数[1,64]。"""
    if off + 2 > len(data):
        return False
    if data[off] != 0xE9:
        return False
    segs = data[off + 1]
    return 1 <= segs <= 64


def has_descriptor(data, off):
    """app 镜像头 +0x20 处是否为描述符魔数 ABCD5432。"""
    d = off + 0x20
    if d + len(DESC_MAGIC) > len(data):
        return False
    return data[d:d + len(DESC_MAGIC)] == DESC_MAGIC


def find_bootloader(data):
    """<0x10000 区间内、页对齐处第一个 ESP app 镜像头。

    S3/C3 → 0x0；P4（头部为 FF）→ 0x2000。找不到返回 None。
    """
    for off in range(0, BOOT_SCAN_MAX, PAGE):
        if is_esp_image(data, off):
            return off
    return None


def find_partition_table(data):
    """在 PT_MAGIC_OFFSETS 对齐偏移上找分区表魔数 AA50。找不到返回 None。"""
    for off in PT_MAGIC_OFFSETS:
        if data[off:off + 2] == PT_MAGIC:
            return off
    return None


def find_app(data, pt_off):
    """分区表之后、页对齐处，第一个「ESP 镜像头 + 描述符魔数」双重命中的 app。

    返回 app 偏移（0x10000 / 0x20000 ...），找不到返回 None。
    """
    if pt_off is not None:
        start = (pt_off + 1 + PAGE - 1) & ~(PAGE - 1)   # pt 之后的下一个页对齐
    else:
        start = 0x10000
    for off in range(start, len(data) - len(DESC_MAGIC), PAGE):
        if is_esp_image(data, off) and has_descriptor(data, off):
            return off
    return None


# ---------------------------------------------------------------------------
# 判定
# ---------------------------------------------------------------------------
def analyze(path):
    """返回 dict：size, bootloader, pt, app(偏移), desc(=app+0x20 或 None),
    verdict, reason。只读文件，不修改。"""
    res = {
        "size": 0, "bootloader": None, "pt": None, "app": None,
        "desc": None, "verdict": "INVALID", "reason": "",
    }
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as e:
        res["reason"] = "无法读取文件（%s）" % e
        return res

    size = len(data)
    res["size"] = size

    if size < MIN_SIZE:
        res["reason"] = "文件过小/疑似截断（%d 字节 < 0x%X）" % (size, MIN_SIZE)
        return res

    pt = find_partition_table(data)
    if pt is None:
        # 文件头若是 app 镜像 → 典型「app-only 被误命名成 merged」
        if is_esp_image(data, 0):
            res["reason"] = "无分区表魔数（疑似 app-only 被误命名为 merged）"
        else:
            res["reason"] = "无分区表魔数（结构异常）"
        return res

    app = find_app(data, pt)
    if app is None:
        res["reason"] = "有分区表但找不到有效 app 描述符（结构异常）"
        return res

    # bootloader：即便没探测到也记录 None，但 VALID 判定以「分区表+app」为准
    res["bootloader"] = find_bootloader(data)
    res["pt"] = pt
    res["app"] = app
    res["desc"] = app + 0x20          # 描述符紧跟 app 镜像头之后（app_offset+0x20）
    res["verdict"] = "VALID"
    res["reason"] = ""
    return res


# ---------------------------------------------------------------------------
# 文件发现 / CLI
# ---------------------------------------------------------------------------
def is_target_name(name):
    low = name.lower()
    return low.endswith(".bin") and (("merged" in low) or ("factory" in low))


def discover(root):
    """在 root 下递归找 *merged*.bin / *factory*.bin，跳过 SKIP_DIRS。

    root 是文件时直接返回 [root]（仍由调用方做名字检查）；
    root 不存在 → 抛 FileNotFoundError。返回去重、排序后的绝对路径列表。
    """
    root = os.path.abspath(root)          # 统一转绝对路径，保证展示用 relpath
    if os.path.isfile(root):
        return [root]

    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        # 原地裁剪：跳过 .pio / node_modules / .git（os.walk 不会进入被删目录）
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if is_target_name(fn):
                found.append(os.path.join(dirpath, fn))
    return sorted(set(found))


def project_name(rel):
    """项目名 = 相对路径的第一段；单文件（无目录）时用父目录名。"""
    parts = rel.split(os.sep)
    if len(parts) > 1:
        return parts[0]
    # rel 只是文件名：取父目录名兜底（调用方传入）
    return None


def disp_width(s):
    """终端显示宽度：CJK 全角按 2，其余按 1（保证表格对齐）。"""
    w = 0
    for ch in s:
        o = ord(ch)
        if (0x1100 <= o <= 0x115F or 0x2E80 <= o <= 0xA4CF
                or 0xAC00 <= o <= 0xD7A3 or 0xF900 <= o <= 0xFAFF
                or 0xFE30 <= o <= 0xFE4F or 0xFF00 <= o <= 0xFF60
                or 0x20000 <= o <= 0x3FFFD):
            w += 2
        else:
            w += 1
    return w


def pad(s, width):
    gap = width - disp_width(s)
    return s + (" " * max(0, gap))


def fmt_off(v):
    return "-" if v is None else ("0x%X" % v)


# ---------------------------------------------------------------------------
def main(argv):
    args = argv[1:]
    if any(a in ("-h", "--help") for a in args):
        print(__doc__)
        return 0

    if not args:
        root = DEFAULT_ROOT
        if not os.path.isdir(root):
            print("错误：默认扫描根目录不存在：%s" % root, file=sys.stderr)
            return 1
        targets = [root]
    else:
        for a in args:
            if not os.path.exists(a):
                print("错误：路径不存在：%s" % a, file=sys.stderr)
                return 1
        targets = args

    files = []
    for t in targets:
        if os.path.isdir(t):
            files.extend(discover(t))
        else:  # isfile → 转绝对路径，保证展示用 relpath 正确
            if not is_target_name(os.path.basename(t)):
                print("提示：显式指定的文件即使名字不含 merged/factory 也照样校验：%s" % t, file=sys.stderr)
            files.append(os.path.abspath(t))

    # 去重（同一文件可能被多个目录参数重复命中）
    files = sorted(set(files))

    if not files:
        print("未找到任何 *merged*.bin / *factory*.bin 文件。", file=sys.stderr)
        return 1

    # ------------------------- 逐文件分析 + 展示路径 -------------------------
    invalid = []       # (relpath, reason)
    display = []       # (project, rel, analyze_result)
    for path in files:
        a = analyze(path)
        # 展示路径：文件相对 DEFAULT_ROOT；不在其下（如直接传了别处单文件）→ 文件名
        if path.startswith(DEFAULT_ROOT + os.sep):
            rel = os.path.relpath(path, DEFAULT_ROOT)
        else:
            rel = os.path.basename(path)
        proj = project_name(rel) or "(file)"
        display.append((proj, rel, a))

    # ------------------------- 打印表格 -------------------------
    headers = ["项目", "文件相对路径", "大小KB", "bootloader偏移",
               "分区表偏移", "app描述符偏移", "判定"]

    def row_cells(d):
        proj, rel, a = d
        size_kb = "%.1f" % (a["size"] / 1024.0)
        return [proj, rel, size_kb, fmt_off(a["bootloader"]),
                fmt_off(a["pt"]), fmt_off(a["desc"]), a["verdict"]]

    widths = [max(disp_width(h), max((disp_width(row_cells(x)[i]) for x in display), default=0))
              for i, h in enumerate(headers)]

    def print_row(cells):
        out = []
        for c, w in zip(cells, widths):
            # 前几列左对齐，最后一列(判定)也左对齐即可；大小KB右对齐
            out.append(pad(c, w))
        print("  ".join(out).rstrip())

    # 表头 + 分隔线
    print_row(headers)
    sep = ["-" * w for w in widths]
    print("  ".join(sep))

    valid_count = 0
    for d in display:
        proj, rel, a = d
        if a["verdict"] == "VALID":
            valid_count += 1
        else:
            invalid.append((rel, a["reason"]))
        print_row(row_cells(d))

    # ------------------------- 汇总 -------------------------
    total = len(display)
    inv_count = len(invalid)
    print()
    print("汇总：总文件数 %d / VALID %d / INVALID %d" % (total, valid_count, inv_count))
    if invalid:
        print()
        print("INVALID 明细：")
        for rel, reason in invalid:
            print("  %s" % rel)
            print("      -> %s" % reason)

    return 0 if inv_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
