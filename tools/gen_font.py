#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
用 Pillow 把中文 TTF 栅格化成 LVGL v8 的 lv_font_fmt_txt 字体 C 文件。

LVGL 位图格式（由 lv_draw_sw_letter.c 反推验证）：
  - 连续位流，行步长 = box_w * bpp 位，**不做字节对齐**
  - bpp=4 时每像素一个 nibble，高位在前
  - glyph_dsc.bitmap_index 是**字节**偏移，且只有 20 bit → 单套字库位图必须 < 1 MB
  - ofs_y = baseline_y - ink_bbox_bottom(exclusive)，y 向下为正
  - adv_w = advance_width * 16 (FP8.4)

字符集（v2，2026-10-05 扩容）：
  ASCII + GB2312 **全集**(0xA1A1~0xF7FE: 682 符号 + 6763 汉字)
  + CJK 标点 U+3000-303F + 全角形式 U+FF00-FFEF
  + 拉丁补充 U+00A0-00FF（外文歌名的 é ñ ü 等）+ 常用/音乐/几何符号

⚠️ 为什么必须扩容（一级汉字不够用）：
  一级汉字只有 3755 个，二级汉字（奕 嵩 泷 靓 痣 ...）一个都没有。
  歌名/歌手名里出现二级字时，LVGL 会走 LV_USE_FONT_PLACEHOLDER 分支
  （lv_font.c:109-124）画一个 line_height/2 x line_height 的空心方框，
  而且步进只有 box_w+2，后面的字还会往前挤 —— 屏幕上就是「方框 + 错位」。

⚠️ 为什么 20px 要拆成两套：
  GB2312 全集在 20px/4bpp 下位图约 1.33 MB，超过 glyph_dsc.bitmap_index 的
  20 bit(1 MB) 上限，LVGL 会直接读错。所以：
    lv_font_cjk_20  = 符号 + 一级汉字（约 780 KB）
    lv_font_cjk_20b = 二级汉字（约 550 KB，靠 .fallback 补上）
  两者同为 20px/4bpp，字形尺寸完全一致，串起来视觉上无缝。
  fallback 字段挂在 struct 里，位图/描述表仍是 flash 里的 const，
  但 **lv_font_cjk_20 本身必须是非 const**（可写），见 lv_font_cjk_setup.c。

用法:
    python gen_font.py              # 生成全部（16px 全量 + 20px 主/次）
    python gen_font.py --probe      # 只探测覆盖率/体积，不写文件
    python gen_font.py --size 16    # 只生成某个字号
    python gen_font.py --ttf msyh.ttc --index 0   # 换字体
