/**
 * @file  wav_player.h
 * @brief SD 卡 WAV（PCM）音乐播放引擎
 *
 *  - 通过 SDMMC 1 线方式挂载 Micro SD 卡到 /sdcard
 *  - 递归扫描 .wav / .WAV 文件（最多 2 层目录）
 *  - 使用板载 ES8311 DAC + NS4150 功放播放（16bit / 立体声输出）
 *  - 独立 FreeRTOS 任务播放，接口线程安全，可被 LVGL 回调直接调用
 */
#ifndef WAV_PLAYER_H
#define WAV_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WAV_MAX_TRACKS      100      /* 最多扫描到的曲目数            */
#define WAV_NAME_LEN        56       /* 显示用文件名（不含路径）长度   */
#define WAV_PATH_LEN        192      /* 完整路径长度                  */

typedef struct {
    char     name[WAV_NAME_LEN];     /* 文件名（显示用），已去掉 .wav */
    char     path[WAV_PATH_LEN];     /* VFS 完整路径，如 /sdcard/a.wav */
    uint32_t sample_rate;            /* 采样率，如 44100              */
    uint16_t channels;               /* 源声道数 1 / 2                */
    uint16_t bits;                   /* 源位深 8 / 16 / 24 / 32       */
    uint32_t data_bytes;             /* PCM 数据长度                  */
    uint32_t duration_ms;            /* 时长（毫秒）                  */
} wav_track_t;

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化音频编解码器、挂载 SD 卡、扫描曲目并启动播放任务
 * @note  必须在 i2c_master_Init() 与 TCA9554(功放使能) 初始化之后调用
 */
esp_err_t wav_player_init(void);

/** @brief 重新扫描 SD 卡（会先停止当前播放） */
esp_err_t wav_rescan(void);

/** @brief SD 卡是否挂载成功 */
bool wav_sd_ready(void);

/** @brief 当前扫描到的曲目数量 */
int  wav_track_count(void);

/** @brief 获取第 index 首曲目信息，越界返回 NULL */
const wav_track_t *wav_track(int index);

/* ------------------------------------------------------------------ */
/* 播放控制（均可在 LVGL 事件回调里直接调用）                            */
/* ------------------------------------------------------------------ */

void wav_play_index(int index);   /* 播放指定曲目，越界则忽略        */
void wav_toggle(void);            /* 播放 <-> 暂停                   */
void wav_next(void);              /* 下一首（到末尾回到第一首）       */
void wav_prev(void);              /* 上一首                          */
void wav_stop(void);              /* 停止播放                        */

/* ------------------------------------------------------------------ */
/* 状态查询（供 UI 定时刷新）                                           */
/* ------------------------------------------------------------------ */

bool        wav_is_playing(void);
bool        wav_is_paused(void);
int         wav_current_index(void);
uint32_t    wav_position_ms(void);
uint32_t    wav_duration_ms(void);
const char *wav_state_text(void);   /* "Playing"/"Paused"/... */

/* 音量 0 ~ 100 */
void wav_set_volume(int vol);
int  wav_get_volume(void);

#ifdef __cplusplus
}
#endif

#endif /* WAV_PLAYER_H */
