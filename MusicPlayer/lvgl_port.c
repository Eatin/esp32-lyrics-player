#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "user_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "src/axs15231b/esp_lcd_axs15231b.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "src/music/music_ui.h"
#include "i2c_bsp.h"

#define LCD_BIT_PER_PIXEL (16)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"

/* 诊断输出：Arduino 预编译库把 CONFIG_LOG_MAXIMUM_LEVEL 钳在 ERROR，
 * ESP_LOGI/ESP_LOGW 会被编译期整个删掉（运行时 esp_log_level_set 无效），
 * 所以排障信息一律走 printf。 */
#include <stdio.h>
#define DBG(fmt, ...) printf("[DBG] " fmt "\n", ##__VA_ARGS__)

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;

static uint16_t *lvgl_dma_buf = NULL; 
static SemaphoreHandle_t lvgl_flush_semap;
extern esp_io_expander_handle_t io_expander;

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
  {0x11, (uint8_t []){0x00}, 0, 100},
  {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  BaseType_t TaskWoken;
  xSemaphoreGiveFromISR(lvgl_flush_semap,&TaskWoken);
  return false;
}

static void example_increase_lvgl_tick(void *arg)
{
  lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

/* ------------------------------------------------------------------ */
/* 横屏（软件转置）                                                     */
/*                                                                     */
/* 逻辑坐标系（LVGL 画布）：EXAMPLE_LCD_H_RES x EXAMPLE_LCD_V_RES       */
/*                          = 640 x 172（横）                          */
/* 面板原生坐标系        ：LCD_NOROT_HRES  x LCD_NOROT_VRES             */
/*                          = 172 x 640（竖）                          */
/*                                                                     */
/* 两种手持方向的映射（面板像素 px,py  <-  逻辑像素 lx,ly）：            */
/*   dir=0（官方默认，设备逆时针转 90° 手持）                            */
/*        px = (172-1) - ly ,  py = lx                                  */
/*   dir=1（反向，设备顺时针转 90° 手持）                                */
/*        px = ly           ,  py = (640-1) - lx                        */
/*                                                                     */
/* 官方示例是先把整屏转置进一块 220KB 的 PSRAM 中转 buffer，再照旧分条推。 */
/* 这里改成「按面板 LVGL_DMA_ROW_STEP(=64) 行为一条，直接组装进 DMA 缓冲」：*/
/*   - 省掉那块 220KB 中转 buffer                                      */
/*   - 读源是 128 字节连续（4 个 cache line），写目标的工作集很小        */
/*     （整个 DMA 缓冲才 22KB，能待在 cache 里），转置开销明显更小       */
/* ------------------------------------------------------------------ */

#define ROT_ROWS_PER_BAND  (LVGL_DMA_BUFF_LEN / 2 / LCD_NOROT_HRES)   /* 64 */

static volatile int s_rot_dir = 0;   /* 0 = 官方朝向；1 = 反向 */

void lvgl_port_set_rot_dir(int dir) { s_rot_dir = (dir ? 1 : 0); }
int  lvgl_port_rot_dir(void)        { return s_rot_dir; }

/** 组装第 band 条（对应面板 py = band*ROT_ROWS_PER_BAND 起的 64 行） */
static void rot_assemble_band(uint16_t *dma, const uint16_t *src, int band)
{
  const int rows = ROT_ROWS_PER_BAND;
  if (s_rot_dir == 0) {
    /* dma[r*172 + (171-ly)] = src[ly*640 + band*rows + r] */
    for (int ly = 0; ly < EXAMPLE_LCD_V_RES; ly++) {
      const uint16_t *s  = src + (size_t)ly * EXAMPLE_LCD_H_RES + (size_t)band * rows;
      uint16_t       *d0 = dma + (EXAMPLE_LCD_V_RES - 1 - ly);
      for (int r = 0; r < rows; r++) d0[(size_t)r * LCD_NOROT_HRES] = s[r];
    }
  } else {
    /* dma[r*172 + ly] = src[ly*640 + (639 - band*rows - r)] */
    const int base = EXAMPLE_LCD_H_RES - 1 - band * rows;
    for (int ly = 0; ly < EXAMPLE_LCD_V_RES; ly++) {
      const uint16_t *s  = src + (size_t)ly * EXAMPLE_LCD_H_RES;
      uint16_t       *d0 = dma + ly;
      for (int r = 0; r < rows; r++) d0[(size_t)r * LCD_NOROT_HRES] = s[base - r];
    }
  }
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
  (void)area;
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
  const int rows       = ROT_ROWS_PER_BAND;               /* 64  */
  const int flush_coun = LCD_NOROT_VRES / rows;           /* 10  */
  const uint16_t *src  = (const uint16_t *)color_map;

  xSemaphoreGive(lvgl_flush_semap);
  for (int i = 0; i < flush_coun; i++)
  {
    xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
    rot_assemble_band(lvgl_dma_buf, src, i);
    esp_lcd_panel_draw_bitmap(panel_handle, 0, i * rows, LCD_NOROT_HRES, (i + 1) * rows, lvgl_dma_buf);
  }
  xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
  lv_disp_flush_ready(drv);
}

/* ---- 调试开关：打印触摸原始数据与心跳，定位「点一下就灭屏」 ---- */
#define TOUCH_DBG        1
#if TOUCH_DBG
static volatile int s_touch_evt = 0;      /* 触摸事件计数        */
static volatile int s_touch_err = 0;      /* I2C 读失败次数      */
static volatile int s_heartbeat = 0;      /* LVGL 任务心跳       */
#endif

static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  //static uint8_t read_touchpad_cmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8};
  uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
  uint8_t buff[32] = {0};
  memset(buff,0,32);
  esp_err_t tr = i2c_master_write_read_dev(disp_touch_dev_handle,read_touchpad_cmd,11,buff,32);
#if TOUCH_DBG
  if (tr != ESP_OK) {
    if (s_touch_err < 5) DBG("touch i2c err: %s", esp_err_to_name(tr));
    s_touch_err++;
  }
  if (buff[1] > 0 && buff[1] < 5 && s_touch_evt < 30) {
    DBG("TOUCH #%d raw=%02x %02x %02x %02x %02x %02x %02x",
        s_touch_evt, buff[0], buff[1], buff[2], buff[3], buff[4], buff[5], buff[6]);
    s_touch_evt++;
  }
#endif
  ESP_ERROR_CHECK_WITHOUT_ABORT(tr);
  uint16_t pointX;
  uint16_t pointY;
  pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  if (buff[1]>0 && buff[1]<5)
  {
    data->state = LV_INDEV_STATE_PR;

    /* 原始触摸坐标：pointX 沿面板「长边」(0..639)，pointY 沿「短边」(0..171)。
     * 竖屏（未旋转）时官方映射就是  panel_px = pointY, panel_py = 639 - pointX 。
     * 横屏只是把同一组面板坐标再套一次转置，换成 LVGL 的逻辑坐标：
     *   dir=0(官方) : lx = panel_py = 639 - pointX ; ly = 171 - panel_px = 171 - pointY
     *   dir=1(反向) : lx = 639 - panel_py = pointX ; ly = panel_px = pointY
     */
    uint16_t rawX = pointX, rawY = pointY;
    if (pointY > LCD_NOROT_HRES - 1) pointY = LCD_NOROT_HRES - 1;   /* 0..171 */
    if (pointX > LCD_NOROT_VRES - 1) pointX = LCD_NOROT_VRES - 1;   /* 0..639 */

    int ppx = pointY;                        /* 面板原生 x */
    int ppy = (LCD_NOROT_VRES - 1) - pointX; /* 面板原生 y */

    if (s_rot_dir == 0) {
      data->point.x = (int16_t)ppy;
      data->point.y = (int16_t)((EXAMPLE_LCD_V_RES - 1) - ppx);
    } else {
      data->point.x = (int16_t)((EXAMPLE_LCD_H_RES - 1) - ppy);
      data->point.y = (int16_t)ppx;
    }

#if TOUCH_DBG
    if (s_touch_evt < 40) {
      DBG("TOUCH raw=(%u,%u) -> lv=(%d,%d) dir=%d",
          rawX, rawY, (int)data->point.x, (int)data->point.y, s_rot_dir);
    }
#endif
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
}

bool lvgl_port_lock(int timeout_ms)
{
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void lvgl_port_unlock(void)
{
  assert(lvgl_mux && "bsp_display_start must be called first");
  xSemaphoreGive(lvgl_mux);
}

static void example_lcd_pwm_off_early(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)1 << EXAMPLE_PIN_NUM_BK_LIGHT);
  /* 背光低电平点亮：面板初始化完成前要先「关」背光，所以这里输出高 */
  gpio_conf.pull_down_en = EXAMPLE_BK_LIGHT_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
  gpio_conf.pull_up_en   = EXAMPLE_BK_LIGHT_ACTIVE_LOW ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_BK_LIGHT_ACTIVE_LOW ? 1 : 0));
}

