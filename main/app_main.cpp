#include "audio_i2s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vocal_fx.h"

namespace {
vocal_fx_platform::AudioI2s audio;
void audio_task(void *) { audio.run(); }
void pitch_task(void *) {
  for (;;) {
    if (vocal_fx_run_pitch_analysis(4))
      taskYIELD();
    else
      vTaskDelay(1);
  }
}
void telemetry_task(void *) {
  for (;;) {
    const auto s = vocal_fx_profile_stats(VocalFxProfileSection::Pipeline);
    const double average =
        s.blocks ? static_cast<double>(s.total_us) / s.blocks : 0.0;
    ESP_LOGI("vocal_fx", "blocks=%llu avg=%.1f us worst=%llu us misses=%llu",
             static_cast<unsigned long long>(s.blocks), average,
             static_cast<unsigned long long>(s.worst_us),
             static_cast<unsigned long long>(s.deadline_misses));
#if CONFIG_VOCAL_FX_PITCH_DEBUG
    const auto pitch = vocal_fx_latest_pitch();
    ESP_LOGI("vocal_fx_pitch", "f0=%.2f confidence=%.3f voiced=%d state=%u",
             pitch.frequency_hz, pitch.confidence, pitch.voiced,
             static_cast<unsigned>(vocal_fx_pitch_track_state()));
#endif
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
    TaskHandle_t audio_task_handle = nullptr;
    if (xTaskCreatePinnedToCore(audio_task, "vocal_audio", 8192, nullptr,
                                configMAX_PRIORITIES - 2, &audio_task_handle,
                                0) != pdPASS) {
      ESP_LOGE("vocal_fx", "failed to create audio task");
      return;
    }
    if (cfg.enable_pitch_analysis &&
        xTaskCreatePinnedToCore(pitch_task, "vocal_pitch", 8192, nullptr,
                                configMAX_PRIORITIES - 5, nullptr,
                                1) != pdPASS) {
      vTaskDelete(audio_task_handle);
      ESP_LOGE("vocal_fx", "failed to create pitch analysis task");
      return;
    }
    if (xTaskCreatePinnedToCore(telemetry_task, "vocal_telemetry", 3072,
                                nullptr, 2, nullptr, 1) != pdPASS)
      ESP_LOGW("vocal_fx", "audio started without telemetry task");
  } else {
    ESP_LOGE("vocal_fx", "configure board I2S pins before starting audio");
  }
}
