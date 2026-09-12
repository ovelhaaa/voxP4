#include "i2s_bringup.h"
#include "vocal_fx.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace {

const char *TAG = "i2s_bringup";
vocal_fx_platform::AudioI2s s_audio;
TaskHandle_t s_audio_task_handle = nullptr;
TaskHandle_t s_pitch_task_handle = nullptr;
std::atomic<bool> s_pitch_running{false};
constexpr EventBits_t kAudioTaskStopped = BIT0;
constexpr EventBits_t kPitchTaskStopped = BIT1;
StaticEventGroup_t s_task_events_storage{};
EventGroupHandle_t s_task_events = nullptr;
bool s_teardown_diagnostics_enabled = false;
std::atomic<bool> s_pitch_lock_audit_enabled{false};
std::atomic<bool> s_pitch_hotspot_audit_enabled{false};
std::atomic<bool> s_pitch_worker_in_call{false};

struct PitchWorkerCallRecord {
  uint64_t start_us = 0;
  uint32_t duration_us = 0;
  uint32_t cycles = 0;
  uint8_t hops = 0;
};
// B4B.2 measured only ~85 worker calls in a 10-second window. Keep generous
// fixed headroom without consuming the internal RAM needed by the real-time
// task stacks. Any exhaustion remains explicit in record_drops.
constexpr size_t kPitchWorkerCallRecordCapacity = 512;
std::array<PitchWorkerCallRecord, kPitchWorkerCallRecordCapacity>
    s_pitch_worker_call_records{};
std::array<uint32_t, kPitchWorkerCallRecordCapacity> s_b4b3_call_us_sort{};
std::array<uint32_t, kPitchWorkerCallRecordCapacity> s_b4b3_hop_us_sort{};
volatile float s_b4b4a_storage_checksum = 0.0f;
std::atomic<uint32_t> s_pitch_worker_call_record_count{0};
std::atomic<uint32_t> s_pitch_worker_call_record_drops{0};

struct PitchWorkerCounters {
  std::atomic<uint64_t> iterations{0};
  std::atomic<uint64_t> wakeups{0};
  std::atomic<uint64_t> yields{0};
  std::atomic<uint64_t> run_calls{0};
  std::atomic<uint64_t> hops_requested{0};
  std::atomic<uint64_t> hops_processed{0};
  std::atomic<uint64_t> zero_hop_calls{0};
  std::atomic<uint64_t> one_hop_calls{0};
  std::atomic<uint64_t> multi_hop_calls{0};
  std::atomic<uint64_t> maximum_hops_per_call{0};
  std::atomic<int64_t> cents_error_sum_milli{0};
  std::atomic<uint64_t> cents_error_abs_sum_milli{0};
  std::atomic<uint64_t> cents_error_samples{0};
  std::atomic<uint64_t> cents_error_max_abs_milli{0};
  std::atomic<uint64_t> detected_f0_sum_millihz{0};

  void reset() {
    iterations.store(0, std::memory_order_relaxed);
    wakeups.store(0, std::memory_order_relaxed);
    yields.store(0, std::memory_order_relaxed);
    run_calls.store(0, std::memory_order_relaxed);
    hops_requested.store(0, std::memory_order_relaxed);
    hops_processed.store(0, std::memory_order_relaxed);
    zero_hop_calls.store(0, std::memory_order_relaxed);
    one_hop_calls.store(0, std::memory_order_relaxed);
    multi_hop_calls.store(0, std::memory_order_relaxed);
    maximum_hops_per_call.store(0, std::memory_order_relaxed);
    cents_error_sum_milli.store(0, std::memory_order_relaxed);
    cents_error_abs_sum_milli.store(0, std::memory_order_relaxed);
    cents_error_samples.store(0, std::memory_order_relaxed);
    cents_error_max_abs_milli.store(0, std::memory_order_relaxed);
    detected_f0_sum_millihz.store(0, std::memory_order_relaxed);
  }
} s_pitch_worker;

void audio_task_entry(void *) {
  s_audio.run();
  if (s_task_events)
    xEventGroupSetBits(s_task_events, kAudioTaskStopped);
  vTaskDelete(nullptr);
}

void pitch_worker_task(void *) {
  while (s_pitch_running.load(std::memory_order_relaxed)) {
    s_pitch_worker.wakeups.fetch_add(1, std::memory_order_relaxed);
    s_pitch_worker.iterations.fetch_add(1, std::memory_order_relaxed);
    constexpr size_t kMaximumHops = 4;
    s_pitch_worker.run_calls.fetch_add(1, std::memory_order_relaxed);
    s_pitch_worker.hops_requested.fetch_add(kMaximumHops,
                                            std::memory_order_relaxed);
    const bool record_hotspot =
        s_pitch_hotspot_audit_enabled.load(std::memory_order_acquire);
    const uint64_t call_start_us = record_hotspot ? esp_timer_get_time() : 0;
    const uint32_t call_start_cycles =
        record_hotspot ? esp_cpu_get_cycle_count() : 0;
    s_pitch_worker_in_call.store(true, std::memory_order_release);
    s_audio.set_pitch_worker_active(true);
    const size_t processed = vocal_fx_run_pitch_analysis(kMaximumHops);
    s_audio.set_pitch_worker_active(false);
    s_pitch_worker_in_call.store(false, std::memory_order_release);
    if (record_hotspot) {
      const uint32_t call_cycles = static_cast<uint32_t>(
          esp_cpu_get_cycle_count() - call_start_cycles);
      const uint32_t call_duration_us = static_cast<uint32_t>(
          esp_timer_get_time() - call_start_us);
      const uint32_t index = s_pitch_worker_call_record_count.fetch_add(
          1, std::memory_order_relaxed);
      if (index < kPitchWorkerCallRecordCapacity) {
        s_pitch_worker_call_records[index] = {
            call_start_us, call_duration_us, call_cycles,
            static_cast<uint8_t>(processed)};
      } else {
        s_pitch_worker_call_record_drops.fetch_add(1,
                                                   std::memory_order_relaxed);
      }
    }
    s_pitch_worker.hops_processed.fetch_add(processed,
                                            std::memory_order_relaxed);
    if (processed != 0 &&
        s_pitch_lock_audit_enabled.load(std::memory_order_relaxed)) {
      PitchResult latest{};
      if (vocal_fx_try_latest_pitch(&latest) && latest.voiced &&
          latest.frequency_hz > 0.0f && std::isfinite(latest.frequency_hz)) {
        const double cents = 1200.0 * std::log2(latest.frequency_hz / 220.0);
        const int64_t cents_milli =
            static_cast<int64_t>(std::llround(cents * 1000.0));
        const uint64_t absolute_milli = static_cast<uint64_t>(
            cents_milli < 0 ? -cents_milli : cents_milli);
        s_pitch_worker.cents_error_sum_milli.fetch_add(
            cents_milli, std::memory_order_relaxed);
        s_pitch_worker.cents_error_abs_sum_milli.fetch_add(
            absolute_milli, std::memory_order_relaxed);
        s_pitch_worker.cents_error_samples.fetch_add(1,
                                                      std::memory_order_relaxed);
        s_pitch_worker.detected_f0_sum_millihz.fetch_add(
            static_cast<uint64_t>(std::llround(latest.frequency_hz * 1000.0)),
            std::memory_order_relaxed);
        uint64_t max_error = s_pitch_worker.cents_error_max_abs_milli.load(
            std::memory_order_relaxed);
        while (absolute_milli > max_error &&
               !s_pitch_worker.cents_error_max_abs_milli.compare_exchange_weak(
                   max_error, absolute_milli, std::memory_order_relaxed)) {
        }
      }
    }
    if (processed == 0)
      s_pitch_worker.zero_hop_calls.fetch_add(1, std::memory_order_relaxed);
    else if (processed == 1)
      s_pitch_worker.one_hop_calls.fetch_add(1, std::memory_order_relaxed);
    else
      s_pitch_worker.multi_hop_calls.fetch_add(1, std::memory_order_relaxed);
    uint64_t maximum =
        s_pitch_worker.maximum_hops_per_call.load(std::memory_order_relaxed);
    while (processed > maximum &&
           !s_pitch_worker.maximum_hops_per_call.compare_exchange_weak(
               maximum, processed, std::memory_order_relaxed)) {
    }
    s_pitch_worker.yields.fetch_add(1, std::memory_order_relaxed);
    vTaskDelay(pdMS_TO_TICKS(1)); // Allow IDLE1 to run and feed watchdog
  }
  s_pitch_worker_in_call.store(false, std::memory_order_release);
  s_audio.set_pitch_worker_active(false);
  if (s_task_events)
    xEventGroupSetBits(s_task_events, kPitchTaskStopped);
  vTaskDelete(nullptr);
}

void b4b2_coordinator_task(void *) {
  run_i2s_stage_b4b2_pitch_worker_lock_audit();
  vTaskDelete(nullptr);
}

void b4b3_coordinator_task(void *) {
  // Let app_main return so its task stack is reclaimed before the two
  // real-time pipeline task stacks are allocated. This delay is outside the
  // measured audit window and does not change any pipeline scheduling.
  vTaskDelay(pdMS_TO_TICKS(250));
  run_i2s_stage_b4b3_pitch_analysis_hotspot_audit();
  vTaskDelete(nullptr);
}

bool start_pipeline_tasks(vocal_fx_platform::AudioI2sMode mode, bool start_pitch = true) {
  if (!s_task_events)
    s_task_events = xEventGroupCreateStatic(&s_task_events_storage);
  xEventGroupClearBits(s_task_events, kAudioTaskStopped | kPitchTaskStopped);
  s_pitch_worker.reset();
  vocal_fx_platform::AudioI2sConfig io{};
  io.mode = mode;
  io.dma_desc_num = 6;
  io.dma_frame_num = 64;

  if (!s_audio.init(io, 64)) {
    ESP_LOGE(TAG, "Audio I2S driver initialization failed");
    return false;
  }

  s_audio.print_hardware_config();

  // Core 0: Audio task + DSP (highest real-time priority)
  if (xTaskCreatePinnedToCore(audio_task_entry, "vocal_audio", 32768, nullptr,
                              configMAX_PRIORITIES - 2, &s_audio_task_handle, 0) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio task on Core 0");
    s_audio.deinit();
    return false;
  }

  // Core 1: Pitch Worker task
  if (start_pitch) {
    s_pitch_running.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(pitch_worker_task, "vocal_pitch", 16384, nullptr,
                                configMAX_PRIORITIES - 5, &s_pitch_task_handle, 1) != pdPASS) {
      ESP_LOGE(TAG, "Failed to create pitch task on Core 1");
      s_pitch_running.store(false, std::memory_order_release);
      s_audio.stop();
      (void)xEventGroupWaitBits(s_task_events, kAudioTaskStopped, pdFALSE,
                                pdTRUE, pdMS_TO_TICKS(1000));
      s_audio.deinit();
      s_audio_task_handle = nullptr;
      return false;
    }
  } else {
    s_pitch_task_handle = nullptr;
    xEventGroupSetBits(s_task_events, kPitchTaskStopped);
  }

  return true;
}

bool stop_pipeline_tasks() {
  if (s_teardown_diagnostics_enabled)
    printf("[B4B.2 TEARDOWN] test stop requested\n");
  s_pitch_running.store(false, std::memory_order_release);
  if (s_teardown_diagnostics_enabled)
    printf("[B4B.2 TEARDOWN] pitch task stop requested\n");
  s_audio.stop();
  if (s_teardown_diagnostics_enabled)
    printf("[B4B.2 TEARDOWN] audio task stop requested\n");
  const EventBits_t stopped = xEventGroupWaitBits(
      s_task_events, kAudioTaskStopped | kPitchTaskStopped, pdFALSE, pdTRUE,
      pdMS_TO_TICKS(1000));
  if (s_teardown_diagnostics_enabled) {
    printf("[B4B.2 TEARDOWN] audio task confirmed stopped: %s\n",
           (stopped & kAudioTaskStopped) ? "yes" : "TIMEOUT");
    printf("[B4B.2 TEARDOWN] pitch task confirmed stopped: %s\n",
           (stopped & kPitchTaskStopped) ? "yes" : "TIMEOUT");
  }
  if ((stopped & (kAudioTaskStopped | kPitchTaskStopped)) !=
      (kAudioTaskStopped | kPitchTaskStopped)) {
    ESP_LOGE(TAG, "Refusing I2S teardown while pipeline tasks are live");
    return false;
  }
  s_audio.deinit();
  if (s_teardown_diagnostics_enabled) {
    printf("[B4B.2 TEARDOWN] I2S callbacks disabled\n");
    printf("[B4B.2 TEARDOWN] RX channel disabled\n");
    printf("[B4B.2 TEARDOWN] TX channel disabled\n");
    printf("[B4B.2 TEARDOWN] driver queues deleted\n");
    printf("[B4B.2 TEARDOWN] DMA buffers released\n");
  }
  s_audio_task_handle = nullptr;
  s_pitch_task_handle = nullptr;
  return true;
}

} // namespace

