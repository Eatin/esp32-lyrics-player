/**
 * @file  music_ui.c
 * @brief 音乐播放器触摸界面（LVGL v8.4，横屏 640 x 172）
 *
 * 本机播放（TF 卡 WAV / 网页播放）是**备用通道**；主力用法是电脑推歌词。
 * 所以这一页做成「列表 + 播放控制」的紧凑横版：
 *
 *   ┌─ 顶栏 ────────────────────────────────────────────────┐  y  0..26
 *   │ 音乐播放器 │ 地址/推送状态              [重扫] [歌词]   │
 *   ├─────────────┬─────────────────────────────────────────┤
 *   │ 曲目列表     │ 歌名                          状态      │  y 30..48
 *   │ (可滚动)     │ ▓▓▓▓▓▓░░░░░░░░░░  进度条                 │  y 54..62
 *   │             │ 时间                        格式信息      │  y 66..84
 *   │             │ Vol 70% · 3 tracks                       │  y 94..112
 *   │             │ [◀] [▶] [▶]     音量  ────●────           │  y124..168
 *   └─────────────┴─────────────────────────────────────────┘
 *    4         248 256                                   634
 *
 * 说明：竖屏版那块 EQ 频谱条在横屏里挤不下（纯装饰），已去掉；
 *       刷新节奏和位置插值逻辑与之前完全一致。
 */
#include "music_ui.h"
#include "wav_player.h"
#include "lyrics_ui.h"
#include "web_player.h"
#include "wifi_link.h"
#include "user_config.h"

#include "lvgl.h"
#include "src/font/lv_font_cjk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 可调项                                                              */
/* ------------------------------------------------------------------ */

/* 列表中最多显示多少首（LVGL 内存池默认 48KB，过大需要同时调大
 * libraries/lvgl8/lv_conf.h 里的 LV_MEM_SIZE） */
#ifndef UI_MAX_ITEMS
#define UI_MAX_ITEMS 48
#endif

/* 字号：中文一律用自带中文字库；只有控制按钮上的 LV_SYMBOL_* 用 montserrat
 * （LV_SYMBOL_* 的字形在 montserrat 里，中文字库没有） */
#define FONT_TITLE (&lv_font_cjk_16)
#define FONT_SM    (&lv_font_cjk_16)
#define FONT_SYM   (&lv_font_montserrat_14)

/* 屏幕尺寸 */
#define SCR_W  EXAMPLE_LCD_H_RES     /* 640 */
#define SCR_H  EXAMPLE_LCD_V_RES     /* 172 */

/* 版面 */
#define BAR_H      26
#define LIST_X     4
#define LIST_Y     30
#define LIST_W     244
#define LIST_H     (SCR_H - LIST_Y - 4)          /* 138 */
#define R_X        (LIST_X + LIST_W + 8)         /* 256 */
#define R_W        (SCR_W - R_X - 6)             /* 378 */

/* 配色 */
#define C_BG       0x0D0F16
#define C_BAR      0x161B27
#define C_PANEL    0x1A2030
#define C_ACCENT   0x4C8DFF
#define C_ACCENT2  0x2AD4A8
#define C_TEXT     0xE8EDF6
#define C_DIM      0x7C879C
#define C_SEL      0x22304A
#define C_HILITE   0x2C3F63

/* ------------------------------------------------------------------ */
/* 控件句柄                                                            */
/* ------------------------------------------------------------------ */
static lv_obj_t *s_lbl_title;      /* 标题栏文字        */
static lv_obj_t *s_list;           /* 列表容器          */
static lv_obj_t *s_lbl_song;       /* 正在播放曲名      */
static lv_obj_t *s_bar;            /* 进度条            */
static lv_obj_t *s_lbl_time;       /* 时间              */
static lv_obj_t *s_lbl_state;      /* 播放状态          */
static lv_obj_t *s_lbl_info;       /* 音频格式信息      */
static lv_obj_t *s_btn_play;       /* 播放/暂停按钮     */
static lv_obj_t *s_lbl_play;       /* 按钮上的符号      */
static lv_obj_t *s_slider_vol;     /* 音量滑条          */
static lv_obj_t *s_lbl_status;     /* Vol / 曲目数       */
static lv_obj_t *s_item[UI_MAX_ITEMS];
static lv_obj_t *s_scr;            /* 播放器主屏（歌词页切回来用） */
static lv_obj_t *s_lbl_url;        /* 顶栏：地址 / 推送状态        */

