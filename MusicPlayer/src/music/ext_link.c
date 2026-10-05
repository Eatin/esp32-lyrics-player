/**
 * @file  ext_link.c
 * @brief 外部歌词推送通道实现（PC 歌词桥 -> 板子）
 *
 * 注意：本 Arduino 环境把 ESP_LOGI/ESP_LOGW 在编译期删掉了
 *      （预编译库 CONFIG_LOG_MAXIMUM_LEVEL=ERROR），所以这里一律用 printf。
 */
#include "ext_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "lyrics.h"
#include "wifi_link.h"
#include "web_player.h"

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

#define XL_MAX_BODY   (256 * 1024)   /* LRC 正文上限（防止恶意长请求打爆内存） */
#define XL_ONLINE_MS  (10 * 1000)    /* 10s 内有推送 => PC 在线               */
#define XL_ACTIVE_MS  (180 * 1000)   /* 3min 内有推送 => 仍然显示这条曲目      */

#define XL_JUMP_MS    (2000)         /* 偏差超过 2s 视为拖动进度，直接重锚     */
#define XL_SLEW_DIV   (4)            /* 小幅漂移只吸收 1/4，保证歌词平滑       */

#define XL_TITLE_LEN  160
#define XL_ARTIST_LEN 128
#define XL_ALBUM_LEN  128

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static char s_title[XL_TITLE_LEN];
static char s_artist[XL_ARTIST_LEN];
static char s_album[XL_ALBUM_LEN];

static volatile uint32_t s_anchor_pos = 0;    /* 锚点位置(ms)        */
static volatile int64_t  s_anchor_us  = 0;    /* 锚点时刻(us)        */
static volatile int      s_state      = 0;    /* 0=停 1=播 2=暂停     */
static volatile int64_t  s_last_rx_us = 0;    /* 最近一次收到推送     */
static volatile int32_t  s_offset_ms  = 0;    /* 歌词整体偏移(ms)     */
static volatile uint32_t s_dur_ms     = 0;
static volatile uint32_t s_rx_count   = 0;
static volatile uint32_t s_jump_count = 0;
static volatile uint32_t s_track_seq  = 0;
static volatile bool     s_ever       = false;
static volatile int      s_udp_ok     = 0;

/* 封面：双缓冲，交替写入，避免 UI 正在画的时候被写坏 */
static uint8_t          *s_cover[2]   = { NULL, NULL };
static volatile int      s_cover_idx  = 0;
static volatile bool     s_cover_ready = false;
static volatile uint32_t s_cover_seq  = 0;

/* 板子 -> PC 的待发命令（取走即清空）。
 * UI 线程写、httpd 线程读，一个字长，volatile 足够；
 * 用 take 读-改-写不是原子操作，但两端都是「后到覆盖 / 取走即空」的语义，
 * 最坏情况只是丢一条连按的命令，不会卡死。 */
static volatile int      s_cmd         = XL_CMD_NONE;
static volatile uint32_t s_cmd_count   = 0;   /* 累计投递条数（诊断用） */

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void xl_copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** URL 解码（支持 %XX 与 '+'） */
static void xl_url_decode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    if (!n) return;
    while (in && *in && o + 1 < n) {
        char c = *in++;
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && in[0] && in[1]) {
            int h = hexv(in[0]), l = hexv(in[1]);
            if (h >= 0 && l >= 0) { out[o++] = (char)((h << 4) | l); in += 2; }
            else                  { out[o++] = c; }
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

/** 从查询串里取一个参数并 URL 解码 */
static bool qs_get(const char *q, const char *key, char *out, size_t n)
{
    if (!q || !q[0]) return false;
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '&');
            size_t len = e ? (size_t)(e - v) : strlen(v);
            char tmp[768];
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, v, len);
            tmp[len] = '\0';
            xl_url_decode(tmp, out, n);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/** JSON 字符串转义（歌名/歌手可能带引号） */
static void jesc(const char *s, char *out, size_t n)
{
    size_t o = 0;
    if (!n) return;
    for (; s && *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')      { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n')             { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r')             { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t')             { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)              { out[o++] = '?'; }
        else                            { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

static esp_err_t reply_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, strlen(body));
}

static esp_err_t reply_ok(httpd_req_t *req)
{
    return reply_json(req, "{\"ok\":1}");
}

