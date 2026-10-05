/**
 * @file  player_page.h
 * @brief 内嵌的网页页面（HTML/JS 单文件，编译进固件）
 *
 *   player_page_html —— 播放器主页（/）
 *   wifi_page_html   —— 网络配置页（/wifi）
 */
#ifndef PLAYER_PAGE_H
#define PLAYER_PAGE_H

#ifdef __cplusplus
extern "C" {
#endif

extern const char        player_page_html[];
extern const unsigned int player_page_len;

extern const char        wifi_page_html[];
extern const unsigned int wifi_page_len;

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_PAGE_H */
