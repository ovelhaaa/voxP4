#include "i2s_bringup.h"
#include "vocal_fx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cmath>
#include <cstdio>

namespace {

const char *TAG = "i2s_bringup";
vocal_fx_platform::AudioI2s s_audio;
TaskHandle_t s_audio_task_handle = nullptr;
TaskHandle_t s_pitch_task_handle = nullptr;
std::atomic<bool> s_pitch_running{false};

void audio_task_entry(void *) {
  s_audio.run();
  vTaskDelete(nullptr);
}

void pitch_worker_task(void *) {
  while (s_pitch_running.load(std::memory_order_relaxed)) {
    if (vocal_fx_run_pitch_analysis(4)) {
      vTaskDelay(pdMS_TO_TICKS(1)); // Allow IDLE1 to run and feed watchdog
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  vTaskDelete(nullptr);
}

bool start_pipeline_tasks(vocal_fx_platform::AudioI2sMode mode, bool start_pitch = true) {
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
    }
  }

  return true;
}

void stop_pipeline_tasks() {
  s_pitch_running.store(false, std::memory_order_release);
  s_audio.stop();
  vTaskDelay(pdMS_TO_TICKS(100));
  s_audio.deinit();
  s_audio_task_handle = nullptr;
  s_pitch_task_handle = nullptr;
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

void run_i2s_stage_b4c_real_analog(void) {
  printf("\n=======================================================\n");
  printf("   STAGE B4C: REAL ANALOG LIVE MUSICAL DSP (READY)     \n");
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
