/**
 * @file  board_io.c
 * @brief ESP32-S3-Touch-LCD-3.49 (V2) 板级 IO 实现
 */
#include "board_io.h"
#include "user_config.h"

#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

/* Arduino 预编译库把日志上限钳在 ERROR，ESP_LOGI 被编译期删掉，排障走 printf */
#define DBG(fmt, ...) printf("[DBG] " fmt "\n", ##__VA_ARGS__)

#include "src/tca9554/esp_io_expander.h"
#include "src/tca9554/esp_io_expander_tca9554.h"

static const char *TAG = "BOARD_IO";

/* 定义在 MusicPlayer.ino 中（lvgl_port.c 也会 extern 使用） */
extern esp_io_expander_handle_t io_expander;

/* ================================================================== */
/* 电源：锂电池供电自锁 / 电压采集                                     */
/* ================================================================== */

/* 保证 TCA9554 句柄存在（board_exio_init 里也有一份同样的逻辑） */
static esp_err_t exio_ensure(void)
{
    if (io_expander != NULL) return ESP_OK;
    i2c_master_bus_handle_t bus = NULL;
    /* I2C 端口 0 已经在 i2c_master_Init() 中建立（SDA=47, SCL=48） */
    esp_err_t e = i2c_master_get_bus_handle(0, &bus);
    if (e != ESP_OK) return e;
    return esp_io_expander_new_i2c_tca9554(
               bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
}

esp_err_t board_power_hold(void)
{
    /* SYS_OUT(GPIO16) 同时是 PWR 按键输入，官方 demo 里配成「输入 + 上拉」，
     * 用它的电平判断当前是不是电池供电。这里一起配好。 */
    gpio_config_t gc = {};
    gc.intr_type    = GPIO_INTR_DISABLE;
    gc.mode         = GPIO_MODE_INPUT;
    gc.pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_SYS_OUT);
    gc.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gc.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&gc);

    esp_err_t e0 = exio_ensure();
    if (e0 != ESP_OK) return e0;

    /* ★关键★ 拉高 SYS_EN(EXIO6)：把「按 PWR 才供电」的硬件闩锁接过来。
     * 必须在用户松开 PWR 键之前完成，所以这个函数要尽早调用。 */
    esp_err_t e1 = esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_SYS_EN,
                                           IO_EXPANDER_OUTPUT);
    esp_err_t e2 = esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1);
    if (e1 != ESP_OK || e2 != ESP_OK) return ESP_FAIL;

    /* 回读确认真的写进去了 */
    uint32_t lv = 0;
    if (esp_io_expander_get_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, &lv) == ESP_OK) {
        if (!(lv & EXAMPLE_EXIO_PIN_SYS_EN)) return ESP_FAIL;
    }
    return ESP_OK;
}

void board_power_off(void)
{
    if (io_expander == NULL) return;

    /* 先把日志吐出去，免得断得太快看不见 */
    printf("[PWR] SYS_EN(EXIO6)=0 -> 松开电源闩锁，锂电池供电时整机断电\n");
    fflush(stdout);
    esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 0);

    /* 关键：断电成功的板子根本走不到下一行（电没了）。
     * 能走到这里 = 还有别的供电源（典型是插着 USB），于是把闩锁重新拉高，
     * 免得「长按一下 = 把电池闩锁松开」之后一拔 USB 就立刻死机。 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1);
    printf("[PWR] 松开后板子仍在运行 -> 判断为 USB 供电，已重新闩上 SYS_EN（未断电）\n");
    fflush(stdout);
}

/* ---------------- 长按 PWR 键关机 ---------------- */

#define PWR_POLL_MS  20

static volatile bool s_pwr_pressed = false;

bool board_pwr_pressed(void) { return s_pwr_pressed; }

