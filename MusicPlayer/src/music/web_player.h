/**
 * @file  web_player.h
 * @brief 网页播放器板端服务：WiFi AP + HTTP，把手机变成播放器、板子当歌词屏
 *
 * 数据流：
 *   手机 Safari 打开 http://<ip>/  ->  页面列出 TF 卡歌曲
 *   -> 页面把音频文件下载成 Blob 后由**手机**播放（声音从手机出）
 *   -> 页面每 500ms 调 /api/sync 上报播放位置，并取回板子发来的控制命令
 *   -> 板子按「锚点 + 本地时间」插值推算位置，驱动歌词滚动
 */
#ifndef WEB_PLAYER_H
#define WEB_PLAYER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------- 配置 ------------------------- */
#define WEB_AP_SSID    "ESP32-Player"     /* 板子自建热点名     */
#define WEB_AP_PASS    "12345678"         /* 热点密码（>=8 位） */
#define WEB_MDNS_HOST  "musicplayer"      /* http://musicplayer.local */

/* ------------------------- 控制命令 ------------------------- */
typedef enum {
    WEB_CMD_NONE = 0,
    WEB_CMD_PLAY,
    WEB_CMD_PAUSE,
    WEB_CMD_TOGGLE,
    WEB_CMD_NEXT,
    WEB_CMD_PREV,
    WEB_CMD_SEEK,
} web_cmd_t;

/* ------------------------- 生命周期 ------------------------- */
/**
 * @brief 扫描 TF 卡曲目并启动 HTTP 服务
 * @param ip 板子 IP（如 "192.168.4.1"），仅用于界面显示；可为 NULL
 * @note  WiFi 必须已经在 AP 模式且 SD 卡已挂载（先调用 wav_player_init()）
 */
esp_err_t   web_player_start(const char *ip);
void        web_player_stop(void);
const char *web_player_ip(void);

/* ------------------------- 曲目表 ------------------------- */
int         web_track_count(void);
const char *web_track_name(int i);      /* 显示名（无扩展名），越界返回 NULL */
bool        web_track_has_lrc(int i);

/* ------------------------- 播放状态 ------------------------- */
bool        web_active(void);           /* 已载入曲目（播放中或暂停）   */
int         web_play_state(void);       /* 0=停止 1=播放 2=暂停         */
int         web_cur_index(void);
const char *web_now_title(void);        /* 优先 LRC [ti]，否则文件名     */
const char *web_now_artist(void);       /* LRC [ar]，无则 ""            */
uint32_t    web_position_ms(void);      /* 本地插值后的播放位置          */
uint32_t    web_duration_ms(void);
bool        web_connected(void);        /* 浏览器 5 秒内有过上报         */

/* ------------------------- 屏幕 -> 浏览器 ------------------------- */
void        web_post_cmd(web_cmd_t c, int32_t arg);

/* ------------------- 统一「正在播放」接口（供 UI 使用） -------------------
 *
 * 板子有两个可能的播放入口：
 *   1) 电脑推送（主用法）：PC 上的酷狗在放歌，歌词桥把 歌名/歌手/进度 推过来
 *   2) 板子本地/网页（备用）：TF 卡曲目，由网页或屏幕控制板子自己播放
 *
 * UI 不关心是哪个，统一用下面这组函数取值即可 —— 有电脑推送就优先用电脑的。
 * ------------------------------------------------------------------------ */

typedef enum {
    NP_SRC_NONE = 0,   /* 什么都没有          */
    NP_SRC_LOCAL,      /* 板子本地 / 网页播放  */
    NP_SRC_PC,         /* 电脑推送（歌词机）   */
} np_source_t;

np_source_t np_source(void);
bool        np_active(void);        /* 有正在播放（或暂停）的曲目 */
int         np_state(void);         /* 0=停止 1=播放 2=暂停       */
const char *np_title(void);
const char *np_artist(void);
const char *np_album(void);         /* 专辑名，无则 ""（只有电脑推送才有） */
uint32_t    np_position_ms(void);   /* 真实位置（进度条/时间用）   */
uint32_t    np_lyric_pos_ms(void);  /* 歌词定位用（含歌词偏移）     */
uint32_t    np_duration_ms(void);
bool        np_online(void);        /* 对应来源是否在线（可用“正在播放”提示） */
int         np_track_key(void);     /* 曲目变化标识（变了就重置歌词滚动） */
const char *np_source_text(void);   /* "电脑推送" / "本机播放" / "等待连接" */

/* ------------------- 专辑封面（横屏歌词页左侧那块图） -------------------
 * 曲图由 PC 端推过来（POST /api/ext/cover，156x156 RGB565 大端）。
 * 本机/网页播放没有封面，np_cover_ready() 返回 false，UI 画占位图即可。
 * ---------------------------------------------------------------------- */

bool           np_cover_ready(void);
const uint8_t *np_cover_data(void);   /* 未就绪返回 NULL */
int            np_cover_px(void);     /* 边长（正方形），未就绪返回 0 */
int            np_cover_seq(void);    /* 换了就 +1，UI 靠它决定重画 */

#ifdef __cplusplus
}
#endif

#endif /* WEB_PLAYER_H */