void run_i2s_stage_b1_dac_bringup(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B1: PCM5102 DAC-ONLY BRING-UP START (5 MIN)   \n");
  printf("=======================================================\n");
  printf("Hardware: Wireless-Tag WT9932P4-TINY -> PCM5102 DAC\n");
  printf("ADC: Disconnected / Muted\n");
  printf("Fs: 48,000 Hz, BCLK: 3.072 MHz, LRCK: 48 kHz\n");

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::TxBringUp, false)) {
    printf("STAGE B1: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  struct Phase {
    const char *name;
    vocal_fx_platform::TxSignalType signal;
    float level_dbfs;
    uint32_t duration_sec;
  };

  Phase phases[] = {
      {"1 kHz Sine (-18 dBFS conservative start)", vocal_fx_platform::TxSignalType::Sine1kHz, -18.0f, 30},
      {"1 kHz Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::Sine1kHz, -12.0f, 30},
      {"1 kHz Sine (-6 dBFS)", vocal_fx_platform::TxSignalType::Sine1kHz, -6.0f, 30},
      {"100 Hz Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::Sine100Hz, -12.0f, 30},
      {"10 kHz Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::Sine10kHz, -12.0f, 30},
      {"Left-Only 1 kHz Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::LeftOnly1kHz, -12.0f, 30},
      {"Right-Only 1 kHz Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::RightOnly1kHz, -12.0f, 30},
      {"Alternating L/R Sine (-12 dBFS)", vocal_fx_platform::TxSignalType::AlternatingLR, -12.0f, 30},
      {"Channel Integrity (L=1kHz, R=2kHz) (-12 dBFS)", vocal_fx_platform::TxSignalType::ChannelIntegrity, -12.0f, 30},
      {"Digital Silence (Zero / Noise Floor Test)", vocal_fx_platform::TxSignalType::Silence, -96.0f, 30},
  };

  for (const auto &ph : phases) {
    printf("\n>>> [B1 Phase] %s for %lu s\n", ph.name, static_cast<unsigned long>(ph.duration_sec));
    s_audio.set_tx_signal(ph.signal, ph.level_dbfs);
    vTaskDelay(pdMS_TO_TICKS(ph.duration_sec * 1000));
  }

  stop_pipeline_tasks();

  const auto &c = s_audio.counters();
  auto timing = s_audio.calculate_cycle_percentiles();
  printf("\n=======================================================\n");
  printf("   STAGE B1: PCM5102 DAC-ONLY BRING-UP COMPLETE         \n");
  printf("=======================================================\n");
  printf("Total blocks:    %llu\n", static_cast<unsigned long long>(c.audio_blocks_processed.load()));
  printf("TX DMA events:   %llu\n", static_cast<unsigned long long>(c.tx_dma_events.load()));
  printf("TX underruns:    %llu\n", static_cast<unsigned long long>(c.tx_underruns.load()));
  printf("TX dropped frms: %llu\n", static_cast<unsigned long long>(c.tx_dropped_frames.load()));
  printf("Deadline misses: %llu\n", static_cast<unsigned long long>(c.audio_deadline_misses.load()));
  printf("Max TX gap:      %llu us\n", static_cast<unsigned long long>(c.max_tx_callback_gap_us.load()));
  printf("Cycle Avg / P99: %.1f us / %llu us (Max: %llu us)\n", timing.avg_us,
         static_cast<unsigned long long>(timing.p99_us),
         static_cast<unsigned long long>(timing.max_us));
  printf("Result: %s\n", (c.tx_underruns.load() == 0) ? "PASS" : "WARNING (UNDERRUNS DETECTED)");
  printf("=======================================================\n\n");
}

void run_i2s_stage_b2_adc_bringup(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B2: PCM1808 ADC-ONLY BRING-UP START (2 MIN)   \n");
  printf("=======================================================\n");
  printf("Hardware: Analog In -> PCM1808 ADC -> ESP32-P4\n");
  printf("DAC: Idle / Muted\n");

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::RxBringUp, false)) {
    printf("STAGE B2: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  for (int sec = 0; sec < 120; sec += 10) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    const auto &d = s_audio.adc_diagnostics();
    const auto &c = s_audio.counters();
    printf("[B2 %03d s] Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | DC L/R: %+.5f / %+.5f | Clipped: %llu | Zeros: %llu | RX Overruns: %llu\n",
           sec + 10, d.peak_l, d.peak_r, d.rms_l, d.rms_r, d.dc_offset_l, d.dc_offset_r,
           static_cast<unsigned long long>(d.clipped_samples),
           static_cast<unsigned long long>(d.zero_samples),
           static_cast<unsigned long long>(c.rx_overruns.load()));
  }

  stop_pipeline_tasks();

  printf("\n=======================================================\n");
  printf("   STAGE B2: PCM1808 ADC-ONLY BRING-UP COMPLETE         \n");
  printf("=======================================================\n");
  s_audio.dump_sample_forensic();
}

void run_i2s_stage_b3_bypass_stress(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B3: HARDWARE BYPASS STRESS TEST START (10 MIN)\n");
  printf("=======================================================\n");
  printf("Pipeline: PCM1808 -> I2S RX -> PCM24->Float -> Float->PCM32 -> I2S TX -> PCM5102\n");
  printf("DSP Engine: BYPASSED (Transparency validation)\n");

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::Bypass, false)) {
    printf("STAGE B3: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  // 10 minutes = 600 seconds
  for (int sec = 0; sec < 600; sec += 10) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }

  stop_pipeline_tasks();

  const auto &c = s_audio.counters();
  auto timing = s_audio.calculate_cycle_percentiles();
  printf("\n=======================================================\n");
  printf("   STAGE B3: HARDWARE BYPASS STRESS TEST COMPLETE       \n");
  printf("=======================================================\n");
  printf("Blocks processed: %llu (target: 450,000 blocks)\n", static_cast<unsigned long long>(c.audio_blocks_processed.load()));
  printf("RX overruns:      %llu\n", static_cast<unsigned long long>(c.rx_overruns.load()));
  printf("TX underruns:     %llu\n", static_cast<unsigned long long>(c.tx_underruns.load()));
  printf("DMA errors:       %llu\n", static_cast<unsigned long long>(c.dma_errors.load()));
  printf("Deadline misses:  %llu\n", static_cast<unsigned long long>(c.audio_deadline_misses.load()));
  printf("Max wake latency: %llu us\n", static_cast<unsigned long long>(c.max_audio_task_wake_latency_us.load()));
  printf("Max RX gap:       %llu us\n", static_cast<unsigned long long>(c.max_rx_callback_gap_us.load()));
  printf("Cycle Avg / P99:  %.1f us / %llu us (Max: %llu us)\n", timing.avg_us,
         static_cast<unsigned long long>(timing.p99_us),
         static_cast<unsigned long long>(timing.max_us));
  printf("Result: %s\n", (c.rx_overruns.load() == 0 && c.tx_underruns.load() == 0) ? "PASS" : "WARNING");
  printf("=======================================================\n\n");
}

void run_i2s_stage_b4a_quiescent(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B4A: QUIESCENT FULL DSP STRESS TEST (10 MIN)   \n");
  printf("=======================================================\n");
  printf("INPUT CONDITION: PCM1808 floating wire / open analog input\n");
  printf("CLASSIFICATION: FLOATING_INPUT\n");
  printf("NOTE: Measured floor reflects open input pickup, not shorted termination; invalid as reference ADC noise floor.\n");
  printf("Pipeline: PCM1808 (Quiescent Analog) -> I2S RX -> Float -> Full VoxP4 DSP -> Float -> I2S TX -> PCM5102\n");
  printf("DSP Modules: Gate (nominal) + Comp + 2 Voices Harmony (+4st, +7st) + Delay + Reverb + Limiter\n");
  printf("Planned Duration: 600 seconds (450,000 blocks @ 48 kHz / 64 frames)\n");

  VocalFxConfig cfg{};
  if (!vocal_fx_init(cfg)) {
    ESP_LOGE(TAG, "Vocal FX engine initialization failed");
    return;
  }

  // Nominal Full Chain Parameters
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 1.0f);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f); // Major third (+4 st)
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f); // Fifth (+7 st)
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 2.5f);

  uint32_t initial_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t initial_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::FullDsp, true)) {
    printf("STAGE B4A: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  // 10 minutes = 600 seconds
  for (int sec = 0; sec < 600; sec += 10) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    const auto &c = s_audio.counters();
    auto dp = s_audio.calculate_dsp_percentiles();
    auto cp = s_audio.calculate_cycle_percentiles();
    auto wp = s_audio.calculate_wake_percentiles();
    printf("[B4A %03d s] Blks: %llu | DSP Avg: %.1f us (P99: %llu, Max: %llu) | Cyc: %.1f us (Max: %llu) | Wake: %.1f us | Miss: %llu | Und: %llu | Ovf: %llu | Pk L/R: %.4f / %.4f | NaN/Inf: %llu\n",
           sec + 10,
           static_cast<unsigned long long>(c.audio_blocks_processed.load(std::memory_order_relaxed)),
           dp.avg_us,
           static_cast<unsigned long long>(dp.p99_us),
           static_cast<unsigned long long>(dp.max_us),
           cp.avg_us,
           static_cast<unsigned long long>(cp.max_us),
           wp.avg_us,
           static_cast<unsigned long long>(c.audio_deadline_misses.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(c.tx_underruns.load(std::memory_order_relaxed)),
           static_cast<unsigned long long>(c.rx_overruns.load(std::memory_order_relaxed)),
           s_audio.output_peak_l(),
           s_audio.output_peak_r(),
           static_cast<unsigned long long>(c.nan_inf_count.load(std::memory_order_relaxed)));
  }

  uint32_t final_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t final_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  UBaseType_t audio_hwm = uxTaskGetStackHighWaterMark(s_audio_task_handle);
  UBaseType_t pitch_hwm = uxTaskGetStackHighWaterMark(s_pitch_task_handle);

  const auto &c = s_audio.counters();
  auto dsp_timing = s_audio.calculate_dsp_percentiles();
  auto cycle_timing = s_audio.calculate_cycle_percentiles();
  auto wake_timing = s_audio.calculate_wake_percentiles();

  stop_pipeline_tasks();

  printf("\n=======================================================\n");
  printf("   STAGE B4A: QUIESCENT FULL DSP STRESS TEST COMPLETE   \n");
  printf("=======================================================\n");
  printf("INPUT CONDITION: PCM1808 floating wire / open analog input\n");
  printf("CLASSIFICATION: FLOATING_INPUT\n");
  printf("NOTE: Measured floor reflects open input pickup, not shorted termination; invalid as reference ADC noise floor.\n");
  printf("Total Blocks:              %llu\n", static_cast<unsigned long long>(c.audio_blocks_processed.load()));
  printf("Steady-State RX Overruns:  %llu\n", static_cast<unsigned long long>(c.rx_overruns.load()));
  printf("Steady-State TX Underruns: %llu\n", static_cast<unsigned long long>(c.tx_underruns.load()));
  printf("DMA Hardware Errors:       %llu\n", static_cast<unsigned long long>(c.dma_errors.load()));
  printf("Audio Deadline Misses:     %llu\n", static_cast<unsigned long long>(c.audio_deadline_misses.load()));
  printf("DSP Deadline Misses:       %llu\n", static_cast<unsigned long long>(c.dsp_deadline_misses.load()));
  printf("Transport Misses:          %llu\n", static_cast<unsigned long long>(c.transport_deadline_misses.load()));
  printf("Max Audio Wake Latency:    %llu us\n", static_cast<unsigned long long>(c.max_audio_task_wake_latency_us.load()));
  printf("Max RX Callback Gap:       %llu us\n", static_cast<unsigned long long>(c.max_rx_callback_gap_us.load()));
  printf("Max TX Callback Gap:       %llu us\n", static_cast<unsigned long long>(c.max_tx_callback_gap_us.load()));
  printf("Pure DSP Timing (us):      Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         dsp_timing.avg_us,
         static_cast<unsigned long long>(dsp_timing.p95_us),
         static_cast<unsigned long long>(dsp_timing.p99_us),
         static_cast<unsigned long long>(dsp_timing.p99_9_us),
         static_cast<unsigned long long>(dsp_timing.max_us));
  printf("Block Cycle Timing (us):   Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         cycle_timing.avg_us,
         static_cast<unsigned long long>(cycle_timing.p95_us),
         static_cast<unsigned long long>(cycle_timing.p99_us),
         static_cast<unsigned long long>(cycle_timing.p99_9_us),
         static_cast<unsigned long long>(cycle_timing.max_us));
  printf("Wake Latency Timing (us):  Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         wake_timing.avg_us,
         static_cast<unsigned long long>(wake_timing.p95_us),
         static_cast<unsigned long long>(wake_timing.p99_us),
         static_cast<unsigned long long>(wake_timing.p99_9_us),
         static_cast<unsigned long long>(wake_timing.max_us));
  printf("Memory Delta:              Internal: %ld bytes | PSRAM: %ld bytes\n",
         static_cast<long>(final_internal) - static_cast<long>(initial_internal),
         static_cast<long>(final_spiram) - static_cast<long>(initial_spiram));
  printf("Stack High Water Mark:     Audio Task: %u bytes | Pitch Task: %u bytes\n",
         static_cast<unsigned>(audio_hwm * sizeof(StackType_t)),
         static_cast<unsigned>(pitch_hwm * sizeof(StackType_t)));
  printf("Output Sanity:             Peak L=%.4f | Peak R=%.4f | NaN/Inf Count=%llu\n",
         s_audio.output_peak_l(), s_audio.output_peak_r(),
         static_cast<unsigned long long>(c.nan_inf_count.load()));
  printf("Result: %s\n", (c.rx_overruns.load() == 0 && c.tx_underruns.load() == 0 && c.dsp_deadline_misses.load() == 0 && c.nan_inf_count.load() == 0) ? "PASS" : "WARNING");
  printf("=======================================================\n");

  printf("\n=== B4A TIMING CSV START ===\n");
  printf("metric,avg_us,p95_us,p99_us,p99_9_us,max_us\n");
  printf("pure_dsp,%.2f,%llu,%llu,%llu,%llu\n",
         dsp_timing.avg_us,
         static_cast<unsigned long long>(dsp_timing.p95_us),
         static_cast<unsigned long long>(dsp_timing.p99_us),
         static_cast<unsigned long long>(dsp_timing.p99_9_us),
         static_cast<unsigned long long>(dsp_timing.max_us));
  printf("block_cycle,%.2f,%llu,%llu,%llu,%llu\n",
         cycle_timing.avg_us,
         static_cast<unsigned long long>(cycle_timing.p95_us),
         static_cast<unsigned long long>(cycle_timing.p99_us),
         static_cast<unsigned long long>(cycle_timing.p99_9_us),
         static_cast<unsigned long long>(cycle_timing.max_us));
  printf("wake_latency,%.2f,%llu,%llu,%llu,%llu\n",
         wake_timing.avg_us,
         static_cast<unsigned long long>(wake_timing.p95_us),
         static_cast<unsigned long long>(wake_timing.p99_us),
         static_cast<unsigned long long>(wake_timing.p99_9_us),
         static_cast<unsigned long long>(wake_timing.max_us));
  printf("=== B4A TIMING CSV END ===\n\n");

  s_audio.print_forensic_outliers();
}

// Stage B4B.1 Forensic Snapshot Structures and Hook
struct FreqSnapshot {
  bool captured = false;
  float injected_f0 = 0.0f;
  const char *stimulus_state_str = "Unknown";
  float stimulus_rms = 0.0f;
  float audio_input_rms = 0.0f;
  const char *gate_state_str = "OPEN";
  float comp_gr_db = 0.0f;
  float yin_f0 = 0.0f;
  float yin_conf = 0.0f;
  float yin_period = 0.0f;
  bool stateful_voiced = false;
  PitchTrackState track_state = PitchTrackState::Unlocked;
  uint32_t pitch_marks_in_frame = 0;
  PitchShiftState psola_state = PitchShiftState::Bypass;
  bool psola_usable = false;
  float psola_active_mix = 0.0f;
  uint64_t delta_grains = 0;
  uint64_t lpc_frames = 0;
  float output_rms = 0.0f;
  float pitch_age_ms = 0.0f;
};
static FreqSnapshot s_b4b1_snapshots[5]; // 0: 110, 1: 147, 2: 220, 3: 330, 4: 440
static int64_t s_b4b1_silence_entry_us = 0;
static int64_t s_b4b1_silence_release_latency_us = -1;

static void b4b1_synthetic_block_hook(uint64_t sample_in_cycle, float f0, void *user_data) {
  auto get_freq_index = [](float f) -> int {
    if (std::fabs(f - 110.0f) < 8.0f) return 0;
    if (std::fabs(f - 147.0f) < 8.0f) return 1;
    if (std::fabs(f - 220.0f) < 8.0f) return 2;
    if (std::fabs(f - 330.0f) < 8.0f) return 3;
    if (std::fabs(f - 440.0f) < 8.0f) return 4;
    return -1;
  };

  int idx = get_freq_index(f0);
  // +500 ms into sustain corresponds to sample_in_cycle around 24000 (each block is 64 frames)
  if (idx >= 0 && !s_b4b1_snapshots[idx].captured && sample_in_cycle >= 24000 && sample_in_cycle < 24000 + 64) {
    s_b4b1_snapshots[idx].captured = true;
    s_b4b1_snapshots[idx].injected_f0 = f0;
    s_b4b1_snapshots[idx].stimulus_state_str = "Tone";
    s_b4b1_snapshots[idx].stimulus_rms = s_audio.current_stimulus_rms();
    s_b4b1_snapshots[idx].audio_input_rms = s_b4b1_snapshots[idx].stimulus_rms;
    s_b4b1_snapshots[idx].gate_state_str = "OPEN";
    s_b4b1_snapshots[idx].comp_gr_db = 0.0f;

    PitchResult pr{};
    (void)vocal_fx_try_latest_pitch(&pr);
    s_b4b1_snapshots[idx].yin_f0 = pr.frequency_hz;
    s_b4b1_snapshots[idx].yin_conf = pr.confidence;
    s_b4b1_snapshots[idx].yin_period = pr.period_samples;
    s_b4b1_snapshots[idx].stateful_voiced = pr.voiced;
    s_b4b1_snapshots[idx].track_state = static_cast<PitchTrackState>(pr.pitch_track_state);

    PitchSyncDiagnostics sync_diag = vocal_fx_pitch_sync_diagnostics();
    s_b4b1_snapshots[idx].pitch_marks_in_frame = sync_diag.last_mark_count;

    PitchShiftTelemetry h0 = vocal_fx_harmony_telemetry(0);
    s_b4b1_snapshots[idx].psola_state = h0.state;
    s_b4b1_snapshots[idx].psola_usable = (h0.state == PitchShiftState::Active || h0.state == PitchShiftState::Acquiring);
    s_b4b1_snapshots[idx].psola_active_mix = vocal_fx_effective_harmony_mix(0);
    s_b4b1_snapshots[idx].delta_grains = h0.grains;

    LpcTelemetry lpc = vocal_fx_lpc_telemetry();
    s_b4b1_snapshots[idx].lpc_frames = lpc.lpc_frames;
    s_b4b1_snapshots[idx].output_rms = std::max(s_audio.output_rms_l(), s_audio.output_rms_r());
    s_b4b1_snapshots[idx].pitch_age_ms = vocal_fx_latest_pitch_age_ms();
  }

  // Silence release tracking: silence starts at sample_in_cycle == 72000
  if (sample_in_cycle >= 72000 && sample_in_cycle < 72000 + 64) {
#ifdef ESP_PLATFORM
    s_b4b1_silence_entry_us = esp_timer_get_time();
#endif
    s_b4b1_silence_release_latency_us = -1;
  }
  if (sample_in_cycle >= 72000 && s_b4b1_silence_release_latency_us < 0) {
    PitchResult pr{};
    if (vocal_fx_try_latest_pitch(&pr) && !pr.voiced) {
#ifdef ESP_PLATFORM
      s_b4b1_silence_release_latency_us = esp_timer_get_time() - s_b4b1_silence_entry_us;
#endif
    }
  }
}

void run_i2s_stage_b4b_synthetic_voiced(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B4B.1: SYNTHETIC VOICED HARMONY ACTIVATION AUDIT\n");
  printf("=======================================================\n");
  printf("Transport: Real Full-Duplex I2S RX & TX DMA Active (100%% Load)\n");
  printf("DSP Source: Deterministic Synthetic Voiced Harmonic Tone (-18 dBFS)\n");
  printf("Harmonic Structure: F0(1.0) + 2*F0(0.5) + 3*F0(0.25) + 4*F0(0.125)\n");
  printf("Cadence: 1500 ms Voiced (10ms attack / 30ms release) / 100 ms Silence\n");
  printf("Frequencies: {110 Hz, 147 Hz, 220 Hz, 330 Hz, 440 Hz}\n");
  printf("Pipeline: I2S RX DMA (real read) -> Synthetic Mono -> Full VoxP4 DSP -> Float -> I2S TX DMA -> PCM5102\n");
  printf("DSP Modules: Gate + Comp + Pitch Tracker (Core 1) + Voicing + Pitch Marks + LPC + 2-Voice TD-PSOLA (+4st, +7st) + Delay + Reverb + Limiter\n");
  printf("Audit Mode: Stage Funnel & Telemetry Diagnostics Active (50s run)\n");

  uint32_t initial_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t initial_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  static VocalFxConfig cfg;
  cfg = {};
  cfg.enable_gate = true;
  cfg.enable_compressor = true;
  cfg.enable_delay = true;
  cfg.enable_reverb = true;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 4.0f;
  cfg.pitch_shift.wet = 1.0f;
  cfg.align_dry_to_harmony = true;
  cfg.enable_harmony_limiter = true;
  cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
  cfg.mute_dry = false;

  if (!vocal_fx_init(cfg)) {
    ESP_LOGE(TAG, "Vocal FX engine initialization failed");
    return;
  }

  // Nominal Full Chain Parameters with 2 Harmony Voices
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 1.0f);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f); // Major third (+4 st)
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f); // Fifth (+7 st)
  vocal_fx_set_harmony_gain(1, 1.0f);
  vocal_fx_set_formant_mode(1, FormantMode::Lpc);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 2.5f);

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::SyntheticVoicedDsp, true)) {
    printf("STAGE B4B: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  // Warmup 500 ms before taking baseline snapshots
  vTaskDelay(pdMS_TO_TICKS(500));

  // Snapshot start counters (Requirement 2 & Baseline)
  const auto &c = s_audio.counters();
  uint64_t start_blocks = c.audio_blocks_processed.load();
  uint64_t start_rx_ovr = c.rx_overruns.load();
  uint64_t start_tx_und = c.tx_underruns.load();
  uint64_t start_dma_err = c.dma_errors.load();
  uint64_t start_audio_misses = c.audio_deadline_misses.load();
  uint64_t start_dsp_misses = c.dsp_deadline_misses.load();
  uint64_t start_transport_misses = c.transport_deadline_misses.load();
  uint64_t start_nan_inf = c.nan_inf_count.load();

  vocal_fx_reset_funnel_stats();

  uint32_t baseline_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t baseline_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t heap_30s_internal = 0;
  uint32_t heap_30s_spiram = 0;

  for (auto &s : s_b4b1_snapshots) s = {};
  s_b4b1_silence_entry_us = 0;
  s_b4b1_silence_release_latency_us = -1;
  s_audio.set_synthetic_block_hook(b4b1_synthetic_block_hook, nullptr);

#ifdef ESP_PLATFORM
  UBaseType_t orig_priority = uxTaskPriorityGet(NULL);
  vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
  esp_task_wdt_config_t twdt_test_cfg = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << 1), // Only monitor Core 1 idle task during full DSP load
      .trigger_panic = false,
  };
  esp_task_wdt_reconfigure(&twdt_test_cfg);
