/**
 * @file  wav_player.c
 * @brief SD 卡 WAV（PCM）音乐播放引擎实现
 *
 * 硬件链路： TF 卡 --(SDMMC 1bit)--> ESP32-S3 --(I2S TDM)--> ES8311 DAC
 *            --(模拟)--> NS4150 功放（TCA9554 EXIO7 使能）--> MX1.25 扬声器
 *
 * 线程模型：
 *   - wav_task（核心 1）负责真正的取数与播放
 *   - LVGL / 主任务通过 post_cmd() 投递命令，互不阻塞
 */
#include "wav_player.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"

static const char *TAG = "WAV";

/* ------------------------- 板级 SD 卡引脚 --------------------------- */
#define SDMMC_D0_PIN    GPIO_NUM_40
#define SDMMC_CLK_PIN   GPIO_NUM_41
#define SDMMC_CMD_PIN   GPIO_NUM_39
#define SD_MOUNT_POINT  "/sdcard"
#define SCAN_MAX_DEPTH  2              /* 扫描深度：/sdcard 及下一级目录 */

/* ------------------------- 播放参数 -------------------------------- */
#define FRAMES_PER_CHUNK   2048                          /* 每次搬运帧数 */
#define READ_BUF_BYTES     (FRAMES_PER_CHUNK * 2 * 4)    /* 源缓冲 16KB   */
#define OUT_BUF_BYTES      (FRAMES_PER_CHUNK * 2 * 2)    /* 输出 8KB      */

/* ------------------------- 播放状态 -------------------------------- */
typedef enum {
    WAV_ST_IDLE = 0,
    WAV_ST_PLAYING,
    WAV_ST_PAUSED,
} wav_state_t;

typedef enum {
    CMD_NONE = 0,
    CMD_PLAY,
    CMD_TOGGLE,
    CMD_STOP,
    CMD_NEXT,
    CMD_PREV,
    CMD_SCAN,
} wav_cmd_t;

/* play_file() 结束后的去向 */
typedef enum {
    ACT_AUTO_NEXT = 0,   /* 自然播放结束 -> 下一首   */
    ACT_STOP,
    ACT_NEXT,
    ACT_PREV,
    ACT_PLAY,            /* 播放 s_next_index         */
    ACT_ERROR,           /* 文件打不开/格式错误       */
} wav_action_t;

/* ------------------------- 全局状态 -------------------------------- */
static wav_track_t      s_tracks[WAV_MAX_TRACKS];
static volatile int     s_count    = 0;
static volatile bool    s_sd_ready = false;
static sdmmc_card_t    *s_card     = NULL;

static esp_codec_dev_handle_t s_playback = NULL;
static volatile wav_state_t   s_state    = WAV_ST_IDLE;
static volatile int           s_cur      = -1;
static volatile uint32_t      s_pos_ms   = 0;
static volatile uint32_t      s_dur_ms   = 0;
static volatile int           s_volume   = 70;
static volatile bool          s_opened   = false;

static volatile wav_cmd_t     s_cmd       = CMD_NONE;
static volatile int           s_req_index = 0;
static volatile bool          s_need_rescan = false;
static volatile wav_action_t  s_action    = ACT_AUTO_NEXT;
static volatile int           s_next_index = 0;

static TaskHandle_t      s_task   = NULL;
static SemaphoreHandle_t s_mutex  = NULL;
static uint8_t          *s_in_buf  = NULL;   /* PSRAM：源数据        */
static uint8_t          *s_out_buf = NULL;   /* PSRAM：16bit 立体声  */

/* 前向声明 */
static void do_rescan(void);
static void run_playlist(int start);

/* ================================================================== */
/*                         小工具                                      */
/* ================================================================== */

static bool is_wav_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return false;
    return (strcasecmp(name + n - 4, ".wav") == 0);
}

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t data_bytes;
    uint32_t data_offset;
} wav_info_t;

