#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-cover.py — oMLX Mate 商店封面（1280×720，另出 2× 海报 2560×1440）

规范：skill `m5burner-store-art`（M5Burner 商店产品图规范，2026-10-19 平台实测）
  * 左下角/底部 22% 是平台黑幕 + 详情页分类/固件名落点 ⇒ **文字一律在 y ≤ 561**
  * 左上 300×110 让给平台版本徽标（标题从 y=118 起）
  * 图内不印版本号；中英双语都在图内（sub + sub_en）
  * 主视觉必须是**真实素材**：屏幕里的画面 = 本固件真机截屏（scripts/shot.py，240×135），
    整数倍最近邻放大（绝不插值糊掉）
  * 左栏文案 = 唯一真源 ~/esp32/tools/covers.json 对应条目
安全区门禁与台账走 ~/esp32/tools/mkcover.py 的 gate_texts() / _ledger()（规范唯一实现）。

两条设备线各出一张（同一份版式代码）：
  oMLX Mate              → StickS3       → cover-1280x720-omlx-mate-sticks3.png
  oMLX Mate [cardputer]  → Cardputer ADV → cover-1280x720-omlx-mate-cardputeradv.png

用法：python3 tools/make-cover.py            # 两条线都出
      python3 tools/make-cover.py sticks3    # 只出 StickS3
      python3 tools/make-cover.py cardputer  # 只出 Cardputer ADV
