#include "audio_i2s.h"
#include "headless_test.h"
#include "i2s_bringup.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocal_fx.h"

extern "C" void app_main(void) {
#if CONFIG_VOXP4_HEADLESS_HW_TEST
  run_headless_hw_test();
#else
  vTaskDelay(pdMS_TO_TICKS(2000));
  run_i2s_bringup_selected_mode();
#endif
}