/* WAV 头解析，返回 0 表示成功 */
static int wav_parse_header(FILE *f, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) return -1;
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return -2;

    bool have_fmt = false;
    uint16_t audio_format = 0;

    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) return -3;
        uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                        ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t want = size > sizeof(fmt) ? sizeof(fmt) : size;
            if (fread(fmt, 1, want, f) != want) return -3;
            if (size > want) fseek(f, (long)(size - want), SEEK_CUR);

            audio_format      = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            info->channels    = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
            info->sample_rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
                                ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            info->bits        = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            if (audio_format == 0xFFFE && size >= 40) {
                /* WAVE_FORMAT_EXTENSIBLE：取子格式的低 16bit */
                audio_format = (uint16_t)fmt[24] | ((uint16_t)fmt[25] << 8);
            }
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) return -4;
            info->data_offset = (uint32_t)ftell(f);
            info->data_bytes  = size;
            return (audio_format == 1) ? 0 : -5;   /* 仅支持 PCM */
        } else {
            fseek(f, (long)size + (size & 1u), SEEK_CUR);   /* 跳过未知块 */
        }
    }
}

/* 任意 PCM 格式 -> 16bit 小端立体声交织，返回写出字节数 */
static uint32_t pcm_to_s16_stereo(const uint8_t *src, uint32_t frames,
                                  const wav_info_t *info, uint8_t *dst)
{
    int16_t *out = (int16_t *)dst;
    uint32_t n = 0;
    const uint16_t ch  = info->channels ? info->channels : 1;
    const uint16_t bps = info->bits / 8;
    const uint8_t *p   = src;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t l = 0, r = 0;
        for (uint16_t c = 0; c < ch; c++, p += bps) {
            int16_t v;
            switch (info->bits) {
                case 8:  v = (int16_t)(((int16_t)p[0] - 128) << 8); break;  /* 无符号 */
                case 16: v = (int16_t)(p[0] | (p[1] << 8));         break;
                case 24: v = (int16_t)(p[1] | (p[2] << 8));         break;  /* 取高 16bit */
                case 32: v = (int16_t)(p[2] | (p[3] << 8));         break;
                default: v = 0; break;
            }
            if (c == 0)      l = v;
            else if (c == 1) r = v;
        }
        if (ch == 1) r = l;                     /* 单声道复制为立体声 */
        out[n++] = l;
        out[n++] = r;
    }
    return n * sizeof(int16_t);
}

static bool rate_supported(uint32_t rate)
{
    static const uint32_t tbl[] = { 8000, 11025, 12000, 16000, 22050,
                                    24000, 32000, 44100, 48000 };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        if (tbl[i] == rate) return true;
    }
    return false;
}

/* 按采样率打开（或重开）播放通道 */
static esp_err_t codec_open_for(uint32_t rate)
{
    if (s_playback == NULL) return ESP_FAIL;

    if (s_opened) {
        esp_codec_dev_close(s_playback);
        s_opened = false;
    }
    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = rate;
    fs.channel         = 2;
    fs.bits_per_sample = 16;
    if (esp_codec_dev_open(s_playback, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed @%uHz", (unsigned)rate);
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_playback, s_volume);
    s_opened = true;
    return ESP_OK;
}

/* ================================================================== */
/*                         曲目扫描                                    */
/* ================================================================== */

static void scan_dir(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < WAV_MAX_TRACKS) {
        if (ent->d_name[0] == '.') continue;

        char full[WAV_PATH_LEN];
        int n = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;

        struct stat st;
        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (depth < SCAN_MAX_DEPTH) scan_dir(full, depth + 1);
            continue;
        }
        if (!is_wav_name(ent->d_name)) continue;

        FILE *f = fopen(full, "rb");
        if (!f) continue;

        wav_info_t info = {};
        int ok = wav_parse_header(f, &info);
        fclose(f);

        if (ok == 0 && info.data_bytes > 0) {
            wav_track_t *t = &s_tracks[s_count];
            memset(t, 0, sizeof(*t));
            strncpy(t->path, full, sizeof(t->path) - 1);
            strncpy(t->name, ent->d_name, sizeof(t->name) - 1);
            size_t ln = strlen(t->name);
            if (ln > 4) t->name[ln - 4] = '\0';      /* 去掉 .wav */

            t->sample_rate = info.sample_rate;
            t->channels    = info.channels;
            t->bits        = info.bits;
            t->data_bytes  = info.data_bytes;
            if (info.sample_rate && info.channels && info.bits) {
                uint32_t bps = info.sample_rate * info.channels * (info.bits / 8);
                t->duration_ms = bps ? (uint32_t)((uint64_t)info.data_bytes * 1000ULL / bps) : 0;
            }
            s_count++;
        } else {
            ESP_LOGW(TAG, "skip (not PCM wav, code=%d): %s", ok, full);
        }
    }
    closedir(dir);
}