static int  s_item_count = -1;     /* 已生成的列表项数   */
static int  s_hl_index   = -2;     /* 当前高亮项         */
static char s_vol_buf[128];

/* 前向声明 */
static void rebuild_list(void);
static void ui_timer_cb(lv_timer_t *t);

lv_obj_t *music_ui_screen(void) { return s_scr; }

/* ================================================================== */
/* 回调                                                                */
/* ================================================================== */

static void fmt_time(uint32_t ms, char *out, size_t n)
{
    uint32_t s = ms / 1000u;
    if (s >= 3600u) {
        snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600u),
                 (unsigned)((s / 60u) % 60u), (unsigned)(s % 60u));
    } else {
        snprintf(out, n, "%02u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
    }
}

static void item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    wav_play_index(idx);
}

static void btn_prev_cb(lv_event_t *e) { (void)e; wav_prev(); }
static void btn_next_cb(lv_event_t *e) { (void)e; wav_next(); }
static void btn_play_cb(lv_event_t *e) { (void)e; wav_toggle(); }

static void btn_rescan_cb(lv_event_t *e)
{
    (void)e;
    wav_rescan();
}

/* 切到歌词界面（电脑推送 / 本机播放都会自动切，这里也给个手动入口） */
static void btn_lyrics_cb(lv_event_t *e)
{
    (void)e;
    lyrics_ui_show();
}

static void slider_vol_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    wav_set_volume((int)lv_slider_get_value(sl));
}

/* ================================================================== */
/* 构建                                                                */
/* ================================================================== */

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

/** 顶栏上的小圆角按钮 */
static lv_obj_t *make_bar_btn(lv_obj_t *parent, int x, int w, const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, 2);
    lv_obj_set_size(b, w, BAR_H - 4);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_HILITE), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = make_label(b, txt, C_TEXT, FONT_SM);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *make_ctrl_btn(lv_obj_t *parent, int x, const char *symbol, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, 0);
    lv_obj_set_size(b, 44, 44);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_HILITE), 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = make_label(b, symbol, C_TEXT, FONT_SYM);
    lv_obj_center(l);
    return b;
}

