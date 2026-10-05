/**
 * @file  web_player.c
 * @brief 网页播放器板端服务实现（HTTP + 曲目扫描 + 播放位置插值）
 *
 * 注意：这套 Arduino 环境里 ESP_LOGI/ESP_LOGW 在编译期就被删掉了
 *      （预编译库 CONFIG_LOG_MAXIMUM_LEVEL=ERROR），排障信息一律用 printf。
 */
#include "web_player.h"
#include "lyrics.h"
#include "player_page.h"
#include "wifi_link.h"
#include "ext_link.h"
#include "lvgl_port.h"
#include "../board/board_io.h"
#include "user_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WEB";

#define DBG(fmt, ...) do { printf("[WEB] " fmt "\n", ##__VA_ARGS__); } while (0)

#define SD_ROOT        "/sdcard"
#define WP_MAX_TRACKS  200
#define WP_NAME_LEN    96
#define WP_REL_LEN     144
#define STREAM_CHUNK   8192

/* ------------------------------------------------------------------ */
/* 曲目表                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char     name[WP_NAME_LEN];    /* 显示名（无扩展名）        */
    char     rel[WP_REL_LEN];      /* 相对 /sdcard 的路径（URL 用） */
    uint32_t size;
    bool     has_lrc;
} wp_track_t;

static wp_track_t *s_tracks = NULL;
static int         s_track_cap = 0;
static int         s_track_count = 0;
static char        s_ip[24] = "192.168.4.1";

/* ------------------------------------------------------------------ */
/* 播放状态                                                            */
/* ------------------------------------------------------------------ */

static httpd_handle_t s_srv = NULL;

static volatile int      s_cur        = -1;
static volatile int      s_rate       = 0;      /* 1=播放 0=暂停/停止 */
static volatile uint32_t s_anchor_pos = 0;      /* 上报时刻的位置(ms) */
static volatile int64_t  s_anchor_us  = 0;      /* 上报时刻           */
static volatile uint32_t s_dur        = 0;
static volatile int64_t  s_last_rx_us = 0;
static volatile int      s_cmd        = WEB_CMD_NONE;
static volatile int32_t  s_cmd_arg    = 0;
static volatile int      s_lyric_for  = -1;

static char s_fallback_title[WP_NAME_LEN] = "";

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static bool has_ext(const char *n, const char *e)
{
    size_t ln = strlen(n), le = strlen(e);
    return ln > le && strcasecmp(n + ln - le, e) == 0;
}

static bool is_audio_name(const char *n)
{
    static const char *exts[] = { ".mp3", ".wav", ".flac", ".m4a", ".aac",
                                  ".ogg", ".opus", ".wma", ".aif", ".aiff" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (has_ext(n, exts[i])) return true;
    }
    return false;
}