#endif

  // Run short diagnostic: 50 seconds (10 steps x 5000ms = 5s per step)
  constexpr int kTotalIterations = 10;
  for (int step = 1; step <= kTotalIterations; ++step) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    int sec = step * 5;

    using vocal_fx_platform::StimulusState;
    StimulusState cur_stim_state = s_audio.current_stimulus_state();
    float cur_freq = s_audio.current_synth_freq_hz();
    float stim_rms = s_audio.current_stimulus_rms();
    PitchResult pr{};
    (void)vocal_fx_try_latest_pitch(&pr);
    PitchShiftTelemetry h0 = vocal_fx_harmony_telemetry(0);
    PitchShiftTelemetry h1 = vocal_fx_harmony_telemetry(1);
    LpcTelemetry lpc = vocal_fx_lpc_telemetry();
    PitchTrackState track = static_cast<PitchTrackState>(pr.pitch_track_state);
    float pitch_age = vocal_fx_latest_pitch_age_ms();

    if (sec == 30) { // At 30s
      heap_30s_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      heap_30s_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    const char *stim_str = (cur_stim_state == StimulusState::Silence) ? "SILENCE" :
                           (cur_stim_state == StimulusState::Attack)  ? "ATTACK"  :
                           (cur_stim_state == StimulusState::Release) ? "RELEASE" : "TONE";
    const char *track_str = (track == PitchTrackState::Locked)   ? "LOCKED"   :
                            (track == PitchTrackState::Coasting) ? "COASTING" :
                            (track == PitchTrackState::Acquiring)? "ACQUIRING": "UNLOCKED";
    uint64_t cur_blks = c.audio_blocks_processed.load(std::memory_order_relaxed);
    uint64_t d_blks = (cur_blks >= start_blocks) ? (cur_blks - start_blocks) : cur_blks;

    printf("[B4B.1 %02d s] Blks: %5llu | Stim: %5.1f Hz %-7s (RMS: %.3f) | Det: %5.1f Hz (Conf: %.2f, %-8s, %s, age: %.1f ms) | H0 St:%d Grns: %llu (InvM:%llu, PMUnd:%llu) | H1 Grns: %llu | LPC: %llu | Miss: %llu\n",
           sec,
           static_cast<unsigned long long>(d_blks),
           cur_freq,
           stim_str,
           stim_rms,
           pr.frequency_hz,
           pr.confidence,
           pr.voiced ? "VOICED" : "UNVOICED",
           track_str,
           pitch_age,
           static_cast<int>(h0.state),
           static_cast<unsigned long long>(h0.grains),
           static_cast<unsigned long long>(h0.invalid_mark),
           static_cast<unsigned long long>(h0.pitch_mark_underflows),
           static_cast<unsigned long long>(h1.grains),
           static_cast<unsigned long long>(lpc.lpc_frames),
           static_cast<unsigned long long>(c.audio_deadline_misses.load(std::memory_order_relaxed)));
  }

  s_audio.set_synthetic_block_hook(nullptr, nullptr);

  // End of test measurements
  uint32_t final_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t final_spiram   = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  UBaseType_t audio_hwm = uxTaskGetStackHighWaterMark(s_audio_task_handle);
  UBaseType_t pitch_hwm = uxTaskGetStackHighWaterMark(s_pitch_task_handle);

  PitchShiftTelemetry h0_final = vocal_fx_harmony_telemetry(0);
  PitchShiftTelemetry h1_final = vocal_fx_harmony_telemetry(1);
  LpcTelemetry lpc_final = vocal_fx_lpc_telemetry();
  VocalFxFunnelStats funnel = vocal_fx_funnel_stats();
  PitchSyncDiagnostics sync_final = vocal_fx_pitch_sync_diagnostics();

  auto dsp_timing = s_audio.calculate_dsp_percentiles();
  auto cycle_timing = s_audio.calculate_cycle_percentiles();
  auto wake_timing = s_audio.calculate_wake_percentiles();

  uint64_t end_blocks = c.audio_blocks_processed.load();
  uint64_t end_rx_ovr = c.rx_overruns.load();
  uint64_t end_tx_und = c.tx_underruns.load();
  uint64_t end_dma_err = c.dma_errors.load();
  uint64_t end_audio_misses = c.audio_deadline_misses.load();
  uint64_t end_dsp_misses = c.dsp_deadline_misses.load();
  uint64_t end_transport_misses = c.transport_deadline_misses.load();
  uint64_t end_wake_us = c.max_audio_task_wake_latency_us.load();
  uint64_t end_rx_gap = c.max_rx_callback_gap_us.load();
  uint64_t end_tx_gap = c.max_tx_callback_gap_us.load();
  uint64_t end_nan_inf = c.nan_inf_count.load();
  float final_peak_l = s_audio.output_peak_l();
  float final_peak_r = s_audio.output_peak_r();

  stop_pipeline_tasks();

  // Deltas
  uint64_t d_blocks = (end_blocks >= start_blocks) ? (end_blocks - start_blocks) : end_blocks;
  uint64_t d_rx_ovr = (end_rx_ovr >= start_rx_ovr) ? (end_rx_ovr - start_rx_ovr) : end_rx_ovr;
  uint64_t d_tx_und = (end_tx_und >= start_tx_und) ? (end_tx_und - start_tx_und) : end_tx_und;
  uint64_t d_dma_err = (end_dma_err >= start_dma_err) ? (end_dma_err - start_dma_err) : end_dma_err;
  uint64_t d_audio_miss = (end_audio_misses >= start_audio_misses) ? (end_audio_misses - start_audio_misses) : end_audio_misses;
  uint64_t d_dsp_miss = (end_dsp_misses >= start_dsp_misses) ? (end_dsp_misses - start_dsp_misses) : end_dsp_misses;
  uint64_t d_trans_miss = (end_transport_misses >= start_transport_misses) ? (end_transport_misses - start_transport_misses) : end_transport_misses;
  uint64_t d_nan_inf = (end_nan_inf >= start_nan_inf) ? (end_nan_inf - start_nan_inf) : end_nan_inf;

  printf("\n=======================================================\n");
  printf("   STAGE B4B.1: HARMONY ACTIVATION AUDIT COMPLETE      \n");
  printf("=======================================================\n");
  printf("Transport: Real Full-Duplex I2S RX & TX DMA Active (100%% Load)\n");
  printf("DSP Source: Deterministic Synthetic Voiced Harmonic Tone (-18 dBFS)\n");
  printf("Test Duration:             50.0 seconds\n");
  printf("Total Blocks Processed:    %llu (delta)\n", static_cast<unsigned long long>(d_blocks));
  printf("Steady-State RX Overruns:  %llu (delta)\n", static_cast<unsigned long long>(d_rx_ovr));
  printf("Steady-State TX Underruns: %llu (delta)\n", static_cast<unsigned long long>(d_tx_und));
  printf("DMA Hardware Errors:       %llu (delta)\n", static_cast<unsigned long long>(d_dma_err));
  printf("Audio Deadline Misses:     %llu (delta)\n", static_cast<unsigned long long>(d_audio_miss));
  printf("DSP Deadline Misses:       %llu (delta)\n", static_cast<unsigned long long>(d_dsp_miss));
  printf("Transport Misses:          %llu (delta)\n", static_cast<unsigned long long>(d_trans_miss));
  printf("Max Audio Wake Latency:    %llu us\n", static_cast<unsigned long long>(end_wake_us));
  printf("Max RX Callback Gap:       %llu us\n", static_cast<unsigned long long>(end_rx_gap));
  printf("Max TX Callback Gap:       %llu us\n", static_cast<unsigned long long>(end_tx_gap));
  printf("Pure DSP Timing (us):      Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         dsp_timing.avg_us,
         static_cast<unsigned long long>(dsp_timing.p95_us),
         static_cast<unsigned long long>(dsp_timing.p99_us),
         static_cast<unsigned long long>(dsp_timing.p99_9_us),
         static_cast<unsigned long long>(dsp_timing.max_us));
  printf("Block Cycle Timing (us):   Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         cycle_timing.avg_us,
         static_cast<unsigned long long>(cycle_timing.p95_us),
         static_cast<unsigned long long>(cycle_timing.p99_us),
         static_cast<unsigned long long>(cycle_timing.p99_9_us),
         static_cast<unsigned long long>(cycle_timing.max_us));
  printf("Wake Latency Timing (us):  Avg=%.1f | P95=%llu | P99=%llu | P99.9=%llu | Max=%llu\n",
         wake_timing.avg_us,
         static_cast<unsigned long long>(wake_timing.p95_us),
         static_cast<unsigned long long>(wake_timing.p99_us),
         static_cast<unsigned long long>(wake_timing.p99_9_us),
         static_cast<unsigned long long>(wake_timing.max_us));
  printf("Memory (Heap):             Initial: Int=%lu, PSRAM=%lu | Baseline: Int=%lu, PSRAM=%lu | 30s: Int=%lu, PSRAM=%lu | End: Int=%lu, PSRAM=%lu\n",
         static_cast<unsigned long>(initial_internal), static_cast<unsigned long>(initial_spiram),
         static_cast<unsigned long>(baseline_internal), static_cast<unsigned long>(baseline_spiram),
         static_cast<unsigned long>(heap_30s_internal), static_cast<unsigned long>(heap_30s_spiram),
         static_cast<unsigned long>(final_internal), static_cast<unsigned long>(final_spiram));
  printf("Memory Delta (End-Base):   Internal: %ld bytes | PSRAM: %ld bytes\n",
         static_cast<long>(final_internal) - static_cast<long>(baseline_internal),
         static_cast<long>(final_spiram) - static_cast<long>(baseline_spiram));
  printf("Stack High Water Mark:     Audio Task: %u bytes | Pitch Task: %u bytes\n",
         static_cast<unsigned>(audio_hwm * sizeof(StackType_t)),
         static_cast<unsigned>(pitch_hwm * sizeof(StackType_t)));
  printf("Output Sanity:             Peak L=%.4f | Peak R=%.4f | NaN/Inf Count=%llu\n",
         final_peak_l, final_peak_r,
         static_cast<unsigned long long>(d_nan_inf));
  printf("Harmony Activity:          Voice 0 Grains=%llu | Voice 1 Grains=%llu | LPC Frames=%llu\n",
         static_cast<unsigned long long>(h0_final.grains),
         static_cast<unsigned long long>(h1_final.grains),
         static_cast<unsigned long long>(lpc_final.lpc_frames));
  printf("=======================================================\n\n");

  printf("=== B4B.1 TIMING CSV START ===\n");
  printf("metric,avg_us,p95_us,p99_us,p99_9_us,max_us\n");
  printf("pure_dsp,%.2f,%llu,%llu,%llu,%llu\n",
         dsp_timing.avg_us,
         static_cast<unsigned long long>(dsp_timing.p95_us),
         static_cast<unsigned long long>(dsp_timing.p99_us),
         static_cast<unsigned long long>(dsp_timing.p99_9_us),
         static_cast<unsigned long long>(dsp_timing.max_us));
  printf("block_cycle,%.2f,%llu,%llu,%llu,%llu\n",
         cycle_timing.avg_us,
         static_cast<unsigned long long>(cycle_timing.p95_us),
         static_cast<unsigned long long>(cycle_timing.p99_us),
         static_cast<unsigned long long>(cycle_timing.p99_9_us),
         static_cast<unsigned long long>(cycle_timing.max_us));
  printf("wake_latency,%.2f,%llu,%llu,%llu,%llu\n",
         wake_timing.avg_us,
         static_cast<unsigned long long>(wake_timing.p95_us),
         static_cast<unsigned long long>(wake_timing.p99_us),
         static_cast<unsigned long long>(wake_timing.p99_9_us),
         static_cast<unsigned long long>(wake_timing.max_us));
  printf("=== B4B.1 TIMING CSV END ===\n\n");

  // Programmatic Sustain Snapshots (+500 ms into sustain)
  printf("\n================================================================================\n");
  printf("             PROGRAMMATIC SUSTAIN SNAPSHOTS (+500 ms INTO SUSTAIN)              \n");
  printf("================================================================================\n");
  printf("%-8s | %-6s | %-6s | %-6s | %-6s | %-8s | %-6s | %-6s | %-6s | %-6s | %-5s | %-6s | %-6s | %-6s | %-6s\n",
         "Target", "DetF0", "Conf", "Period", "Voiced", "Track", "Marks", "H0St", "Usable", "Mix", "dGrn", "LPC", "OutRMS", "AgeMs", "Gate");
  printf("---------+--------+--------+--------+--------+----------+--------+--------+--------+--------+-------+--------+--------+--------+-------\n");
  const float expected_freqs[5] = {110.0f, 147.0f, 220.0f, 330.0f, 440.0f};
  for (int i = 0; i < 5; ++i) {
    if (s_b4b1_snapshots[i].captured) {
      const char *t_str = (s_b4b1_snapshots[i].track_state == PitchTrackState::Locked) ? "LOCKED" :
                          (s_b4b1_snapshots[i].track_state == PitchTrackState::Coasting) ? "COAST" :
                          (s_b4b1_snapshots[i].track_state == PitchTrackState::Acquiring) ? "ACQ" : "UNLOCK";
      printf("%6.1f Hz | %6.1f | %6.2f | %6.1f | %-6s | %-8s | %-6u | %-6d | %-6s | %6.2f | %5llu | %6llu | %6.3f | %6.1f | %-6s\n",
             s_b4b1_snapshots[i].injected_f0,
             s_b4b1_snapshots[i].yin_f0,
             s_b4b1_snapshots[i].yin_conf,
             s_b4b1_snapshots[i].yin_period,
             s_b4b1_snapshots[i].stateful_voiced ? "YES" : "NO",
             t_str,
             static_cast<unsigned>(s_b4b1_snapshots[i].pitch_marks_in_frame),
             static_cast<int>(s_b4b1_snapshots[i].psola_state),
             s_b4b1_snapshots[i].psola_usable ? "YES" : "NO",
             s_b4b1_snapshots[i].psola_active_mix,
             static_cast<unsigned long long>(s_b4b1_snapshots[i].delta_grains),
             static_cast<unsigned long long>(s_b4b1_snapshots[i].lpc_frames),
             s_b4b1_snapshots[i].output_rms,
             s_b4b1_snapshots[i].pitch_age_ms,
             s_b4b1_snapshots[i].gate_state_str);
    } else {
      printf("%6.1f Hz | NOT CAPTURED DURING TEST WINDOW\n", expected_freqs[i]);
    }
  }
  printf("================================================================================\n\n");

  // Silence Release Dynamics
  printf("=== SILENCE RELEASE DYNAMICS ===\n");
  if (s_b4b1_silence_release_latency_us >= 0) {
    printf("Silence Release Latency to Unvoiced: %.2f ms\n", s_b4b1_silence_release_latency_us * 0.001f);
  } else {
    printf("Silence Release Latency to Unvoiced: PENDING (no unvoiced transition captured within silence window)\n");
  }
  printf("================================\n\n");

  // Stage Funnel Diagnostic Table
  printf("\n================================================================================\n");
  printf("                      STAGE B4B.1 FUNNEL DIAGNOSTIC TABLE                       \n");
  printf("================================================================================\n");
  printf("%-42s | %-16s | %-16s\n", "PIPELINE STAGE", "COUNT", "STATUS");
  printf("-------------------------------------------+------------------+-----------------\n");
  printf("%-42s | %-16llu | %s\n", "1. Audio Blocks Processed", static_cast<unsigned long long>(d_blocks), d_blocks > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "2. Synthetic Tone Blocks (RMS > -40 dBFS)", static_cast<unsigned long long>(funnel.synthetic_tone_blocks), funnel.synthetic_tone_blocks > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "3. Tap Blocks Processed", static_cast<unsigned long long>(funnel.pitch_analysis_blocks), funnel.pitch_analysis_blocks > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "4. Pitch Results Published", static_cast<unsigned long long>(funnel.pitch_results_produced), funnel.pitch_results_produced > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "5. Voiced Pitch Results", static_cast<unsigned long long>(funnel.voiced_pitch_results), funnel.voiced_pitch_results > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "6. Pitch Marks Generated", static_cast<unsigned long long>(funnel.pitch_marks_generated), funnel.pitch_marks_generated > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "7. Pitch Marks Transferred (DSP Core 0)", static_cast<unsigned long long>(funnel.pitch_marks_transferred), funnel.pitch_marks_transferred > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "8. Harmony Target Activations", static_cast<unsigned long long>(funnel.harmony_target_activations), funnel.harmony_target_activations > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "9. PSOLA Process Invocations", static_cast<unsigned long long>(funnel.psola_process_calls), funnel.psola_process_calls > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "10. Pitch Marks Consumed (AddGrain)", static_cast<unsigned long long>(funnel.pitch_marks_consumed), funnel.pitch_marks_consumed > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "11. Grain Schedule Attempts", static_cast<unsigned long long>(funnel.grain_schedule_attempts), funnel.grain_schedule_attempts > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "12. Grains Scheduled", static_cast<unsigned long long>(funnel.grains_scheduled), funnel.grains_scheduled > 0 ? "PASS" : "ZERO DROP");
  printf("%-42s | %-16llu | %s\n", "13. Grains Rendered", static_cast<unsigned long long>(funnel.grains_rendered), funnel.grains_rendered > 0 ? "PASS" : "ZERO DROP");
  printf("-------------------------------------------+------------------+-----------------\n");

  const char *first_zero = "NONE (ALL STAGES ACTIVE)";
  if (d_blocks == 0) first_zero = "Stage 1: Audio Blocks Processed";
  else if (funnel.synthetic_tone_blocks == 0) first_zero = "Stage 2: Synthetic Tone Blocks";
  else if (funnel.pitch_analysis_blocks == 0) first_zero = "Stage 3: Tap Blocks Processed";
  else if (funnel.pitch_results_produced == 0) first_zero = "Stage 4: Pitch Results Published";
  else if (funnel.voiced_pitch_results == 0) first_zero = "Stage 5: Voiced Pitch Results";
  else if (funnel.pitch_marks_generated == 0) first_zero = "Stage 6: Pitch Marks Generated";
  else if (funnel.pitch_marks_transferred == 0) first_zero = "Stage 7: Pitch Marks Transferred";
  else if (funnel.harmony_target_activations == 0) first_zero = "Stage 8: Harmony Target Activations";
  else if (funnel.psola_process_calls == 0) first_zero = "Stage 9: PSOLA Process Invocations";
  else if (funnel.pitch_marks_consumed == 0) first_zero = "Stage 10: Pitch Marks Consumed";
  else if (funnel.grain_schedule_attempts == 0) first_zero = "Stage 11: Grain Schedule Attempts";
  else if (funnel.grains_scheduled == 0) first_zero = "Stage 12: Grains Scheduled";
  else if (funnel.grains_rendered == 0) first_zero = "Stage 13: Grains Rendered";
  printf(">>> FIRST ZERO STAGE IDENTIFIED: %s <<<\n", first_zero);
  printf("================================================================================\n\n");

  // Core 0 <-> Core 1 Pitch Sync Diagnostics
  printf("=== CORE 0 <-> CORE 1 PITCH SYNC DIAGNOSTICS ===\n");
  printf("try_pitch_attempts:       %llu\n", static_cast<unsigned long long>(sync_final.try_pitch_attempts));
  printf("try_pitch_successes:      %llu\n", static_cast<unsigned long long>(sync_final.try_pitch_successes));
  printf("try_marks_attempts:       %llu\n", static_cast<unsigned long long>(sync_final.try_marks_attempts));
  printf("try_marks_successes:      %llu\n", static_cast<unsigned long long>(sync_final.try_marks_successes));
  printf("try_marks_start_gt_end:   %llu\n", static_cast<unsigned long long>(sync_final.try_marks_start_gt_end));
  printf("last_mark_count:          %llu\n", static_cast<unsigned long long>(sync_final.last_mark_count));
  printf("last_mark_end:            %llu\n", static_cast<unsigned long long>(sync_final.last_mark_end));
  printf("================================================\n\n");

  // Transport Error Event Log
  s_audio.print_transport_errors();

  // Outliers
  s_audio.print_forensic_outliers();

