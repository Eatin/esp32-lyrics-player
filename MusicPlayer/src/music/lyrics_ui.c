/**
 * @file  lyrics_ui.c
 * @brief 歌词界面（横屏 640 x 172，中文字体）
 *
 * 「桌面歌词机」形态：长条屏横放，左边专辑封面，右边两行歌词。
 * 这一页是长期挂在桌面上的主界面，所以**界面上不放任何按钮/状态字**：
 *
 *   x: 8       164  180                                632
 *   ┌──────────┬─────────────────────────────────────────┐
 *   │          │ 歌名                                     │  y  6..32
 *   │          │ 歌手 · 专辑                              │  y 34..56
 *   │  专辑封面 │                                          │
 *   │ 156x156  │ 当前歌词（大、亮）                        │  y 60..90
 *   │          │ 下一句（小、暗）                          │  y 96..118
 *   │          │ ▓▓▓▓▓▓░░░░░░░░░░░  进度条                │  y 130..136
 *   └──────────┴─────────────────────────────────────────┘  时间 /（仅有异常时）来源  y 144..166
 *
 * 封面：由 PC 端推过来（156x156 RGB565 大端，见 ext_link.h）。
 *       没封面时画一个「唱片」占位图（纯几何图形，不依赖字体）。
 *
 * 两个刻意的「不显示」：
 *   1) 没有「返回」按钮。歌词页是常驻主界面，露一个按钮很碍眼。
 *      想回播放器页：**在屏幕上长按约 0.8 秒**（隐藏手势，见 scr_long_press_cb），
 *      或者等停播 60 秒自动回去。
 *   2) 右下角的「来源 / IP」行只在**不正常**时才出现 ——
 *      歌在放且有歌词 = 电脑推送一切正常，一个字都不显示；
 *      只有没推送 / 没歌词时才亮出来（那时它才有用：告诉你去哪个 IP 看控制台）。
 *
 * 手势一览（都没有实体按钮，靠触摸）：
 *   长按 0.8s      -> 回播放器页
 *   左滑           -> 下一曲（投递 XL_CMD_NEXT，由电脑上的播放器执行）
 *   右滑           -> 上一曲（投递 XL_CMD_PREV）
 *   切歌后屏中央弹 1.2 秒提示，见 toast_show()。
 *   滑动判定是自己实现的（不用 LVGL 的 GESTURE，原因见 scr_pressing_cb 上方的注释）。
 */
#include "lyrics_ui.h"
#include "music_ui.h"
#include "lyrics.h"
#include "web_player.h"
#include "wifi_link.h"
#include "ext_link.h"
#include "user_config.h"

#include "lvgl.h"
#include "src/font/lv_font_cjk.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

#define SCR_W   EXAMPLE_LCD_H_RES      /* 640 */
#define SCR_H   EXAMPLE_LCD_V_RES      /* 172 */

#define C_BG      0x0D0F16
#define C_PANEL   0x1A2030
#define C_ACCENT  0x4C8DFF
#define C_CUR     0x66D9FF      /* 当前歌词行 */
#define C_NEXT    0x93A2BA      /* 下一句     */
#define C_DIM     0x5F6B82
#define C_TEXT    0xE8EDF6
#define C_GREEN   0x2AD4A8

/* 封面与右侧面板 */
#define COVER_X   8                                       /* 左边距 */
#define COVER_Y   ((SCR_H - COVER_PX) / 2)                /* 8 —— 竖直居中 */
#define PANEL_X   (COVER_X + COVER_PX + 16)               /* 180 */
#define PANEL_W   (SCR_W - PANEL_X - 8)                   /* 452 */

#define F_TITLE   (&lv_font_cjk_20)
#define F_CUR     (&lv_font_cjk_20)
#define F_SUB     (&lv_font_cjk_16)

/* ------------------------------------------------------------------ */

