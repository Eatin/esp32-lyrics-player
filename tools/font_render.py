#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
独立校验器：直接解析 gen_font.py 生成的 .c 文件，
完全按 LVGL 的 get_glyph_dsc_id / lv_draw_sw_letter 规则渲染成 PNG。
用来在烧录前确认字体文件格式正确。
"""
import re
import sys
from PIL import Image

SRC = r"D:\wb\wave\MusicPlayer\src\font\lv_font_cjk_16.c"
OUT = r"D:\wb\wave\tools\font_check.png"
TEXT = "你好，世界！Hello World 2026 / 周杰伦·晴天"


def load(path):
    s = open(path, encoding="utf-8").read()

    # --- glyph_bitmap ---
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\};", s, re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))

    # --- glyph_dsc ---
    gdsc = []
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\};", s, re.S)
    for g in re.finditer(
        r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}",
        m.group(1)):
        bi, aw, bw, bh, ox, oy = (int(v) for v in g.groups())
        gdsc.append(dict(bi=bi, aw=aw, bw=bw, bh=bh, ox=ox, oy=oy))

    # --- unicode_list_1 ---
    m = re.search(r"unicode_list_1\[\]\s*=\s*\{(.*?)\};", s, re.S)
    ulist = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))] if m else []

    # --- cmaps ---
    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", s, re.S)
    blk = m.group(1)
    cmaps = []
    for sub in re.finditer(
        r"\.range_start = (\d+),\s*\.range_length = (\d+),\s*\.glyph_id_start = (\d+),(.*?)\.type = LV_FONT_FMT_TXT_CMAP_(\w+)",
        blk, re.S):
        rs, rl, gs, _, tp = sub.groups()
        cmaps.append(dict(rs=int(rs), rl=int(rl), gs=int(gs), type=tp))

    line_height = int(re.search(r"\.line_height = (\d+),", s).group(1))
    base_line = int(re.search(r"\.base_line = (\d+),", s).group(1))
    bpp = int(re.search(r"\.bpp = (\d+),", s).group(1))
    return dict(bitmap=bitmap, gdsc=gdsc, ulist=ulist, cmaps=cmaps,
                lh=line_height, bl=base_line, bpp=bpp)


def lookup(F, cp):
    """完全照抄 LVGL get_glyph_dsc_id"""
    for c in F["cmaps"]:
        rcp = cp - c["rs"]
        if rcp < 0 or rcp > c["rl"]:
            continue
        if c["type"] == "FORMAT0_TINY":
            return c["gs"] + rcp
        if c["type"] == "SPARSE_TINY":
            if rcp in F["ulist"]:
                return c["gs"] + F["ulist"].index(rcp)
    return 0


def render(F, text, scale=3):
    lh, bl, bpp = F["lh"], F["bl"], F["bpp"]
    # 先算总宽
    pen = 0
    items = []
    for ch in text:
        gid = lookup(F, ord(ch))
        g = F["gdsc"][gid]
        items.append((g, pen))
        pen += g["aw"] / 16.0
    W = int(pen) + 4
    H = lh + 4
    img = Image.new("L", (W * scale, H * scale), 255)
    px = img.load()

    for g, pen_x in items:
        if g["bw"] == 0 or g["bh"] == 0:
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
    img.save(OUT)
    print("bitmap=%d bytes, glyphs=%d, lh=%d bl=%d bpp=%d, cmaps=%s"
          % (len(F["bitmap"]), len(F["gdsc"]), lh, bl, bpp,
             [(c["type"], c["rl"]) for c in F["cmaps"]]))
    # 抽查几个字的查找结果
    for ch in "你好世界H1,":
        print("  %s U+%04X -> gid=%d box=%dx%d ofs=(%d,%d)"
              % (ch, ord(ch), lookup(F, ord(ch)),
                 F["gdsc"][lookup(F, ord(ch))]["bw"], F["gdsc"][lookup(F, ord(ch))]["bh"],
                 F["gdsc"][lookup(F, ord(ch))]["ox"], F["gdsc"][lookup(F, ord(ch))]["oy"]))
    print("saved:", OUT)


if __name__ == "__main__":
    F = load(SRC)
    render(F, TEXT)
