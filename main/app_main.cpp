#include "audio_i2s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocal_fx.h"

namespace {
vocal_fx_platform::AudioI2s audio;
void audio_task(void *) { audio.run(); }
void telemetry_task(void *) {
  for (;;) {
    const auto s = vocal_fx_profile_stats(VocalFxProfileSection::Pipeline);
    const double average =
        s.blocks ? static_cast<double>(s.total_us) / s.blocks : 0.0;
    ESP_LOGI("vocal_fx", "blocks=%llu avg=%.1f us worst=%llu us misses=%llu",
             static_cast<unsigned long long>(s.blocks), average,
             static_cast<unsigned long long>(s.worst_us),
             static_cast<unsigned long long>(s.deadline_misses));
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
} // namespace

extern "C" void app_main(void) {
  VocalFxConfig cfg{};
  if (!vocal_fx_init(cfg)) {
    ESP_LOGE("vocal_fx", "engine initialization failed");
    return;
  }
  vocal_fx_platform::AudioI2sConfig io{};
  ESP_LOGI("vocal_fx", "DSP buffers=%u bytes, internal heap=%u, PSRAM=%u",
           static_cast<unsigned>(vocal_fx_dsp_memory_bytes()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (audio.init(io, cfg.block_size)) {
    xTaskCreatePinnedToCore(audio_task, "vocal_audio", 8192, nullptr,
                            configMAX_PRIORITIES - 2, nullptr, 0);
    xTaskCreatePinnedToCore(telemetry_task, "vocal_telemetry", 3072, nullptr, 2,
                            nullptr, 1);
  } else {
    ESP_LOGE("vocal_fx", "configure board I2S pins before starting audio");
  }
}
