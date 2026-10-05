/**
 ******************************************************************************
 * @file    MusicPlayer.ino
 * @brief   ESP32-S3-Touch-LCD-3.49 (V2) 触摸音乐播放器
 *
 * 功能：
 *   - 从 Micro SD 卡（FAT32）读取 WAV(PCM) 音乐，通过板载 ES8311 + NS4150 播放
 *   - 3.49" 172x640 触摸屏 LVGL 界面：曲目列表 / 进度 / 时间 / 上一首 / 播放暂停 /
 *     下一首 / 音量滑条 / 频谱动画 / 重新扫描
 *   - 使用板载 TCA9554 扩展 IO 控制 LCD 复位、背光使能与音频功放
 *
 * 硬件链路：
 *   TF 卡 --SDMMC(1bit)--> ESP32-S3 --I2S(TDM)--> ES8311 DAC
 *         --模拟--> NS4150 功放(EXIO7 使能) --> MX1.25 扬声器
 *
 * 版本适配：
 *   本示例默认适配 V2 版（PCB 丝印 Rev1.1 / 外壳贴 V2 标签）。
 *   若使用已停产的 V1 版，请见 README.md 的「V1 版适配」一节。
 *
 * 编译环境：
 *   - Arduino IDE + esp32 by Espressif Systems v3.3.0
 *   - LVGL v8.4.0（离线库，见 README.md）
 *   - 开发板: ESP32S3 Dev Module / Flash 16MB / PSRAM: OPI PSRAM /
 *     Partition: 16M Flash (3MB APP/9.9MB FATFS) / USB CDC On Boot: Enabled
 ******************************************************************************
 */
#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "i2c_bsp.h"
#include "src/board/board_io.h"
#include "src/font/lv_font_cjk.h"
#include "src/music/wav_player.h"
#include "src/music/music_ui.h"
#include "src/music/web_player.h"
#include "src/music/wifi_link.h"
#include "src/music/ext_link.h"

/* 供 lvgl_port.c / board_io.c 使用 */
esp_io_expander_handle_t io_expander = NULL;