/** 取查询串到缓冲区，返回是否成功 */
static bool get_qs(httpd_req_t *req, char *buf, size_t n)
{
    buf[0] = '\0';
    size_t l = httpd_req_get_url_query_len(req) + 1;
    if (l > 1 && l <= n) {
        return httpd_req_get_url_query_str(req, buf, l) == ESP_OK;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 位置锚点与漂移平滑                                                   */
/* ------------------------------------------------------------------ */

static void xl_note_pos(uint32_t p, int state)
{
    int64_t now = esp_timer_get_time();

    if (state < 0) state = 0;
    if (state > 2) state = 2;

    bool state_changed = (state != s_state);

    if (s_anchor_us == 0 || state_changed || state != 1) {
        /* 首次 / 状态变化 / 暂停或停止：直接重锚，位置就是上报值 */
        s_anchor_pos = p;
        s_anchor_us  = now;
    } else {
        /* 播放中且状态未变：算一下预期值，做平滑纠偏 */
        int64_t exp = (int64_t)s_anchor_pos + (now - s_anchor_us) / 1000;
        int64_t d   = (int64_t)p - exp;

        if (d > XL_JUMP_MS || d < -XL_JUMP_MS) {
            /* 大幅跳变 = 用户拖了进度条 */
            s_anchor_pos = p;
            s_anchor_us  = now;
            s_jump_count++;
        } else {
            /* 小幅漂移：只吸收一部分，避免歌词来回跳 */
            int64_t np = exp + d / XL_SLEW_DIV;
            if (np < 0) np = 0;
            s_anchor_pos = (uint32_t)np;
            s_anchor_us  = now;
        }
    }

    s_state      = state;
    s_last_rx_us = now;
    s_ever       = true;
    s_rx_count++;
}

/* ------------------------------------------------------------------ */
/* HTTP handlers                                                       */
/* ------------------------------------------------------------------ */

/** POST /api/ext/track?dur=&title=&artist=   （body = LRC 原文） */
esp_err_t ext_link_h_track(httpd_req_t *req)
{
    char q[512];
    char v[256];

    get_qs(req, q, sizeof(q));

    if (qs_get(q, "title", v, sizeof(v)))  xl_copy(s_title,  sizeof(s_title),  v);
    else                                   s_title[0] = '\0';

    if (qs_get(q, "artist", v, sizeof(v))) xl_copy(s_artist, sizeof(s_artist), v);
    else                                   s_artist[0] = '\0';

    if (qs_get(q, "album", v, sizeof(v)))  xl_copy(s_album,  sizeof(s_album),  v);
    else                                   s_album[0] = '\0';

    if (qs_get(q, "dur", v, sizeof(v)))    s_dur_ms = (uint32_t)strtoul(v, NULL, 10);
    else                                   s_dur_ms = 0;

    /* ---- 读取 body（LRC 原文，UTF-8）---- */
    int total = (int)req->content_len;
    int lines = -1;

    if (total > 0) {
        if (total > XL_MAX_BODY) {
            printf("[EXT] LRC too large: %d bytes\n", total);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lrc too large");
            return ESP_FAIL;
        }

        char *buf = (char *)heap_caps_malloc((size_t)total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = (char *)malloc((size_t)total + 1);
        if (!buf) {
            printf("[EXT] no mem for %d bytes\n", total);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            return ESP_FAIL;
        }

        int got = 0;
        while (got < total) {
            int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
            if (r > 0) { got += r; continue; }
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;   /* 超时就再试 */
            break;
        }
        buf[got] = '\0';

        lyrics_reset();
        lines = lyrics_parse(buf, (size_t)got);
        free(buf);
    } else {
        /* 纯音乐：清掉上一首的歌词 */
        lyrics_reset();
        lines = 0;
    }

    /* LRC 自带 [ti:]/[ar:] 时，外部没给就用它 */
    if (!s_title[0]  && lyrics_meta(0)[0]) xl_copy(s_title,  sizeof(s_title),  lyrics_meta(0));
    if (!s_artist[0] && lyrics_meta(1)[0]) xl_copy(s_artist, sizeof(s_artist), lyrics_meta(1));

    /* 新曲目：位置归零、置为播放、并把偏移保留（对轴习惯通常每首通用） */
    s_anchor_pos = 0;
    s_anchor_us  = esp_timer_get_time();
    s_state      = 1;
    s_last_rx_us = s_anchor_us;
    s_ever       = true;
    s_rx_count++;
    s_track_seq++;

    printf("[EXT] track: \"%s\" - \"%s\"  dur=%ums  lrc=%d lines\n",
           s_title, s_artist, (unsigned)s_dur_ms, lines);

    return reply_ok(req);
}

/** POST /api/ext/pos?p=<ms>&s=<0|1|2>[&d=<总时长ms>] */
esp_err_t ext_link_h_pos(httpd_req_t *req)
{
    char q[160];
    char v[32];
    uint32_t p = 0;
    int      st = s_state;

    get_qs(req, q, sizeof(q));
    if (qs_get(q, "p", v, sizeof(v))) p  = (uint32_t)strtoul(v, NULL, 10);
    if (qs_get(q, "s", v, sizeof(v))) st = (int)strtol(v, NULL, 10);

    /* 可选的时长：换歌的那一瞬间酷狗界面还没刷新出时长，
     * 于是 track 推送时 dur=0。之后每秒的位置推送里带上真实时长，
     * 进度条就能自动恢复，不用为此重新推一次曲目（那会重置滚动位置）。 */
    if (qs_get(q, "d", v, sizeof(v))) {
        uint32_t d = (uint32_t)strtoul(v, NULL, 10);
        if (d) s_dur_ms = d;
    }

    xl_note_pos(p, st);

    /* 顺风车：把 UI 投递的切歌命令捎回给 PC（取走即清空，不会重复触发）。
     * 无命令时保持原来的最小响应 {"ok":1}，这个接口是高频的，别乱加字节。 */
    int c = ext_link_take_cmd();
    if (c != XL_CMD_NONE) {
        char body[64];
        snprintf(body, sizeof(body), "{\"ok\":1,\"cmd\":\"%s\"}", ext_link_cmd_name(c));
        printf("[EXT] PC 取走命令: %s\n", ext_link_cmd_name(c));
        return reply_json(req, body);
    }
    return reply_ok(req);
}

/** GET /api/ext/state */
esp_err_t ext_link_h_state(httpd_req_t *req)
{
    char et[XL_TITLE_LEN * 2], ea[XL_ARTIST_LEN * 2], eb[XL_ALBUM_LEN * 2];
    jesc(s_title,  et, sizeof(et));
    jesc(s_artist, ea, sizeof(ea));
    jesc(s_album,  eb, sizeof(eb));

    uint32_t quiet = ext_link_silence_ms();

    char body[1024];
    snprintf(body, sizeof(body),
             "{\"ok\":1,\"ever\":%d,\"on\":%d,\"live\":%d,\"s\":%d,\"p\":%u,\"lp\":%u,\"d\":%u,"
             "\"off\":%ld,\"ago\":%u,\"rx\":%u,\"jump\":%u,"
             "\"title\":\"%s\",\"ar\":\"%s\",\"al\":\"%s\",\"lrc\":%d,\"lines\":%d,"
             "\"cover\":%d,\"cseq\":%u}",
             (int)s_ever, (int)ext_link_active(), (int)ext_link_online(),
             (int)s_state, (unsigned)ext_link_position_ms(), (unsigned)ext_link_lyric_pos_ms(),
             (unsigned)s_dur_ms, (long)s_offset_ms, (unsigned)quiet,
             (unsigned)s_rx_count, (unsigned)s_jump_count,
             et, ea, eb, (int)lyrics_ready(), lyrics_count(),
             (int)s_cover_ready, (unsigned)s_cover_seq);

    return reply_json(req, body);
}

/* ------------------------------------------------------------------ */
/* 封面                                                                */
/* ------------------------------------------------------------------ */

/**
 * POST /api/ext/cover             body = XL_COVER_PX*XL_COVER_PX*2 字节 RGB565(大端)
 * POST /api/ext/cover?clear=1     清掉封面，UI 回到占位图
 */
esp_err_t ext_link_h_cover(httpd_req_t *req)
{
    char q[64];
    char v[16];
    const int want = XL_COVER_PX * XL_COVER_PX * 2;

    get_qs(req, q, sizeof(q));

    /* --- 清理 --- */
    if (qs_get(q, "clear", v, sizeof(v)) && v[0] == '1') {
        s_cover_ready = false;
        s_cover_seq++;
        printf("[EXT] cover cleared\n");
        return reply_ok(req);
    }

    const int total = (int)req->content_len;
    if (total <= 0) {
        s_cover_ready = false;
        s_cover_seq++;
        return reply_ok(req);
    }
    if (total != want) {
        printf("[EXT] cover bad size: %d (want %d)\n", total, want);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad cover size");
        return ESP_FAIL;
    }

    /* 懒分配两块 PSRAM 缓冲 */
    if (!s_cover[0]) {
        s_cover[0] = (uint8_t *)heap_caps_malloc((size_t)want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_cover[1] = (uint8_t *)heap_caps_malloc((size_t)want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_cover[0] || !s_cover[1]) {
        printf("[EXT] cover oom (%d bytes x2)\n", want);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    /* 写进「当前没在显示」的那一块 */
    uint8_t *dst = s_cover[1 - s_cover_idx];

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)dst + got, (size_t)(total - got));
        if (r > 0) { got += r; continue; }
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        break;
    }
    if (got != total) {
        printf("[EXT] cover short read: %d/%d\n", got, total);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "short read");
        return ESP_FAIL;
    }

    /* 只有整帧收完才切换，UI 不会看到半张图 */
    s_cover_idx = 1 - s_cover_idx;
    s_cover_ready = true;
    s_cover_seq++;

    printf("[EXT] cover ok: %d bytes (seq=%u)\n", total, (unsigned)s_cover_seq);
    return reply_ok(req);
}

bool           ext_link_cover_ready(void) { return s_cover_ready; }
const uint8_t *ext_link_cover_data(void)  { return s_cover_ready ? s_cover[s_cover_idx] : NULL; }
uint32_t       ext_link_cover_seq(void)   { return s_cover_seq; }

/** POST /api/ext/offset?d=<delta_ms>  或 ?v=<abs_ms> */
esp_err_t ext_link_h_offset(httpd_req_t *req)
{
    char q[128];
    char v[32];

    get_qs(req, q, sizeof(q));

    if (qs_get(q, "v", v, sizeof(v))) {
        long a = strtol(v, NULL, 10);
        if (a >  10000) a =  10000;
        if (a < -10000) a = -10000;
        s_offset_ms = (int32_t)a;
    } else if (qs_get(q, "d", v, sizeof(v))) {
        long d = strtol(v, NULL, 10);
        long a = (long)s_offset_ms + d;
        if (a >  10000) a =  10000;
        if (a < -10000) a = -10000;
        s_offset_ms = (int32_t)a;
    }

    printf("[EXT] offset = %ldms\n", (long)s_offset_ms);
    return ext_link_h_state(req);
}

/** POST /api/ext/clear —— PC 停推/换源时清空 */
esp_err_t ext_link_h_clear(httpd_req_t *req)
{
    s_title[0] = '\0';
    s_artist[0] = '\0';
    s_album[0] = '\0';
    s_anchor_pos = 0;
    s_anchor_us  = esp_timer_get_time();
    s_state      = 0;
    s_dur_ms     = 0;
    s_last_rx_us = 0;
    s_ever       = false;
    lyrics_reset();
    /* 封面也一起收掉，避免「歌清空了封面还挂着」 */
    if (s_cover_ready) { s_cover_ready = false; s_cover_seq++; }
    printf("[EXT] cleared\n");
    return reply_ok(req);
}

/* ------------------------------------------------------------------ */
/* 板子 -> PC 命令                                                     */
/* ------------------------------------------------------------------ */

void ext_link_post_cmd(int cmd)
{
    if (cmd == XL_CMD_NONE) return;
    s_cmd = cmd;
    s_cmd_count++;
    printf("[EXT] 投递命令 -> %s（等 PC 下次推位置时取走）\n", ext_link_cmd_name(cmd));
}

int ext_link_take_cmd(void)
{
    int c = s_cmd;
    if (c != XL_CMD_NONE) s_cmd = XL_CMD_NONE;
    return c;
}

const char *ext_link_cmd_name(int cmd)
{
    switch (cmd) {
        case XL_CMD_NEXT:   return "next";
        case XL_CMD_PREV:   return "prev";
        case XL_CMD_TOGGLE: return "toggle";
        default:            return "";
    }
}

bool ext_link_cmd_pending(void) { return s_cmd != XL_CMD_NONE; }

/** GET  /api/ext/cmd          —— 取走一条命令（PC 暂停时轮询用）
 *  POST /api/ext/cmd?c=next   —— 手动投递一条命令
 *
 *  后者是给「不想碰屏幕也能切歌」和自动化测试用的（仅局域网可达）。
 *  语义和 UI 手势投递完全一样：进命令槽，等 PC 来取。 */
esp_err_t ext_link_h_cmd(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        char q[64], v[16];
        get_qs(req, q, sizeof(q));
        if (qs_get(q, "c", v, sizeof(v))) {
            int c = XL_CMD_NONE;
            if      (!strcmp(v, "next"))   c = XL_CMD_NEXT;
            else if (!strcmp(v, "prev"))   c = XL_CMD_PREV;
            else if (!strcmp(v, "toggle")) c = XL_CMD_TOGGLE;
            if (c != XL_CMD_NONE) {
                ext_link_post_cmd(c);
                char body[72];
                snprintf(body, sizeof(body), "{\"ok\":1,\"queued\":\"%s\"}",
                         ext_link_cmd_name(c));
                return reply_json(req, body);
            }
        }
        return reply_json(req, "{\"ok\":0,\"err\":\"need c=next|prev|toggle\"}");
    }

    int c = ext_link_take_cmd();
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":1,\"cmd\":\"%s\",\"n\":%u}",
             ext_link_cmd_name(c), (unsigned)s_cmd_count);
    return reply_json(req, body);
}