static void example_lcd_exio_init(void)
{
  if (io_expander == NULL) {
    i2c_master_bus_handle_t tca9554_i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &tca9554_i2c_bus));
    ESP_ERROR_CHECK(esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander));
  }

  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander, EXAMPLE_EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
  /* 注意：必须把 NS_MODE(EXIO7) 一起声明为输出，否则这里会把 board_exio_init()
   * 设好的功放使能重新变回输入，扬声器无声。
   * SYS_EN(EXIO6) 同理：锂电池电源闩锁由 board_power_hold() 拉高，这里再保一次，
   * 防止后续任何一条初始化路径把它改回输入或拉低，导致拔 USB 就断电。 */
  ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander,
                    EXAMPLE_EXIO_PIN_BL_EN | EXAMPLE_EXIO_PIN_LCD_RST |
                    EXAMPLE_EXIO_PIN_NS_MODE | EXAMPLE_EXIO_PIN_SYS_EN,
                    IO_EXPANDER_OUTPUT));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, 0));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_NS_MODE, 1));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_SYS_EN, 1));

  /* 诊断：把 TCA9554 全部 8 个引脚的实际电平打出来
   * （esp_io_expander_print_state 内部用的是 ESP_LOGI，被编译期删掉了，用不了） */
  lvgl_port_dump_exio("exio_init");
}