void setup()
{
    /* ══ 第 0 步：抢在用户松开 PWR 键之前，把电源闩锁接过来 ══
     * 锂电池供电时，板上电源靠「按住 PWR 键」维持，闩锁由 TCA9554 的
     * EXIO6(SYS_EN) 控制。固件必须尽早把它拉高自锁，否则松手 / 拔 USB
     * 的瞬间整机立刻断电（就是之前「只能用 USB 供电」的原因）。
     * 所以这一段放在最前面，之前不打印任何东西、不做任何耗时操作。 */
    i2c_master_Init();          /* 只建立 I2C 总线，很快（TCA9554 挂在 port0） */
    esp_err_t pwr_hold = board_power_hold();

    Serial.begin(115200);
    delay(300);
    /* 打开 INFO 级日志：Arduino 默认把日志级别钳在 ERROR，
     * 导致 lvgl_port / board_io 里的 ESP_LOGI 全部被吞掉，无法排障 */
    esp_log_level_set("*", ESP_LOG_INFO);
    Serial.println();
    Serial.println("==============================================");
    Serial.println(" ESP32-S3-Touch-LCD-3.49  Music Player (V2)");
    Serial.println("==============================================");

    /* 电源自检：SYS_EN 是否成功拉高 + 当前供电源 + 电池电压 */
    {
        uint32_t batt_mv = board_battery_mv();
        Serial.printf("[PWR] 电池自锁 SYS_EN(EXIO6)=1 -> %s | "
                      "供电源=%s (SYS_OUT/GPIO%d=%d) | 电池=%.2fV (%d%%)\n",
                      esp_err_to_name(pwr_hold),
                      board_on_battery() ? "锂电池" : "USB",
                      (int)EXAMPLE_PIN_NUM_SYS_OUT,
                      gpio_get_level(EXAMPLE_PIN_NUM_SYS_OUT),
                      batt_mv / 1000.0f, board_battery_pct());
        if (pwr_hold != ESP_OK) {
            Serial.println("[PWR] !! 电源闩锁没接上：拔掉 USB 后会立刻断电！");
        }
    }

    /* 诊断：复位原因。若反复出现 PANIC/BROWNOUT，说明是「熄屏」的真凶 */
    esp_reset_reason_t rr = esp_reset_reason();
    const char *rr_name = "?";
    switch (rr) {
        case ESP_RST_POWERON:  rr_name = "POWERON";  break;
        case ESP_RST_EXT:      rr_name = "EXT";      break;
        case ESP_RST_SW:       rr_name = "SW";       break;
        case ESP_RST_PANIC:    rr_name = "PANIC(崩溃)";  break;
        case ESP_RST_INT_WDT:  rr_name = "INT_WDT";  break;
        case ESP_RST_TASK_WDT: rr_name = "TASK_WDT"; break;
        case ESP_RST_WDT:      rr_name = "WDT";      break;
        case ESP_RST_BROWNOUT: rr_name = "BROWNOUT(电压不足!)"; break;
        case ESP_RST_DEEPSLEEP:rr_name = "DEEPSLEEP";break;
        case ESP_RST_USB:      rr_name = "USB";      break;
        case ESP_RST_JTAG:     rr_name = "JTAG";     break;
        default: break;
    }
    Serial.printf("reset_reason = %d (%s),  uptime_since_boot_us=%lld\n",
                  (int)rr, rr_name, (long long)esp_timer_get_time());

    /* 1) I2C 总线
     *    port0: SDA=47 SCL=48  -> ES8311 / ES7210 / TCA9554 / PCF85063 / QMI8658
     *    port1: SDA=17 SCL=18  -> AXS15231B 触摸
     *    （已在第 0 步建立，这里不再重复调用）
     */

    /* 2) TCA9554 扩展 IO：LCD 复位、背光使能、音频功放使能、电源闩锁 */
    board_exio_init();

    /* 2b) PWR 键：长按 2 秒关机（松开 SYS_EN 闩锁，电池供电即断电）。
     *     插着 USB 时长按不会真的关机（另有电源在供），但会把闩锁先松开
     *     再自动闩回去，所以可以放心在台面上调试。 */
    {
        esp_err_t pe = board_pwr_button_init();
        Serial.printf("[PWR] PWR 键长按关机：%s（长按 %d ms）\n",
                      esp_err_to_name(pe), EXAMPLE_PWR_LONGPRESS_MS);
    }

    /* 3) 音频编解码器 + SD 卡挂载 + 扫描 + 播放任务 */
    if (wav_player_init() != ESP_OK) {
        Serial.println("[WARN] audio/sdcard init failed, UI still running");
    }
    Serial.printf("Tracks found: %d, SD ready: %d\n",
                  wav_track_count(), (int)wav_sd_ready());

    /* 4) 字体装配：把 20px 的补充字库（GB2312 二级汉字）挂成 fallback。
     *    必须在创建任何 LVGL 文字控件之前调用，否则 20px 下的二级汉字
     *    （奕 嵩 泷 靓 痣 …）会退化成 LVGL 的占位方框。
     *    纯内存操作，不依赖 LVGL 初始化。 */
    lv_font_cjk_setup();

    /* 5) LCD + 触摸 + LVGL + 播放器界面 */
    lvgl_port_init();

    /* 6) 背光 PWM 调光（200/255 ≈ 78%） */
    if (backlight_pwm_init(200) != ESP_OK) {
        Serial.println("[WARN] LEDC PWM unavailable, backlight forced full-on");
    }

    /* 诊断：确认背光/复位/功放/SYS_EN 四个 EXIO 的最终状态 */
    lvgl_port_dump_exio("after_setup");

    /* 6) 网络接入 + 网页播放器
     *    优先连**家里局域网**（STA）：手机连着同一个路由器就能访问板子，且**保留外网**，
     *    所以流媒体 App 还能用；连不上则自动退回热点 ESP32-Player 兜底。
     *    选歌后音频由**手机**播放，板子显示同步歌词。 */
    if (wifi_link_start() != ESP_OK) {
        Serial.println("[WARN] 网络接入失败");
    }

    if (web_player_start(wifi_link_ip()) == ESP_OK) {
        Serial.printf("web player ready: %s  (%d tracks)\n",
                      wifi_link_url(), web_track_count());
    } else {
        Serial.println("[WARN] web player start failed");
    }

    /* 7) 外部歌词推送通道（PC 端「歌词桥」）
     *    PC 上跑 kugou_bridge.py：读 Windows SMTC 拿到酷狗在放的歌与进度，
     *    抓歌词后推给板子；板子只当歌词机。
     *    同时开 UDP 48899 做自动发现，PC 不用手填 IP。 */
    ext_link_init();
    Serial.printf("ext link: UDP %d listening (PC 广播 ESP32LYRICS?)\n", EXT_UDP_PORT);

    Serial.println();
    Serial.println("Ready. 板子 = 歌词机（等电脑推送）");
    if (wifi_link_is_sta()) {
        Serial.printf(">>> 局域网模式: 电脑与本板同一 WiFi 即可，地址 %s\n", wifi_link_url());
        Serial.printf(">>>             或 http://%s.local/\n", WEB_MDNS_HOST);
    } else {
        Serial.printf(">>> 热点模式: 先连 WiFi \"%s\" 密码 \"%s\"，打开 %swifi 填家里 WiFi\n",
                      WEB_AP_SSID, WEB_AP_PASS, wifi_link_url());
        Serial.println(">>>           （要收电脑推送，板子必须和电脑在同一个路由器下）");
    }
}

void loop()
{
    /* 播放逻辑运行在独立任务中，主循环空闲即可 */
    static uint32_t n = 0;
    static uint32_t last = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    /* 网络保活：STA 掉线自动重连、DHCP 续租后刷新 IP */
    wifi_link_tick();

    /* 心跳：每 5 秒一行。若「熄屏」后这里还在打印，说明 CPU 没死，
       问题在背光/显示；若这里也停了，说明整机挂了或重启。
       顺带报供电源与电池电压，方便确认锂电池是不是真的在工作。 */
    if (now - last >= 5) {
        last = now;
        Serial.printf("[LOOP] alive %lus n=%u heap=%u net=%s ip=%s tracks=%d "
                      "np[src=%d st=%d pos=%ums title=%s ago=%ums rx=%u] "
                      "pwr=%s batt=%.2fV\n",
                      (unsigned long)now, (unsigned)n++,
                      (unsigned)esp_get_free_heap_size(),
                      wifi_link_mode_text(), wifi_link_ip(),
                      wav_track_count(),
                      (int)np_source(), np_state(), (unsigned)np_position_ms(),
                      np_title(), (unsigned)ext_link_silence_ms(),
                      (unsigned)ext_link_rx_count(),
                      board_on_battery() ? "BATT" : "USB",
                      board_battery_mv() / 1000.0f);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
