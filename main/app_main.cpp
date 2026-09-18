#include "audio_i2s.h"
#include "headless_test.h"
#include "i2s_bringup.h"
#include "voxlink_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocal_fx.h"

extern "C" void app_main(void) {
#if CONFIG_VOXP4_HEADLESS_HW_TEST
  run_headless_hw_test();
#else
  // No-op unless CONFIG_VOXLINK_ENABLE_SERVER is set. The control task runs on
  // Core 1 and cannot affect the qualified realtime audio engine.
  voxlink_service_start();
  vTaskDelay(pdMS_TO_TICKS(2000));
  run_i2s_bringup_selected_mode();
#endif
}