#ifdef ESP_PLATFORM
  esp_task_wdt_config_t twdt_restore_cfg = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1), // Restore monitoring for both cores
      .trigger_panic = false,
  };
  esp_task_wdt_reconfigure(&twdt_restore_cfg);
  vTaskPrioritySet(NULL, orig_priority);
#endif

  stop_pipeline_tasks();
}

namespace {
const char *pitch_track_name(PitchTrackState state) {
  switch (state) {
  case PitchTrackState::Unlocked: return "UNLOCKED";
  case PitchTrackState::Acquiring: return "ACQUIRING";
  case PitchTrackState::Locked: return "LOCKED";
  case PitchTrackState::Coasting: return "COASTING";
  }
  return "UNKNOWN";
}

const char *mark_reset_reason_name(PitchMarkResetReason reason) {
  switch (reason) {
  case PitchMarkResetReason::None: return "none";
  case PitchMarkResetReason::CorrelationBelowThreshold:
    return "mark correlation below threshold";
  case PitchMarkResetReason::PeriodInvalid: return "period invalid";
  case PitchMarkResetReason::VoicedLost: return "voiced lost";
  case PitchMarkResetReason::AnalysisReset: return "analysis reset";
  case PitchMarkResetReason::MarkTimeout: return "mark timeout";
  case PitchMarkResetReason::Other: return "other";
  }
  return "other";
}

void print_pitch_audit_event(const PitchAuditEvent &event) {
  const double timestamp_ms = event.input_position / 48.0;
  if (event.type == PitchAuditEventType::TrackTransition) {
    printf("[B4B.2 TRACK] t=%.3fms %s -> %s F0=%.3fHz conf=%.4f period=%.3f coherent=%u failures=%u pitch_age=%.3fms backlog=%.3fms reason=%s\n",
           timestamp_ms, pitch_track_name(event.old_state),
           pitch_track_name(event.new_state), event.detected_f0_hz,
           event.confidence, event.period_samples,
           static_cast<unsigned>(event.coherent_marks),
           static_cast<unsigned>(event.mark_failures),
           event.pitch_age_samples / 48.0, event.backlog_samples / 48.0,
           mark_reset_reason_name(event.reason));
  } else if (event.type == PitchAuditEventType::CoherentMarkReset) {
    printf("[B4B.2 MARK RESET] t=%.3fms coherent=%u failures=%u reason=%s F0=%.3fHz conf=%.4f age=%.3fms backlog=%.3fms\n",
           timestamp_ms, static_cast<unsigned>(event.coherent_marks),
           static_cast<unsigned>(event.mark_failures),
           mark_reset_reason_name(event.reason), event.detected_f0_hz,
           event.confidence, event.pitch_age_samples / 48.0,
           event.backlog_samples / 48.0);
  } else if (event.type == PitchAuditEventType::CorrelationReject) {
    printf("[B4B.2 MARK REJECT] t=%.3fms predicted=%llu offset=%d best=%.5f period=%.3f F0=%.3fHz\n",
           timestamp_ms,
           static_cast<unsigned long long>(event.predicted_position),
           static_cast<int>(event.best_offset), event.best_correlation,
           event.period_samples, event.detected_f0_hz);
  } else if (event.coherent_marks <= 3) {
    printf("[B4B.2 MARK] t=%.3fms coherent=%u best=%.5f offset=%d predicted=%llu\n",
           timestamp_ms, static_cast<unsigned>(event.coherent_marks),
           event.best_correlation, static_cast<int>(event.best_offset),
           static_cast<unsigned long long>(event.predicted_position));
  }
}
} // namespace

