#ifndef USER_CONFIG_H
#define USER_CONFIG_H

//spi & i2c handle
#define LCD_HOST SPI3_HOST

// touch I2C port
#define Touch_SCL_NUM (GPIO_NUM_18)
#define Touch_SDA_NUM (GPIO_NUM_17)

// touch esp
#define ESP_SCL_NUM (GPIO_NUM_48)
#define ESP_SDA_NUM (GPIO_NUM_47)

//  DISP
#define EXAMPLE_PIN_NUM_LCD_CS     (GPIO_NUM_9) 
#define EXAMPLE_PIN_NUM_LCD_PCLK   (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_LCD_DATA0  (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1  (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2  (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3  (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_TE     (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_LCD_RST    (-1)
#define EXAMPLE_PIN_NUM_BK_LIGHT   (GPIO_NUM_42)
#define EXAMPLE_PIN_NUM_EXIO_INT   (GPIO_NUM_8)

/* ★关键★ ESP32-S3-Touch-LCD-3.49 的背光是「低电平点亮」：
 * 官方 xiaozhi-esp32 V2 板级配置里为
 *     #define DISPLAY_BACKLIGHT_PIN          GPIO_NUM_42
 *     #define DISPLAY_BACKLIGHT_OUTPUT_INVERT true
 * 即 GPIO42 = 0 才是「背光全亮」，PWM 占空比必须按 255-duty 反相写。
 * 写成高电平点亮会导致：屏要么全黑，要么只有很暗的一点亮度。 */
#define EXAMPLE_BK_LIGHT_ACTIVE_LOW  1

#define EXAMPLE_PIN_NUM_BAT_ADC    (GPIO_NUM_4)
/* ★注意★ GPIO16 是**一个脚两个用途**：
 *   - 对外叫 SYS_OUT，官方 07 在开机时读它判断「当前是不是电池供电」；
 *   - 同时它就是背面那颗 **PWR 电源键**（官方 button_bsp.c 里 USER_KEY_2 = 16，
 *     低电平有效，内部上拉）。
 * 所以按键按下的瞬间这个脚也会被拉到 0，读「供电源」时要避开按下的时刻。 */
#define EXAMPLE_PIN_NUM_SYS_OUT    (GPIO_NUM_16)

/* PWR 键长按多久触发关机（毫秒）。
 * 官方 07_BATT_PWR_Test 是「长按 -> SYS_EN(EXIO6) 拉低 -> 断电」，
 * 这里取 2000ms：足够长，不会误触（PWR 键在板子背面）。 */
#define EXAMPLE_PWR_LONGPRESS_MS   2000

/* 锂电池电压采集：GPIO4 接在分压电阻上，官方 01_ADC_Test 里
 *     value = 0.001 * vol_mV * 3
 * 即实际电池电压 = ADC 引脚电压 x 3。 */
#define EXAMPLE_BAT_ADC_DIVIDER    3

#define EXAMPLE_EXIO_PIN_TOUCH_INT (1ULL << 0)
#define EXAMPLE_EXIO_PIN_BL_EN     (1ULL << 1)
#define EXAMPLE_EXIO_PIN_IMU_INT1  (1ULL << 2)
#define EXAMPLE_EXIO_PIN_IMU_INT2  (1ULL << 3)
#define EXAMPLE_EXIO_PIN_RTC_INT   (1ULL << 4)
#define EXAMPLE_EXIO_PIN_LCD_RST   (1ULL << 5)
/* ★ EXIO6 = SYS_EN：锂电池供电的电源「自锁」使能。
 *   板上没有真正的电源开关：按 PWR 键时硬件给整机供电，松开就断。
 *   固件必须在启动早期把 SYS_EN 拉高，把闩锁接过来，
 *   这样松开 PWR（或拔掉 USB）之后板子才继续带电运行。
 *   官方 ESP-IDF 07_BATT_PWR_Test 的 tca9554_init() 就是干这个的：
 *       esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, OUTPUT);
 *       esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1);
 *   对应实现见 src/board/board_io.c 的 board_power_hold()。 */
#define EXAMPLE_EXIO_PIN_SYS_EN    (1ULL << 6)
/* EXIO7 = NS_MODE：NS4150 音频功放使能，置 1 才有声音 */
#define EXAMPLE_EXIO_PIN_NS_MODE   (1ULL << 7)


#define I2C_TOUCH_ADDR                    0x3b
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)


#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 5



/*ADDR*/
#define EXAMPLE_RTC_ADDR 0x51

#define EXAMPLE_IMU_ADDR 0x6b


/* 屏幕方向 ------------------------------------------------------------
 * 面板原生（不旋转）是 172 x 640 竖屏。
 * 本产品按「桌面歌词机」横放使用，所以**逻辑画布**取 640 x 172，
 * 由 lvgl_port.c 在刷屏时做软件转置（官方 09_LVGL_V8_Test 同款做法，
 * 见 ref/ex/09_LVGL_V8_Test__lvgl_port.c 的 USER_DISP_ROT_90 分支）。
 *
 * 手持方向有两种（逆时针 / 顺时针各 90°），运行时可切，不用重新烧录：
 *     POST /api/rot?dir=0   官方朝向（默认）
 *     POST /api/rot?dir=1   反向
 */
#define USER_DISP_ROT_90    1
#define USER_DISP_ROT_NONO  0
#define Rotated USER_DISP_ROT_90




#if (Rotated == USER_DISP_ROT_NONO)
#define EXAMPLE_LCD_H_RES 172   
#define EXAMPLE_LCD_V_RES 640
#else
#define EXAMPLE_LCD_H_RES 640   
#define EXAMPLE_LCD_V_RES 172
#endif

/* 面板「原生」分辨率（转置后的输出尺寸），与是否旋转无关 */
#define LCD_NOROT_HRES     172
#define LCD_NOROT_VRES     640

/* DMA 分块：一次搬 LCD_NOROT_HRES x LVGL_DMA_ROW_STEP 个像素。
 * 640 / 64 = 10 条，正好铺满一屏，所以 DMA 缓冲长度必须能被整除。 */
#define LVGL_DMA_ROW_STEP  64
#define LVGL_DMA_BUFF_LEN (LCD_NOROT_HRES * LVGL_DMA_ROW_STEP * 2)
#define LVGL_SPIRAM_BUFF_LEN (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

/* 横屏版面：左侧专辑封面是正方形，边长 COVER_PX（须小于 EXAMPLE_LCD_V_RES） */
#define COVER_PX           156
/* 右侧歌词区可用的宽度 */
#define LYR_PANEL_X        (COVER_PX + 20)
#define LYR_PANEL_W        (EXAMPLE_LCD_H_RES - LYR_PANEL_X - 6)





#endif