static lv_obj_t *s_scr        = NULL;
static lv_obj_t *s_cv         = NULL;   /* 封面画布   */
static lv_obj_t *s_ph         = NULL;   /* 封面占位图 */
static lv_obj_t *s_lbl_song   = NULL;
static lv_obj_t *s_lbl_meta   = NULL;
static lv_obj_t *s_lbl_cur    = NULL;
static lv_obj_t *s_lbl_next   = NULL;
static lv_obj_t *s_lbl_hint   = NULL;
static lv_obj_t *s_bar        = NULL;
static lv_obj_t *s_lbl_time   = NULL;
static lv_obj_t *s_lbl_src    = NULL;
static lv_obj_t *s_lbl_toast  = NULL;   /* 滑动切歌时的临时提示 */

static uint8_t *s_cv_buf      = NULL;   /* 封面像素（自己一份，和 ext_link 解耦） */
static char     s_tmp[256];

static bool s_visible       = false;
static int  s_last_idx      = -2;
static int  s_last_track    = -2;
static int  s_last_cover    = -1;
static uint32_t s_toast_at   = 0;       /* 提示弹出时刻(lv_tick)，0=没在显示 */

/* ---- 滑动切歌的状态（判定逻辑见 scr_pressing_cb 上方的注释）---- */
#define SWIPE_MIN_DX  60   /* 水平净位移超过这么多才算一次滑动 */
#define SWIPE_SLANT   2    /* |dx| 必须大于 |dy|*2 才算横向滑动 */
#define SWIPE_JITTER  45   /* 单帧跳这么多像素：先怀疑是毛刺   */
#define SWIPE_HOLD_GUARD 30  /* 已经横移这么多就不算「长按」 */

static lv_point_t s_sw_last;            /* 上一个可信位置     */
static int32_t    s_sw_dx      = 0;     /* 累计水平位移       */
static int32_t    s_sw_dy      = 0;     /* 累计竖直位移       */
static bool       s_sw_active  = false;
static int        s_sw_jump    = 0;     /* 连续「可疑跳变」计数 */

static void lyrics_timer_cb(lv_timer_t *t);

/* ------------------------------------------------------------------ */

static void fmt_ms(uint32_t ms, char *out, size_t n)
{
    uint32_t s = ms / 1000u;
    snprintf(out, n, "%02u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

/** 歌词页不放返回按钮，改用隐藏手势：长按任意位置 -> 回播放器页。
 *  长按时间由 lvgl_port.c 里的 indev long_press_time 决定（已设 800ms）。
 *
 *  注意：LVGL 的长按是**纯计时**的，手指慢慢划也会在 800ms 时触发。
 *  所以这里先看一眼横向位移 —— 已经在划了就当它是滑动，不当作长按。 */
static void scr_long_press_cb(lv_event_t *e)
{
    (void)e;
    if (s_sw_active && LV_ABS(s_sw_dx) > SWIPE_HOLD_GUARD) return;
    lyrics_ui_back();
}

/** 屏中央弹一条 1.2 秒后自动消失的提示（滑动切歌的即时反馈）。 */
static void toast_show(const char *txt)
{
    if (!s_lbl_toast) return;
    lv_label_set_text(s_lbl_toast, txt);
    lv_obj_clear_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lbl_toast);
    s_toast_at = lv_tick_get();
    if (!s_toast_at) s_toast_at = 1;   /* 0 是「没在显示」的哨兵值，避开 */
}

/* ---------------- 滑动切歌：自己做判定，不用 LVGL 的 GESTURE ----------------
 *
 * 为什么不用 LV_EVENT_GESTURE？
 *   它把每次采样的位移**无条件累加**（lv_indev.c 的 indev_gesture），
 *   所以**一个坏采样就能把方向带偏** —— 实测抓到过：
 *   手指明明在左滑（面板 pointX 95→129→167…→317 稳定递增），
 *   中间有一帧 AXS15231B 返回了 raw=(0,384) 的野值，
 *   累加后变成「右滑」，于是该发「下一曲」却发了「上一曲」。
 *
 * 这里改成自己算：按下记起点，按压过程中累计位移，但**单帧跳变超过
 *   SWIPE_JITTER 就先当毛刺丢掉**（连续两帧都跳且方向一致才采信，
 *   这样真·快速滑动不会丢帧）。松手时看总位移够不够、够不够「横」，
 *   再用净位移判方向 —— 野值再也影响不了结论。
 */

static void scr_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_indev_get_point(indev, &s_sw_last);
    s_sw_dx = s_sw_dy = 0;
    s_sw_jump   = 0;
    s_sw_active = true;
}

