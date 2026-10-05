/**
 * @file  ext_link.h
 * @brief 外部歌词推送通道：**PC 端「歌词桥」 -> 板子**（板子只当歌词机）
 *
 * 使用场景（当前主推）：
 *   电脑上用酷狗放歌，板子完全不碰音频，只负责显示同步歌词。
 *   PC 端脚本读 Windows SMTC 拿到「歌名 / 歌手 / 时长 / 播放位置 / 播放状态」，
 *   去歌词接口抓 LRC，再通过下面这套接口推给板子：
 *
 *     POST /api/ext/track?dur=<ms>[&title=..&artist=..]      body = LRC 原文(UTF-8)
 *     POST /api/ext/pos?p=<ms>&s=<0|1|2>[&d=<总时长ms>]      每 ~0.5s 一次
 *     POST /api/ext/cover                                    body = 156x156 RGB565 原始像素
 *     POST /api/ext/cover?clear=1                            去掉封面（显示占位图）
 *     GET  /api/ext/state                                    查询当前状态(JSON)
 *     POST /api/ext/offset?d=<ms>   或  ?v=<ms>               歌词整体偏移（对轴微调）
 *     POST /api/ext/clear                                    清空外部曲目
 *
 * 封面为什么推「原始 RGB565」而不是 JPEG：
 *   ESP32-S3 没有 JPEG 硬解，软解要占几十 KB flash 和几百 ms CPU；
 *   而 PC 那边本来就要用 Pillow 缩放，顺手转成 156x156 RGB565 一共才 48KB，
 *   局域网上一次 POST 就传完了。板子端零解码、零 flash 开销。
 *   字节序用「标准 RGB565 的大端」（即 pack('>H', v)），
 *   正好匹配 lv_conf.h 里的 LV_COLOR_16_SWAP=1。
 *
 * 位置策略：板子只把 PC 上报的位置当**锚点**，之后用板子自己的微秒时钟插值，
 *   所以 PC 上报间隔哪怕抖到 2 秒，歌词依然是平滑的。
 *   上报值与预期偏差 >2s 视为「拖动进度」直接重锚；小幅漂移只吸收 1/4，避免抖词。
 *
 * 自动发现：PC 向 255.255.255.255:48899 广播 "ESP32LYRICS?"，
 *   板子单播回 "ESP32LYRICS!<ip>|<mode>|<tracks>|<ssid>"，PC 就不用你手填 IP。
 */
#ifndef EXT_LINK_H
#define EXT_LINK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** UDP 自动发现端口（PC 广播 / 板子应答） */
#define EXT_UDP_PORT   48899

/* ------------------------- 生命周期 ------------------------- */

/**
 * @brief 启动 UDP 自动发现任务
 * @note  需要在 WiFi 起来之后调用（应答里要填本机 IP）
 */
void ext_link_init(void);

/* ------------------------- 状态查询 ------------------------- */

/** 是否「持有」一条外部曲目（收到过后，且 3 分钟内有过推送） */
bool        ext_link_active(void);
/** PC 是否「在线」（10 秒内有过推送） */
bool        ext_link_online(void);
/** 距上次推送的毫秒数（无推送时返回一个大值） */
uint32_t    ext_link_silence_ms(void);

int         ext_link_state(void);          /* 0=停止 1=播放 2=暂停 */
const char *ext_link_title(void);          /* 歌名，无则 ""        */
const char *ext_link_artist(void);         /* 歌手，无则 ""        */
const char *ext_link_album(void);          /* 专辑，无则 ""        */
uint32_t    ext_link_duration_ms(void);

/** 真实播放位置（插值后，未加歌词偏移）—— 用于进度条与时间显示 */
uint32_t    ext_link_position_ms(void);
/** 歌词定位用的位置 = 真实位置 + 偏移 */
uint32_t    ext_link_lyric_pos_ms(void);

int32_t     ext_link_offset_ms(void);      /* 当前歌词偏移 */
void        ext_link_set_offset(int32_t ms);
void        ext_link_add_offset(int32_t d);

uint32_t    ext_link_rx_count(void);       /* 累计收到的推送次数 */
uint32_t    ext_link_jump_count(void);     /* 被判定为「拖动」的次数 */
uint32_t    ext_link_track_seq(void);      /* 换歌序号：只在收到新曲目时 +1 */

/* ------------------------- 专辑封面 ------------------------- */

/** 封面边长（正方形），必须与 user_config.h 的 COVER_PX 一致 */
#define XL_COVER_PX  156

/** 是否已经收到过封面（false 时 UI 显示占位图） */
bool           ext_link_cover_ready(void);
/** 封面像素：XL_COVER_PX*XL_COVER_PX 个 RGB565（大端字节序），未就绪返回 NULL */
const uint8_t *ext_link_cover_data(void);
/** 收到新封面 / 被清除 都会 +1（UI 靠它决定要不要重画） */
uint32_t       ext_link_cover_seq(void);

/* ------------------------- HTTP handlers ------------------------- */
/* 这些函数在 web_player.c 里注册到 httpd 上（纯 C，可直接取地址） */

esp_err_t ext_link_h_track (httpd_req_t *req);   /* POST /api/ext/track   */
esp_err_t ext_link_h_pos   (httpd_req_t *req);   /* POST /api/ext/pos     */
esp_err_t ext_link_h_state (httpd_req_t *req);   /* GET  /api/ext/state   */
esp_err_t ext_link_h_offset(httpd_req_t *req);   /* POST /api/ext/offset  */
esp_err_t ext_link_h_clear (httpd_req_t *req);   /* POST /api/ext/clear   */
esp_err_t ext_link_h_cover (httpd_req_t *req);   /* POST /api/ext/cover   */

/* UDP 自动发现任务的启动次数（诊断用） */
int         ext_link_udp_ok(void);

#ifdef __cplusplus
}
#endif

#endif /* EXT_LINK_H */