void run_i2s_stage_b4b2_pitch_worker_lock_audit(void) {
  constexpr int kDurationSeconds = 15;
  constexpr uint64_t kToneStartSample = 24000;
  printf("\n=======================================================\n");
  printf("   B4B_2_PITCH_WORKER_LOCK_AUDIT\n");
  printf("=======================================================\n");
  printf("Duration: 15 seconds\n");
  printf("Stimulus: 500 ms silence, 20 ms attack, then 220.0 Hz pure sine at -18 dBFS\n");
  printf("Pipeline: PCM1808 -> real I2S RX DMA/read -> synthetic replacement -> all DSP taps -> I2S TX DMA -> PCM5102\n");
  printf("Continuity policy: Baseline (unchanged)\n");
  printf("Expected period: 218.18 samples at 48 kHz\n");
  printf("PSRAM device: 32 MB HEX\n");
  printf("Reported runtime speed: 20 MHz\n");
  printf("Physical flash detected: 16 MB\n");
  printf("Image header configured: 2 MB\n");
  printf("FLASH NOTE: size mismatch deferred to a later milestone; partition table unchanged.\n");

  const uint32_t initial_internal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t initial_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  static VocalFxConfig config;
  config = {};
  config.enable_gate = true;
  config.enable_compressor = true;
  config.enable_delay = true;
  config.enable_reverb = true;
  config.enable_pitch_analysis = true;
  config.pitch_shift.enabled = true;
  config.pitch_shift.semitones = 4.0f;
  config.pitch_shift.wet = 1.0f;
  config.pitch_shift.continuity_policy = PsolaContinuityPolicy::Baseline;
  config.psola_continuity_policy = PsolaContinuityPolicy::Baseline;
  config.align_dry_to_harmony = true;
  config.enable_harmony_limiter = true;

  if (!vocal_fx_init(config)) {
    ESP_LOGE(TAG, "B4B.2 Vocal FX initialization failed");
    return;
  }
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_harmony_gain(1, 1.0f);
  vocal_fx_set_formant_mode(1, FormantMode::Lpc);

  vocal_fx_reset_funnel_stats();
  s_pitch_lock_audit_enabled.store(true, std::memory_order_release);
  s_teardown_diagnostics_enabled = true;
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t audit_wdt_config = {
      .timeout_ms = 10000,
      // Serial telemetry can monopolize either idle task during this bounded
      // diagnostic.  The normal dual-core watchdog configuration is restored
      // after all audit output has been emitted.
      .idle_core_mask = 0,
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&audit_wdt_config);
#endif
  if (!start_pipeline_tasks(
          vocal_fx_platform::AudioI2sMode::PitchWorkerLockAudit, true)) {
    s_pitch_lock_audit_enabled.store(false, std::memory_order_release);
    s_teardown_diagnostics_enabled = false;
    printf("B4B.2: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  const uint32_t baseline_internal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t baseline_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t heap_5s_internal = 0, heap_5s_spiram = 0;
  uint32_t heap_10s_internal = 0, heap_10s_spiram = 0;
  uint64_t previous_calls = 0, previous_hops = 0;
  TickType_t next_telemetry_wake = xTaskGetTickCount();

  for (int second = 1; second <= kDurationSeconds; ++second) {
    vTaskDelayUntil(&next_telemetry_wake, pdMS_TO_TICKS(1000));
    s_audio.set_diagnostic_activity(true, false, false, false);
    const uint64_t calls =
        s_pitch_worker.run_calls.load(std::memory_order_relaxed);
    const uint64_t hops =
        s_pitch_worker.hops_processed.load(std::memory_order_relaxed);
    const uint64_t interval_calls = calls - previous_calls;
    const uint64_t interval_hops = hops - previous_hops;
    previous_calls = calls;
    previous_hops = hops;
    const PitchAnalysisAuditTelemetry audit =
        vocal_fx_pitch_analysis_audit_telemetry();
    PitchResult pitch{};
    (void)vocal_fx_try_latest_pitch(&pitch);
    const PitchShiftDebug harmony = vocal_fx_harmony_debug(0);
    const VocalFxFunnelStats funnel = vocal_fx_funnel_stats();
    const PitchSyncDiagnostics sync = vocal_fx_pitch_sync_diagnostics();
    const auto identity = s_audio.synthetic_input_identity();
    PitchAuditEvent events[24];
    const size_t event_count =
        vocal_fx_read_pitch_audit_events(events, 24);
    const uint64_t blocks = s_audio.counters().audio_blocks_processed.load(
        std::memory_order_relaxed);
    const float error_cents =
        pitch.frequency_hz > 0.0f
            ? static_cast<float>(1200.0 *
                                 std::log2(pitch.frequency_hz / 220.0))
            : 0.0f;
    if (second == 5) {
      heap_5s_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      heap_5s_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    } else if (second == 10) {
      heap_10s_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      heap_10s_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    s_audio.set_diagnostic_activity(false, true, true, false);
    printf("\n[B4B.2 t=%ds]\n", second);
    printf("Audio blocks: %llu\nWorker iterations: %llu\nWorker calls/s: %llu\nHops processed/s: %llu\nAvg hops/call: %.3f\n",
           static_cast<unsigned long long>(blocks),
           static_cast<unsigned long long>(s_pitch_worker.iterations.load(
               std::memory_order_relaxed)),
           static_cast<unsigned long long>(interval_calls),
           static_cast<unsigned long long>(interval_hops),
           interval_calls ? static_cast<double>(interval_hops) / interval_calls
                          : 0.0);
    printf("FIFO current: %u\nFIFO max: %u\nFIFO drops: %llu\n",
           static_cast<unsigned>(audit.fifo_current_occupancy),
           static_cast<unsigned>(audit.fifo_maximum_occupancy),
           static_cast<unsigned long long>(audit.fifo_drops));
    printf("Pitch age ms: %.3f\nBacklog ms: %.3f\n",
           audit.latest_pitch_age_ms, audit.analysis_backlog_ms);
    printf("Injected F0: 220.000 Hz\nDetected F0: %.3f Hz\nError cents: %+.3f\nConfidence: %.4f\nVoiced: %s\nTrack state: %s\nCoherent marks: %u\nMark count: %llu\n",
           pitch.frequency_hz, error_cents, pitch.confidence,
           pitch.voiced ? "true" : "false",
           pitch_track_name(static_cast<PitchTrackState>(
               pitch.pitch_track_state)),
           static_cast<unsigned>(pitch.coherent_marks),
           static_cast<unsigned long long>(sync.last_mark_count));
    printf("PSOLA usable: %s\nHarmony mix: %.4f\nGrain attempts: %llu\nGrains rendered: %llu\n",
           harmony.usable ? "true" : "false",
           vocal_fx_effective_harmony_mix(0),
           static_cast<unsigned long long>(funnel.grain_schedule_attempts),
           static_cast<unsigned long long>(funnel.grains_rendered));
    printf("Buffer identity: synth_rms=%.7f pitch_rms=%.7f dsp_rms=%.7f checks=%llu mismatches=%llu hashes=%08lx/%08lx/%08lx\n",
           identity.synthetic_rms, identity.pitch_tap_rms,
           identity.dsp_input_rms,
           static_cast<unsigned long long>(identity.checks),
           static_cast<unsigned long long>(identity.mismatches),
           static_cast<unsigned long>(identity.synthetic_checksum),
           static_cast<unsigned long>(identity.pitch_tap_checksum),
           static_cast<unsigned long>(identity.dsp_input_checksum));
    for (size_t i = 0; i < event_count; ++i)
      print_pitch_audit_event(events[i]);
    s_audio.set_diagnostic_activity(false, false, false, false);
  }

  const bool teardown_clean = stop_pipeline_tasks();
  s_pitch_lock_audit_enabled.store(false, std::memory_order_release);
  s_teardown_diagnostics_enabled = false;

  const uint32_t end_internal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t end_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const auto audit = vocal_fx_pitch_analysis_audit_telemetry();
  PitchResult pitch{};
  (void)vocal_fx_try_latest_pitch(&pitch);
  const PitchShiftDebug harmony = vocal_fx_harmony_debug(0);
  const VocalFxFunnelStats funnel = vocal_fx_funnel_stats();
  const auto identity = s_audio.synthetic_input_identity();
  const auto &transport = s_audio.counters();
  const uint64_t worker_calls =
      s_pitch_worker.run_calls.load(std::memory_order_relaxed);
  const uint64_t hops =
      s_pitch_worker.hops_processed.load(std::memory_order_relaxed);
  const uint64_t error_samples =
      s_pitch_worker.cents_error_samples.load(std::memory_order_relaxed);
  const double worker_calls_per_second =
      static_cast<double>(worker_calls) / kDurationSeconds;
  const double hops_per_second =
      static_cast<double>(hops) / kDurationSeconds;
  const double average_hops_per_call =
      worker_calls ? static_cast<double>(hops) / worker_calls : 0.0;
  const double mean_abs_error_cents =
      error_samples
          ? s_pitch_worker.cents_error_abs_sum_milli.load(
                std::memory_order_relaxed) /
                (1000.0 * error_samples)
          : 0.0;
  const double mean_f0 =
      error_samples
          ? s_pitch_worker.detected_f0_sum_millihz.load(
                std::memory_order_relaxed) /
                (1000.0 * error_samples)
          : 0.0;
  const double max_error_cents =
      s_pitch_worker.cents_error_max_abs_milli.load(
          std::memory_order_relaxed) /
      1000.0;
  const bool ever_locked = audit.first_locked_input_position != 0;
  const double lock_time_ms =
      ever_locked && audit.first_locked_input_position > kToneStartSample
          ? (audit.first_locked_input_position - kToneStartSample) / 48.0
          : 0.0;

  size_t spike_count = 0, harness_context_spikes = 0;
  for (size_t i = 0; i < s_audio.outlier_count(); ++i) {
    const auto &event = s_audio.outliers()[i];
    if (event.pure_dsp_us >= 8000 && event.pure_dsp_us <= 9500) {
      ++spike_count;
      if (event.snapshot_active || event.telemetry_formatting_active ||
          event.serial_printing_active || event.diagnostic_queue_flush_active)
        ++harness_context_spikes;
    }
  }

  const bool worker_healthy = hops_per_second >= 160.0 &&
                              audit.fifo_drops == 0 &&
                              audit.analysis_backlog_ms < 100.0f;
  const bool psola_pass = harmony.usable &&
                          funnel.grain_schedule_attempts > 0 &&
                          funnel.grains_rendered > 0;
  const bool diagnostic_queue_clean =
      transport.rx_diagnostic_queue_overflows.load(std::memory_order_relaxed) ==
          0 &&
      transport.tx_diagnostic_queue_overflows.load(std::memory_order_relaxed) ==
          0;
  const char *classification = "HARNESS / SCHEDULER ISSUE";
  if (!worker_healthy)
    classification = "PITCH_WORKER_STARVATION";
  else if (!ever_locked) {
    classification = mean_abs_error_cents <= 50.0 &&
                             audit.coherent_marks_maximum < 3 &&
                             audit.rejected_marks > 0
                         ? "PITCH_MARK_COHERENCE_ISSUE"
                         : "PITCH_TRACKING_INSTABILITY";
  } else if (!psola_pass)
    classification = "PSOLA_ACTIVATION_ISSUE";
  else if (teardown_clean && diagnostic_queue_clean &&
           identity.mismatches == 0)
    classification = "B4B.2 PASS";

  printf("\n=======================================================================\n");
  printf("Stage B4B.2 — Pitch Worker Cadence & Lock Acquisition Audit Report\n");
  printf("=======================================================================\n");
  printf("1. Pitch worker executions/s: %.2f (iterations=%llu, wakeups=%llu, yields=%llu)\n",
         worker_calls_per_second,
         static_cast<unsigned long long>(s_pitch_worker.iterations.load()),
         static_cast<unsigned long long>(s_pitch_worker.wakeups.load()),
         static_cast<unsigned long long>(s_pitch_worker.yields.load()));
  printf("2. Analysis hops processed/s: %.2f\n", hops_per_second);
  printf("3. Expected ~200 hops/s reached: %s\n",
         (hops_per_second >= 180.0 && hops_per_second <= 220.0) ? "yes"
                                                               : "no");
  printf("4. Hops/call: average=%.4f max=%llu zero=%llu one=%llu multi=%llu requested=%llu processed=%llu\n",
         average_hops_per_call,
         static_cast<unsigned long long>(s_pitch_worker.maximum_hops_per_call.load()),
         static_cast<unsigned long long>(s_pitch_worker.zero_hop_calls.load()),
         static_cast<unsigned long long>(s_pitch_worker.one_hop_calls.load()),
         static_cast<unsigned long long>(s_pitch_worker.multi_hop_calls.load()),
         static_cast<unsigned long long>(s_pitch_worker.hops_requested.load()),
         static_cast<unsigned long long>(hops));
  printf("5. FIFO backlog: current=%u avg=%.2f max=%u; accumulated=%s; pushes=%llu pops=%llu\n",
         static_cast<unsigned>(audit.fifo_current_occupancy),
         audit.fifo_average_occupancy,
         static_cast<unsigned>(audit.fifo_maximum_occupancy),
         audit.analysis_backlog_ms < 100.0f ? "no" : "yes",
         static_cast<unsigned long long>(audit.fifo_pushes),
         static_cast<unsigned long long>(audit.fifo_pops));
  printf("6. FIFO drops=%llu overflow_attempts=%llu\n",
         static_cast<unsigned long long>(audit.fifo_drops),
         static_cast<unsigned long long>(audit.fifo_overflow_attempts));
  printf("7. Maximum analysis backlog=%llu samples / %.3f ms (current=%llu / %.3f ms)\n",
         static_cast<unsigned long long>(audit.analysis_backlog_max_samples),
         audit.analysis_backlog_max_ms,
         static_cast<unsigned long long>(audit.analysis_backlog_samples),
         audit.analysis_backlog_ms);
  printf("   Algorithmic analysis latency=%llu samples / %.3f ms (separate from worker backlog)\n",
         static_cast<unsigned long long>(audit.algorithmic_latency_samples),
         audit.algorithmic_latency_samples / 48.0);
  printf("8. Pitch age ms: average=%.3f P95<=%.3f P99<=%.3f max=%.3f latest=%.3f\n",
         audit.pitch_age_average_ms, audit.pitch_age_p95_ms,
         audit.pitch_age_p99_ms, audit.pitch_age_max_ms,
         audit.latest_pitch_age_ms);
  printf("9. Pure 220 Hz detected correctly: %s (mean F0=%.4f Hz, final=%.4f Hz)\n",
         mean_abs_error_cents <= 50.0 ? "yes" : "no", mean_f0,
         pitch.frequency_hz);
  printf("10. F0 error: mean absolute=%.3f cents max absolute=%.3f cents\n",
         mean_abs_error_cents, max_error_cents);
  printf("11. Tracker reached LOCKED: %s\n", ever_locked ? "yes" : "no");
  printf("12. Time from tone start to LOCKED: %.3f ms\n", lock_time_ms);
  printf("13. Maximum coherent_marks: %u\n",
         static_cast<unsigned>(audit.coherent_marks_maximum));
  printf("14. Coherent mark resets=%llu [correlation=%llu period_invalid=%llu voiced_lost=%llu analysis_reset=%llu timeout=%llu other=%llu]\n",
         static_cast<unsigned long long>(audit.coherent_mark_resets),
         static_cast<unsigned long long>(audit.reset_correlation_below_threshold),
         static_cast<unsigned long long>(audit.reset_period_invalid),
         static_cast<unsigned long long>(audit.reset_voiced_lost),
         static_cast<unsigned long long>(audit.reset_analysis),
         static_cast<unsigned long long>(audit.reset_mark_timeout),
         static_cast<unsigned long long>(audit.reset_other));
  printf("   Mark correlation: avg=%.5f min=%.5f max=%.5f accepted=%llu rejected=%llu event_drops=%llu\n",
         audit.best_correlation_average, audit.best_correlation_minimum,
         audit.best_correlation_maximum,
         static_cast<unsigned long long>(audit.accepted_marks),
         static_cast<unsigned long long>(audit.rejected_marks),
         static_cast<unsigned long long>(audit.audit_event_drops));
  printf("15. PSOLA usable: %s\n", harmony.usable ? "yes" : "no");
  printf("16. Grain schedule attempts: %llu\n",
         static_cast<unsigned long long>(funnel.grain_schedule_attempts));
  printf("17. Grains rendered: %llu\n",
         static_cast<unsigned long long>(funnel.grains_rendered));
  printf("18. PureDsp 8-9.5 ms spikes=%zu; coincident with harness telemetry=%zu; attribution=%s\n",
         spike_count, harness_context_spikes,
         spike_count == 0
             ? "not reproduced"
             : (harness_context_spikes == spike_count ? "harness-associated"
                                                      : "DSP/unattributed samples remain"));
  printf("19. Diagnostic queue overflow RX/TX=%llu/%llu; actual DMA errors RX/TX=%llu/%llu; read/write failures=%llu/%llu; dropped frames RX/TX=%llu/%llu\n",
         static_cast<unsigned long long>(transport.rx_diagnostic_queue_overflows.load()),
         static_cast<unsigned long long>(transport.tx_diagnostic_queue_overflows.load()),
         static_cast<unsigned long long>(transport.actual_rx_dma_errors.load()),
         static_cast<unsigned long long>(transport.actual_tx_dma_errors.load()),
         static_cast<unsigned long long>(transport.i2s_read_failures.load()),
         static_cast<unsigned long long>(transport.i2s_write_failures.load()),
         static_cast<unsigned long long>(transport.rx_dropped_frames.load()),
         static_cast<unsigned long long>(transport.tx_dropped_frames.load()));
  printf("   Event queue producer/consumer/current/max depth RX=%llu/%llu/%llu/%llu TX=%llu/%llu/%llu/%llu\n",
         static_cast<unsigned long long>(transport.rx_dma_events.load()),
         static_cast<unsigned long long>(transport.i2s_read_successes.load()),
         static_cast<unsigned long long>(transport.rx_event_queue_depth.load()),
         static_cast<unsigned long long>(transport.rx_event_queue_max_depth.load()),
         static_cast<unsigned long long>(transport.i2s_write_successes.load()),
         static_cast<unsigned long long>(transport.tx_dma_events.load()),
         static_cast<unsigned long long>(transport.tx_event_queue_depth.load()),
         static_cast<unsigned long long>(transport.tx_event_queue_max_depth.load()));
  printf("   Event queue producer/consumer rates per second RX=%.2f/%.2f TX=%.2f/%.2f\n",
         transport.rx_dma_events.load() / static_cast<double>(kDurationSeconds),
         transport.i2s_read_successes.load() /
             static_cast<double>(kDurationSeconds),
         transport.i2s_write_successes.load() /
             static_cast<double>(kDurationSeconds),
         transport.tx_dma_events.load() /
             static_cast<double>(kDurationSeconds));
  printf("20. Spinlock crash reproduced: no (report reached after teardown)\n");
  printf("21. Teardown clean: %s\n", teardown_clean ? "yes" : "no");
  printf("   Buffer identity checks=%llu mismatches=%llu (synthetic==pitch==DSP: %s)\n",
         static_cast<unsigned long long>(identity.checks),
         static_cast<unsigned long long>(identity.mismatches),
         identity.checks > 0 && identity.mismatches == 0 ? "yes" : "no");
  printf("   Heap bytes Initial Int/PSRAM=%lu/%lu Baseline=%lu/%lu 5s=%lu/%lu 10s=%lu/%lu End=%lu/%lu Delta End-Baseline=%ld/%ld\n",
         static_cast<unsigned long>(initial_internal),
         static_cast<unsigned long>(initial_spiram),
         static_cast<unsigned long>(baseline_internal),
         static_cast<unsigned long>(baseline_spiram),
         static_cast<unsigned long>(heap_5s_internal),
         static_cast<unsigned long>(heap_5s_spiram),
         static_cast<unsigned long>(heap_10s_internal),
         static_cast<unsigned long>(heap_10s_spiram),
         static_cast<unsigned long>(end_internal),
         static_cast<unsigned long>(end_spiram),
         static_cast<long>(end_internal) - static_cast<long>(baseline_internal),
         static_cast<long>(end_spiram) - static_cast<long>(baseline_spiram));
  printf("22. FINAL CLASSIFICATION: %s\n", classification);
  printf("First failed arrow: %s\n",
         !worker_healthy
             ? "synthetic input -> timely analysis"
             : (mean_abs_error_cents > 50.0
                    ? "Injected F0 -> Detected F0"
                    : (!ever_locked
                           ? "Detected F0 -> coherent_marks >= 3 / LOCKED"
                           : (!harmony.usable
                                  ? "LOCKED -> PSOLA usable"
                                  : (funnel.grain_schedule_attempts == 0
                                         ? "PSOLA usable -> grain schedule attempts"
                                         : (funnel.grains_rendered == 0
                                                ? "grain attempts -> grains rendered"
                                                : "none"))))));
  printf("B4C READY: no\n");
  printf("=======================================================================\n");
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t normal_wdt_config = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&normal_wdt_config);
#endif
}

namespace {
VocalFxProfileStats profile_delta(const VocalFxProfileStats &end,
                                  const VocalFxProfileStats &start) {
  return {end.blocks - start.blocks,
          end.total_us - start.total_us,
          end.worst_us,
          end.deadline_misses - start.deadline_misses,
          end.total_cycles - start.total_cycles,
          end.worst_cycles};
}

uint32_t percentile_sorted(const uint32_t *values, size_t count,
                           uint32_t percentile) {
  if (!values || count == 0)
    return 0;
  const size_t index = std::min<size_t>(
      count - 1, (count * static_cast<size_t>(percentile) + 99) / 100 - 1);
  return values[index];
}

struct TransportSnapshot {
  uint64_t rx_queue_overflows = 0;
  uint64_t tx_queue_overflows = 0;
  uint64_t rx_dma_errors = 0;
  uint64_t tx_dma_errors = 0;
  uint64_t read_failures = 0;
  uint64_t write_failures = 0;
  uint64_t rx_dropped_frames = 0;
  uint64_t tx_dropped_frames = 0;
  uint64_t blocks = 0;
};

TransportSnapshot transport_snapshot() {
  const auto &counters = s_audio.counters();
  return {
      counters.rx_diagnostic_queue_overflows.load(std::memory_order_relaxed),
      counters.tx_diagnostic_queue_overflows.load(std::memory_order_relaxed),
      counters.actual_rx_dma_errors.load(std::memory_order_relaxed),
      counters.actual_tx_dma_errors.load(std::memory_order_relaxed),
      counters.i2s_read_failures.load(std::memory_order_relaxed),
      counters.i2s_write_failures.load(std::memory_order_relaxed),
      counters.rx_dropped_frames.load(std::memory_order_relaxed),
      counters.tx_dropped_frames.load(std::memory_order_relaxed),
      counters.audio_blocks_processed.load(std::memory_order_relaxed),
  };
}

struct OldSlidingStorageBenchmark {
  uint64_t total_us = 0;
  uint64_t total_cycles = 0;
  uint32_t shifted_samples = 0;
  float checksum = 0.0f;
};

OldSlidingStorageBenchmark benchmark_old_lpc_sliding_storage() {
  constexpr uint32_t kSamples = 48000;
  // This benchmark runs only after both real-time tasks have confirmed stop.
  // Reuse coordinator stack space rather than reserving scarce internal BSS.
  std::array<float, 1024> old_sliding_frame{};
  for (size_t i = 0; i < old_sliding_frame.size(); ++i)
    old_sliding_frame[i] = static_cast<float>(i) * (1.0f / 1024.0f);
  const uint64_t start_us = esp_timer_get_time();
  const uint32_t start_cycles = esp_cpu_get_cycle_count();
  for (uint32_t i = old_sliding_frame.size(); i < kSamples; ++i) {
    std::move(old_sliding_frame.begin() + 1, old_sliding_frame.end(),
              old_sliding_frame.begin());
    old_sliding_frame.back() =
        static_cast<float>(i & 0x3ffU) * (1.0f / 1024.0f);
  }
  OldSlidingStorageBenchmark result{};
  result.total_cycles = static_cast<uint32_t>(
      esp_cpu_get_cycle_count() - start_cycles);
  result.total_us = esp_timer_get_time() - start_us;
  result.shifted_samples =
      kSamples - static_cast<uint32_t>(old_sliding_frame.size());
  for (float value : old_sliding_frame) result.checksum += value;
  s_b4b4a_storage_checksum = result.checksum;
  return result;
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
configRUN_TIME_COUNTER_TYPE task_runtime_counter(TaskHandle_t task) {
  TaskStatus_t status{};
  vTaskGetInfo(task, &status, pdFALSE, eInvalid);
  return status.ulRunTimeCounter;
}
#endif
} // namespace

void run_i2s_stage_b4b3_pitch_analysis_hotspot_audit(void) {
  constexpr uint32_t kDurationSeconds = 10;
  constexpr double kAnalysisSampleRate = 12000.0;
  constexpr double kHopSize = 60.0;
  constexpr double kRequiredHopsPerSecond =
      kAnalysisSampleRate / kHopSize;
  constexpr double kBudgetUsPerHop = 1000000.0 / kRequiredHopsPerSecond;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  // Measured by B4B.3 on this same ESP32-P4 rev1.3 at 360 MHz. These are
  // fixed comparison inputs, not estimates derived from the optimized run.
  constexpr double kBeforeLpcUsPerFrame = 41731.39;
  constexpr double kBeforeWindowDrainUsPerFrame = 29663.58;
  constexpr double kBeforeAutocorrelationUsPerFrame = 11822.73;
  constexpr double kBeforeLevinsonUsPerFrame = 162.26;
  constexpr double kBeforePublicationUsPerFrame = 14.91;
  constexpr double kBeforeYinDifferenceUsPerHop = 4114.46;
  constexpr double kBeforePitchAnalysisUsPerHop = 5107.40;
  constexpr double kBeforeCombinedUsPerHop = 46852.56;
  constexpr double kBeforeCore1Percent = 99.33;
  constexpr double kBeforePitchHopsPerSecond = 21.19;
#endif

  printf("\n=======================================================\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  printf("   B4B.4A — LPC RING BUFFER OPTIMIZATION AUDIT\n");
#else
  printf("   B4B_3_PITCH_ANALYSIS_HOTSPOT_AUDIT\n");
#endif
  printf("=======================================================\n");
  printf("Duration: 10 seconds\n");
  printf("Stimulus: 500 ms silence, 20 ms attack, 220 Hz pure sine at -18 dBFS\n");
  printf("Transport: real PCM1808 RX/read and PCM5102 TX; synthetic input feeds all DSP taps\n");
  printf("Pitch priority: configMAX_PRIORITIES - 5 (unchanged)\n");
  printf("CPU frequency: 360 MHz\n");
  printf("No DSP algorithm or analysis parameter is changed by this audit.\n");

  static VocalFxConfig config;
  config = {};
  config.enable_gate = true;
  config.enable_compressor = true;
  config.enable_delay = true;
  config.enable_reverb = true;
  config.enable_pitch_analysis = true;
  config.pitch_shift.enabled = true;
  config.pitch_shift.semitones = 4.0f;
  config.pitch_shift.wet = 1.0f;
  config.pitch_shift.continuity_policy = PsolaContinuityPolicy::Baseline;
  config.psola_continuity_policy = PsolaContinuityPolicy::Baseline;
  config.align_dry_to_harmony = true;
  config.enable_harmony_limiter = true;
  if (!vocal_fx_init(config)) {
    ESP_LOGE(TAG, "B4B.3 Vocal FX initialization failed");
    return;
  }
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_harmony_gain(1, 1.0f);
  vocal_fx_set_formant_mode(1, FormantMode::Lpc);

  s_teardown_diagnostics_enabled = true;
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t audit_wdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&audit_wdt_config);
#endif
  if (!start_pipeline_tasks(
          vocal_fx_platform::AudioI2sMode::PitchWorkerLockAudit, true)) {
    s_teardown_diagnostics_enabled = false;
    printf("B4B.3: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  std::array<VocalFxProfileStats,
             static_cast<size_t>(PitchAnalysisProfileSection::Count)>
      pitch_profile_start{};
  for (size_t i = 0; i < pitch_profile_start.size(); ++i)
    pitch_profile_start[i] = vocal_fx_pitch_profile_stats(
        static_cast<PitchAnalysisProfileSection>(i));
  std::array<VocalFxProfileStats,
             static_cast<size_t>(LpcProfileSection::Count)>
      lpc_profile_start{};
  for (size_t i = 0; i < lpc_profile_start.size(); ++i)
    lpc_profile_start[i] =
        vocal_fx_lpc_profile_stats(static_cast<LpcProfileSection>(i));
  const PitchAnalysisAuditTelemetry audit_start =
      vocal_fx_pitch_analysis_audit_telemetry();
  const YinForensicTelemetry yin_start = vocal_fx_yin_forensic_telemetry();
  const PitchMarkForensicTelemetry marks_start =
      vocal_fx_pitch_mark_forensic_telemetry();
  const TransportSnapshot transport_start = transport_snapshot();
  const LpcTelemetry lpc_telemetry_start = vocal_fx_lpc_telemetry();

  s_pitch_worker_call_record_count.store(0, std::memory_order_relaxed);
  s_pitch_worker_call_record_drops.store(0, std::memory_order_relaxed);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  const TaskHandle_t diagnostic_task = xTaskGetCurrentTaskHandle();
  const TaskHandle_t idle1_task = xTaskGetIdleTaskHandleForCore(1);
  const configRUN_TIME_COUNTER_TYPE worker_runtime_start =
      task_runtime_counter(s_pitch_task_handle);
  const configRUN_TIME_COUNTER_TYPE diagnostic_runtime_start =
      task_runtime_counter(diagnostic_task);
  const configRUN_TIME_COUNTER_TYPE idle1_runtime_start =
      task_runtime_counter(idle1_task);
#endif
  const uint64_t window_start_us = esp_timer_get_time();
  s_pitch_hotspot_audit_enabled.store(true, std::memory_order_release);
  vTaskDelay(pdMS_TO_TICKS(kDurationSeconds * 1000));
  s_pitch_hotspot_audit_enabled.store(false, std::memory_order_release);
  while (s_pitch_worker_in_call.load(std::memory_order_acquire))
    vTaskDelay(1);
  const uint64_t window_end_us = esp_timer_get_time();
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  const configRUN_TIME_COUNTER_TYPE worker_runtime_end =
      task_runtime_counter(s_pitch_task_handle);
  const configRUN_TIME_COUNTER_TYPE diagnostic_runtime_end =
      task_runtime_counter(diagnostic_task);
  const configRUN_TIME_COUNTER_TYPE idle1_runtime_end =
      task_runtime_counter(idle1_task);
#endif

  std::array<VocalFxProfileStats,
             static_cast<size_t>(PitchAnalysisProfileSection::Count)>
      pitch_profile_end{};
  for (size_t i = 0; i < pitch_profile_end.size(); ++i)
    pitch_profile_end[i] = vocal_fx_pitch_profile_stats(
        static_cast<PitchAnalysisProfileSection>(i));
  std::array<VocalFxProfileStats,
             static_cast<size_t>(LpcProfileSection::Count)>
      lpc_profile_end{};
  for (size_t i = 0; i < lpc_profile_end.size(); ++i)
    lpc_profile_end[i] =
        vocal_fx_lpc_profile_stats(static_cast<LpcProfileSection>(i));

  const PitchAnalysisAuditTelemetry audit_end =
      vocal_fx_pitch_analysis_audit_telemetry();
  const YinForensicTelemetry yin_end = vocal_fx_yin_forensic_telemetry();
  const PitchMarkForensicTelemetry marks_end =
      vocal_fx_pitch_mark_forensic_telemetry();
  const TransportSnapshot transport_end = transport_snapshot();
  const LpcTelemetry lpc_telemetry_end = vocal_fx_lpc_telemetry();
  const bool teardown_clean = stop_pipeline_tasks();
  const LpcFrameCostSummary lpc_frame_cost =
      vocal_fx_lpc_frame_cost_summary();
  const OldSlidingStorageBenchmark old_storage =
      benchmark_old_lpc_sliding_storage();
  s_teardown_diagnostics_enabled = false;

  VocalFxBufferAudit buffer_audits[32]{};
  const size_t buffer_audit_count =
      vocal_fx_audit_buffers(buffer_audits, std::size(buffer_audits));

  std::array<VocalFxProfileStats,
             static_cast<size_t>(PitchAnalysisProfileSection::Count)>
      pitch_profile{};
  for (size_t i = 0; i < pitch_profile.size(); ++i)
    pitch_profile[i] =
        profile_delta(pitch_profile_end[i], pitch_profile_start[i]);
  std::array<VocalFxProfileStats,
             static_cast<size_t>(LpcProfileSection::Count)>
      lpc_profile{};
  for (size_t i = 0; i < lpc_profile.size(); ++i)
    lpc_profile[i] = profile_delta(lpc_profile_end[i], lpc_profile_start[i]);

  const uint32_t raw_record_count =
      s_pitch_worker_call_record_count.load(std::memory_order_acquire);
  const size_t record_count = std::min<size_t>(
      raw_record_count, kPitchWorkerCallRecordCapacity);
  size_t call_count = 0, hop_cost_count = 0;
  uint64_t call_total_us = 0, call_total_cycles = 0, completed_hops = 0;
  uint32_t call_max_us = 0, hop_max_us = 0;
  std::array<uint64_t, 5> class_calls{}, class_total_us{};
  for (size_t i = 0; i < record_count; ++i) {
    const auto &record = s_pitch_worker_call_records[i];
    if (record.start_us < window_start_us || record.start_us >= window_end_us)
      continue;
    s_b4b3_call_us_sort[call_count++] = record.duration_us;
    call_total_us += record.duration_us;
    call_total_cycles += record.cycles;
    call_max_us = std::max(call_max_us, record.duration_us);
    const size_t hops = std::min<size_t>(record.hops, 4);
    ++class_calls[hops];
    class_total_us[hops] += record.duration_us;
    completed_hops += hops;
    if (hops > 0) {
      const uint32_t cost = record.duration_us / hops;
      s_b4b3_hop_us_sort[hop_cost_count++] = cost;
      hop_max_us = std::max(hop_max_us, cost);
    }
  }
  std::sort(s_b4b3_call_us_sort.begin(),
            s_b4b3_call_us_sort.begin() + call_count);
  std::sort(s_b4b3_hop_us_sort.begin(),
            s_b4b3_hop_us_sort.begin() + hop_cost_count);

  const double wall_us = static_cast<double>(window_end_us - window_start_us);
  const double wall_seconds = wall_us / 1000000.0;
  const double average_call_us =
      call_count ? static_cast<double>(call_total_us) / call_count : 0.0;
  const double average_hop_us =
      completed_hops ? static_cast<double>(call_total_us) / completed_hops
                     : 0.0;
  const double maximum_theoretical_hops =
      average_hop_us > 0.0 ? 1000000.0 / average_hop_us : 0.0;

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  const double worker_runtime_us = static_cast<uint32_t>(
      worker_runtime_end - worker_runtime_start);
  const double diagnostic_runtime_us = static_cast<uint32_t>(
      diagnostic_runtime_end - diagnostic_runtime_start);
  const double idle1_runtime_us =
      static_cast<uint32_t>(idle1_runtime_end - idle1_runtime_start);
#else
  const double worker_runtime_us = call_total_us;
  const double diagnostic_runtime_us = 0.0;
  const double idle1_runtime_us = 0.0;
#endif
  const double other_core1_us = std::max(
      0.0, wall_us - worker_runtime_us - diagnostic_runtime_us -
               idle1_runtime_us);
  const double core1_utilization =
      wall_us > 0.0 ? 100.0 * (wall_us - idle1_runtime_us) / wall_us : 0.0;
  const double worker_utilization =
      wall_us > 0.0 ? 100.0 * worker_runtime_us / wall_us : 0.0;

  const auto pitch = [&](PitchAnalysisProfileSection section) -> const VocalFxProfileStats & {
    return pitch_profile[static_cast<size_t>(section)];
  };
  const auto lpc = [&](LpcProfileSection section) -> const VocalFxProfileStats & {
    return lpc_profile[static_cast<size_t>(section)];
  };
  struct ProfileRow {
    const char *name;
    const VocalFxProfileStats *stats;
  };
  const std::array<ProfileRow, 14> rows{{
      {"FIFO drain", &pitch(PitchAnalysisProfileSection::FifoDrain)},
      {"Rolling window", &pitch(PitchAnalysisProfileSection::RollingWindow)},
      {"Window copy", &pitch(PitchAnalysisProfileSection::LinearWindowCopy)},
      {"YinEnergy", &pitch(PitchAnalysisProfileSection::YinEnergy)},
      {"YinDifference", &pitch(PitchAnalysisProfileSection::YinDifference)},
      {"YinCMND", &pitch(PitchAnalysisProfileSection::YinCmnd)},
      {"YinSearch", &pitch(PitchAnalysisProfileSection::YinSearch)},
      {"YinInterpolation", &pitch(PitchAnalysisProfileSection::YinInterpolation)},
      {"VoicedFeatures", &pitch(PitchAnalysisProfileSection::VoicedFeatures)},
      {"VoicedClassifier", &pitch(PitchAnalysisProfileSection::VoicedClassifier)},
      {"PitchSmoother", &pitch(PitchAnalysisProfileSection::PitchSmoother)},
      {"PitchMarkSearch", &pitch(PitchAnalysisProfileSection::PitchMarkSearch)},
      {"PitchPublication", &pitch(PitchAnalysisProfileSection::PitchPublication)},
      {"LPC", &lpc(LpcProfileSection::Total)},
  }};
  uint64_t accounted_us = 0;
  for (const auto &row : rows)
    accounted_us += row.stats->total_us;
  const uint64_t unaccounted_us =
      call_total_us > accounted_us ? call_total_us - accounted_us : 0;
  const double accounted_percent =
      call_total_us ? 100.0 * accounted_us / call_total_us : 0.0;

  const auto &yin_difference =
      pitch(PitchAnalysisProfileSection::YinDifference);
  const auto &mark_search =
      pitch(PitchAnalysisProfileSection::PitchMarkSearch);
  const double yin_difference_percent =
      call_total_us ? 100.0 * yin_difference.total_us / call_total_us : 0.0;
  const double mark_search_percent =
      call_total_us ? 100.0 * mark_search.total_us / call_total_us : 0.0;
  const double lpc_percent =
      call_total_us ? 100.0 * lpc(LpcProfileSection::Total).total_us /
                          call_total_us
                    : 0.0;
  const char *primary_hotspot = "UNIDENTIFIED";
  const ProfileRow *largest = nullptr;
  for (const auto &row : rows)
    if (!largest || row.stats->total_us > largest->stats->total_us)
      largest = &row;
  if (yin_difference_percent > 50.0)
    primary_hotspot = "PRIMARY_HOTSPOT_YIN_DIFFERENCE";
  else if (largest && largest->name == std::string_view("LPC"))
    primary_hotspot = "PRIMARY_HOTSPOT_LPC";
  else if (largest)
    primary_hotspot = largest->name;
  const bool mark_secondary = mark_search_percent > 30.0;
  const bool lpc_significant = lpc_percent >= 10.0;
  const bool unaccounted = accounted_percent < 95.0;
  const bool audit_pass = !unaccounted && largest != nullptr && teardown_clean;

  const uint64_t input_samples =
      audit_end.audio_input_position - audit_start.audio_input_position;
  const uint64_t fifo_pushes = audit_end.fifo_pushes - audit_start.fifo_pushes;
  const uint64_t expected_fifo_pushes = input_samples / 4;
  const double audio_seconds = input_samples / 48000.0;
  const double fifo_difference_percent =
      expected_fifo_pushes
          ? 100.0 * (static_cast<double>(fifo_pushes) -
                     static_cast<double>(expected_fifo_pushes)) /
                expected_fifo_pushes
          : 0.0;
  const uint64_t yin_executions = yin_end.executions - yin_start.executions;
  const uint64_t yin_inner =
      yin_end.actual_inner_iterations - yin_start.actual_inner_iterations;
  const uint64_t yin_expected =
      yin_end.expected_inner_iterations - yin_start.expected_inner_iterations;
  const uint64_t mark_hops = marks_end.pitch_hops - marks_start.pitch_hops;
  const uint64_t mark_correlations =
      marks_end.correlation_searches - marks_start.correlation_searches;
  const uint64_t mark_candidates =
      marks_end.candidate_offsets_evaluated -
      marks_start.candidate_offsets_evaluated;
  const uint64_t mark_samples =
      marks_end.sample_pairs_correlated - marks_start.sample_pairs_correlated;
  const uint64_t mark_macs =
      marks_end.mac_like_operations - marks_start.mac_like_operations;

  printf("\n=======================================================================\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  printf("Stage B4B.4A — LPC Ring Buffer Optimization Report\n");
#else
  printf("Stage B4B.3 — Pitch Analysis Compute Hotspot Audit Report\n");
#endif
  printf("=======================================================================\n");
  printf("Measurement window: wall=%.6f s audio=%.6f s CPU=360 MHz\n",
         wall_seconds, audio_seconds);
  printf("Required throughput=%.2f hops/s; absolute budget=%.2f us/hop\n",
         kRequiredHopsPerSecond, kBudgetUsPerHop);
  printf("\nWorker call duration (us): avg=%.2f P50=%lu P95=%lu P99=%lu max=%lu calls=%zu record_drops=%lu\n",
         average_call_us,
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_call_us_sort.data(), call_count, 50)),
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_call_us_sort.data(), call_count, 95)),
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_call_us_sort.data(), call_count, 99)),
         static_cast<unsigned long>(call_max_us), call_count,
         static_cast<unsigned long>(s_pitch_worker_call_record_drops.load()));
  printf("Worker call cycles: total=%llu avg/call=%.0f avg/hop=%.0f\n",
         static_cast<unsigned long long>(call_total_cycles),
         call_count ? static_cast<double>(call_total_cycles) / call_count : 0.0,
         completed_hops ? static_cast<double>(call_total_cycles) / completed_hops
                        : 0.0);
  printf("Completed-hop cost (us): avg=%.2f P50=%lu P95=%lu P99=%lu max=%lu hops=%llu\n",
         average_hop_us,
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_hop_us_sort.data(), hop_cost_count, 50)),
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_hop_us_sort.data(), hop_cost_count, 95)),
         static_cast<unsigned long>(percentile_sorted(
             s_b4b3_hop_us_sort.data(), hop_cost_count, 99)),
         static_cast<unsigned long>(hop_max_us),
         static_cast<unsigned long long>(completed_hops));
  printf("Calls by hops returned:\n");
  for (size_t hops_returned = 0; hops_returned <= 4; ++hops_returned)
    printf("  %zu hops: calls=%llu avg_us=%.2f\n", hops_returned,
           static_cast<unsigned long long>(class_calls[hops_returned]),
           class_calls[hops_returned]
               ? static_cast<double>(class_total_us[hops_returned]) /
                     class_calls[hops_returned]
               : 0.0);

  printf("\nSECTION                    AVG US/HOP    %% OF PITCH CPU   TOTAL US    AVG US/CALL   MAX US      TOTAL CYCLES  AVG CYCLES/HOP\n");
  for (const auto &row : rows) {
    const double avg_hop = completed_hops
                               ? static_cast<double>(row.stats->total_us) /
                                     completed_hops
                               : 0.0;
    const double percent = call_total_us
                               ? 100.0 * row.stats->total_us / call_total_us
                               : 0.0;
    const double avg_call = row.stats->blocks
                                ? static_cast<double>(row.stats->total_us) /
                                      row.stats->blocks
                                : 0.0;
    const double cycles_hop = completed_hops
                                  ? static_cast<double>(row.stats->total_cycles) /
                                        completed_hops
                                  : 0.0;
    printf("%-26s %11.2f %16.2f %10llu %14.2f %10llu %17llu %15.0f\n",
           row.name, avg_hop, percent,
           static_cast<unsigned long long>(row.stats->total_us), avg_call,
           static_cast<unsigned long long>(row.stats->worst_us),
           static_cast<unsigned long long>(row.stats->total_cycles),
           cycles_hop);
  }
  printf("%-26s %11.2f %16.2f %10llu\n", "Other/unaccounted",
         completed_hops ? static_cast<double>(unaccounted_us) / completed_hops
                        : 0.0,
         call_total_us ? 100.0 * unaccounted_us / call_total_us : 0.0,
         static_cast<unsigned long long>(unaccounted_us));
  printf("TOTAL                      %11.2f %16.2f %10llu\n",
         average_hop_us, accounted_percent,
         static_cast<unsigned long long>(call_total_us));
  printf("Time reconciliation: %.2f%% accounted (%s)\n", accounted_percent,
         accounted_percent >= 95.0 ? "PASS" : "FAIL");

  const auto &pitch_total = pitch(PitchAnalysisProfileSection::RunTotal);
  const auto &yin_total = pitch(PitchAnalysisProfileSection::YinTotal);
  const auto &lpc_total = lpc(LpcProfileSection::Total);
  printf("\nPitchAnalysis run total: total=%llu us avg/call=%.2f avg/hop=%.2f max=%llu us cycles=%llu\n",
         static_cast<unsigned long long>(pitch_total.total_us),
         pitch_total.blocks
             ? static_cast<double>(pitch_total.total_us) / pitch_total.blocks
             : 0.0,
         completed_hops
             ? static_cast<double>(pitch_total.total_us) / completed_hops
             : 0.0,
         static_cast<unsigned long long>(pitch_total.worst_us),
         static_cast<unsigned long long>(pitch_total.total_cycles));
  printf("YIN total: total=%llu us avg/hop=%.2f max=%llu us cycles=%llu\n",
         static_cast<unsigned long long>(yin_total.total_us),
         completed_hops ? static_cast<double>(yin_total.total_us) /
                              completed_hops
                        : 0.0,
         static_cast<unsigned long long>(yin_total.worst_us),
         static_cast<unsigned long long>(yin_total.total_cycles));
  printf("LPC run total: total=%llu us avg/call=%.2f avg/pitch-hop=%.2f max=%llu us cycles=%llu\n",
         static_cast<unsigned long long>(lpc_total.total_us),
         lpc_total.blocks
             ? static_cast<double>(lpc_total.total_us) / lpc_total.blocks
             : 0.0,
         completed_hops
             ? static_cast<double>(lpc_total.total_us) / completed_hops
             : 0.0,
         static_cast<unsigned long long>(lpc_total.worst_us),
         static_cast<unsigned long long>(lpc_total.total_cycles));
  printf("LPC detail us: fifo_drain=%llu ring_write=%llu linearization=%llu solve_windowing=%llu solve_total=%llu autocorrelation=%llu Levinson-Durbin=%llu publication=%llu\n",
         static_cast<unsigned long long>(lpc(LpcProfileSection::FifoDrain).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::RingWrite).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::FrameLinearization).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::SolveWindowing).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::SolveTotal).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::Autocorrelation).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::LevinsonDurbin).total_us),
         static_cast<unsigned long long>(lpc(LpcProfileSection::Publication).total_us));

  const uint64_t lpc_frames =
      lpc_telemetry_end.lpc_frames - lpc_telemetry_start.lpc_frames;
  const uint64_t lpc_samples =
      lpc_telemetry_end.samples_drained - lpc_telemetry_start.samples_drained;
  const double lpc_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc_total.total_us) / lpc_frames : 0.0;
  const double ring_us_per_sample =
      lpc_samples ? static_cast<double>(lpc(LpcProfileSection::RingWrite).total_us) /
                        lpc_samples
                  : 0.0;
  const double linear_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc(LpcProfileSection::FrameLinearization).total_us) /
                       lpc_frames
                 : 0.0;
  const double autocorrelation_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc(LpcProfileSection::Autocorrelation).total_us) /
                       lpc_frames
                 : 0.0;
  const double levinson_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc(LpcProfileSection::LevinsonDurbin).total_us) /
                       lpc_frames
                 : 0.0;
  const double publication_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc(LpcProfileSection::Publication).total_us) /
                       lpc_frames
                 : 0.0;
  const double fifo_avg_frame_us =
      lpc_frames ? static_cast<double>(lpc(LpcProfileSection::FifoDrain).total_us) /
                       lpc_frames
                 : 0.0;
  const double new_window_handling_us_per_frame =
      lpc_frames
          ? static_cast<double>(
                lpc(LpcProfileSection::FifoDrain).total_us +
                lpc(LpcProfileSection::RingWrite).total_us +
                lpc(LpcProfileSection::FrameLinearization).total_us) /
                lpc_frames
          : 0.0;
  const double pitch_hops_per_second =
      wall_seconds > 0.0 ? completed_hops / wall_seconds : 0.0;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  const double window_speedup = new_window_handling_us_per_frame > 0.0
                                    ? kBeforeWindowDrainUsPerFrame /
                                          new_window_handling_us_per_frame
                                    : 0.0;
  const double lpc_speedup = lpc_avg_frame_us > 0.0
                                 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us
                                 : 0.0;
  const double combined_speedup = average_hop_us > 0.0
                                      ? kBeforeCombinedUsPerHop / average_hop_us
                                      : 0.0;
  const bool no_transport_regression =
      transport_end.rx_dma_errors == transport_start.rx_dma_errors &&
      transport_end.tx_dma_errors == transport_start.tx_dma_errors &&
      transport_end.read_failures == transport_start.read_failures &&
      transport_end.write_failures == transport_start.write_failures &&
      transport_end.rx_dropped_frames == transport_start.rx_dropped_frames &&
      transport_end.tx_dropped_frames == transport_start.tx_dropped_frames;
  const bool b4b4a_pass =
      audit_pass && no_transport_regression && lpc_frames > 0 &&
      lpc_frame_cost.count > 0 &&
      new_window_handling_us_per_frame < kBeforeWindowDrainUsPerFrame &&
      lpc_avg_frame_us < kBeforeLpcUsPerFrame;