/* ------------------------------------------------------------------ */
/* UDP 自动发现                                                        */
/* ------------------------------------------------------------------ */

static void xl_udp_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("[EXT] udp socket failed: %d\n", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(EXT_UDP_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[EXT] udp bind %d failed: %d\n", EXT_UDP_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_udp_ok = 1;
    printf("[EXT] UDP discovery listening on %d  (PC 广播 ESP32LYRICS?)\n", EXT_UDP_PORT);

    char buf[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;                 /* 超时：继续等 */
        buf[n] = '\0';

        if (strncmp(buf, "ESP32LYRICS?", 12) != 0) continue;

        char rep[320];
        snprintf(rep, sizeof(rep), "ESP32LYRICS!%s|%s|tracks=%d|ssid=%s|port=80|ver=1",
                 wifi_link_ip(), wifi_link_mode_text(),
                 web_track_count(), wifi_link_ssid());

        sendto(sock, rep, strlen(rep), 0, (struct sockaddr *)&from, fl);
        printf("[EXT] discovery from %s -> %s\n", inet_ntoa(from.sin_addr), rep);
    }
}

void ext_link_init(void)
{
    /* 固定 4KB 栈：只是 recvfrom + 组包，够用 */
    BaseType_t ok = xTaskCreate(xl_udp_task, "ext_udp", 4096, NULL, 4, NULL);
    if (ok != pdPASS) {
        printf("[EXT] udp task create failed\n");
    }
}

/* ------------------------------------------------------------------ */
/* 查询接口                                                            */
/* ------------------------------------------------------------------ */

int ext_link_udp_ok(void) { return s_udp_ok; }

uint32_t ext_link_silence_ms(void)
{
    if (s_last_rx_us == 0) return 0xFFFFFFFFu;
    int64_t d = (esp_timer_get_time() - s_last_rx_us) / 1000;
    if (d < 0) d = 0;
    return (uint32_t)d;
}

bool ext_link_active(void)
{
    if (!s_ever || s_last_rx_us == 0) return false;
    return ext_link_silence_ms() < (uint32_t)XL_ACTIVE_MS;
}

bool ext_link_online(void)
{
    if (!s_ever || s_last_rx_us == 0) return false;
    return ext_link_silence_ms() < (uint32_t)XL_ONLINE_MS;
}

int         ext_link_state(void)       { return s_state; }
const char *ext_link_title(void)       { return s_title; }
const char *ext_link_artist(void)      { return s_artist; }
const char *ext_link_album(void)       { return s_album; }
uint32_t    ext_link_duration_ms(void) { return s_dur_ms; }
uint32_t    ext_link_rx_count(void)    { return s_rx_count; }
uint32_t    ext_link_jump_count(void)  { return s_jump_count; }
uint32_t    ext_link_track_seq(void)   { return s_track_seq; }

uint32_t ext_link_position_ms(void)
{
    uint32_t pos = s_anchor_pos;
    if (s_state == 1) {
        int64_t dt = esp_timer_get_time() - s_anchor_us;
        if (dt > 0) pos += (uint32_t)(dt / 1000);
    }
    if (s_dur_ms && pos > s_dur_ms) pos = s_dur_ms;
    return pos;
}

uint32_t ext_link_lyric_pos_ms(void)
{
    int64_t p = (int64_t)ext_link_position_ms() + (int64_t)s_offset_ms;
    if (p < 0) p = 0;
    return (uint32_t)p;
}

int32_t ext_link_offset_ms(void) { return s_offset_ms; }

void ext_link_set_offset(int32_t ms)
{
    if (ms >  10000) ms =  10000;
    if (ms < -10000) ms = -10000;
    s_offset_ms = ms;
}

void ext_link_add_offset(int32_t d) { ext_link_set_offset(s_offset_ms + d); }
