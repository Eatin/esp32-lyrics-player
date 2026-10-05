# -*- coding: utf-8 -*-
"""
20px 字库的两种取舍方案对比（LVGL glyph bitmap_index 只有 20bit = 1MB 上限）：
  A) 4bpp + 只收 GB2312 一级汉字（画质满血，但二级字要靠 fallback 给 16px）
  B) 2bpp + GB2312 全集（全覆盖，但只有 4 级灰度）
把两者渲染成 PNG，肉眼对比。

产物在 tools/fonttest/
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_font as G
import font_render as FR

OUT = r"D:\wb\wave\tools\fonttest"
os.makedirs(OUT, exist_ok=True)


def gb_row(c):
    try:
        b = c.encode("gb2312")
    except Exception:
        return None
    return b[0] if len(b) == 2 else None


def subset_level1(cs):
    """去掉 GB2312 二级汉字（0xD8~0xF7），其余保留"""
    keep = []
    for c in cs:
        r = gb_row(c)
        if r is not None and 0xB0 <= r <= 0xD7:      # 一级汉字
            keep.append(c)
        elif r is not None and 0xA1 <= r <= 0xA9:    # 符号/假名/全角
            keep.append(c)
        elif r is None:                              # 非 GB2312（额外补的标点/符号/拉丁）
            keep.append(c)
        elif ord(c) < 0x80:
            keep.append(c)
        # 0xD8~0xF7 二级汉字 -> 丢弃
    return keep


TEXT = "陈奕迅 汪苏泷 张靓颖 许嵩 朱砂痣"
TEXT2 = "起风了 光年之外 芒种 赤伶 年轮"


def main():
    full = G.build_charset()
    l1 = subset_level1(full)
    l2 = [c for c in full if c not in set(l1)]
    print("全集 %d   一级子集 %d   二级子集 %d" % (len(full), len(l1), len(l2)))

    variants = [
        ("A_20px_4bpp_l1",  20, 4, l1,   "20px 4bpp 只收一级"),
        ("B_20px_2bpp_full", 20, 2, full, "20px 2bpp 全收"),
        ("C_16px_4bpp_full", 16, 4, full, "16px 4bpp 全收"),
    ]
    for name, size, bpp, cs, desc in variants:
        p = os.path.join(OUT, name + ".c")
        print("[%s] %s  charset=%d" % (name, desc, len(cs)))
        try:
            nb, ng = G.emit_c(name, size, bpp, cs, p)
        except AssertionError as e:
            print("   !! 生成失败: %s" % e)
            continue

        F = FR.load(p)
        for i, t in enumerate((TEXT, TEXT2)):
            FR.OUT = os.path.join(OUT, "%s_sample%d.png" % (name, i))
            FR.render(F, t, scale=4)


if __name__ == "__main__":
    main()