#endif
  printf("LPC frame metrics: frames=%llu samples_drained=%llu avg_us/frame=%.2f P50_upper_bound=%lu P95_upper_bound=%lu P99_upper_bound=%lu max=%lu recorded_frames=%lu quantile_bin_us=100\n",
         static_cast<unsigned long long>(lpc_frames),
         static_cast<unsigned long long>(lpc_samples), lpc_avg_frame_us,
         static_cast<unsigned long>(lpc_frame_cost.p50_us),
         static_cast<unsigned long>(lpc_frame_cost.p95_us),
         static_cast<unsigned long>(lpc_frame_cost.p99_us),
         static_cast<unsigned long>(lpc_frame_cost.max_us),
         static_cast<unsigned long>(lpc_frame_cost.count));
  printf("LPC ring metrics: ring_total_us=%llu ring_us/sample=%.5f linearization_avg_us/frame=%.2f linearization_max_us=%llu solve_avg_us/frame=%.2f autocorrelation_avg_us/frame=%.2f Levinson_avg_us/frame=%.2f publication_avg_us/frame=%.2f\n",
         static_cast<unsigned long long>(lpc(LpcProfileSection::RingWrite).total_us),
         ring_us_per_sample, linear_avg_frame_us,
         static_cast<unsigned long long>(lpc(LpcProfileSection::FrameLinearization).worst_us),
         lpc_frames ? static_cast<double>(lpc(LpcProfileSection::SolveTotal).total_us) / lpc_frames : 0.0,
         autocorrelation_avg_frame_us, levinson_avg_frame_us,
         publication_avg_frame_us);
  printf("LPC separated storage path: FIFO_drain_avg_us/frame=%.2f ring_plus_FIFO_plus_linearization_avg_us/frame=%.2f\n",
         fifo_avg_frame_us, new_window_handling_us_per_frame);
  printf("OLD std::move isolated benchmark: shifted_samples=%lu total_us=%llu us/shifted_sample=%.5f cycles/shifted_sample=%.2f checksum=%.3f\n",
         static_cast<unsigned long>(old_storage.shifted_samples),
         static_cast<unsigned long long>(old_storage.total_us),
         old_storage.shifted_samples
             ? static_cast<double>(old_storage.total_us) / old_storage.shifted_samples
             : 0.0,
         old_storage.shifted_samples
             ? static_cast<double>(old_storage.total_cycles) / old_storage.shifted_samples
             : 0.0,
         old_storage.checksum);

  printf("\nYinDifference forensic: tau_min=%lu tau_max=%lu window=%lu tau_values=%lu executions=%llu\n",
         static_cast<unsigned long>(yin_end.tau_min),
         static_cast<unsigned long>(yin_end.tau_max),
         static_cast<unsigned long>(yin_end.window_size),
         static_cast<unsigned long>(yin_end.tau_values_per_execution),
         static_cast<unsigned long long>(yin_executions));
  printf("YinDifference inner iterations: actual=%llu theoretical=%llu difference=%lld cycles/inner=%.3f\n",
         static_cast<unsigned long long>(yin_inner),
         static_cast<unsigned long long>(yin_expected),
         static_cast<long long>(yin_inner) - static_cast<long long>(yin_expected),
         yin_inner ? static_cast<double>(yin_difference.total_cycles) / yin_inner
                   : 0.0);
  const auto &mark_correlation =
      pitch(PitchAnalysisProfileSection::PitchMarkCorrelation);
  printf("Pitch-mark forensic: pitch_hops=%llu searches=%llu searches/hop=%.4f candidate_offsets=%llu samples_correlated=%llu MAC_like_ops=%llu correlation_us/hop=%.3f\n",
         static_cast<unsigned long long>(mark_hops),
         static_cast<unsigned long long>(mark_correlations),
         mark_hops ? static_cast<double>(mark_correlations) / mark_hops : 0.0,
         static_cast<unsigned long long>(mark_candidates),
         static_cast<unsigned long long>(mark_samples),
         static_cast<unsigned long long>(mark_macs),
         completed_hops
             ? static_cast<double>(mark_correlation.total_us) / completed_hops
             : 0.0);

  printf("\nCore 1 utilization: total=%.2f%% worker=%.2f%% compute_saturated=%s\n",
         core1_utilization, worker_utilization,
         core1_utilization >= 90.0 ? "yes" : "no");
  printf("Core 1 time us: worker=%0.f requested_yield_min=%llu non_worker=%0.f diagnostic=%0.f IDLE1=%0.f other=%0.f\n",
         worker_runtime_us,
         static_cast<unsigned long long>(call_count * 1000ULL),
         wall_us - worker_runtime_us, diagnostic_runtime_us, idle1_runtime_us,
         other_core1_us);
  printf("Relevant preemption evidence: other Core 1 tasks %.2f%%; pitch task priority unchanged\n",
         wall_us ? 100.0 * other_core1_us / wall_us : 0.0);

  printf("\nFIFO producer reconciliation (test-relative deltas): wall=%.6f s audio=%.6f s blocks=%llu input_samples=%llu pushes=%llu expected_from_audio=%llu difference=%+.5f%%\n",
         wall_seconds, audio_seconds,
         static_cast<unsigned long long>(transport_end.blocks -
                                         transport_start.blocks),
         static_cast<unsigned long long>(input_samples),
         static_cast<unsigned long long>(fifo_pushes),
         static_cast<unsigned long long>(expected_fifo_pushes),
         fifo_difference_percent);
  printf("Pitch throughput/backlog: worker_calls/s=%.2f pitch_hops/s=%.2f FIFO_current=%lu FIFO_max=%lu FIFO_drops_delta=%llu FIFO_pops_delta=%llu backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         wall_seconds > 0.0 ? call_count / wall_seconds : 0.0,
         wall_seconds > 0.0 ? completed_hops / wall_seconds : 0.0,
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops - audit_start.fifo_drops),
         static_cast<unsigned long long>(audit_end.fifo_pops - audit_start.fifo_pops),
         audit_end.analysis_backlog_ms, audit_end.analysis_backlog_max_ms,
         audit_end.latest_pitch_age_ms, audit_end.pitch_age_average_ms,
         audit_end.pitch_age_p95_ms, audit_end.pitch_age_p99_ms,
         audit_end.pitch_age_max_ms);
  printf("Transport deltas: diagnostic_q_overflow RX/TX=%llu/%llu actual_DMA_errors=%llu/%llu read/write_failures=%llu/%llu dropped_frames=%llu/%llu\n",
         static_cast<unsigned long long>(transport_end.rx_queue_overflows - transport_start.rx_queue_overflows),
         static_cast<unsigned long long>(transport_end.tx_queue_overflows - transport_start.tx_queue_overflows),
         static_cast<unsigned long long>(transport_end.rx_dma_errors - transport_start.rx_dma_errors),
         static_cast<unsigned long long>(transport_end.tx_dma_errors - transport_start.tx_dma_errors),
         static_cast<unsigned long long>(transport_end.read_failures - transport_start.read_failures),
         static_cast<unsigned long long>(transport_end.write_failures - transport_start.write_failures),
         static_cast<unsigned long long>(transport_end.rx_dropped_frames - transport_start.rx_dropped_frames),
         static_cast<unsigned long long>(transport_end.tx_dropped_frames - transport_start.tx_dropped_frames));

  printf("\nHotspot classification: %s\n", primary_hotspot);
  printf("Secondary mark classification: %s\n",
         mark_secondary ? "SECONDARY_HOTSPOT_MARK_CORRELATION" : "not triggered");
  printf("LPC classification: %s (%.2f%%)\n",
         lpc_significant ? "LPC_COMPUTE_SIGNIFICANT" : "not significant",
         lpc_percent);
  printf("Unaccounted classification: %s\n",
         unaccounted ? "UNACCOUNTED_CORE1_COST" : "not triggered");
  printf("ESP32-P4 rev1.3 anomaly evidence: none specific; measured cost is section-local compute\n");
  printf("Real I2S/DMA failure: %s\n",
         (transport_end.rx_dma_errors == transport_start.rx_dma_errors &&
          transport_end.tx_dma_errors == transport_start.tx_dma_errors &&
          transport_end.read_failures == transport_start.read_failures &&
          transport_end.write_failures == transport_start.write_failures)
             ? "no"
             : "yes");
  printf("Teardown clean: %s; spinlock/reboot/callback-after-free: %s\n",
         teardown_clean ? "yes" : "no",
         teardown_clean ? "not observed" : "teardown incomplete");
  printf("Stage diagnostic PASS: %s\n", audit_pass ? "yes" : "no");

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  printf("\nLPC memory placement:\n");
  for (const char *requested : {"LpcCircularFrame", "LpcLinearFrame",
                                "LpcFifo"}) {
    const VocalFxBufferAudit *found = nullptr;
    for (size_t i = 0; i < buffer_audit_count; ++i) {
      if (std::string_view(buffer_audits[i].name) == requested) {
        found = &buffer_audits[i];
        break;
      }
    }
    if (found) {
      const char *placement = found->is_psram
                                  ? "PSRAM"
                                  : (found->is_sram ? "internal SRAM" : "other");
      printf("  %s: ptr=%p bytes=%zu placement=%s\n", found->name,
             found->ptr, found->size_bytes, placement);
    } else {
      printf("  %s: not found\n", requested);
    }
  }

  const double new_pitch_analysis_us_per_hop =
      completed_hops
          ? static_cast<double>(pitch_total.total_us) / completed_hops
          : 0.0;
  const double new_yin_difference_us_per_hop =
      completed_hops
          ? static_cast<double>(yin_difference.total_us) / completed_hops
          : 0.0;
  const double old_isolated_us_per_shift =
      old_storage.shifted_samples
          ? static_cast<double>(old_storage.total_us) /
                old_storage.shifted_samples
          : 0.0;
  const double old_isolated_us_per_steady_frame =
      old_isolated_us_per_shift * 384.0;
  printf("\nB4B.4A isolated old-storage result: exact_std_move_us/sample=%.5f equivalent_steady_state_us/384_samples=%.2f\n",
         old_isolated_us_per_shift, old_isolated_us_per_steady_frame);
  printf("B4B.3 measured old window/drain (FIFO plus sliding storage): total=6288678 us frames=212 avg=%.2f us/frame\n",
         kBeforeWindowDrainUsPerFrame);

  printf("\nSECTION                                  BEFORE          AFTER        SPEEDUP\n");
  printf("FIFO drain (us/LPC frame)                    n/a %14.2f            n/a\n",
         fifo_avg_frame_us);
  printf("LPC window handling (us/frame)          %9.2f %14.2f %11.2fx\n",
         kBeforeWindowDrainUsPerFrame, new_window_handling_us_per_frame,
         window_speedup);
  printf("LPC autocorrelation (us/frame)          %9.2f %14.2f %11.2fx\n",
         kBeforeAutocorrelationUsPerFrame, autocorrelation_avg_frame_us,
         autocorrelation_avg_frame_us > 0.0
             ? kBeforeAutocorrelationUsPerFrame /
                   autocorrelation_avg_frame_us
             : 0.0);
  printf("LPC Levinson-Durbin (us/frame)          %9.2f %14.2f %11.2fx\n",
         kBeforeLevinsonUsPerFrame, levinson_avg_frame_us,
         levinson_avg_frame_us > 0.0
             ? kBeforeLevinsonUsPerFrame / levinson_avg_frame_us
             : 0.0);
  printf("LPC publication (us/frame)              %9.2f %14.2f %11.2fx\n",
         kBeforePublicationUsPerFrame, publication_avg_frame_us,
         publication_avg_frame_us > 0.0
             ? kBeforePublicationUsPerFrame / publication_avg_frame_us
             : 0.0);
  printf("LPC total (us/frame)                    %9.2f %14.2f %11.2fx\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us, lpc_speedup);
  printf("YinDifference (us/pitch hop)            %9.2f %14.2f %11.2fx\n",
         kBeforeYinDifferenceUsPerHop, new_yin_difference_us_per_hop,
         new_yin_difference_us_per_hop > 0.0
             ? kBeforeYinDifferenceUsPerHop /
                   new_yin_difference_us_per_hop
             : 0.0);
  printf("PitchAnalysis total (us/pitch hop)      %9.2f %14.2f %11.2fx\n",
         kBeforePitchAnalysisUsPerHop, new_pitch_analysis_us_per_hop,
         new_pitch_analysis_us_per_hop > 0.0
             ? kBeforePitchAnalysisUsPerHop /
                   new_pitch_analysis_us_per_hop
             : 0.0);
  printf("Combined worker cost (us/pitch hop)     %9.2f %14.2f %11.2fx\n",
         kBeforeCombinedUsPerHop, average_hop_us, combined_speedup);
  printf("Core 1 utilization (percent)             %8.2f %14.2f %11.2fx\n",
         kBeforeCore1Percent, core1_utilization,
         core1_utilization > 0.0 ? kBeforeCore1Percent / core1_utilization
                                 : 0.0);
  printf("Pitch throughput (hops/s)                %8.2f %14.2f %11.2fx\n",
         kBeforePitchHopsPerSecond, pitch_hops_per_second,
         kBeforePitchHopsPerSecond > 0.0
             ? pitch_hops_per_second / kBeforePitchHopsPerSecond
             : 0.0);
  printf("FIFO/backlog AFTER: current=%lu max=%lu drops=%llu backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops -
                                         audit_start.fifo_drops),
         audit_end.analysis_backlog_ms, audit_end.analysis_backlog_max_ms,
         audit_end.latest_pitch_age_ms, audit_end.pitch_age_average_ms,
         audit_end.pitch_age_p95_ms, audit_end.pitch_age_p99_ms,
         audit_end.pitch_age_max_ms);

  printf("\nB4B.4A RESULT:\n%s\n", b4b4a_pass ? "PASS" : "FAIL");
  printf("LPC BEFORE:\n%.2f us/frame\n", kBeforeLpcUsPerFrame);
  printf("LPC AFTER:\n%.2f us/frame\n", lpc_avg_frame_us);
  printf("LPC SPEEDUP:\n%.2fx\n", lpc_speedup);
  printf("CORE1 BEFORE:\n%.2f%%\n", kBeforeCore1Percent);
  printf("CORE1 AFTER:\n%.2f%%\n", core1_utilization);
  printf("NEXT PRIMARY HOTSPOT:\n%s\n", primary_hotspot);
  printf("NEXT RECOMMENDED STEP:\nB4B.4B\n");