static void scr_pressing_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sw_active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const int32_t dx = (int32_t)p.x - s_sw_last.x;
    const int32_t dy = (int32_t)p.y - s_sw_last.y;

    if (LV_ABS(dx) > SWIPE_JITTER || LV_ABS(dy) > SWIPE_JITTER) {
        /* 一步跳这么远，先当毛刺扔掉；连续两次都跳才认作真·快速滑动 */
        if (++s_sw_jump < 2) return;
    } else {
        s_sw_jump = 0;
    }

    s_sw_dx += dx;
    s_sw_dy += dy;
    s_sw_last = p;
}

/** 松手时判定：左滑 = 下一曲，右滑 = 上一曲。
 *  板子**不播音频**，所以只是把命令投递给 ext_link；真正切歌的是电脑上的
 *  播放器 —— PC 下次 POST /api/ext/pos 时取走并按下系统媒体键
 *  （见 ext_link.h 的「反向通道」）。切完 PC 推来新曲目，屏上两行自动更新。 */
static void scr_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sw_active) return;
    s_sw_active = false;

    if (LV_ABS(s_sw_dx) < SWIPE_MIN_DX) return;                    /* 位移不够 */
    if (LV_ABS(s_sw_dx) < LV_ABS(s_sw_dy) * SWIPE_SLANT) return;   /* 太斜了   */

    if (s_sw_dx < 0) {                     /* lv x 变小 = 手指往左划 */
        ext_link_post_cmd(XL_CMD_NEXT);
        toast_show("下一曲 \u2192");
    } else {
        ext_link_post_cmd(XL_CMD_PREV);
        toast_show("\u2190 上一曲");
    }
}

/** 把子树里所有对象的「可点击」标志清掉。
 *  LVGL 的触摸命中只认带 CLICKABLE 的对象（见 lv_indev_search_obj / lv_obj_hit_test），
 *  清掉之后整块屏的触摸都会落到 s_scr 自己身上，于是「长按任意位置」都能生效 ——
 *  否则手指按在歌词/封面上时事件会被那个 label 吃掉。 */
static void make_children_click_transparent(lv_obj_t *o)
{
    uint32_t n = lv_obj_get_child_cnt(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        make_children_click_transparent(c);
    }
}

