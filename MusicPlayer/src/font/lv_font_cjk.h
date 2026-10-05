/**
 * @file  lv_font_cjk.h
 * @brief 自研中文字体（由 tools/gen_font.py 从系统黑体 simhei.ttf 生成）
 *
 * 字符集：ASCII + GB2312 **全集**（682 符号 + 6763 汉字）
 *         + CJK 标点 U+3000-303F + 全角形式 U+FF00-FFEF
 *         + 拉丁补充 U+00A0-00FF + 常用/音乐/几何符号，共 7840 字形。
 *
 * 为什么不是只收一级汉字：
 *   一级汉字仅 3755 个，二级汉字（奕 嵩 泷 靓 痣 …）一个都没有。
 *   歌名/歌手名里出现二级字时，LVGL 会走 LV_USE_FONT_PLACEHOLDER 分支
 *   （lv_font.c:109-124）画一个空心方框，并且步进只有 box_w+2，
 *   后面的字还会往前挤 —— 屏幕上就是「方框 + 错位」。
 *
 * 为什么 20px 拆成两套：
 *   glyph_dsc.bitmap_index 只有 20 bit（<1 MB）。GB2312 全集在 20px/4bpp 下
 *   位图约 1.33 MB，超限。故：
 *     lv_font_cjk_20   符号 + 一级汉字（781 KB）★非 const，可写★
 *     lv_font_cjk_20b  GB2312 二级汉字（549 KB）
 *   两者同为 20px/4bpp、line_height/base_line 一致，靠 fallback 串起来，
 *   视觉上无缝。16px 的全集位图 872 KB，单套即可，无需拆。
 *
 * @note 必须在画任何文字之前调用一次 lv_font_cjk_setup()，否则二级汉字
 *       在 20px 下仍会退化成方框。
 */
#ifndef LV_FONT_CJK_H
#define LV_FONT_CJK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 16px：GB2312 全集，单套（位图 872 KB）—— 次要信息行用 */
extern const lv_font_t lv_font_cjk_16;

/** 20px 主字库：符号 + 一级汉字（位图 781 KB）。
 *  ★故意声明为非 const★ —— 要把 fallback 写进结构体，
 *  而 const 数据在 ESP32 上位于 flash，写会崩。 */
extern lv_font_t lv_font_cjk_20;

/** 20px 补充字库：GB2312 二级汉字（位图 549 KB），由 fallback 兜底 */
extern const lv_font_t lv_font_cjk_20b;

/**
 * @brief 装配字体：把 lv_font_cjk_20b 挂成 lv_font_cjk_20 的 fallback。
 * @note  纯内存操作，不依赖 LVGL 初始化，越早调用越好（setup() 开头即可）。
 */
void lv_font_cjk_setup(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_FONT_CJK_H */
