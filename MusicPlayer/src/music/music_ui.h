/**
 * @file  music_ui.h
 * @brief 音乐播放器触摸界面（LVGL v8.4）
 */
#ifndef MUSIC_UI_H
#define MUSIC_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建音乐播放器界面（由 lvgl_port_init() 在持有 LVGL 锁时调用）
 */
void music_ui_init(void);

/**
 * @brief 播放器主屏对象（供歌词界面切回来使用）
 */
lv_obj_t *music_ui_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_UI_H */
