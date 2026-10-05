/**
 * @file  wifi_link.cpp
 * @brief 局域网优先 / 热点兜底 的网络接入实现
 *
 * 注意 1：本文件必须是 .cpp —— Arduino 的 WiFi / Preferences / String 都是 C++ API，
 *         放进 .c 文件会因 C 编译器不认识 class 而编译失败。
 * 注意 2：本 Arduino 环境把 ESP_LOGI/ESP_LOGW 在编译期删掉了
 *         （预编译库 CONFIG_LOG_MAXIMUM_LEVEL=ERROR），所以这里一律用 printf。
 */
#include "wifi_link.h"
#include "web_player.h"      /* WEB_AP_SSID / WEB_AP_PASS / WEB_MDNS_HOST */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#include <stdio.h>
#include <string.h>

#define NVS_NS           "wlink"
#define STA_TIMEOUT_MS   15000   /* 连家里路由器的等待上限 */
#define AP_CHANNEL       6
#define AP_MAX_CONN      4

static wifi_link_mode_t s_mode = WIFI_LINK_NONE;
static char s_ip[20]        = "0.0.0.0";
static char s_url[28]       = "http://0.0.0.0/";
static char s_ssid[40]      = "";
static bool s_mdns_started  = false;

/* ------------------------------------------------------------------ */

static void set_ip(const char *ip)
{
    strncpy(s_ip, ip, sizeof(s_ip) - 1);
    s_ip[sizeof(s_ip) - 1] = '\0';
    snprintf(s_url, sizeof(s_url), "http://%s/", s_ip);
}

static const char *status_name(wl_status_t st)
{
    switch (st) {
        case WL_IDLE_STATUS:     return "IDLE";
        case WL_NO_SSID_AVAIL:   return "找不到该SSID(是不是5G?板子只支持2.4G)";
        case WL_SCAN_COMPLETED:  return "SCAN_DONE";
        case WL_CONNECTED:       return "已连接";
        case WL_CONNECT_FAILED:  return "认证失败(密码错误?)";
        case WL_CONNECTION_LOST: return "连接丢失";
        case WL_DISCONNECTED:    return "未连接(信号弱或凭据过期)";
        default:                 return "未知";
    }
}

static void start_mdns(void)
{
    if (s_mdns_started) return;
    if (MDNS.begin(WEB_MDNS_HOST)) {
        MDNS.addService("http", "tcp", 80);
        s_mdns_started = true;
        printf("[WiFi] mDNS   : http://%s.local/\n", WEB_MDNS_HOST);
    } else {
        printf("[WiFi] mDNS 启动失败（不影响用 IP 访问）\n");
    }
}

static void start_ap(void)
{
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    bool ok = WiFi.softAP(WEB_AP_SSID, WEB_AP_PASS, AP_CHANNEL, false, AP_MAX_CONN);

    IPAddress ip = WiFi.softAPIP();
    char buf[20];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    set_ip(buf);

    s_mode = WIFI_LINK_AP;
    strncpy(s_ssid, WEB_AP_SSID, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    printf("[WiFi] AP 热点: %s  ok=%d  ip=%s\n", WEB_AP_SSID, (int)ok, s_ip);
    start_mdns();
}

/* ------------------------------------------------------------------ */

const char *wifi_link_saved_ssid(void)
{
    static char saved[40] = "";
    Preferences p;
    if (!p.begin(NVS_NS, true)) return "";
    String s = p.getString("ssid", "");
    p.end();
    strncpy(saved, s.c_str(), sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';
    return saved;
}

esp_err_t wifi_link_start(void)
{
    Preferences p;
    p.begin(NVS_NS, true);
    String ssid = p.getString("ssid", "");
    String pass = p.getString("pass", "");
    p.end();

    /* 关掉 core 自带的 NVS 持久化，凭据统一由本模块管理，避免两边打架 */
    WiFi.persistent(false);
    WiFi.setSleep(false);

    if (ssid.length() > 0) {
        printf("[WiFi] 局域网模式：连接 \"%s\" ...\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());

        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - t0) < STA_TIMEOUT_MS) {
            delay(200);
            printf(".");
        }
        printf("\n");

        if (WiFi.status() == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            char buf[20];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            set_ip(buf);

            s_mode = WIFI_LINK_STA;
            strncpy(s_ssid, ssid.c_str(), sizeof(s_ssid) - 1);
            s_ssid[sizeof(s_ssid) - 1] = '\0';

            WiFi.setAutoReconnect(true);
            printf("[WiFi] STA 已连接  ip=%s  rssi=%d dBm  (手机连同一个路由器的 WiFi 即可访问)\n",
                   s_ip, (int)WiFi.RSSI());
            start_mdns();
            return ESP_OK;
        }

        printf("[WiFi] STA 失败：%s\n", status_name(WiFi.status()));
        printf("[WiFi] -> 退回热点模式兜底（打开 http://192.168.4.1/wifi 重新配置）\n");
        WiFi.disconnect(true, false);
    } else {
        printf("[WiFi] 未配置家庭 WiFi -> 直接开热点（浏览器打开 /wifi 页面可配置）\n");
    }

    start_ap();
    return ESP_OK;
}

void wifi_link_tick(void)
{
    if (s_mode != WIFI_LINK_STA) return;

    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 5000) return;
    last = now;

    if (WiFi.status() == WL_CONNECTED) {
        /* DHCP 续租可能导致 IP 变化，及时更新界面上的地址 */
        IPAddress ip = WiFi.localIP();
        char buf[20];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        if (strcmp(buf, s_ip) != 0) {
            set_ip(buf);
            printf("[WiFi] IP 变化 -> %s\n", s_ip);
        }
        return;
    }

    printf("[WiFi] 掉线（%s），尝试重连...\n", status_name(WiFi.status()));
    WiFi.reconnect();
}

/* ------------------------------------------------------------------ */

esp_err_t wifi_link_save(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;

    Preferences p;
    if (!p.begin(NVS_NS, false)) return ESP_FAIL;
    p.putString("ssid", ssid);
    p.putString("pass", pass ? pass : "");
    p.end();

    printf("[WiFi] 已保存家庭 WiFi：SSID=\"%s\"（重启后生效）\n", ssid);
    return ESP_OK;
}

esp_err_t wifi_link_forget(void)
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) return ESP_FAIL;
    p.clear();
    p.end();
    printf("[WiFi] 已清除家庭 WiFi 凭据（重启后进入热点模式）\n");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

wifi_link_mode_t wifi_link_mode(void)      { return s_mode; }
bool             wifi_link_is_sta(void)    { return s_mode == WIFI_LINK_STA; }
bool             wifi_link_online(void)    { return s_mode == WIFI_LINK_STA && WiFi.status() == WL_CONNECTED; }
const char      *wifi_link_ip(void)        { return s_ip; }
const char      *wifi_link_url(void)       { return s_url; }
const char      *wifi_link_ssid(void)      { return s_ssid; }
const char      *wifi_link_mode_text(void)
{
    return s_mode == WIFI_LINK_STA ? "局域网" : "热点";
}
