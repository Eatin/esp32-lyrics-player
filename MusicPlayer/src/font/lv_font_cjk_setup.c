/**
 * @file  lv_font_cjk_setup.c
 * @brief 把 20px 的补充字库（二级汉字）挂成主字库的 fallback
 *
 * 背景：LVGL 的 lv_font_fmt_txt_glyph_dsc_t.bitmap_index 只有 20 bit，
 *       单套字库的位图必须 < 1 MB。GB2312 全集在 20px/4bpp 下要 1.33 MB，
 *       装不下，所以拆成 lv_font_cjk_20（符号+一级汉字）和
 *       lv_font_cjk_20b（二级汉字）两套，用 lv_font_t.fallback 串起来。
 *
 * 注意：LVGL 8.4 没有 lv_font_set_fallback() 这个 API，只有结构体字段，
 *       所以只能直接赋值 —— 这也是 lv_font_cjk_20 必须是非 const 的原因
 *       （const 数据在 ESP32 上位于 flash，写会 exception）。
 */
#include "lv_font_cjk.h"

void lv_font_cjk_setup(void)
{
    /* 只在没挂过的时候挂，重复调用无副作用 */
    if (lv_font_cjk_20.fallback == NULL) {
        lv_font_cjk_20.fallback = &lv_font_cjk_20b;
    }
}
