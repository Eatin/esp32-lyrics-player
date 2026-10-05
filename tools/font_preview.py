#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
字库预览：完全按 LVGL 的「主字体 + fallback 链」规则，把一段文字渲染成 PNG。
用来在烧录前肉眼确认某个字在板子上会怎么显示（方框 / 缺字 / 大小不一致）。

用法:
    python font_preview.py "陈奕迅 汪苏泷 张靓颖 许嵩 朱砂痣"
    python font_preview.py "起风了" --size 16 --scale 6 --out preview.png
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import font_render as FR

FONT_DIR = r"D:\wb\wave\MusicPlayer\src\font"

# 每个字号一条 fallback 链，顺序与固件里 lv_font_cjk_setup() 挂的一致
CHAINS = {
    20: ["lv_font_cjk_20", "lv_font_cjk_20b"],
    16: ["lv_font_cjk_16"],
}


def load_chain(size):
    return [FR.load(os.path.join(FONT_DIR, n + ".c")) for n in CHAINS[size]]


def render_chain(chain, text, out, scale=4):
    """主字体找不到的字，依次问后面的字体（同 LVGL lv_font_get_glyph_dsc 的 fallback 循环）"""
    from PIL import Image
    lh = chain[0]["lh"]
    bl = chain[0]["bl"]
    bpp = chain[0]["bpp"]

    pen = 0
    items = []
    missing = []
    for ch in text:
        for F in chain:
            gid = FR.lookup(F, ord(ch))
            if gid:
                items.append((F, F["gdsc"][gid], pen))
                pen += F["gdsc"][gid]["aw"] / 16.0
                break
        else:
            missing.append(ch)
            items.append((None, None, pen))
            pen += lh * 0.6

    W = int(pen) + 4
    H = lh + 4
    img = Image.new("L", (W * scale, H * scale), 255)
    px = img.load()
    for F, g, pen_x in items:
        if g is None or g["bw"] == 0 or g["bh"] == 0:
            continue
        gx = int(round(pen_x + g["ox"]))
        gy = (lh - bl) - g["bh"] - g["oy"]
        for row in range(g["bh"]):
            for col in range(g["bw"]):
                bit_ofs = row * g["bw"] * bpp + col * bpp
                byte = F["bitmap"][g["bi"] + (bit_ofs >> 3)]
                shift = 8 - bpp - (bit_ofs & 7)
                v = (byte >> shift) & ((1 << bpp) - 1)
                if v:
                    val = 255 - (v * 255 // ((1 << bpp) - 1))
                    for dy in range(scale):
                        for dx in range(scale):
                            X, Y = (gx + col) * scale + dx, (gy + row) * scale + dy
                            if 0 <= X < W * scale and 0 <= Y < H * scale:
                                px[X, Y] = val
    img.save(out)
    return missing


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("text")
    ap.add_argument("--size", type=int, default=20)
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()

    out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "fonttest", "preview_%dpx.png" % a.size)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    chain = load_chain(a.size)
    miss = render_chain(chain, a.text, out, a.scale)
    print("chain=%s  text=%s" % (" -> ".join(CHAINS[a.size]), a.text))
    print("  字库完全没有的字: %s" % ("".join(miss) if miss else "无 ✓"))
    print("  saved: %s" % out)