void music_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---------- 顶栏 ---------- */
    lv_obj_t *topbar = make_panel(scr, 0, 0, SCR_W, BAR_H, C_BAR);
    lv_obj_set_style_radius(topbar, 0, 0);
    s_lbl_title = make_label(topbar, "音乐播放器", C_TEXT, FONT_TITLE);
    lv_obj_align(s_lbl_title, LV_ALIGN_LEFT_MID, 8, 0);

    /* 顶栏中段：地址 / 电脑推送状态 */
    s_lbl_url = make_label(topbar, "", C_DIM, FONT_SM);
    lv_obj_set_pos(s_lbl_url, 108, 5);
    lv_obj_set_width(s_lbl_url, SCR_W - 108 - 132);
    lv_label_set_long_mode(s_lbl_url, LV_LABEL_LONG_DOT);

    make_bar_btn(topbar, SCR_W - 130, 62, "重扫", btn_rescan_cb);
    make_bar_btn(topbar, SCR_W - 64,  58, "歌词", btn_lyrics_cb);

    /* ---------- 左：曲目列表 ---------- */
    s_list = lv_obj_create(scr);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_pos(s_list, LIST_X, LIST_Y);
    lv_obj_set_size(s_list, LIST_W, LIST_H);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_list, 6, 0);
    lv_obj_set_style_pad_all(s_list, 3, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(C_HILITE), LV_PART_SCROLLBAR);

    /* ---------- 右：正在播放 ---------- */
    s_lbl_song = make_label(scr, "No track", C_TEXT, FONT_SM);
    lv_obj_set_pos(s_lbl_song, R_X, 30);
    lv_obj_set_width(s_lbl_song, R_W - 128);
    lv_label_set_long_mode(s_lbl_song, LV_LABEL_LONG_DOT);

    s_lbl_state = make_label(scr, "Stopped", C_ACCENT2, FONT_SM);
    lv_obj_set_pos(s_lbl_state, SCR_W - 128, 30);
    lv_obj_set_width(s_lbl_state, 122);
    lv_obj_set_style_text_align(s_lbl_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_state, LV_LABEL_LONG_DOT);

    s_bar = lv_bar_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_pos(s_bar, R_X, 54);
    lv_obj_set_size(s_bar, R_W, 8);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x2B3242), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_lbl_time = make_label(scr, "00:00 / 00:00", C_DIM, FONT_SM);
    lv_obj_set_pos(s_lbl_time, R_X, 66);

    s_lbl_info = make_label(scr, "", C_DIM, FONT_SM);
    lv_obj_set_pos(s_lbl_info, R_X + R_W - 234, 66);
    lv_obj_set_width(s_lbl_info, 234);
    lv_obj_set_style_text_align(s_lbl_info, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_lbl_info, LV_LABEL_LONG_DOT);

    s_lbl_status = make_label(scr, "", C_DIM, FONT_SM);
    lv_obj_set_pos(s_lbl_status, R_X, 94);
    lv_obj_set_width(s_lbl_status, R_W);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_DOT);

    /* ---------- 右：控制按钮 ---------- */
    lv_obj_t *ctrl = lv_obj_create(scr);
    lv_obj_remove_style_all(ctrl);
    lv_obj_set_pos(ctrl, R_X, 124);
    lv_obj_set_size(ctrl, 3 * 52 + 8, 44);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);

    make_ctrl_btn(ctrl, 0,   LV_SYMBOL_PREV,  btn_prev_cb);
    s_btn_play = make_ctrl_btn(ctrl, 52, LV_SYMBOL_PLAY, btn_play_cb);
    lv_obj_set_style_bg_color(s_btn_play, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_width(s_btn_play, 0, 0);
    s_lbl_play = lv_obj_get_child(s_btn_play, 0);
    make_ctrl_btn(ctrl, 104, LV_SYMBOL_NEXT, btn_next_cb);

    /* ---------- 右：音量 ---------- */
    lv_obj_t *vl = make_label(scr, "音量", C_DIM, FONT_SM);
    lv_obj_set_pos(vl, R_X + 164, 112);

    s_slider_vol = lv_slider_create(scr);
    lv_obj_set_pos(s_slider_vol, R_X + 164, 136);
    lv_obj_set_size(s_slider_vol, R_W - 164, 12);
    lv_slider_set_range(s_slider_vol, 0, 100);
    lv_slider_set_value(s_slider_vol, wav_get_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider_vol, lv_color_hex(0x2B3242), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider_vol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_vol, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider_vol, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_vol, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_vol, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_vol, slider_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_item_count = -1;
    s_hl_index   = -2;

    /* 400ms 周期刷新进度、状态 */
    lv_timer_create(ui_timer_cb, 400, NULL);

    /* 首次构建列表 */
    rebuild_list();
    ui_timer_cb(NULL);

    /* 歌词界面（创建但先不显示） */
    lyrics_ui_init();
}

/* ================================================================== */
/* 列表重建                                                            */
/* ================================================================== */

#define ITEM_W (LIST_W - 12)
#define ITEM_H 24

static void rebuild_list(void)
{
    lv_obj_clean(s_list);

    int total = wav_track_count();
    int show  = total > UI_MAX_ITEMS ? UI_MAX_ITEMS : total;
    s_item_count = show;

    if (show == 0) {
        lv_obj_t *l = make_label(s_list, wav_sd_ready() ? "没有找到 .wav\n把文件放到 TF 卡根目录"
                                                        : "未检测到 TF 卡",
                                 C_DIM, FONT_SM);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, ITEM_W);
        lv_obj_set_style_pad_top(l, 16, 0);
        s_hl_index = -2;
        return;
    }

    for (int i = 0; i < show; i++) {
        const wav_track_t *t = wav_track(i);
        if (!t) break;

        char buf[WAV_NAME_LEN + 16];
        snprintf(buf, sizeof(buf), "%02d  %s", i + 1, t->name);

        lv_obj_t *b = lv_btn_create(s_list);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, ITEM_W, ITEM_H);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_BAR), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_pad_left(b, 6, 0);
        lv_obj_set_style_pad_right(b, 6, 0);
        lv_obj_add_event_cb(b, item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l = make_label(b, buf, C_TEXT, FONT_SM);
        lv_obj_set_width(l, ITEM_W - 16);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        s_item[i] = b;
    }
    s_hl_index = -2;
}