static const char *ctype_for(const char *name)
{
    if (has_ext(name, ".mp3"))  return "audio/mpeg";
    if (has_ext(name, ".wav"))  return "audio/wav";
    if (has_ext(name, ".flac")) return "audio/flac";
    if (has_ext(name, ".m4a"))  return "audio/mp4";
    if (has_ext(name, ".aac"))  return "audio/aac";
    if (has_ext(name, ".ogg"))  return "audio/ogg";
    if (has_ext(name, ".opus")) return "audio/ogg";
    if (has_ext(name, ".wma"))  return "audio/x-ms-wma";
    return "application/octet-stream";
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int url_decode(const char *in, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= out_sz) return -1;
        if (in[i] == '%') {
            int hi = hexval(in[i + 1]);
            int lo = in[i + 1] ? hexval(in[i + 2]) : -1;
            if (hi < 0 || lo < 0) return -1;
            out[o++] = (char)((hi << 4) | lo);
            i += 2;
        } else if (in[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return 0;
}

static void url_encode(const char *in, char *out, size_t out_sz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

static void json_escape(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* 把 /sdcard/<rel> 的扩展名换成 .lrc 并检查存在性 */
static bool lrc_path_for(const char *rel, char *out, size_t n)
{
    if (snprintf(out, n, SD_ROOT "/%s", rel) >= (int)n) return false;
    char *dot = strrchr(out, '.');
    if (!dot) return false;
    strcpy(dot, ".lrc");
    if (access(out, F_OK) == 0) return true;
    strcpy(dot, ".LRC");
    return access(out, F_OK) == 0;
}

/* ------------------------------------------------------------------ */
/* 扫描 TF 卡                                                          */
/* ------------------------------------------------------------------ */

static void scan_dir(const char *dir, const char *rel_prefix, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_track_count < s_track_cap) {
        if (e->d_name[0] == '.') continue;

        char full[WP_REL_LEN + 24];
        char rel[WP_REL_LEN];
        if (snprintf(full, sizeof(full), "%s/%s", dir, e->d_name) >= (int)sizeof(full)) continue;
        if (rel_prefix[0])
            snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, e->d_name);
        else
            snprintf(rel, sizeof(rel), "%s", e->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < 2) scan_dir(full, rel, depth + 1);
            continue;
        }
        if (!is_audio_name(e->d_name)) continue;

        wp_track_t *t = &s_tracks[s_track_count];
        memset(t, 0, sizeof(*t));
        strncpy(t->rel, rel, sizeof(t->rel) - 1);
        strncpy(t->name, e->d_name, sizeof(t->name) - 1);
        char *dot = strrchr(t->name, '.');
        if (dot) *dot = '\0';
        t->size = (uint32_t)st.st_size;

        char lp[WP_REL_LEN + 24];
        snprintf(lp, sizeof(lp), "%s/%s", dir, e->d_name);
        char *d2 = strrchr(lp, '.');
        if (d2) {
            strcpy(d2, ".lrc");
            t->has_lrc = (stat(lp, &st) == 0);
            if (!t->has_lrc) {
                strcpy(d2, ".LRC");
                t->has_lrc = (stat(lp, &st) == 0);
            }
        }
        s_track_count++;
    }
    closedir(d);
}

static int track_cmp(const void *a, const void *b)
{
    return strcasecmp(((const wp_track_t *)a)->name, ((const wp_track_t *)b)->name);
}

static void build_index(void)
{
    if (!s_tracks) {
        size_t bytes = sizeof(wp_track_t) * WP_MAX_TRACKS;
        s_tracks = (wp_track_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        if (!s_tracks) s_tracks = (wp_track_t *)malloc(bytes);
        if (!s_tracks) {
            DBG("track table alloc failed");
            return;
        }
        s_track_cap = WP_MAX_TRACKS;
    }
    s_track_count = 0;
    scan_dir(SD_ROOT, "", 0);
    if (s_track_count > 1) {
        qsort(s_tracks, s_track_count, sizeof(wp_track_t), track_cmp);
    }
    DBG("indexed %d audio track(s)", s_track_count);
}

/* ------------------------------------------------------------------ */
/* HTTP 处理函数                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, player_page_html, player_page_len);
}

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_list(httpd_req_t *req)
{
    char buf[1200];
    char nm[WP_NAME_LEN * 2 + 8];
    char en[WP_REL_LEN * 3 + 8];

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    snprintf(buf, sizeof(buf), "{\"sd\":%d,\"n\":%d,\"items\":[",
             access(SD_ROOT, F_OK) == 0 ? 1 : 0, s_track_count);
    httpd_resp_send_chunk(req, buf, strlen(buf));

    for (int i = 0; i < s_track_count; i++) {
        url_encode(s_tracks[i].rel, en, sizeof(en));
        json_escape(s_tracks[i].name, nm, sizeof(nm));
        snprintf(buf, sizeof(buf),
                 "%s{\"i\":%d,\"n\":\"%s\",\"u\":\"/media/%s\",\"l\":%d,\"sz\":%u}",
                 i ? "," : "", i, nm, en,
                 s_tracks[i].has_lrc ? 1 : 0, (unsigned)s_tracks[i].size);
        httpd_resp_send_chunk(req, buf, strlen(buf));
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* 把文件（可带 Range）以 chunked 方式发出 */
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *ctype)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "file not found");
        return ESP_OK;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stat failed");
        return ESP_OK;
    }

    long size  = (long)st.st_size;
    long start = 0;
    long end   = size > 0 ? size - 1 : 0;
    bool ranged = false;

    char range[80];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long a = 0, b = -1;
        if (sscanf(range, "bytes=%ld-%ld", &a, &b) >= 1) {
            if (a < 0) a = 0;
            if (a > end) a = end;
            if (b < a || b > end) b = end;
            start = a;
            end = b;
            ranged = true;
        }
    }

    long len = (size > 0) ? (end - start + 1) : 0;

    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (ranged) {
        char cr[80];
        snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", cr);
    }

    if (len > 0) {
        char *buf = (char *)malloc(STREAM_CHUNK);
        if (buf) {
            fseek(f, start, SEEK_SET);
            long remain = len;
            while (remain > 0) {
                size_t want = (remain > STREAM_CHUNK) ? STREAM_CHUNK : (size_t)remain;
                size_t rd = fread(buf, 1, want, f);
                if (rd == 0) break;
                if (httpd_resp_send_chunk(req, buf, rd) != ESP_OK) {
                    DBG("client gone while streaming (sent %ld/%ld)", len - remain, len);
                    break;
                }
                remain -= (long)rd;
            }
            free(buf);
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_media(httpd_req_t *req)
{
    char rel[WP_REL_LEN];
    char path[WP_REL_LEN + 24];

    if (strncmp(req->uri, "/media/", 7) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad uri");
        return ESP_OK;
    }
    if (url_decode(req->uri + 7, rel, sizeof(rel)) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad encoding");
        return ESP_OK;
    }
    if (rel[0] == '/' || strstr(rel, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    snprintf(path, sizeof(path), SD_ROOT "/%s", rel);
    return send_file(req, path, ctype_for(rel));
}

static esp_err_t h_lrc(httpd_req_t *req)
{
    char rel[WP_REL_LEN];
    char path[WP_REL_LEN + 24];

    if (strncmp(req->uri, "/lrc/", 5) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad uri");
        return ESP_OK;
    }
    if (url_decode(req->uri + 5, rel, sizeof(rel)) != 0 ||
        rel[0] == '/' || strstr(rel, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad encoding");
        return ESP_OK;
    }
    if (!lrc_path_for(rel, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no lyric");
        return ESP_OK;
    }
    /* 不声明 charset：页面按原始字节读取并自行判断 UTF-8 / GBK */
    return send_file(req, path, "application/octet-stream");
}

static esp_err_t h_lyrics_post(httpd_req_t *req)
{
    int idx = -1;
    char q[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[24];
        if (httpd_query_key_value(q, "i", v, sizeof(v)) == ESP_OK) idx = atoi(v);
    }

    int total = req->content_len;
    if (total < 0 || total > 96 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_OK;
    }

    char *body = (char *)malloc((size_t)total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[got] = '\0';

    if (idx >= 0 && idx < s_track_count) {
        int n = lyrics_parse(body, (size_t)got);
        s_lyric_for = idx;
        DBG("lyrics for track %d: %d bytes -> %d line(s)", idx, got, n);
    }
    free(body);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":1}", 8);
}

static esp_err_t h_sync(httpd_req_t *req)
{
    char q[192];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[24];
        int      i  = -1;
        uint32_t p  = 0, d = 0;
        int      st = 0;

        if (httpd_query_key_value(q, "i", v, sizeof(v)) == ESP_OK) i  = atoi(v);
        if (httpd_query_key_value(q, "p", v, sizeof(v)) == ESP_OK) p  = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "d", v, sizeof(v)) == ESP_OK) d  = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) st = atoi(v);

        s_last_rx_us = esp_timer_get_time();

        if (i != s_cur) {
            s_cur = i;
            if (s_lyric_for != i) {
                lyrics_reset();
                s_lyric_for = -1;
            }
            const char *nm = web_track_name(i);
            snprintf(s_fallback_title, sizeof(s_fallback_title), "%s", nm ? nm : "");
            DBG("now playing [%d] %s", i, s_fallback_title);
        }
        s_dur        = d;
        s_anchor_pos = p;
        s_anchor_us  = esp_timer_get_time();
        s_rate       = (st == 1) ? 1 : 0;
    }

    /* 取走待发命令 */
    char out[96];
    int  c   = s_cmd;
    int32_t a = s_cmd_arg;
    s_cmd = WEB_CMD_NONE;

    switch (c) {
        case WEB_CMD_PLAY:   strcpy(out, "{\"cmd\":\"play\"}");  break;
        case WEB_CMD_PAUSE:  strcpy(out, "{\"cmd\":\"pause\"}"); break;
        case WEB_CMD_TOGGLE: strcpy(out, "{\"cmd\":\"toggle\"}");break;
        case WEB_CMD_NEXT:   strcpy(out, "{\"cmd\":\"next\"}");  break;
        case WEB_CMD_PREV:   strcpy(out, "{\"cmd\":\"prev\"}");  break;
        case WEB_CMD_SEEK:   snprintf(out, sizeof(out), "{\"cmd\":\"seek\",\"arg\":%ld}", (long)a); break;
        default:             strcpy(out, "{}"); break;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, strlen(out));
}

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 网络配置（STA 局域网 / AP 热点 切换）                                */
/* ------------------------------------------------------------------ */

/* 从 application/x-www-form-urlencoded 正文里取一个字段并做 URL 解码 */
static void form_field(const char *body, const char *key, char *out, size_t n)
{
    out[0] = '\0';
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *e = strchr(v, '&');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            char tmp[192];
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, v, len);
            tmp[len] = '\0';
            url_decode(tmp, out, n);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

/* 电源状态：供电源 / 电池电压 / 电量 / SYS_EN 闩锁 / PWR 键
 *   GET  /api/pwr          -> {"batt":0|1,"mv":3900,"pct":66,"sys_out":1,"pwr":0,"rot":0}
 *   POST /api/pwr?off=1    -> 立刻关机（等价于长按 PWR 键）；返回 {"ok":1} 后断电
 */
static esp_err_t h_pwr(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        bool off = false;
        char q[64] = {0};
        size_t ql = httpd_req_get_url_query_len(req) + 1;
        if (ql > 1 && ql < sizeof(q) &&
            httpd_req_get_url_query_str(req, q, ql) == ESP_OK) {
            char v[8] = {0};
            if (httpd_query_key_value(q, "off", v, sizeof(v)) == ESP_OK && v[0] == '1') {
                off = true;
            }
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_send(req, off ? "{\"ok\":1,\"off\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
        if (off) {
            /* 先给 TCP 一点时间把响应发出去，再动电源 */
            vTaskDelay(pdMS_TO_TICKS(150));
            board_power_off();
        }
        return ESP_OK;
    }

    uint32_t mv = board_battery_mv();
    char out[192];
    snprintf(out, sizeof(out),
             "{\"batt\":%d,\"mv\":%u,\"pct\":%d,\"sys_out\":%d,\"pwr\":%d,\"rot\":%d}",
             board_on_battery() ? 1 : 0,
             (unsigned)mv, board_battery_pct(),
             (int)gpio_get_level(EXAMPLE_PIN_NUM_SYS_OUT),
             board_pwr_pressed() ? 1 : 0,
             lvgl_port_rot_dir());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, strlen(out));
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, wifi_page_html, wifi_page_len);
}

static esp_err_t h_net(httpd_req_t *req)
{
    char e_ssid[160], e_saved[160];
    json_escape(wifi_link_ssid(),       e_ssid,  sizeof(e_ssid));
    json_escape(wifi_link_saved_ssid(), e_saved, sizeof(e_saved));

    char out[480];
    snprintf(out, sizeof(out),
             "{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"url\":\"%s\",\"saved\":\"%s\"}",
             wifi_link_is_sta() ? "sta" : "ap",
             e_ssid, wifi_link_ip(), wifi_link_url(), e_saved);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out, strlen(out));
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    char body[384];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv fail");
            return ESP_OK;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[80] = "", pass[96] = "";
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));

    esp_err_t e = wifi_link_save(ssid, pass);
    const char *msg = (e == ESP_OK) ? "{\"ok\":1}" : "{\"ok\":0,\"err\":\"save\"}";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, msg, strlen(msg));

    if (e == ESP_OK) {
        DBG("WiFi 凭据已保存(SSID=\"%s\")，1 秒后重启", ssid);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    wifi_link_forget();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "{\"ok\":1}", 8);

    DBG("已清除 WiFi 凭据，1 秒后重启（回到热点模式）");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_player_start(const char *ip)
{
    if (ip && ip[0]) {
        strncpy(s_ip, ip, sizeof(s_ip) - 1);
        s_ip[sizeof(s_ip) - 1] = '\0';
    }

    build_index();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 24;
    cfg.stack_size        = 8192;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;
    cfg.max_open_sockets  = 7;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        DBG("httpd_start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                 .method = HTTP_GET,  .handler = h_root,         .user_ctx = NULL },
        { .uri = "/favicon.ico",      .method = HTTP_GET,  .handler = h_favicon,      .user_ctx = NULL },
        { .uri = "/api/list",         .method = HTTP_GET,  .handler = h_list,         .user_ctx = NULL },
        { .uri = "/api/sync",         .method = HTTP_GET,  .handler = h_sync,         .user_ctx = NULL },
        { .uri = "/api/lyrics*",      .method = HTTP_POST, .handler = h_lyrics_post,  .user_ctx = NULL },
        { .uri = "/media/*",          .method = HTTP_GET,  .handler = h_media,        .user_ctx = NULL },
        { .uri = "/lrc/*",            .method = HTTP_GET,  .handler = h_lrc,          .user_ctx = NULL },
        { .uri = "/wifi",             .method = HTTP_GET,  .handler = h_wifi,         .user_ctx = NULL },
        { .uri = "/api/net",          .method = HTTP_GET,  .handler = h_net,          .user_ctx = NULL },
        { .uri = "/api/pwr",          .method = HTTP_GET,  .handler = h_pwr,          .user_ctx = NULL },
        { .uri = "/api/pwr",          .method = HTTP_POST, .handler = h_pwr,          .user_ctx = NULL },
        { .uri = "/api/wifi",         .method = HTTP_POST, .handler = h_wifi_post,    .user_ctx = NULL },
        { .uri = "/api/wifi/forget",  .method = HTTP_POST, .handler = h_wifi_forget,  .user_ctx = NULL },

        /* 电脑推送（歌词桥）--------------------------------------------- */
        { .uri = "/api/ext/track",    .method = HTTP_POST, .handler = ext_link_h_track,  .user_ctx = NULL },
        { .uri = "/api/ext/pos",      .method = HTTP_POST, .handler = ext_link_h_pos,    .user_ctx = NULL },
        { .uri = "/api/ext/state",    .method = HTTP_GET,  .handler = ext_link_h_state,  .user_ctx = NULL },
        { .uri = "/api/ext/offset",   .method = HTTP_POST, .handler = ext_link_h_offset, .user_ctx = NULL },
        { .uri = "/api/ext/clear",    .method = HTTP_POST, .handler = ext_link_h_clear,  .user_ctx = NULL },
        { .uri = "/api/ext/cover",    .method = HTTP_POST, .handler = ext_link_h_cover,  .user_ctx = NULL },

        /* 屏幕朝向（横屏两个方向之间切换，免烧录）----------------------- */
        { .uri = "/api/rot",          .method = HTTP_POST, .handler = lvgl_port_h_rot,   .user_ctx = NULL },
        { .uri = "/api/rot",          .method = HTTP_GET,  .handler = lvgl_port_h_rot,   .user_ctx = NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(s_srv, &uris[i]);
        if (e != ESP_OK) {
            DBG("register %s failed: %d", uris[i].uri, (int)e);
        }
    }

    DBG("HTTP server up: http://%s/  (%d tracks)", s_ip, s_track_count);
    return ESP_OK;
}

void web_player_stop(void)
{
    if (s_srv) {
        httpd_stop(s_srv);
        s_srv = NULL;
    }
}

const char *web_player_ip(void) { return s_ip; }

/* ------------------------------------------------------------------ */
/* 查询接口                                                            */
/* ------------------------------------------------------------------ */

int web_track_count(void) { return s_track_count; }

const char *web_track_name(int i)
{
    if (i < 0 || i >= s_track_count || !s_tracks) return NULL;
    return s_tracks[i].name;
}

bool web_track_has_lrc(int i)
{
    if (i < 0 || i >= s_track_count || !s_tracks) return false;
    return s_tracks[i].has_lrc;
}

bool web_active(void)
{
    return (s_cur >= 0 && s_cur < s_track_count);
}

int web_play_state(void)
{
    if (!web_active()) return 0;
    return s_rate ? 1 : 2;
}

int web_cur_index(void) { return s_cur; }

const char *web_now_title(void)
{
    if (lyrics_ready() && lyrics_meta(0)[0]) return lyrics_meta(0);
    return s_fallback_title;
}

const char *web_now_artist(void)
{
    if (lyrics_ready() && lyrics_meta(1)[0]) return lyrics_meta(1);
    return "";
}

uint32_t web_position_ms(void)
{
    if (s_cur < 0) return 0;
    uint32_t pos = s_anchor_pos;
    if (s_rate == 1) {
        int64_t dt = esp_timer_get_time() - s_anchor_us;
        if (dt > 0) pos += (uint32_t)(dt / 1000);
    }
    if (s_dur && pos > s_dur) pos = s_dur;
    return pos;
}

uint32_t web_duration_ms(void) { return s_dur; }

bool web_connected(void)
{
    if (s_last_rx_us == 0) return false;
    return (esp_timer_get_time() - s_last_rx_us) < 5 * 1000 * 1000LL;
}

void web_post_cmd(web_cmd_t c, int32_t arg)
{
    s_cmd_arg = arg;
    s_cmd = (int)c;
}

/* ------------------------------------------------------------------ */
/* 统一「正在播放」：电脑推送优先，其次板子本地/网页                    */
/* ------------------------------------------------------------------ */

np_source_t np_source(void)
{
    if (ext_link_active()) return NP_SRC_PC;
    if (web_active())      return NP_SRC_LOCAL;
    return NP_SRC_NONE;
}

bool np_active(void) { return np_source() != NP_SRC_NONE; }

int np_state(void)
{
    switch (np_source()) {
        case NP_SRC_PC:    return ext_link_state();
        case NP_SRC_LOCAL: return web_play_state();
        default:           return 0;
    }
}

const char *np_title(void)
{
    if (np_source() == NP_SRC_PC) {
        const char *t = ext_link_title();
        if (t && t[0]) return t;
    }
    return web_now_title();
}

const char *np_artist(void)
{
    if (np_source() == NP_SRC_PC) {
        const char *a = ext_link_artist();
        if (a && a[0]) return a;
    }
    return web_now_artist();
}

const char *np_album(void)
{
    if (np_source() == NP_SRC_PC) {
        const char *a = ext_link_album();
        if (a && a[0]) return a;
    }
    return "";
}

uint32_t np_position_ms(void)
{
    return (np_source() == NP_SRC_PC) ? ext_link_position_ms() : web_position_ms();
}

uint32_t np_lyric_pos_ms(void)
{
    return (np_source() == NP_SRC_PC) ? ext_link_lyric_pos_ms() : web_position_ms();
}

uint32_t np_duration_ms(void)
{
    return (np_source() == NP_SRC_PC) ? ext_link_duration_ms() : web_duration_ms();
}

bool np_online(void)
{
    return (np_source() == NP_SRC_PC) ? ext_link_online() : web_connected();
}

int np_track_key(void)
{
    /* 用一个很大的偏移，把「电脑推送的曲目」与「TF 卡曲目索引」区分开 */
    if (np_source() == NP_SRC_PC) return 100000 + (int)ext_link_track_seq();
    return web_cur_index();
}

const char *np_source_text(void)
{
    switch (np_source()) {
        case NP_SRC_PC:
            return ext_link_online() ? "电脑推送" : "电脑已断开";
        case NP_SRC_LOCAL:
            return "本机播放";
        default:
            return "等待电脑连接";
    }
}

/* ------------------------- 专辑封面 ------------------------- */

bool np_cover_ready(void)
{
    /* 只有「电脑推送」这条链路会带封面 */
    return (np_source() == NP_SRC_PC) && ext_link_cover_ready();
}

const uint8_t *np_cover_data(void)
{
    return np_cover_ready() ? ext_link_cover_data() : NULL;
}

int np_cover_px(void)
{
    return np_cover_ready() ? XL_COVER_PX : 0;
}

int np_cover_seq(void)
{
    return (np_source() == NP_SRC_PC) ? (int)ext_link_cover_seq() : 0;
}