/* 诊断：读回 TCA9554 引脚实际电平 + GPIO42 电平，确认背光是否真的被打开 */
void lvgl_port_dump_exio(const char *where)
{
  if (!io_expander) { DBG("[EXIO@%s] io_expander is NULL", where); return; }
  uint32_t lv = 0;
  if (esp_io_expander_get_level(io_expander, 0xFF, &lv) == ESP_OK) {
    DBG("[EXIO@%s] all=%02x | BL_EN(1)=%d LCD_RST(5)=%d SYS_EN(6)=%d NS_MODE(7)=%d | GPIO42=%d",
        where, (unsigned)(lv & 0xFF),
        (int)((lv & EXAMPLE_EXIO_PIN_BL_EN) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_LCD_RST) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_SYS_EN) ? 1 : 0),
        (int)((lv & EXAMPLE_EXIO_PIN_NS_MODE) ? 1 : 0),
        gpio_get_level(EXAMPLE_PIN_NUM_BK_LIGHT));
  } else {
    DBG("[EXIO@%s] read level failed", where);
  }
}

static void example_lcd_reset(void)
{
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 0));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
}

static void example_lcd_backlight_set(bool enable)
{
  /* 背光低电平点亮：enable=true 时 GPIO42 输出低 */
  int gpio_level = EXAMPLE_BK_LIGHT_ACTIVE_LOW ? (enable ? 0 : 1) : (enable ? 1 : 0);
  esp_err_t e1 = gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, gpio_level);
  esp_err_t e2 = esp_io_expander_set_level(io_expander, EXAMPLE_EXIO_PIN_BL_EN, enable ? 1 : 0);
  DBG("backlight %s  gpio%d=%d gpio=%s exio=%s",
      enable ? "ON" : "OFF", (int)EXAMPLE_PIN_NUM_BK_LIGHT, gpio_level,
      esp_err_to_name(e1), esp_err_to_name(e2));
}