static void highlight(int idx)
{
    if (idx == s_hl_index) return;
    if (s_hl_index >= 0 && s_hl_index < s_item_count && s_item[s_hl_index]) {
        lv_obj_set_style_bg_color(s_item[s_hl_index], lv_color_hex(C_BAR), 0);
        lv_obj_t *l = lv_obj_get_child(s_item[s_hl_index], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    }
    if (idx >= 0 && idx < s_item_count && s_item[idx]) {
        lv_obj_set_style_bg_color(s_item[idx], lv_color_hex(C_SEL), 0);
        lv_obj_t *l = lv_obj_get_child(s_item[idx], 0);
        if (l) lv_obj_set_style_text_color(l, lv_color_hex(C_ACCENT), 0);
        lv_obj_scroll_to_view(s_item[idx], LV_ANIM_ON);
    }
    s_hl_index = idx;
}

/* ================================================================== */
/* 定时刷新                                                            */
/* ================================================================== */

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 曲目数量变化 -> 重建列表 */
    int total = wav_track_count();
    int show  = total > UI_MAX_ITEMS ? UI_MAX_ITEMS : total;
    if (show != s_item_count) {
        rebuild_list();
    }

    /* 播放按钮图标 */
    const char *sym = wav_is_playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
    if (strcmp(lv_label_get_text(s_lbl_play), sym) != 0) {
        lv_label_set_text(s_lbl_play, sym);
    }

    /* 当前曲目 */
    int cur = wav_current_index();
    const wav_track_t *tk = wav_track(cur);
    lv_label_set_text(s_lbl_song, tk ? tk->name : "No track");

    /* 进度 */
    uint32_t pos = wav_position_ms();
    uint32_t dur = wav_duration_ms();
    if (dur > 0) {
        uint32_t v = (uint32_t)((uint64_t)pos * 1000ULL / dur);
        if (v > 1000) v = 1000;
        lv_bar_set_value(s_bar, (int32_t)v, LV_ANIM_OFF);
    } else {
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    }

    char tb[40], t1[16], t2[16];
    fmt_time(pos, t1, sizeof(t1));
    fmt_time(dur, t2, sizeof(t2));
    snprintf(tb, sizeof(tb), "%s / %s", t1, t2);
    lv_label_set_text(s_lbl_time, tb);

    lv_label_set_text(s_lbl_state, wav_state_text());

    /* 格式信息 */
    if (tk) {
        char ib[64];
        snprintf(ib, sizeof(ib), "%u Hz  %u bit  %s",
                 (unsigned)tk->sample_rate, tk->bits,
                 tk->channels >= 2 ? "Stereo" : "Mono");
        lv_label_set_text(s_lbl_info, ib);
    } else {
        lv_label_set_text(s_lbl_info, "");
    }

    /* 列表高亮 */
    highlight(cur);

    /* Vol / 曲目数 / 是否有电脑在推 */
    snprintf(s_vol_buf, sizeof(s_vol_buf), "Vol %d%%   %d track%s   %s",
             wav_get_volume(), total, total == 1 ? "" : "s", np_source_text());
    lv_label_set_text(s_lbl_status, s_vol_buf);

    /* 顶栏中段：优先显示「电脑推送」的歌名；没有推送时显示本机地址 */
    if (np_source() == NP_SRC_PC) {
        snprintf(s_vol_buf, sizeof(s_vol_buf), "%s  %s - %s",
                 np_source_text(), np_title(), np_artist());
    } else {
        snprintf(s_vol_buf, sizeof(s_vol_buf), "%s %s",
                 wifi_link_mode_text(), wifi_link_ip());
    }
    lv_label_set_text(s_lbl_url, s_vol_buf);

    /* 音量滑条跟随（避免与用户拖动打架） */
    if (!lv_obj_has_state(s_slider_vol, LV_STATE_PRESSED)) {
        int v = wav_get_volume();
        if (lv_slider_get_value(s_slider_vol) != v) {
            lv_slider_set_value(s_slider_vol, v, LV_ANIM_OFF);
        }
    }
}