"""
import json
import os
import sys

sys.path.insert(0, os.path.expanduser("~/esp32/tools"))
from mkcover import gate_texts, _ledger  # noqa: E402  版面门禁 / 台账（唯一实现）

from PIL import Image, ImageDraw, ImageFilter, ImageFont  # noqa: E402

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
COVERS = os.path.expanduser("~/esp32/tools/covers.json")
IMG = os.path.join(PROJ, "docs", "images")
OUT = os.path.join(PROJ, "releases")

W, H = 1280, 720
S = 3                      # 超采样倍率：文字/图形按 3× 画完再 BOX 缩小
CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
BOLD = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
REG = "/System/Library/Fonts/Supplemental/Arial.ttf"

BG = (8, 16, 14)           # 跟设备屏幕同族的近黑绿
WHITE = (240, 244, 241)
GREY = (146, 160, 154)

# key → (covers.json 条目名, 产物名, 机身形态, hero 帧, 次帧)
TARGETS = {
    "sticks3": ("oMLX Mate", "cover-1280x720-omlx-mate-sticks3.png", "sticks3",
                "device-savings.png", "device-cost-30d.png"),
    "cardputer": ("oMLX Mate [cardputer]", "cover-1280x720-omlx-mate-cardputeradv.png", "cardputer",
                  "cardputer-savings.png", "cardputer-cost-30d.png"),
}


def font(path, size, index=0):
    try:
        return ImageFont.truetype(path, size, index=index)
    except Exception:
        return ImageFont.truetype(REG, size)


def load_frame(name, sc=2):
    """真机截屏 → 整数倍最近邻（绝不 LANCZOS：糊掉像素就不是“真实界面”了）
    回退：cardputer-*.png 缺失时用 device-*.png —— 同一份固件、同一块 240×135 面板，
    两种设备渲染出的帧逐像素相同（用户 2026-10-23 确认可用）。"""
    p = os.path.join(IMG, name)
    used = name
    if not os.path.exists(p) and name.startswith("cardputer-"):
        alt = os.path.join(IMG, "device-" + name[len("cardputer-"):])
        if os.path.exists(alt):
            print("  （回退用同一固件的 StickS3 帧：%s）" % os.path.basename(alt))
            p, used = alt, os.path.basename(alt)
    if not os.path.exists(p):
        raise SystemExit("缺真机帧：%s（先用 scripts/shot.py 从设备截屏）" % p)
    im = Image.open(p).convert("RGB")
    return im.resize((im.width * sc, im.height * sc), Image.NEAREST), used


def device(frame_name, sc, kind):
    """机身 = 代码画的圆角外壳；屏幕 = 真实帧（整数倍最近邻）。
    sticks3 = 窄边 + 下侧两个按键凸点；cardputer = 屏幕下留一条机械键盘区（形态特征）"""
    scr, used = load_frame(frame_name, sc)
    pad = 16
    if kind == "cardputer":
        kb_h = 46 * sc
        bez = Image.new("RGB", (scr.width + pad * 2, scr.height + pad + kb_h), (24, 24, 28))
        bd = ImageDraw.Draw(bez)
        bd.rounded_rectangle([0, 0, bez.width - 1, bez.height - 1], radius=22,
                             fill=(26, 26, 30), outline=(58, 58, 64), width=1)
        bd.rounded_rectangle([pad - 4, pad - 4, pad + scr.width + 4 - 1,
                              pad + scr.height + 4 - 1], radius=8, outline=(14, 14, 16), width=1)
        bez.paste(scr, (pad, pad))
        deck_y0 = pad + scr.height + 6
        deck_y1 = bez.height - 5
        bd.rounded_rectangle([pad // 2, deck_y0, bez.width - pad // 2, deck_y1],
                             radius=5, fill=(33, 33, 38))
        cols, rows, gap = 15, 4, 4
        kw = (scr.width - (cols + 1) * gap) // cols
        kh = (deck_y1 - deck_y0 - (rows + 1) * gap) // rows
        for r in range(rows):
            for c in range(cols):
                kx = pad + gap + c * (kw + gap)
                ky = deck_y0 + gap + r * (kh + gap)
                bd.rounded_rectangle([kx, ky, kx + kw, ky + kh], radius=2,
                                     fill=(52, 52, 58), outline=(38, 38, 43), width=1)
                bd.line([(kx + 1, ky + 1), (kx + kw - 1, ky + 1)], fill=(70, 70, 78), width=1)
        return bez, used
    bez = Image.new("RGB", (scr.width + pad * 2, scr.height + pad * 2 + 8), (24, 24, 28))
    bd = ImageDraw.Draw(bez)
    bd.rounded_rectangle([0, 0, bez.width - 1, bez.height - 1], radius=20,
                         fill=(26, 26, 30), outline=(58, 58, 64), width=1)
    bd.rounded_rectangle([pad - 4, pad - 4, pad + scr.width + 4 - 1,
                          pad + scr.height + 4 - 1], radius=8, outline=(14, 14, 16), width=1)
    bez.paste(scr, (pad, pad))
    for frac in (0.30, 0.62):
        bx = pad + int(scr.width * frac)
        bd.rounded_rectangle([bx, bez.height - 6, bx + 44, bez.height], radius=3, fill=(58, 58, 64))
    return bez, used


def compose(key, scale):
    """scale=1 → 1280×720；scale=2 → 2560×1440（版式同一份代码）"""
    entry_name, cover_name, kind, hero_name, back_name = TARGETS[key]
    cover = json.load(open(COVERS, encoding="utf-8"))[entry_name]
    accent = cover.get("accent", "#10BA84")
    acc = tuple(int(accent.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4))

    # ---------- 第一遍：超采样画背景与文字 ----------
    w3, h3 = W * S, H * S
    img = Image.new("RGB", (w3, h3), BG)

    # 背景：右半一块极淡的青绿辉光（设备大数字的颜色，两处同源）
    glow = Image.new("RGB", (w3, h3), BG)
    ImageDraw.Draw(glow).ellipse([int(w3 * 0.50), int(-h3 * 0.18), int(w3 * 1.12), int(h3 * 1.15)],
                                 fill=(10, 34, 27))
    img = Image.blend(img, glow.filter(ImageFilter.GaussianBlur(90 * S // 3)), 0.85)
    d = ImageDraw.Draw(img, "RGBA")

    x0 = 62
    texts = []

    def txt(xy, s, f, fill):
        """记账口径与 mkcover.Canvas 一致：bbox 取 getbbox 在设计空间的真实边界。"""
        fnt = font(f[0], f[1] * S, f[2] if len(f) > 2 else 0)
        b = fnt.getbbox(s)
        texts.append({"s": s, "kind": "text", "bbox": [
            int(xy[0] + b[0] / S), int(xy[1] + b[1] / S),
            int(xy[0] + b[2] / S), int(xy[1] + b[3] / S)]})
        d.text((xy[0] * S - b[0], xy[1] * S - b[1]), s, font=fnt, fill=fill)
        return (b[2] - b[0]) / S

    # ---- 左栏：标题 / 中文副标 / 卖点 / 英文副标 ----
    txt((x0, 118), cover["title"], (BOLD, 96), WHITE)
    txt((x0, 246), cover["sub"], (CJK, 33), WHITE)
    y = 316
    for b in cover["bullets"][:4]:
        d.ellipse([x0 * S, (y + 8) * S, (x0 + 9) * S, (y + 17) * S], fill=acc)
        txt((x0 + 24, y), b, (CJK, 25), WHITE)
        y += 40
    txt((x0, y + 16), cover.get("sub_en", ""), (BOLD, 20), GREY)

    # 机器门禁：所有文字必须在安全区内（黑幕区/版本徽标区/左右边界/版本号/双语）
    n_text = gate_texts(entry_name, texts)
    print("  版面门禁：%d 条文字全部在安全区内 ✓（底线 y≤561，左上 300×110 空置）" % n_text)

    # 左栏文字不许越过机身界线（视觉判据，不是平台判据）
    over = [t for t in texts if t["bbox"][2] > 640]
    assert not over, "左栏文字越界（会被机身压住）: " + "; ".join(
        "%r 右边界 %d" % (t["s"][:20], t["bbox"][2]) for t in over)

    img = img.resize((W * scale, H * scale), Image.BOX)
    base = img

    # ---------- 第二遍：贴真实屏幕（保持像素锐利，不参与超采样）----------
    # 前：省钱页（今日已省大数字）2×
    front, hero_used = device(hero_name, 2, kind)
    if kind == "cardputer":
        # 键盘机身比 StickS3 高一截，两块级联必把后块压掉大半 → 单块居中偏右，机身完整
        placed = [(front, (int(700 * scale), int(168 * scale)), "主")]
    else:
        back, _ = device(back_name, 1, kind)
        placed = [(back, (int(946 * scale), int(330 * scale)), "后"),
                  (front, (int(672 * scale), int(46 * scale)), "前")]
    for im, (px, py), tag in placed:
        assert px >= 56 * scale and px + im.width <= (W - 56) * scale, "%s一块机身越左右安全边" % tag
        assert py + im.height <= 561 * scale, "%s一块机身压到平台黑幕区" % tag
    sh_layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    sd = ImageDraw.Draw(sh_layer)
    for im, (px, py), _tag in placed:
        sd.rounded_rectangle([px + 6, py + 10, px + im.width + 6, py + im.height + 10],
                             radius=22, fill=(0, 0, 0, 160))
    base = Image.alpha_composite(base.convert("RGBA"),
                                 sh_layer.filter(ImageFilter.GaussianBlur(10 * scale))).convert("RGB")
    for im, (px, py), _tag in placed:
        base.paste(im, (px, py))
    return base, texts, hero_used


def main(argv):
    keys = [k for k in TARGETS if not argv or argv[0] in (k, "--all")]
    if not keys:
        raise SystemExit("未知目标：%s（可选 %s）" % (argv[0], " / ".join(TARGETS)))
    os.makedirs(OUT, exist_ok=True)
    for key in keys:
        entry_name, cover_name, kind, hero_name, _back = TARGETS[key]
        entry = {"name": entry_name, "_project": "omlx-mate"}
        poster = cover_name.replace("cover-1280x720-", "poster-2560x1440-")
        for name, scale in ((cover_name, 1), (poster, 2)):
            im, texts, hero_used = compose(key, scale)
            p = os.path.join(OUT, name)
            im.save(p)
            _ledger(os.path.splitext(p)[0] + ".texts.json", entry, p, "frame",
                    {"kind": "frame", "path": "docs/images/" + hero_used,
                     "px": [480, 270], "scale": 2}, texts)
            print("→ %s  %s" % (p, im.size))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