void example_lvgl_port_task(void *arg)
{
  uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
  for(;;)
  {
    if (lvgl_port_lock(-1)) 
    {
      task_delay_ms = lv_timer_handler();
      //Release the mutex
      lvgl_port_unlock();
    }
#if TOUCH_DBG
    /* 每约 2 秒打印一次心跳；触摸计数变化时立即打印 */
    static int last_evt = -1, last_hb = 0;
    s_heartbeat++;
    if (s_touch_evt != last_evt) {
      last_evt = s_touch_evt;
      DBG("[HB] alive hb=%d touches=%d err=%d", s_heartbeat, s_touch_evt, s_touch_err);
      last_hb = s_heartbeat;
    } else if (s_heartbeat - last_hb >= 400) {
      last_hb = s_heartbeat;
      DBG("[HB] alive hb=%d touches=%d err=%d", s_heartbeat, s_touch_evt, s_touch_err);
    }
#endif
    if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS)
    {
      task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

void lvgl_port_init(void)
{
  lvgl_flush_semap = xSemaphoreCreateBinary();

  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv;      // contains callback functions
  ESP_LOGI(TAG, "Initialize LCD reset and backlight");
  example_lcd_pwm_off_early();
  example_lcd_exio_init();

  ESP_LOGI(TAG, "Initialize QSPI bus");
  spi_bus_config_t buscfg = {};
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
    buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
	  esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    
  esp_lcd_panel_io_spi_config_t io_config = {};
	io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
    io_config.dc_gpio_num = -1;          
    io_config.spi_mode = 3;              
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;    
    io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
    //io_config.user_ctx = &disp_drv,         
    io_config.lcd_cmd_bits = 32;         
    io_config.lcd_param_bits = 8;        
    io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
  axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
  esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

  ESP_LOGI(TAG, "Install panel driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

  example_lcd_reset();
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  example_lcd_backlight_set(true);

  lv_init();

  lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN , MALLOC_CAP_DMA);
  assert(lvgl_dma_buf);
  lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
  lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN , MALLOC_CAP_SPIRAM);
  assert(buffer_1);
  assert(buffer_2);
  lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES);

  ESP_LOGI(TAG, "Register display driver to LVGL");
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 1;          //full_refresh must be 1
  disp_drv.user_data = panel;
  lv_disp_drv_register(&disp_drv);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

  static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  /* 长按判定 800ms（默认 400ms 太短）。歌词页没有返回按钮，
   * 靠「长按任意位置」回播放器页，时间拉长一点免得误触。 */
  indev_drv.long_press_time = 800;
  indev_drv.read_cb = example_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  lvgl_mux = xSemaphoreCreateMutex();
  assert(lvgl_mux);
  /* 栈从 4000 提升到 8192：触摸会在此任务内跑事件分发+点击回调+重绘，
     4000 字节偏小，容易在点击时栈溢出 panic */
  xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 8192, NULL, 4, NULL,0); //运行于内核_0
  if (lvgl_port_lock(-1))
  {
    music_ui_init();
    lvgl_port_unlock();
  }
}

/* ------------------------------------------------------------------ */
/* HTTP：运行时切换横屏朝向（省得为了试朝向反复烧录）                    */
/*   POST /api/rot?dir=0|1                                             */
/* ------------------------------------------------------------------ */

esp_err_t lvgl_port_h_rot(httpd_req_t *req)
{
  char q[64] = {0};
  size_t l = httpd_req_get_url_query_len(req) + 1;
  if (l > 1 && l <= sizeof(q) && httpd_req_get_url_query_str(req, q, l) == ESP_OK) {
    const char *p = strstr(q, "dir=");
    if (p) {
      int d = atoi(p + 4);
      if (d == 0 || d == 1) {
        lvgl_port_set_rot_dir(d);
        printf("[ROT] dir -> %d\n", d);
      }
    }
  }

  char body[64];
  snprintf(body, sizeof(body), "{\"ok\":1,\"dir\":%d}", lvgl_port_rot_dir());
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, strlen(body));
}