/** 没有封面时，按歌名取一个稳定的颜色，画一张「唱片」占位图 */
static uint32_t ph_color(const char *title)
{
    static const uint32_t pal[] = {
        0x3B5BDB, 0x7048E8, 0x0CA678, 0xE8590C, 0xC2255C, 0x1098AD,
    };
    unsigned h = 2166136261u;
    for (const char *p = title; p && *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* ------------------------------------------------------------------ */

void lyrics_ui_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------------- 左：专辑封面 ---------------- */
    s_cv_buf = (uint8_t *)heap_caps_malloc((size_t)COVER_PX * COVER_PX * 2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cv_buf) s_cv_buf = (uint8_t *)malloc((size_t)COVER_PX * COVER_PX * 2);
    if (s_cv_buf) memset(s_cv_buf, 0, (size_t)COVER_PX * COVER_PX * 2);

    s_cv = lv_canvas_create(s_scr);
    lv_obj_set_pos(s_cv, COVER_X, COVER_Y);
    if (s_cv_buf) {
        lv_canvas_set_buffer(s_cv, s_cv_buf, COVER_PX, COVER_PX, LV_IMG_CF_TRUE_COLOR);
    }
    lv_obj_add_flag(s_cv, LV_OBJ_FLAG_HIDDEN);

    /* 占位图：深色方板 + 唱片圆盘 + 中心孔 */
    s_ph = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ph);
    lv_obj_set_pos(s_ph, COVER_X, COVER_Y);
    lv_obj_set_size(s_ph, COVER_PX, COVER_PX);
    lv_obj_set_style_bg_color(s_ph, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_ph, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ph, 4, 0);
    lv_obj_clear_flag(s_ph, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = lv_obj_create(s_ph);
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, 100, 100);
    lv_obj_set_style_bg_color(disc, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(disc);

    lv_obj_t *hole = lv_obj_create(disc);
    lv_obj_remove_style_all(hole);
    lv_obj_set_size(hole, 30, 30);
    lv_obj_set_style_bg_color(hole, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(hole);

    /* ---------------- 右：歌名 ---------------- */
    /* 这里以前有个「返回」按钮，现在去掉了（歌词页是常驻主界面）。
     * 回播放器页的办法：长按屏幕约 0.8 秒，或等停播 60 秒自动返回。
     * 另加一个手势：左右滑动切歌（见 scr_released_cb）。 */
    lv_obj_add_event_cb(s_scr, scr_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    /* 滑动切歌不用 LVGL 的 LV_EVENT_GESTURE —— 它无条件累加每帧位移，
     * 一个坏采样就会把方向带偏（原因见 scr_pressing_cb 上方注释），
     * 所以自己用 PRESSED / PRESSING / RELEASED 三件套算。 */
    lv_obj_add_event_cb(s_scr, scr_pressed_cb,    LV_EVENT_PRESSED,      NULL);
    lv_obj_add_event_cb(s_scr, scr_pressing_cb,   LV_EVENT_PRESSING,     NULL);
    lv_obj_add_event_cb(s_scr, scr_released_cb,   LV_EVENT_RELEASED,     NULL);

    s_lbl_song = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_song, "歌词");
    lv_obj_set_style_text_font(s_lbl_song, F_TITLE, 0);
    lv_obj_set_style_text_color(s_lbl_song, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(s_lbl_song, PANEL_X, 6);
    lv_obj_set_width(s_lbl_song, PANEL_W);
    lv_label_set_long_mode(s_lbl_song, LV_LABEL_LONG_DOT);

    /* ---------------- 右：歌手 · 专辑 ---------------- */
    s_lbl_meta = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_meta, "");
    lv_obj_set_style_text_font(s_lbl_meta, F_SUB, 0);
    lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(s_lbl_meta, PANEL_X, 34);
    lv_obj_set_width(s_lbl_meta, PANEL_W);
    lv_label_set_long_mode(s_lbl_meta, LV_LABEL_LONG_DOT);

    /* ---------------- 右：两行歌词 ---------------- */
    s_lbl_cur = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_cur, "");
    lv_obj_set_style_text_font(s_lbl_cur, F_CUR, 0);
    lv_obj_set_style_text_color(s_lbl_cur, lv_color_hex(C_CUR), 0);
    lv_obj_set_pos(s_lbl_cur, PANEL_X, 60);
    lv_obj_set_width(s_lbl_cur, PANEL_W);
    lv_label_set_long_mode(s_lbl_cur, LV_LABEL_LONG_DOT);

    s_lbl_next = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_next, "");
    lv_obj_set_style_text_font(s_lbl_next, F_SUB, 0);
    lv_obj_set_style_text_color(s_lbl_next, lv_color_hex(C_NEXT), 0);
    lv_obj_set_pos(s_lbl_next, PANEL_X, 96);
    lv_obj_set_width(s_lbl_next, PANEL_W);
    lv_label_set_long_mode(s_lbl_next, LV_LABEL_LONG_DOT);

    /* 没有歌词时的提示（占住两行歌词的位置） */
    s_lbl_hint = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_hint, "");
    lv_obj_set_style_text_font(s_lbl_hint, F_SUB, 0);
    lv_obj_set_style_text_color(s_lbl_hint, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(s_lbl_hint, PANEL_X, 62);
    lv_obj_set_width(s_lbl_hint, PANEL_W);
    lv_label_set_long_mode(s_lbl_hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);

    /* ---------------- 右：进度 + 时间 ---------------- */
    s_bar = lv_bar_create(s_scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, PANEL_X, 130);
    lv_obj_set_size(s_bar, PANEL_W, 6);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2B3242), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_lbl_time = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_time, "00:00 / 00:00");
    lv_obj_set_style_text_font(s_lbl_time, F_SUB, 0);
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(s_lbl_time, PANEL_X, 144);

    s_lbl_src = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_src, "");
    lv_obj_set_style_text_font(s_lbl_src, F_SUB, 0);
    lv_obj_set_style_text_color(s_lbl_src, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(s_lbl_src, PANEL_X, 144);
    lv_obj_set_width(s_lbl_src, PANEL_W);
    lv_obj_set_style_text_align(s_lbl_src, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_src, LV_LABEL_LONG_DOT);
    /* 开局先藏起来，等定时器判断「是不是正常播放」再决定要不要露出来 */
    lv_obj_add_flag(s_lbl_src, LV_OBJ_FLAG_HIDDEN);

    /* ---------------- 滑动切歌的临时提示（默认隐藏） ---------------- */
    /* 放在 make_children_click_transparent() 之前，这样它的 CLICKABLE 也会被收掉，
     * 提示出现的那 1.2 秒里不会把滑动手势吃掉。 */
    s_lbl_toast = lv_label_create(s_scr);
    lv_label_set_text(s_lbl_toast, "");
    lv_obj_set_style_text_font(s_lbl_toast, F_CUR, 0);
    lv_obj_set_style_text_color(s_lbl_toast, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_lbl_toast, lv_color_hex(0x1B2436), 0);
    lv_obj_set_style_bg_opa(s_lbl_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_lbl_toast, 8, 0);
    lv_obj_set_style_pad_hor(s_lbl_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_lbl_toast, 6, 0);
    lv_obj_align(s_lbl_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);

    /* 这一页没有任何按钮了：把所有子对象的点击权收掉，
     * 让「长按任意位置」这种隐藏手势能真正落到整块屏上。 */
    make_children_click_transparent(s_scr);

    /* 200ms 刷新（位置插值很平滑，200ms 足够） */
    lv_timer_create(lyrics_timer_cb, 200, NULL);
}

