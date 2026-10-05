/**
 * @file  lyrics.c
 * @brief LRC 歌词解析与时间轴检索实现
 */
#include "lyrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "LYR";

typedef struct {
    uint32_t t_ms;
    char     text[LYRIC_TEXT_LEN];
} lyric_line_t;

static lyric_line_t *s_lines = NULL;
static int           s_cap   = 0;
static int           s_count = 0;
static bool          s_ready = false;

static char s_ti[LYRIC_META_LEN];
static char s_ar[LYRIC_META_LEN];
static char s_al[LYRIC_META_LEN];

/* ------------------------------------------------------------------ */

static void ensure_alloc(void)
{
    if (s_lines) return;
    size_t bytes = sizeof(lyric_line_t) * LYRIC_MAX_LINES;
    s_lines = (lyric_line_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!s_lines) {
        s_lines = (lyric_line_t *)malloc(bytes);
    }
    s_cap = s_lines ? LYRIC_MAX_LINES : 0;
    if (!s_lines) {
        ESP_LOGE(TAG, "lyric buffer alloc failed (%u bytes)", (unsigned)bytes);
    }
}

static void copy_trim(char *dst, size_t dst_sz, const char *src, size_t len)
{
    while (len && (*src == ' ' || *src == '\t')) { src++; len--; }
    while (len && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                   src[len - 1] == '\r')) { len--; }
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 解析 "mm:ss.xx" / "mm:ss" / "m:ss.xxx"，成功返回 true */
static bool parse_time_tag(const char *s, size_t len, uint32_t *out_ms)
{
    size_t i = 0;
    long   mm = 0;
    int    digits = 0;

    while (i < len && s[i] >= '0' && s[i] <= '9') { mm = mm * 10 + (s[i] - '0'); i++; digits++; }
    if (digits == 0 || i >= len || s[i] != ':') return false;
    i++;
    if (i + 1 >= len) return false;
    if (!isdigit((unsigned char)s[i]) || !isdigit((unsigned char)s[i + 1])) return false;

    long ss = (s[i] - '0') * 10 + (s[i + 1] - '0');
    i += 2;

    long frac = 0;
    int  fdig = 0;
    if (i < len && (s[i] == '.' || s[i] == ':')) {
        i++;
        while (i < len && isdigit((unsigned char)s[i])) {
            if (fdig < 3) { frac = frac * 10 + (s[i] - '0'); fdig++; }
            i++;
        }
    }
    while (fdig < 3) { frac *= 10; fdig++; }   /* 归一化到毫秒 */

    if (i != len) return false;                /* 尾部残留 -> 不是纯时间标签 */
    *out_ms = (uint32_t)((mm * 60 + ss) * 1000 + frac);
    return true;
}

static void add_line(uint32_t t_ms, const char *text, size_t len)
{
    if (s_count >= s_cap) return;
    if (len == 0) return;

    lyric_line_t *L = &s_lines[s_count];
    L->t_ms = t_ms;
    copy_trim(L->text, sizeof(L->text), text, len);
    if (L->text[0] == '\0') return;
    s_count++;
}

static int line_cmp(const void *a, const void *b)
{
    uint32_t ta = ((const lyric_line_t *)a)->t_ms;
    uint32_t tb = ((const lyric_line_t *)b)->t_ms;
    return (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
}

/* ------------------------------------------------------------------ */

void lyrics_reset(void)
{
    s_count = 0;
    s_ready = false;
    s_ti[0] = s_ar[0] = s_al[0] = '\0';
}

int lyrics_parse(const char *lrc, size_t len)
{
    lyrics_reset();
    if (!lrc || len == 0) return 0;

    ensure_alloc();
    if (!s_lines) return -1;

    const char *p   = lrc;
    const char *end = lrc + len;

    /* 跳过 UTF-8 BOM */
    if (len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }

    uint32_t times[64];

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        size_t      ll = (size_t)(le - p);
        if (ll && p[ll - 1] == '\r') ll--;

        const char *q  = p;
        const char *qe = p + ll;
        int  ntimes    = 0;

        while (q < qe && *q == '[') {
            const char *rb = (const char *)memchr(q, ']', (size_t)(qe - q));
            if (!rb) break;
            size_t      tl  = (size_t)(rb - q - 1);
            const char *tag = q + 1;

            uint32_t t;
            if (parse_time_tag(tag, tl, &t)) {
                if (ntimes < 64) times[ntimes++] = t;
            } else if (tl >= 3 && tag[2] == ':') {
                char k0 = (char)tolower((unsigned char)tag[0]);
                char k1 = (char)tolower((unsigned char)tag[1]);
                if (k0 == 't' && k1 == 'i')      copy_trim(s_ti, sizeof(s_ti), tag + 3, tl - 3);
                else if (k0 == 'a' && k1 == 'r') copy_trim(s_ar, sizeof(s_ar), tag + 3, tl - 3);
                else if (k0 == 'a' && k1 == 'l') copy_trim(s_al, sizeof(s_al), tag + 3, tl - 3);
            }
            q = rb + 1;
        }

        /* 时间标签之后的全部内容即歌词文本 */
        if (ntimes > 0) {
            for (int k = 0; k < ntimes; k++) {
                add_line(times[k], q, (size_t)(qe - q));
            }
        }

        p = nl ? nl + 1 : end;
    }

    if (s_count > 1) {
        qsort(s_lines, s_count, sizeof(lyric_line_t), line_cmp);
    }
    s_ready = (s_count > 0);
    ESP_LOGI(TAG, "parsed %d lyric line(s), ti=%s ar=%s", s_count, s_ti, s_ar);
    return s_count;
}

int lyrics_count(void) { return s_count; }
bool lyrics_ready(void) { return s_ready; }

const char *lyrics_line(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return s_lines[i].text;
}

uint32_t lyrics_time_ms(int i)
{
    if (i < 0 || i >= s_count) return 0;
    return s_lines[i].t_ms;
}

int lyrics_index_at(uint32_t pos_ms)
{
    if (s_count <= 0) return -1;
    if (pos_ms < s_lines[0].t_ms) return -1;

    /* 二分：找最后一个 t_ms <= pos_ms */
    int lo = 0, hi = s_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (s_lines[mid].t_ms <= pos_ms) { ans = mid; lo = mid + 1; }
        else                             { hi = mid - 1; }
    }
    return ans;
}

const char *lyrics_meta(int which)
{
    switch (which) {
        case 0:  return s_ti;
        case 1:  return s_ar;
        case 2:  return s_al;
        default: return "";
    }
}