static int track_cmp(const void *a, const void *b)
{
    return strcasecmp(((const wav_track_t *)a)->name, ((const wav_track_t *)b)->name);
}

static esp_err_t sdcard_mount(void)
{
    if (s_card) return ESP_OK;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files              = 8;
    mount_config.allocation_unit_size   = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = SDMMC_CLK_PIN;
    slot.cmd   = SDMMC_CMD_PIN;
    slot.d0    = SDMMC_D0_PIN;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s (请插入 FAT32 格式 TF 卡)", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

static void do_rescan(void)
{
    s_state = WAV_ST_IDLE;
    if (s_opened && s_playback) { esp_codec_dev_close(s_playback); s_opened = false; }
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    s_sd_ready = false;
    s_count    = 0;
    s_cur      = -1;
    memset(s_tracks, 0, sizeof(s_tracks));

    if (sdcard_mount() == ESP_OK) {
        s_sd_ready = true;
        scan_dir(SD_MOUNT_POINT, 0);
        qsort(s_tracks, s_count, sizeof(wav_track_t), track_cmp);
        ESP_LOGI(TAG, "rescan done, %d track(s)", s_count);
    }
}

/* ================================================================== */
/*                         播放任务                                    */
/* ================================================================== */

static wav_cmd_t take_cmd(int *index)
{
    if (s_mutex == NULL) return CMD_NONE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    wav_cmd_t c = s_cmd;
    s_cmd = CMD_NONE;
    if (index) *index = s_req_index;
    xSemaphoreGive(s_mutex);
    return c;
}

static void post_cmd(wav_cmd_t c, int index)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_req_index = index;
    s_cmd = c;
    xSemaphoreGive(s_mutex);
}

