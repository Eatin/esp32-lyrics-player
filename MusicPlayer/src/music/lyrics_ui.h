/**
 * @file  lyrics_ui.h
 * @brief 歌词显示界面（172x640 竖屏，中文字体）
 */
#ifndef LYRICS_UI_H
#define LYRICS_UI_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建歌词界面（不立即显示）。须在持有 LVGL 锁时调用。 */
void lyrics_ui_init(void);

/** @brief 切换到歌词界面 */
void lyrics_ui_show(void);

/** @brief 切回播放器界面 */
void lyrics_ui_back(void);

/** @brief 当前是否显示歌词界面 */
bool lyrics_ui_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* LYRICS_UI_H */