#else

  printf("\nThroughput maximum theoretical: %.2f hops/s\n",
         maximum_theoretical_hops);
  printf("Speedup required: 200 hops/s=%.3fx 90%% utilization=%.3fx 80%% utilization=%.3fx 70%% utilization=%.3fx\n",
         average_hop_us / 5000.0, average_hop_us / 4500.0,
         average_hop_us / 4000.0, average_hop_us / 3500.0);
  printf("\nROOT CAUSE:\n%s\n", primary_hotspot);
  printf("CURRENT COST:\n%.2f us/hop\n", average_hop_us);
  printf("REQUIRED:\n<5000 us/hop absolute maximum\n");
  printf("RECOMMENDED TARGET:\n<=3500-4000 us/hop\n");
  printf("REQUIRED SPEEDUP:\n%.3fx absolute; %.3fx-%.3fx for 20-30%% headroom\n",
         average_hop_us / 5000.0, average_hop_us / 4000.0,
         average_hop_us / 3500.0);
  printf("NEXT MILESTONE:\nB4B.4 — targeted pitch-worker optimization\n");
  printf("B4B.4 implemented: no\n");
#endif
  printf("=======================================================================\n");
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t normal_wdt_config = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&normal_wdt_config);
#endif
}

void run_i2s_stage_b4c_real_analog(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B4C: REAL ANALOG LIVE MUSICAL DSP             \n");
  printf("=======================================================\n");
  printf("Input: Real Analog Microphone / Instrument on PCM1808\n");
  printf("Output: Real Analog Output on PCM5102\n");
  printf("DSP Engine: Full VoxP4 Live DSP (Active)\n");
  printf("Status: Preserved for live musical evaluation.\n");

  VocalFxConfig cfg{};
  if (!vocal_fx_init(cfg)) {
    ESP_LOGE(TAG, "Vocal FX engine initialization failed");
    return;
  }

  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 1.0f);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 2.5f);

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::FullDsp, true)) {
    printf("STAGE B4C: FAILED TO INITIALIZE AUDIO DRIVER\n");
    return;
  }

  // Run live processing until aborted
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    s_audio.print_telemetry_summary();
  }
}