/* 播放单个文件，直到播完或被命令打断；结果写入 s_action */
static void play_file(int index)
{
    s_action = ACT_AUTO_NEXT;

    if (index < 0 || index >= s_count) { s_action = ACT_ERROR; return; }

    wav_info_t info = {};
    FILE *f = fopen(s_tracks[index].path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open failed: %s", s_tracks[index].path);
        s_action = ACT_ERROR;
        return;
    }
    if (wav_parse_header(f, &info) != 0 || info.channels == 0 || info.bits == 0) {
        ESP_LOGE(TAG, "bad wav: %s", s_tracks[index].path);
        fclose(f);
        s_action = ACT_ERROR;
        return;
    }

    uint32_t rate = info.sample_rate ? info.sample_rate : 44100;
    if (!rate_supported(rate)) {
        ESP_LOGW(TAG, "rate %uHz not in ES8311 table, try anyway", (unsigned)rate);
    }
    if (codec_open_for(rate) != ESP_OK) {
        fclose(f);
        s_action = ACT_ERROR;
        return;
    }

    s_cur    = index;
    s_dur_ms = s_tracks[index].duration_ms;
    s_pos_ms = 0;
    s_state  = WAV_ST_PLAYING;
    ESP_LOGI(TAG, "playing [%d/%d] %s  (%uHz %uch %ubit)",
             index + 1, s_count, s_tracks[index].name,
             (unsigned)rate, info.channels, info.bits);

    const uint32_t bytes_per_frame = (uint32_t)info.channels * (info.bits / 8);
    uint32_t remain      = info.data_bytes;
    uint32_t frames_done = 0;

    while (remain > 0) {
        /* ---- 处理控制命令 ---- */
        int idx = 0;
        wav_cmd_t c = take_cmd(&idx);
        if (c != CMD_NONE) {
            switch (c) {
                case CMD_STOP:   s_action = ACT_STOP;  break;
                case CMD_NEXT:   s_action = ACT_NEXT;  break;
                case CMD_PREV:   s_action = ACT_PREV;  break;
                case CMD_PLAY:   s_action = ACT_PLAY; s_next_index = idx; break;
                case CMD_SCAN:   s_need_rescan = true; s_action = ACT_STOP; break;
                case CMD_TOGGLE:
                    s_state = (s_state == WAV_ST_PAUSED) ? WAV_ST_PLAYING : WAV_ST_PAUSED;
                    break;
                default: break;
            }
            if (s_action != ACT_AUTO_NEXT) break;      /* 被打断，退出本曲 */
        }

        if (s_state == WAV_ST_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        uint32_t want = FRAMES_PER_CHUNK;
        if ((uint64_t)want * bytes_per_frame > remain) {
            want = remain / bytes_per_frame;
        }
        if (want == 0) break;

        size_t rd = fread(s_in_buf, 1, want * bytes_per_frame, f);
        uint32_t got = rd / bytes_per_frame;
        if (got == 0) break;

        uint32_t out_bytes = pcm_to_s16_stereo(s_in_buf, got, &info, s_out_buf);
        if (out_bytes == 0) break;

        if (esp_codec_dev_write(s_playback, s_out_buf, (int)out_bytes) != ESP_OK) {
            ESP_LOGW(TAG, "codec write failed");
            break;
        }

        remain      -= rd;
        frames_done += got;
        if (info.sample_rate) {
            s_pos_ms = (uint32_t)((uint64_t)frames_done * 1000ULL / info.sample_rate);
        }
    }

    fclose(f);
    s_pos_ms = 0;
}

/* 连续播放：直到收到 STOP 或允许重扫 */
static void run_playlist(int start)
{
    if (s_count <= 0) {
        s_state = WAV_ST_IDLE;
        return;
    }
    int idx = (start < 0 || start >= s_count) ? 0 : start;
    int err_run = 0;

    for (;;) {
        play_file(idx);

        switch (s_action) {
            case ACT_STOP:
                s_state = WAV_ST_IDLE;
                if (s_opened && s_playback) { esp_codec_dev_close(s_playback); s_opened = false; }
                return;
            case ACT_PLAY:
                idx = (s_next_index >= 0 && s_next_index < s_count) ? s_next_index : idx;
                err_run = 0;
                break;
            case ACT_PREV:
                idx = (idx <= 0) ? s_count - 1 : idx - 1;
                err_run = 0;
                break;
            case ACT_ERROR:
                if (++err_run >= s_count) {          /* 全部文件都打不开 */
                    ESP_LOGE(TAG, "no playable track");
                    s_state = WAV_ST_IDLE;
                    return;
                }
                idx = (idx + 1) % s_count;
                break;
            case ACT_NEXT:
            case ACT_AUTO_NEXT:
            default:
                idx = (idx + 1) % s_count;
                err_run = 0;
                break;
        }

        if (s_need_rescan) {                          /* 让出给重扫流程 */
            s_state = WAV_ST_IDLE;
            return;
        }
    }
}

static void wav_task(void *arg)
{
    (void)arg;
    for (;;) {
        int idx = 0;
        wav_cmd_t c = take_cmd(&idx);

        switch (c) {
            case CMD_PLAY:
                run_playlist(idx);
                break;
            case CMD_NEXT:
                run_playlist(s_count ? (s_cur < 0 ? 0 : (s_cur + 1) % s_count) : 0);
                break;
            case CMD_PREV:
                run_playlist(s_count ? (s_cur <= 0 ? s_count - 1 : s_cur - 1) : 0);
                break;
            case CMD_TOGGLE:
                if (s_state == WAV_ST_PLAYING)      s_state = WAV_ST_PAUSED;
                else if (s_state == WAV_ST_PAUSED)  s_state = WAV_ST_PLAYING;
                else                                run_playlist(s_cur < 0 ? 0 : s_cur);
                break;
            case CMD_STOP:
                s_state = WAV_ST_IDLE;
                if (s_opened && s_playback) { esp_codec_dev_close(s_playback); s_opened = false; }
                break;
            case CMD_SCAN:
                s_need_rescan = true;
                break;
            case CMD_NONE:
            default:
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }

        if (s_need_rescan) {
            s_need_rescan = false;
            do_rescan();
        }
    }
}

/* ================================================================== */
/*                         对外接口                                    */
/* ================================================================== */

esp_err_t wav_player_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    /* 1) 音频编解码器：ES8311(DAC) + ES7210(ADC)，板型名与 board_cfg.h 一致 */
    set_codec_board_type("S3_LCD_3_49");
    codec_init_cfg_t codec_cfg = {
        .in_mode    = CODEC_I2S_MODE_TDM,
        .out_mode   = CODEC_I2S_MODE_TDM,
        .in_use_tdm = false,
        .reuse_dev  = false,
    };
    if (init_codec(&codec_cfg) != 0) {
        ESP_LOGE(TAG, "init_codec failed");
        return ESP_FAIL;
    }
    s_playback = get_playback_handle();
    if (s_playback == NULL) {
        ESP_LOGE(TAG, "no playback handle");
        return ESP_FAIL;
    }

    /* 2) 缓冲区放在 PSRAM */
    s_in_buf  = (uint8_t *)heap_caps_malloc(READ_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_out_buf = (uint8_t *)heap_caps_malloc(OUT_BUF_BYTES,  MALLOC_CAP_SPIRAM);
    if (!s_in_buf || !s_out_buf) {
        ESP_LOGE(TAG, "no PSRAM buffer");
        return ESP_ERR_NO_MEM;
    }

    /* 3) SD 卡 + 扫描 */
    if (sdcard_mount() == ESP_OK) {
        s_sd_ready = true;
        scan_dir(SD_MOUNT_POINT, 0);
        qsort(s_tracks, s_count, sizeof(wav_track_t), track_cmp);
        ESP_LOGI(TAG, "found %d wav track(s)", s_count);
    }

    /* 4) 播放任务（核心 1） */
    xTaskCreatePinnedToCore(wav_task, "wav_task", 6 * 1024, NULL, 3, &s_task, 1);
    return ESP_OK;
}

esp_err_t wav_rescan(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    post_cmd(CMD_SCAN, 0);
    return ESP_OK;
}

bool wav_sd_ready(void)     { return s_sd_ready; }
int  wav_track_count(void)  { return s_count; }

const wav_track_t *wav_track(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_tracks[index];
}

void wav_play_index(int index)
{
    if (index >= 0 && index < s_count) post_cmd(CMD_PLAY, index);
}
void wav_toggle(void) { post_cmd(CMD_TOGGLE, 0); }
void wav_next(void)   { post_cmd(CMD_NEXT, 0); }
void wav_prev(void)   { post_cmd(CMD_PREV, 0); }
void wav_stop(void)   { post_cmd(CMD_STOP, 0); }

bool     wav_is_playing(void)    { return s_state == WAV_ST_PLAYING; }
bool     wav_is_paused(void)     { return s_state == WAV_ST_PAUSED; }
int      wav_current_index(void) { return s_cur; }
uint32_t wav_position_ms(void)   { return s_pos_ms; }
uint32_t wav_duration_ms(void)   { return s_dur_ms; }

const char *wav_state_text(void)
{
    if (!s_sd_ready)  return "No SD Card";
    if (s_count == 0) return "No WAV Files";
    switch (s_state) {
        case WAV_ST_PLAYING: return "Playing";
        case WAV_ST_PAUSED:  return "Paused";
        default:             return "Stopped";
    }
}

void wav_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_playback) esp_codec_dev_set_out_vol(s_playback, vol);
}

int wav_get_volume(void) { return s_volume; }
