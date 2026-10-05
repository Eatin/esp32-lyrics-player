#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
字库审计：解析 gen_font.py 生成的 lv_font_cjk_*.c，
得到字库**真实覆盖的码点集合**，再去固件源码里把所有字符串字面量捞出来对账，
最后报告「固件要用、但字库里没有」的字。

注意：20px 拆成了两套（lv_font_cjk_20 主 + lv_font_cjk_20b 靠 fallback 补），
      所以覆盖集合是这几个文件的**并集**。

用法:
    python font_audit.py                 # 审计整个 MusicPlayer 工程
    python font_audit.py 我爱你 周杰伦     # 额外检查命令行给出的文本
    python font_audit.py --charset       # 顺便统计字库覆盖率/缺口分布
"""
import os
import re
import sys

FONT_C = r"D:\wb\wave\MusicPlayer\src\font\lv_font_cjk_20.c"
# 参与并集的其它字库（fallback / 其它字号）
FONT_C_EXTRA = [
    r"D:\wb\wave\MusicPlayer\src\font\lv_font_cjk_20b.c",
    r"D:\wb\wave\MusicPlayer\src\font\lv_font_cjk_16.c",
]
SRC_ROOT = r"D:\wb\wave\MusicPlayer"
SKIP_DIRS = {"font", "build_w1", ".git"}


# ----------------------------------------------------------------------
def parse_font(path):
    """返回覆盖的码点集合 set[int]（严格按 LVGL 查表规则）。"""
    s = open(path, encoding="utf-8").read()
    cmaps = []
    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", s, re.S)
    blk = m.group(1)
    for sub in re.finditer(
        r"\.range_start = (\d+),\s*\.range_length = (\d+),\s*\.glyph_id_start = (\d+),(.*?)\.type = LV_FONT_FMT_TXT_CMAP_(\w+)",
        blk, re.S):
        rs, rl, gs, body, tp = sub.groups()
        cmaps.append(dict(rs=int(rs), rl=int(rl), gs=int(gs), type=tp, body=body))

    m = re.search(r"unicode_list_1\[\]\s*=\s*\{(.*?)\};", s, re.S)
    ulist = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", m.group(1))] if m else []

    covered = set()
    for c in cmaps:
        if c["type"] == "FORMAT0_TINY":
            for cp in range(c["rs"], c["rs"] + c["rl"]):
                covered.add(cp)
        elif c["type"] == "SPARSE_TINY":
            for v in ulist:
                covered.add(c["rs"] + v)
    return covered


def cjk_only(ch):
    o = ord(ch)
    return not (0x20 <= o <= 0x7E)


def scan_sources(root):
    """返回 {字: [出现在哪些文件的哪些行]}"""
    hits = {}
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in filenames:
            if not fn.endswith((".c", ".cpp", ".h", ".ino", ".hpp")):
                continue
            fp = os.path.join(dirpath, fn)
            try:
                txt = open(fp, encoding="utf-8", errors="replace").read()
            except Exception:
                continue
            for ln, line in enumerate(txt.splitlines(), 1):
                for lit in pat.findall(line):
                    for ch in lit:
                        if cjk_only(ch):
                            hits.setdefault(ch, []).append(
                                "%s:%d" % (os.path.relpath(fp, root), ln))
    return hits


def main():
    global SRC_ROOT, FONT_C, FONT_C_EXTRA
    argv = sys.argv[1:]
    show_charset = "--charset" in argv
    argv = [a for a in argv if a != "--charset"]

    # 允许换工程，方便在别的项目里复用：
    #   font_audit.py --font <字库.c> --src <源码根目录>
    if "--src" in argv:
        i = argv.index("--src")
        SRC_ROOT = argv[i + 1]
        del argv[i:i + 2]
    if "--font" in argv:
        i = argv.index("--font")
        f = argv[i + 1]
        FONT_C, FONT_C_EXTRA = f, [f]
        del argv[i:i + 2]

    covered = set()
    for p in [FONT_C] + [q for q in FONT_C_EXTRA if os.path.exists(q)]:
        covered |= parse_font(p)
    print("字库覆盖（%s 的并集）" % ", ".join(
        os.path.basename(p) for p in [FONT_C] + [q for q in FONT_C_EXTRA if os.path.exists(q)]))
    print("  覆盖码点 %d 个（BMP）" % len(covered))
    if show_charset:
        latin = sum(1 for c in covered if c < 0x80)
        kana = sum(1 for c in covered if 0x3040 <= c <= 0x30FF)
        cjk = sum(1 for c in covered if 0x4E00 <= c <= 0x9FFF)
        print("    ASCII=%d  假名=%d  汉字=%d  其他=%d"
              % (latin, kana, cjk, len(covered) - latin - kana - cjk))

    hits = scan_sources(SRC_ROOT)
    print("\n固件源码里出现的非 ASCII 字：%d 种" % len(hits))

    missing = sorted(c for c in hits if ord(c) not in covered)
    ok = sorted(c for c in hits if ord(c) in covered)

    if missing:
        print("\n!! 缺字 %d 个（字库里没有 -> 会显示成空白/方框）：" % len(missing))
        for ch in missing:
            loc = hits[ch]
            print("   %s  U+%04X  %s%s" % (ch, ord(ch), loc[0],
                                          ("  (+%d 处)" % (len(loc) - 1)) if len(loc) > 1 else ""))
    else:
        print("\nOK：固件里用到的字全部在字库里。")

    if argv:
        extra = set()
        for t in argv:
            extra.update(c for c in t if cjk_only(c))
        miss2 = sorted(c for c in extra if ord(c) not in covered)
        print("\n命令行补充文本检查：%d 字，缺 %d 个" % (len(extra), len(miss2)))
        if miss2:
            print("   " + " ".join("%s(U+%04X)" % (c, ord(c)) for c in miss2))
    print("\n（已在字库内的非 ASCII 字 %d 种）" % len(ok))


if __name__ == "__main__":
    main()
