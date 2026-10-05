/**
 * @file  board_io.h
 * @brief ESP32-S3-Touch-LCD-3.49 (V2) 板级 IO：TCA9554 扩展 IO + 背光 PWM
 */
#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ★锂电池供电自锁★ —— 把电源闩锁从 PWR 按键接到固件手里
 *
 * 本板没有真正的电源开关：锂电池供电时，按住 PWR 键硬件才给整机供电，
 * 松开就断。闩锁由 TCA9554 的 EXIO6（SYS_EN）控制，固件必须在启动早期
 * 把它设为输出并拉高，之后松开 PWR 键板子才会继续带电运行。
 *
 * 官方 ESP-IDF 07_BATT_PWR_Test 的 tca9554_init() 做的正是这件事。
 * 不调用的后果：插着 USB 一切正常，一拔 USB（或松开 PWR）立刻断电。
 *
 * @note 必须尽可能早调用，越早越好（i2c_master_Init() 之后第一件事）。
 *       内部会顺带把 SYS_OUT(GPIO16) 配成输入，供 board_on_battery() 读。
 * @return ESP_OK 成功
 */
esp_err_t board_power_hold(void);

/**
 * @brief 松开电源闩锁（EXIO6 = 0）→ 锂电池供电时整机断电
 *
 * 若板子还有别的供电源（USB 插着），断电不会成功；此时本函数会在
 * 短暂延时后**把闩锁重新拉高**并打日志，避免出现「长按一下就把电池
 * 闩锁松开、之后一拔 USB 立刻死机」的隐患。
 *
 * @note 由长按 PWR 键、或网页控制台 POST /api/pwr?off=1 触发。
 */
void board_power_off(void);

/**
 * @brief 启动「长按 PWR 键关机」监视任务（20ms 轮询 GPIO16）
 *
 * PWR 键就是 GPIO16（低电平有效，内部上拉）。持续按下 >=
 * EXAMPLE_PWR_LONGPRESS_MS 后调用 board_power_off()。
 *
 * 只有先见过「松开」（高电平）才会开始计时，这样 USB 供电时引脚若被
 * 电源电路持续拉低，也不会被误判成「一直按着」而触发关机。
 *
 * @note 在 board_exio_init() 之后调用即可；任务常驻，不需要手动喂狗。
 */
esp_err_t board_pwr_button_init(void);

/** @brief PWR 键当前是否按下（按下 = true） */
bool board_pwr_pressed(void);

/** @brief 供电源：true = 电池供电，false = USB（读 SYS_OUT / GPIO16） */
bool board_on_battery(void);

/** @brief 电池电压，单位 mV。GPIO4 经 1:3 分压，读失败返回 0 */
uint32_t board_battery_mv(void);

/** @brief 电池电量估算百分比 0~100（3.30V=0%，4.20V=100%），读失败返回 -1 */
int board_battery_pct(void);

/**
 * @brief 初始化 TCA9554 扩展 IO
 *
 *  - TOUCH_INT  -> 输入
 *  - BL_EN      -> 输出（背光使能）
 *  - LCD_RST    -> 输出
 *  - NS_MODE    -> 输出高（使能 NS4150 音频功放）
 *  - SYS_EN     -> 输出高（锂电池电源闩锁，兜底再拉一次）
 *
 * @note 必须在 i2c_master_Init() 之后、lvgl_port_init() 之前调用
 */
void board_exio_init(void);

/** @brief 初始化背光 PWM（LEDC），duty 0~255
 *  @return ESP_OK 成功；失败时已自动退化为 GPIO 常亮 */
esp_err_t backlight_pwm_init(uint8_t duty);

/** @brief 设置背光亮度，duty 0~255 */
void backlight_set(uint8_t duty);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_IO_H */