static void pwr_btn_task(void *arg)
{
    (void)arg;
    /* armed：必须先见过「松开」（高电平）才允许开始计时。
     * 引脚在 USB 供电时可能被电源电路一直拉低，若不做这个前置判断，
     * 就会把它当成「一直按着」，一上电就触发关机。 */
    bool armed = false;
    int  held  = 0;
    bool logged_once = false;

    for (;;) {
        int lv = gpio_get_level(EXAMPLE_PIN_NUM_SYS_OUT);

        if (lv) {                      /* 松开 */
            armed = true;
            held  = 0;
            s_pwr_pressed = false;
        } else if (armed) {            /* 按下 */
            s_pwr_pressed = true;
            held += PWR_POLL_MS;
            if (!logged_once) {
                logged_once = true;
                DBG("PWR 键按下，长按 %dms 关机", EXAMPLE_PWR_LONGPRESS_MS);
            }
            if (held >= EXAMPLE_PWR_LONGPRESS_MS) {
                armed = false;
                held  = 0;
                logged_once = false;
                board_power_off();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PWR_POLL_MS));
    }
}

esp_err_t board_pwr_button_init(void)
{
    gpio_config_t gc = {};
    gc.intr_type    = GPIO_INTR_DISABLE;
    gc.mode         = GPIO_MODE_INPUT;
    gc.pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_SYS_OUT);
    gc.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gc.pull_up_en   = GPIO_PULLUP_ENABLE;
    esp_err_t e = gpio_config(&gc);
    if (e != ESP_OK) return e;

    if (xTaskCreate(pwr_btn_task, "pwr_btn", 2560, NULL, 3, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool board_on_battery(void)
{
    return gpio_get_level(EXAMPLE_PIN_NUM_SYS_OUT) != 0;
}

/* ADC1_CH3 == GPIO4（板载电池分压） */
#define BAT_ADC_CHANNEL ADC_CHANNEL_3

uint32_t board_battery_mv(void)
{
    static adc_oneshot_unit_handle_t s_adc  = NULL;
    static adc_cali_handle_t         s_cali = NULL;
    static bool                      s_done = false;

    if (!s_done) {
        s_done = true;
        adc_oneshot_unit_init_cfg_t unit = {};
        unit.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) { s_adc = NULL; return 0; }

        adc_oneshot_chan_cfg_t ch = {};
        ch.atten    = ADC_ATTEN_DB_12;
        ch.bitwidth = ADC_BITWIDTH_12;
        if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ch) != ESP_OK) {
            s_adc = NULL;
            return 0;
        }
        adc_cali_curve_fitting_config_t cc = {};
        cc.unit_id  = ADC_UNIT_1;
        cc.atten    = ADC_ATTEN_DB_12;
        cc.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) != ESP_OK) {
            s_cali = NULL;   /* 没有校准就退回线性换算 */
        }
    }
    if (s_adc == NULL) return 0;

    int raw = 0;
    if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) return 0;

    int pin_mv = 0;
    if (s_cali == NULL || adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) != ESP_OK) {
        pin_mv = (int)((float)raw * 3300.0f / 4096.0f);
    }
    /* 分压 1:3，乘回去才是电池真实电压 */
    return (uint32_t)pin_mv * EXAMPLE_BAT_ADC_DIVIDER;
}

int board_battery_pct(void)
{
    uint32_t mv = board_battery_mv();
    if (mv == 0) return -1;
    /* 粗略线性映射：3.30V 空 / 4.20V 满（锂电池放电曲线中段平缓，够用即可） */
    const int lo = 3300, hi = 4200;
    if ((int)mv <= lo) return 0;
    if ((int)mv >= hi) return 100;
    return (int)(((int)mv - lo) * 100 / (hi - lo));
}