/* ------------------------------------------------------------------ */

void lyrics_ui_show(void)
{
    if (!s_scr || s_visible) return;
    s_visible       = true;
    s_last_idx      = -2;
    s_last_cover    = -1;   /* 强制刷一次封面 */
    lv_scr_load_anim(s_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void lyrics_ui_back(void)
{
    if (!s_visible) return;
    s_visible = false;
    lv_obj_t *m = music_ui_screen();
    if (m) lv_scr_load_anim(m, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

bool lyrics_ui_visible(void) { return s_visible; }

/* ------------------------------------------------------------------ */

static void refresh_cover(void)
{
    int seq = np_cover_seq();
    if (seq == s_last_cover) return;
    s_last_cover = seq;

    const uint8_t *d = np_cover_data();
    if (d && s_cv_buf) {
        memcpy(s_cv_buf, d, (size_t)COVER_PX * COVER_PX * 2);
        lv_obj_invalidate(s_cv);
        lv_obj_clear_flag(s_cv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ph, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cv, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ph, LV_OBJ_FLAG_HIDDEN);
        /* 占位圆盘换成按歌名取的颜色 */
        lv_obj_t *disc = lv_obj_get_child(s_ph, 0);
        if (disc) lv_obj_set_style_bg_color(disc, lv_color_hex(ph_color(np_title())), 0);
    }
}

/* ------------------------------------------------------------------ */

static void lyrics_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 自动切屏：开始播放 -> 进歌词页；长时间停播 -> 回播放器 */
    static uint32_t last_rx_ms = 0;
    if (np_active() && np_online()) last_rx_ms = lv_tick_get();

    if (!s_visible && np_state() != 0) {
        lyrics_ui_show();
    } else if (s_visible && np_state() == 0) {
        if (lv_tick_elaps(last_rx_ms) > 60000) lyrics_ui_back();
    }

    if (!s_visible) return;

    /* ---- 滑动切歌的提示：1.2 秒后自动收掉 ---- */
    if (s_toast_at && lv_tick_elaps(s_toast_at) >= 1200) {
        s_toast_at = 0;
        if (s_lbl_toast) lv_obj_add_flag(s_lbl_toast, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 左侧封面 ---- */
    refresh_cover();

    /* ---- 歌名 ---- */
    const char *ti = np_title();
    lv_label_set_text(s_lbl_song, (ti && ti[0]) ? ti : "歌词");

    /* ---- 歌手 · 专辑 ---- */
    const char *ar = np_artist();
    const char *al = np_album();
    if (ar && ar[0]) {
        if (al && al[0]) snprintf(s_tmp, sizeof(s_tmp), "%s · %s", ar, al);
        else             snprintf(s_tmp, sizeof(s_tmp), "%s", ar);
        lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(C_DIM), 0);
    } else if (np_source() == NP_SRC_LOCAL) {
        snprintf(s_tmp, sizeof(s_tmp), "本机播放");
        lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(C_GREEN), 0);
    } else {
        snprintf(s_tmp, sizeof(s_tmp), "等待电脑连接…");
        lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(C_DIM), 0);
    }
    lv_label_set_text(s_lbl_meta, s_tmp);

    /* ---- 进度 ---- */
    uint32_t pos = np_position_ms();
    uint32_t dur = np_duration_ms();
    if (dur > 0) {
        uint32_t v = (uint32_t)((uint64_t)pos * 1000ULL / dur);
        lv_bar_set_value(s_bar, (int32_t)(v > 1000 ? 1000 : v), LV_ANIM_OFF);
    } else {
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }
    char t1[16], t2[16];
    fmt_ms(pos, t1, sizeof(t1));
    fmt_ms(dur, t2, sizeof(t2));
    snprintf(s_tmp, sizeof(s_tmp), "%s / %s", t1, t2);
    lv_label_set_text(s_lbl_time, s_tmp);

    /* ---- 右下角：来源行，**只在出问题时才显示** ----
     * 歌在放 + 有歌词 = 电脑推送一切正常，此时整行隐藏（不显示 IP、
     * 也不显示「有歌词」这种废话）。只有没推送 / 没歌词时才亮出来，
     * 那时这一行才有用：告诉你控制台在哪个 IP。 */
    if (np_active() && lyrics_ready()) {
        lv_obj_add_flag(s_lbl_src, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (np_source() == NP_SRC_PC) {
            snprintf(s_tmp, sizeof(s_tmp), "%s · %s", np_source_text(), wifi_link_ip());
        } else if (np_source() == NP_SRC_LOCAL) {
            snprintf(s_tmp, sizeof(s_tmp), "%s", np_source_text());
        } else {
            snprintf(s_tmp, sizeof(s_tmp), "等待电脑推送 · %s", wifi_link_ip());
        }
        lv_label_set_text(s_lbl_src, s_tmp);
        lv_obj_clear_flag(s_lbl_src, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- 两行歌词 ---- */
    int cur_track = np_track_key();
    if (cur_track != s_last_track) {
        s_last_track = cur_track;
        s_last_idx   = -2;
    }

    if (lyrics_ready()) {
        lv_obj_add_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_cur,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_lbl_next, LV_OBJ_FLAG_HIDDEN);

        int idx = lyrics_index_at(np_lyric_pos_ms());
        if (idx != s_last_idx) {
            s_last_idx = idx;
            lv_label_set_text(s_lbl_cur,  idx >= 0 ? lyrics_line(idx) : "");
            const char *nx = lyrics_line(idx + 1);
            lv_label_set_text(s_lbl_next, nx ? nx : "");
        }
    } else {
        lv_obj_add_flag(s_lbl_cur,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_next, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_lbl_hint,
                          (np_source() == NP_SRC_PC)
                              ? "这首歌没有歌词，可在网页控制台手动上传 .lrc"
                              : "在电脑上运行 kugou_bridge.py，用酷狗放歌即可");
        lv_obj_clear_flag(s_lbl_hint, LV_OBJ_FLAG_HIDDEN);
        s_last_idx = -2;
    }
}
