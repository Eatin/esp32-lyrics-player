#include "sdcard_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define sdcard_write_Test


static void gpio_init(void)
{
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = 0x1ULL << GPIO_NUM_8;
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

  gpio_set_level(GPIO_NUM_8,true);
}

void setup()
{
  gpio_init();
  Serial.begin(115200);
  delay(3000);
  sdcard_init();
}

uint32_t value = 1;
char test[45] = {""};
char rtest[45] = {""};
void loop()
{
#ifdef sdcard_write_Test
  snprintf(test,45,"sdcard_writeTest : %ld\n",value);
  s_example_write_file("/sdcard/writeTest.txt",test);
  delay(500);
  s_example_read_file("/sdcard/writeTest.txt",rtest,NULL);
  Serial.printf("rtest:%s\n",rtest);
  delay(500);
  value++;
#endif
}