"""
import os
import sys
import time
from PIL import Image, ImageDraw, ImageFont

FONT_TTF = r"C:\Windows\Fonts\simhei.ttf"
FONT_INDEX = 0
OUT_DIR = r"D:\wb\wave\MusicPlayer\src\font"
# 把最终字符集导出成清单，供 PC 端 kugou_bridge.py 做「缺字预警」
# （判断"这个字板子能不能显示"，不能简单用 GB2312 能否编码来替代 ——
#  比如 · (U+00B7) 和 — (U+2014) 都不在 GB2312 里，但我们手工补进了字库。）
CHARSET_TXT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "board_charset.txt")

# (符号名, 字号, bpp, 字符集键, 是否 const)
#   full = GB2312 全集；l1 = 符号+一级汉字（主）；l2 = 二级汉字（fallback 补）
TIER_SPECS = [
    ("lv_font_cjk_16",  16, 4, "full", True),
    ("lv_font_cjk_20",  20, 4, "l1",   False),   # 主字库：需可写以挂 fallback
    ("lv_font_cjk_20b", 20, 4, "l2",   True),    # 补充字库：二级汉字
]

MAX_BITMAP = (1 << 20)   # glyph_dsc.bitmap_index 只有 20 bit


# ----------------------------------------------------------------------
def gb2312_row(ch):
    """返回该字在 GB2312 中的区码首字节；不是 GB2312 单字则返回 None"""
    try:
        b = ch.encode("gb2312")
    except Exception:
        return None
    return b[0] if len(b) == 2 else None


def is_level2(ch):
    """GB2312 二级汉字（区码 0xD8~0xF7，次常用字）"""
    r = gb2312_row(ch)
    return r is not None and 0xD8 <= r <= 0xF7


def build_charset():
    chars = set()

    # 1) ASCII 可打印（0x20~0x7E，共 95 个）
    for c in range(0x20, 0x7F):
        chars.add(chr(c))

    # 2) GB2312 全集：1~9 区符号(0xA1)、16~87 区汉字(0xB0~0xF7)
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                chars.add(bytes([hi, lo]).decode("gb2312"))
            except Exception:
                pass

    # 3) CJK 标点 U+3000-303F
    for cp in range(0x3000, 0x3040):
        chars.add(chr(cp))

    # 4) 全角/半角形式 U+FF00-FFEF
    for cp in range(0xFF00, 0xFFF0):
        chars.add(chr(cp))

    # 5) 拉丁补充 U+00A0-00FF（é è ñ ü ø å ß ...）
    for cp in range(0x00A0, 0x0100):
        chars.add(chr(cp))

    # 6) 常用符号 / 音乐符号
    for ch in ("♪♫♩♬♭♯★☆●○◎◐◑■□▲△▼▽◆◇"
               "♥♡♠♣♦→←↑↓↔⇔⇒"
               "√∞∴∵≈≠≤≥±×÷∝"
               "°℃℉§¶©®™€¥￥£₩‰‱′″‴※⌒⊙⊕⊗"
               "—–―‘’“”「」『』【】〔〕〖〗〈〉《》"
               "～·¨〃々∶〓、。，．；：？！"
               "─│┌┐└┘├┤┬┴┼═║╔╗╚╝╠╣╦╩╬"
               "①②③④⑤⑥⑦⑧⑨⑩"):
        chars.add(ch)

    return sorted(c for c in chars if c == " " or (c.strip() != "" and ord(c) >= 0x20))


def charset_by_key(key):
    full = build_charset()
    if key == "full":
        return full, "GB2312 全集"
    if key == "l1":
        return ([c for c in full if not is_level2(c)],
                "符号 + 一级汉字")
    if key == "l2":
        return ([c for c in full if is_level2(c)],
                "GB2312 二级汉字")
    raise ValueError(key)


def render_glyph(font, ch, bpp_value_max):
    """返回 (ofs_x, ofs_y, box_w, box_h, nibbles列表, adv_w)；渲染不出来时 box 全 0"""
    adv = font.getlength(ch)
    adv_w = max(0, min(4095, int(round(adv * 16))))

    W, H = 160, 160
    base_y = 80
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    try:
        d.text((30, base_y), ch, font=font, fill=255, anchor="ls")
    except Exception:
        return 0, 0, 0, 0, [], adv_w

    bbox = img.getbbox()
    if bbox is None:
        # 字形是空白的（空格、全角空格，或本字体没有这个字）。
        # ★必须保留 adv_w：否则 ASCII 空格的步进变 0，英文单词全挤在一起。
        return 0, 0, 0, 0, [], adv_w

    x0, y0, x1, y1 = bbox
    box_w = x1 - x0
    box_h = y1 - y0
    ofs_x = x0 - 30
    ofs_y = base_y - y1

    px = img.load()
    nib = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            v = px[x, y]
            nib.append((v * (bpp_value_max + 1)) // 256)   # 0..bpp_value_max
    return ofs_x, ofs_y, box_w, box_h, nib, adv_w


def pack_nibbles(nib, bpp):
    out = bytearray()
    if bpp == 4:
        for i in range(0, len(nib), 2):
            hi = nib[i]
            lo = nib[i + 1] if i + 1 < len(nib) else 0
            out.append(((hi & 0x0F) << 4) | (lo & 0x0F))
    elif bpp == 2:
        for i in range(0, len(nib), 4):
            b = 0
            for k in range(4):
                v = nib[i + k] if i + k < len(nib) else 0
                b |= (v & 0x03) << (6 - 2 * k)
            out.append(b)
    elif bpp == 1:
        for i in range(0, len(nib), 8):
            b = 0
            for k in range(8):
                v = nib[i + k] if i + k < len(nib) else 0
                b |= (v & 0x01) << (7 - k)
            out.append(b)
    else:
        raise ValueError("bpp must be 1/2/4")
    return bytes(out)


def emit_c(symbol, size, bpp, charset, out_path, is_const=True, note=""):
    font = ImageFont.truetype(FONT_TTF, size, index=FONT_INDEX)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    base_line = descent
    bpp_max = (1 << bpp) - 1

    bitmap = bytearray()
    gdsc = []          # (bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y)
    gdsc.append((0, 0, 0, 0, 0, 0))     # id 0 保留
    blank = []

    ascii_chars = sorted(c for c in charset if 0x20 <= ord(c) <= 0x7E)
    other_chars = sorted(c for c in charset if not (0x20 <= ord(c) <= 0x7E))

    t0 = time.time()
    for ch in ascii_chars + other_chars:
        ofs_x, ofs_y, bw, bh, nib, adv_w = render_glyph(font, ch, bpp_max)
        idx = len(bitmap)
        if bw > 0 and bh > 0:
            bitmap.extend(pack_nibbles(nib, bpp))
        else:
            blank.append(ch)
        gdsc.append((idx, adv_w, bw, bh, ofs_x, ofs_y))
    dt = time.time() - t0

    if len(bitmap) >= MAX_BITMAP:
        raise ValueError(
            "%s: 位图 %d B 超过 glyph_dsc.bitmap_index 的 20bit 上限 %d B（%.0f KB / %.0f KB）。\n"
            "        解决：降 bpp、减字符集，或像 20px 那样拆成主/次两套用 fallback 串起来。"
            % (symbol, len(bitmap), MAX_BITMAP, len(bitmap) / 1024.0, MAX_BITMAP / 1024.0))

    n_ascii = len(ascii_chars)
    glyph_id_other = 1 + n_ascii

    if other_chars:
        cps = [ord(c) for c in other_chars]
        rstart = min(cps)
        rspan = min(65535, max(cps) - rstart + 1)
        ulist = [cp - rstart for cp in cps]
        assert max(ulist) < 65536, "unicode_list 超出 uint16"
    else:
        rstart, rspan, ulist = 0, 0, []

    # ---- 拼 C 文件 ----
    L = []
    L.append("/*******************************************************************************")
    L.append(" * 中文字体 %s  (由 tools/gen_font.py 自动生成，请勿手工修改)" % symbol)
    L.append(" *")
    L.append(" * Size: %d px   Bpp: %d   字符集: %s（%d 字形）" % (size, bpp, note, len(gdsc) - 1))
    L.append(" * 源字体: %s" % os.path.basename(FONT_TTF))
    L.append(" * line_height=%d  base_line=%d  位图=%d B" % (line_height, base_line, len(bitmap)))
    L.append(" ******************************************************************************/")
    L.append("")
    L.append("#include \"lvgl.h\"")
    L.append("")
    L.append("#ifndef %s" % symbol.upper())
    L.append("#define %s 1" % symbol.upper())
    L.append("#endif")
    L.append("")
    L.append("#if %s" % symbol.upper())
    L.append("")
    L.append("/*-----------------")
    L.append(" *    BITMAPS")
    L.append(" *----------------*/")
    L.append("")
    L.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        chunk = bitmap[i:i + 16]
        L.append("    " + ", ".join("0x%02x" % b for b in chunk) + ",")
    L.append("};")
    L.append("")
    L.append("/*-----------------")
    L.append(" *  GLYPH DESCRIPTION")
    L.append(" *----------------*/")
    L.append("")
    L.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    for i, (bi, aw, bw, bh, ox, oy) in enumerate(gdsc):
        assert 0 <= bi < MAX_BITMAP, "bitmap_index 超 20bit: %d" % bi
        assert 0 <= aw < (1 << 12), "adv_w 超 12bit: %d" % aw
        assert 0 <= bw < 256 and 0 <= bh < 256, "box 超 uint8: %d x %d" % (bw, bh)
        assert -128 <= ox < 128 and -128 <= oy < 128, "ofs 超 int8: %d,%d" % (ox, oy)
        L.append("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d}%s"
                 % (bi, aw, bw, bh, ox, oy, "," if i < len(gdsc) - 1 else ""))
    L.append("};")
    L.append("")

    if other_chars:
        L.append("static const uint16_t unicode_list_1[] = {")
        for i in range(0, len(ulist), 12):
            L.append("    " + ", ".join("0x%04x" % v for v in ulist[i:i + 12]) + ",")
        L.append("};")
        L.append("")

    L.append("/*-----------------")
    L.append(" *  CHARACTER MAPPING")
    L.append(" *----------------*/")
    L.append("")
    L.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    entries = []
    if n_ascii > 0:
        # range_length = 字符个数（32..126 共 95 个）。写 n_ascii-1 会漏掉 '~'
        entries.append(
            "    {\n"
            "        .range_start = 32, .range_length = %d, .glyph_id_start = 1,\n"
            "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
            "    }" % n_ascii)
    if other_chars:
        entries.append(
            "    {\n"
            "        .range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
            "        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, .list_length = %d,\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
            "    }" % (rstart, rspan, glyph_id_other, len(ulist)))
    L.append(",\n".join(entries))
    L.append("};")
    L.append("")
    L.append("/*--------------------")
    L.append(" *  ALL CUSTOM DATA")
    L.append(" *--------------------*/")
    L.append("")
    L.append("static lv_font_fmt_txt_glyph_cache_t cache;")
    L.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    L.append("    .glyph_bitmap = glyph_bitmap,")
    L.append("    .glyph_dsc = glyph_dsc,")
    L.append("    .cmaps = cmaps,")
    L.append("    .kern_dsc = NULL,")
    L.append("    .kern_scale = 0,")
    L.append("    .cmap_num = %d," % len(entries))
    L.append("    .bpp = %d," % bpp)
    L.append("    .kern_classes = 0,")
    L.append("    .bitmap_format = 0,")
    L.append("    .cache = &cache")
    L.append("};")
    L.append("")
    L.append("/*-----------------")
    L.append(" *  PUBLIC FONT")
    L.append(" *----------------*/")
    L.append("")
    # ★ 挂 fallback 的字体必须可写（非 const），否则改不了 struct 里的 fallback 字段
    L.append("%slv_font_t %s = {" % ("const " if is_const else "", symbol))
    L.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    L.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    L.append("    .line_height = %d," % line_height)
    L.append("    .base_line = %d," % base_line)
    L.append("    .subpx = LV_FONT_SUBPX_NONE,")
    L.append("    .underline_position = -%d," % max(1, size // 8))
    L.append("    .underline_thickness = 1,")
    L.append("    .dsc = &font_dsc")
    L.append("};")
    L.append("")
    L.append("#endif /*#if %s*/" % symbol.upper())
    L.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))

    print("  -> %-22s %2dpx bpp=%d  bitmap=%4.0f KB  glyphs=%5d  lh=%d bl=%d  %s  (%.1fs)"
          % (os.path.basename(out_path), size, bpp, len(bitmap) / 1024.0,
             len(gdsc) - 1, line_height, base_line,
             "const" if is_const else "可写", dt))
    if blank:
        # 空格/全角空格本来就是空白，不算问题
        real = [c for c in blank if not c.isspace()]
        if real:
            print("     !! %d 个字本字体渲染不出来（已按空白计入）: %s%s"
                  % (len(real), "".join(real[:30]), " ..." if len(real) > 30 else ""))
    return len(bitmap), len(gdsc) - 1


# ----------------------------------------------------------------------
def probe():
    for symbol, size, bpp, key, _ in TIER_SPECS:
        cs, note = charset_by_key(key)
        font = ImageFont.truetype(FONT_TTF, size, index=FONT_INDEX)
        nb = 0
        blank = 0
        for ch in cs:
            ox, oy, bw, bh, nib, aw = render_glyph(font, ch, (1 << bpp) - 1)
            if bw == 0 and not ch.isspace():
                blank += 1
            else:
                nb += bw * bh * bpp / 8.0
        flag = "OK " if nb < MAX_BITMAP else "!! 超 1MB"
        print("  %-16s %2dpx bpp=%d  %-16s chars=%5d  空白=%d  位图=%5.0f KB  %s"
              % (symbol, size, bpp, note, len(cs), blank, nb / 1024.0, flag))


if __name__ == "__main__":
    argv = sys.argv[1:]
    if "--probe" in argv:
        print("字符集总大小: %d" % len(build_charset()))
        probe()
        sys.exit(0)

    if "--ttf" in argv:
        FONT_TTF = argv[argv.index("--ttf") + 1]
    if "--index" in argv:
        FONT_INDEX = int(argv[argv.index("--index") + 1])
    if "--out" in argv:
        OUT_DIR = argv[argv.index("--out") + 1]
    only = int(argv[argv.index("--size") + 1]) if "--size" in argv else None

    os.makedirs(OUT_DIR, exist_ok=True)
    print("源字体: %s" % FONT_TTF)
    total = 0
    for symbol, size, bpp, key, is_const in TIER_SPECS:
        if only is not None and size != only:
            continue
        cs, note = charset_by_key(key)
        nb, _ = emit_c(symbol, size, bpp, cs,
                       os.path.join(OUT_DIR, symbol + ".c"),
                       is_const=is_const, note=note)
        total += nb

    if only is None:
        full = build_charset()
        with open(CHARSET_TXT, "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(full))
        print("字符清单 -> %s（%d 字，供 kugou_bridge.py 缺字预警）"
              % (os.path.basename(CHARSET_TXT), len(full)))
    print("完成。位图合计 %.0f KB" % (total / 1024.0))