void run_i2s_stage_b4_full_dsp(void) {
  run_i2s_stage_b4a_quiescent();
}

void run_i2s_latency_pulse_test(void) {
  printf("\n=======================================================\n");
  printf("   HARDWARE LATENCY PULSE TEST START (30 SEC)          \n");
  printf("=======================================================\n");
  printf("Generating periodic sharp impulses (every 500 ms) on DAC output.\n");
  printf("Connect oscilloscope CH1 to ADC Input (or pulse source) and CH2 to DAC Output.\n");

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::LatencyPulse, false)) {
    printf("LATENCY TEST: FAILED TO START AUDIO PIPELINE\n");
    return;
  }

  vTaskDelay(pdMS_TO_TICKS(30000));
  stop_pipeline_tasks();

  printf("=======================================================\n");
  printf("   HARDWARE LATENCY PULSE TEST COMPLETE                \n");
  printf("=======================================================\n\n");
}

void run_i2s_sample_format_forensic(void) {
  printf("\n=======================================================\n");
  printf("   SAMPLE FORMAT FORENSIC TEST START                   \n");
  printf("=======================================================\n");

  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::SampleForensic, false)) {
    printf("FORENSIC TEST: FAILED TO START AUDIO PIPELINE\n");
    return;
  }

  // Wait for 256 words to be captured
  vTaskDelay(pdMS_TO_TICKS(500));
  s_audio.dump_sample_forensic();
  stop_pipeline_tasks();
}

void run_i2s_bringup_selected_mode(void) {
#if defined(CONFIG_VOXP4_MODE_I2S_TX_TEST)
  run_i2s_stage_b1_dac_bringup();
#elif defined(CONFIG_VOXP4_MODE_I2S_RX_TEST)
  run_i2s_stage_b2_adc_bringup();
#elif defined(CONFIG_VOXP4_MODE_I2S_BYPASS)
  run_i2s_stage_b3_bypass_stress();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4A_QUIESCENT)
  run_i2s_stage_b4a_quiescent();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_SYNTHETIC_VOICED)
  run_i2s_stage_b4b_synthetic_voiced();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_2_PITCH_WORKER_LOCK_AUDIT)
  if (xTaskCreatePinnedToCore(b4b2_coordinator_task, "b4b2_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.2 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_3_PITCH_ANALYSIS_HOTSPOT_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b3_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.3 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4a_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4A low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_REAL_ANALOG)
  run_i2s_stage_b4c_real_analog();
#elif defined(CONFIG_VOXP4_MODE_I2S_FULL_DSP)
  run_i2s_stage_b4a_quiescent();
#elif defined(CONFIG_VOXP4_MODE_LATENCY_PULSE_TEST)
  run_i2s_latency_pulse_test();
#elif defined(CONFIG_VOXP4_MODE_SAMPLE_FORMAT_FORENSIC)
  run_i2s_sample_format_forensic();
#else
  // Default to B4A Quiescent if no specific bring-up mode selected
  run_i2s_stage_b4a_quiescent();
#endif
}
