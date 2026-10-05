#include "user_config.h"
#include "lvgl_port.h"
#include "esp_err.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "i2c_bsp.h"
#include "src/ui_src/generated/gui_guider.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"

esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;
esp_io_expander_handle_t io_expander = NULL;
static EventGroupHandle_t lvgl_button_groups;
static uint8_t *audio_ptr = NULL;

void tca9554_init(void)
{
  i2c_master_bus_handle_t tca9554_i2c_bus_ = NULL;
  ESP_ERROR_CHECK(i2c_master_get_bus_handle(0,&tca9554_i2c_bus_));
  esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
	esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7, 1);
}

void audio_init(void)
{
  set_codec_board_type("S3_LCD_3_49");
  codec_init_cfg_t codec_cfg = 
  {
    .in_mode = CODEC_I2S_MODE_TDM,
    .out_mode = CODEC_I2S_MODE_TDM,
    .in_use_tdm = false,
    .reuse_dev = false,
  };
  ESP_ERROR_CHECK(init_codec(&codec_cfg));
  playback = get_playback_handle();
  record = get_record_handle();
}

void audio_play_init(void)
{
	esp_codec_dev_set_out_vol(playback, 100.0); //设置100声音大小
  esp_codec_dev_set_in_gain(record, 35.0);   //设置录音时的增益
  esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate = 24000;
    fs.channel = 2;
    fs.bits_per_sample = 16;
  esp_codec_dev_open(playback, &fs); //打开播放
  esp_codec_dev_open(record, &fs);   //打开录音
}

int audio_playback_read(void *data_ptr,uint32_t len)
{
  int err = esp_codec_dev_read(record, data_ptr, len);
	return err;
}

int audio_playback_write(void *data_ptr,uint32_t len)
{
	int err = esp_codec_dev_write(playback, data_ptr, len);
  return err;
}

void audio_playback_set_vol(uint8_t vol)
{
  esp_codec_dev_set_out_vol(playback, vol);   //设置声音大小
}

void i2s_audio_Test(void *arg)
{
	lv_ui *ui = (lv_ui *)arg;

	for(;;)
	{
		EventBits_t even = xEventGroupWaitBits(lvgl_button_groups,0x00ffffff,pdTRUE,pdFALSE,pdMS_TO_TICKS(8 * 1000));
		if(even & 0x01)
		{
			lv_label_set_text(ui->screen_label_2, "正在录音");
			lv_label_set_text(ui->screen_label_3, "Recording...");

			audio_playback_read(audio_ptr,192 * 1000);
			lv_label_set_text(ui->screen_label_2, "录音完成");
			lv_label_set_text(ui->screen_label_3, "Rec Done");
		}
		else if(even & 0x02)
		{
			lv_label_set_text(ui->screen_label_2, "正在播放");
			lv_label_set_text(ui->screen_label_3, "Playing...");
			audio_playback_write(audio_ptr,192 * 1000);
			lv_label_set_text(ui->screen_label_2, "播放完成");
			lv_label_set_text(ui->screen_label_3, "Play Done");
		}
		else
		{
			lv_label_set_text(ui->screen_label_2, "等待操作");
			lv_label_set_text(ui->screen_label_3, "Idle");
		}
	}
}

static void audio_Test_Callback(lv_event_t *e)
{
	assert(audio_ptr);
	lv_event_code_t code = lv_event_get_code(e);
  lv_ui *ui = (lv_ui *)e->user_data;
  lv_obj_t * module = e->current_target;
  switch (code)
  {
    case LV_EVENT_CLICKED:
    {
      if(module == ui->screen_btn_1)
      {
		    xEventGroupSetBits(lvgl_button_groups,0x01);
      }
	    if(module == ui->screen_btn_2)
	    {
		    xEventGroupSetBits(lvgl_button_groups,0x02);
	    }
      break;
    }
    default:
      break;
  }
}

void setup()
{
  lvgl_button_groups = xEventGroupCreate();
  audio_ptr = (uint8_t *)heap_caps_malloc(288 * 1000 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
  i2c_master_Init();
  tca9554_init();
  Serial.begin(115200);
  lvgl_port_init();
  lv_label_set_text(src_ui.screen_label_2, "等待操作");
	lv_label_set_text(src_ui.screen_label_3, "Idle");
  audio_init();
  audio_play_init();

  xTaskCreatePinnedToCore(i2s_audio_Test, "i2s_audio_Test", 4 * 1024, &src_ui, 3, NULL,1);                           //audio    

  /*even add*/
  lv_obj_add_event_cb(src_ui.screen_btn_1, audio_Test_Callback, LV_EVENT_ALL, &src_ui); 
	lv_obj_add_event_cb(src_ui.screen_btn_2, audio_Test_Callback, LV_EVENT_ALL, &src_ui);
}

void loop()
{

}