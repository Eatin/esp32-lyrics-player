/**
 * @file  wifi_link.h
 * @brief 网络接入：**优先连家里局域网（WiFi STA），失败自动退回自建热点（AP）**
 *
 * 为什么默认是「热点」？因为热点最省事、到哪都能用。
 * 但热点有个硬伤：**手机连上板子热点后就没有外网了**。
 * 所以如果你要「一边用手机听流媒体、一边看板子歌词」，必须让板子和手机待在同一个
 * 家里局域网里（STA 模式），这样手机保留互联网，同时又能访问板子。
 *
 * 行为：
 *   - NVS 里有凭据 -> 以 STA 连路由器（15s 超时）
 *       * 成功：手机浏览器访问 http://<板子IP>/ 或 http://musicplayer.local/
 *       * 失败：退回热点 ESP32-Player，并在串口打印失败原因
 *   - NVS 里没凭据 -> 直接开热点
 *   - 无论哪种模式，热点模式下都能打开 http://192.168.4.1/wifi 填家庭 WiFi，
 *     保存后重启即转为 STA 模式（所以不会把自己锁在外面）
 */
#ifndef WIFI_LINK_H
#define WIFI_LINK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_LINK_NONE = 0,
    WIFI_LINK_STA,      /* 已连上家里路由器（局域网） */
    WIFI_LINK_AP,       /* 自建热点兜底               */
} wifi_link_mode_t;

/** 开机接入网络；成功返回 ESP_OK（无论 STA 还是 AP，都算「可访问」） */
esp_err_t        wifi_link_start(void);

wifi_link_mode_t wifi_link_mode(void);
bool             wifi_link_is_sta(void);      /* 当前是局域网模式 */
bool             wifi_link_online(void);      /* STA 且链路已连上 */
const char      *wifi_link_ip(void);          /* "192.168.1.23"   */
const char      *wifi_link_url(void);         /* "http://192.168.1.23/" */
const char      *wifi_link_ssid(void);        /* 当前 SSID */
const char      *wifi_link_mode_text(void);   /* "局域网" / "热点" */
const char      *wifi_link_saved_ssid(void);  /* NVS 里已保存的家庭 SSID，无则 "" */

/** 保存家庭 WiFi 凭据到 NVS（不重启，调用方决定何时重启） */
esp_err_t        wifi_link_save(const char *ssid, const char *pass);
/** 清除已保存的家庭 WiFi 凭据 */
esp_err_t        wifi_link_forget(void);

/** 掉线重连 / DHCP 续租后 IP 变化检测；请在 loop() 里周期调用 */
void             wifi_link_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_LINK_H */