void board_exio_init(void)
{
    ESP_ERROR_CHECK(exio_ensure());

    /* 输入：触摸中断 */
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander,
                       EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));

    /* 输出：背光使能 / LCD 复位 / 功放使能 / 电源闩锁 */
    ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander,
                       EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST |
                       EXAMPLE_EXIO_PIN_NS_MODE | EXAMPLE_EXIO_PIN_SYS_EN,
                       IO_EXPANDER_OUTPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
    /* NS_MODE = 1：打开 NS4150 功放，扬声器才有声音 */
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, 1));
    /* 电源闩锁再保一次（board_power_hold() 已经拉高过） */
    ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1));

    uint32_t lv = 0;
    esp_io_expander_get_level(io_expander, 0xFF, &lv);
    DBG("board_exio_init done: all=%02x (BL_EN=%d LCD_RST=%d SYS_EN=%d NS_MODE=%d)",
        (unsigned)(lv & 0xFF),
        (int)((lv & EXAMPLE_EXIO_PIN_BL_EN) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_LCD_RST) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_SYS_EN) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_NS_MODE) ? 1 : 0));
}

esp_err_t backlight_pwm_init(uint8_t duty)
{
    /* duty 是「亮度」0~255。背光低电平点亮，所以要写进 LEDC 的占空比是 255-duty。 */
#if EXAMPLE_BK_LIGHT_ACTIVE_LOW
    const uint8_t ledc_duty = (uint8_t)(255 - duty);
    const int     full_on_gpio = 0;   /* 低电平 = 全亮 */
#else
    const uint8_t ledc_duty = duty;
    const int     full_on_gpio = 1;
#endif

    /* 注意：这里以前用 LEDC_SLOW_CLK_RC_FAST + 50kHz，
     * RC_FAST 只有约 17.5MHz，8bit 分辨率下 50kHz 需要分频比 1.37，
     * 边界值容易配置失败；若失败则 GPIO42 会停在 LEDC 尚未输出的状态，
     * 表现为「背光不亮」。改用 LEDC_AUTO_CLK + 20kHz，稳定且无音频噪声。 */
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = LEDC_TIMER_3,
        .freq_hz         = 20000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e1 = ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num   = EXAMPLE_PIN_NUM_BK_LIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_3,
        .duty       = ledc_duty,
        .hpoint     = 0,
    };
    esp_err_t e2 = ledc_channel_config(&ch_conf);

    /* 同时打开硬件背光使能 */
    esp_err_t e3 = ESP_OK;
    if (io_expander) {
        e3 = esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 1);
    }

    DBG("backlight_pwm_init: timer=%s ch=%s bl_en=%s brightness=%u ledc_duty=%u (GPIO%d, %s)",
        esp_err_to_name(e1), esp_err_to_name(e2), esp_err_to_name(e3),
        (unsigned)duty, (unsigned)ledc_duty, (int)EXAMPLE_PIN_NUM_BK_LIGHT,
        EXAMPLE_BK_LIGHT_ACTIVE_LOW ? "ACTIVE-LOW" : "ACTIVE-HIGH");

    /* 诊断：回读 LEDC 实际频率/占空比与引脚电平 */
    uint32_t real_freq = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_3);
    uint32_t real_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    DBG("LEDC actual: freq=%u Hz duty=%u/255  gpio%d_level=%d",
        (unsigned)real_freq, (unsigned)real_duty,
        (int)EXAMPLE_PIN_NUM_BK_LIGHT,
        gpio_get_level(EXAMPLE_PIN_NUM_BK_LIGHT));

    if (e1 != ESP_OK || e2 != ESP_OK) {
        /* 退化方案：LEDC 失败就直接把 GPIO42 拉成「全亮」电平，保证背光是亮的 */
        ESP_LOGW(TAG, "LEDC unavailable -> fallback to GPIO full-on");
        gpio_config_t gc = {};
        gc.intr_type    = GPIO_INTR_DISABLE;
        gc.mode         = GPIO_MODE_OUTPUT;
        gc.pin_bit_mask = (1ULL << EXAMPLE_PIN_NUM_BK_LIGHT);
        gpio_config(&gc);
        gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, full_on_gpio);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void backlight_set(uint8_t duty)
{
    /* duty 是亮度 0~255 */
#if EXAMPLE_BK_LIGHT_ACTIVE_LOW
    uint32_t ledc_duty = (uint32_t)(255 - duty);
#else
    uint32_t ledc_duty = duty;
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, ledc_duty));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1));
}
