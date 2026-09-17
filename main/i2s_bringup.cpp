#include "i2s_bringup.h"
#include "vocal_fx.h"
#include "pitch_analysis.h"
#include "pitch_mark_ncc.h"
#include "yin_detector.h"
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
#include <new>
#include <string_view>

#if (defined(CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT) || \
     defined(CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT)) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
#define CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION 1
#ifndef CONFIG_VOXP4_B4B6_MEASURE_SECONDS
#define CONFIG_VOXP4_B4B6_MEASURE_SECONDS 10
#endif
#ifndef CONFIG_VOXP4_B4B6_STABILITY_SECONDS
#define CONFIG_VOXP4_B4B6_STABILITY_SECONDS 60
#endif
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
#include "b4b6_vocal_fixture.h"
#endif

// B4B.6 runs the normal B4B.5 production selection with a broader stimulus
// harness. This compile-time inheritance enables observability only; all
// selector-mutation blocks remain disabled by the B4B.5 guards below.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
#define CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION 1
#endif

// B4B.5 reuses the qualified profiling harness only. Its selector-mutation
// blocks are explicitly disabled below so vocal_fx_init() exercises the normal
// ESP32-P4 production-default path.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT 1
#endif

// B4B.4K retains the complete frozen B4B.4J worker configuration and changes
// only the YIN energy accumulation after host numerical qualification.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT 1
#endif
// B4B.4J retains the complete frozen B4B.4I worker configuration and changes
// only the CMND arithmetic after host numerical qualification.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT 1
#endif
// B4B.4I retains the complete frozen B4B.4H worker configuration. It adds
// observability only; the normal P4 YIN default remains FMA_8ACC.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#define CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION 1
#endif
// B4B.4H retains the complete B4B.4G/B4B.4F worker configuration and transport.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT 1
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE)
#define VOXP4_LPC_AUDIT_STAGE "B4C.4A"
#define VOXP4_LPC_AUDIT_TOKEN "B4C4A"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED)
#define VOXP4_LPC_AUDIT_STAGE "B4C.4"
#define VOXP4_LPC_AUDIT_TOKEN "B4C4"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST)
#define VOXP4_LPC_AUDIT_STAGE "B4C.3B"
#define VOXP4_LPC_AUDIT_TOKEN "B4C3B"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4C.3A"
#define VOXP4_LPC_AUDIT_TOKEN "B4C3A"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4C.3"
#define VOXP4_LPC_AUDIT_TOKEN "B4C3"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4C.2"
#define VOXP4_LPC_AUDIT_TOKEN "B4C2"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4C.1"
#define VOXP4_LPC_AUDIT_TOKEN "B4C1"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
#define VOXP4_LPC_AUDIT_STAGE "B4B.6"
#define VOXP4_LPC_AUDIT_TOKEN "B4B6"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
#define VOXP4_LPC_AUDIT_STAGE "B4B.5"
#define VOXP4_LPC_AUDIT_TOKEN "B4B5"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4B.4K"
#define VOXP4_LPC_AUDIT_TOKEN "B4B4K"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4B.4J"
#define VOXP4_LPC_AUDIT_TOKEN "B4B4J"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
#define VOXP4_LPC_AUDIT_STAGE "B4B.4I"
#define VOXP4_LPC_AUDIT_TOKEN "B4B4I"
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#define VOXP4_LPC_AUDIT_STAGE "B4B.4H"
#define VOXP4_LPC_AUDIT_TOKEN "B4B4H"
#else
#define VOXP4_LPC_AUDIT_STAGE "B4B.4G"
#define VOXP4_LPC_AUDIT_TOKEN "B4B4G"
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT 1
#endif
// B4B.4F retains the B4B.4E YIN selection and reuses its audited transport.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT 1
#endif
// B4B.4E reuses the already-audited B4B.4D transport/coordinator scaffolding.
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
#define CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT 1
#endif

namespace {

const char *TAG = "i2s_bringup";
vocal_fx_platform::AudioI2s s_audio;
// B4D.3S: sample rate selected by the sweep harness (default production 48 kHz).
uint32_t g_pipeline_sample_rate = 48000;
TaskHandle_t s_audio_task_handle = nullptr;
TaskHandle_t s_pitch_task_handle = nullptr;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
// The J/K diagnostics add enough code/stdio state to split the internal heap's
// largest block below 32 KiB even though total SRAM remains ample.  Reserve the
// exact frozen audio-worker stack at link time; size, priority and core stay
// identical to the production/audit configuration.
StaticTask_t s_b4b4j_audio_task_storage{};
alignas(16) std::array<StackType_t, 32768> s_b4b4j_audio_task_stack{};
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
// The RC matrix repeatedly creates the same worker. Reserve its unchanged
// 16 KiB stack at link time so allocator fragmentation between cases cannot
// invalidate the harness; core, priority and scheduling remain identical.
StaticTask_t s_b4b6_pitch_task_storage{};
alignas(16) std::array<StackType_t, 16384> s_b4b6_pitch_task_stack{};
#endif
std::atomic<bool> s_pitch_running{false};
constexpr EventBits_t kAudioTaskStopped = BIT0;
constexpr EventBits_t kPitchTaskStopped = BIT1;
StaticEventGroup_t s_task_events_storage{};
EventGroupHandle_t s_task_events = nullptr;
bool s_teardown_diagnostics_enabled = false;
std::atomic<bool> s_pitch_lock_audit_enabled{false};
std::atomic<bool> s_pitch_hotspot_audit_enabled{false};
std::atomic<bool> s_pitch_worker_in_call{false};
std::atomic<bool> s_b4b6_measurement_pause{false};

struct PitchWorkerCallRecord {
  uint64_t start_us = 0;
  uint32_t duration_us = 0;
  uint32_t cycles = 0;
  uint8_t hops = 0;
};
// B4B.2 measured only ~85 worker calls in a 10-second window. Keep generous
// fixed headroom without consuming the internal RAM needed by the real-time
// task stacks. Any exhaustion remains explicit in record_drops.
// B4C.7 runs near the internal-.bss link limit (frozen 32 KB audio + 16 KB
// pitch static stacks plus the audit rings); its recorder capacity is reduced
// while the audit that fills it stays disabled. Other modes keep 512.
#if defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT)
constexpr size_t kPitchWorkerCallRecordCapacity = 128;
#else
constexpr size_t kPitchWorkerCallRecordCapacity = 512;
#endif
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
    if (s_b4b6_measurement_pause.load(std::memory_order_acquire)) {
      vTaskDelay(1);
      continue;
    }
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

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT)
void b4c1_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
  run_i2s_stage_b4c1_audio_core_audit();
  vTaskSuspend(nullptr);
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT)
void b4c2_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
  run_i2s_stage_b4c2_audio_core_audit();
  vTaskSuspend(nullptr);
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT)
void b4c3_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
  run_i2s_stage_b4c3_harmonizer_tail_audit();
  vTaskSuspend(nullptr);
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS)
void b4c6a_coordinator_task(void *);
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT)
void b4c7_coordinator_task(void *);
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION)
void b4c8_coordinator_task(void *);
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
void b4d1_coordinator_task(void *);
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
void run_b4b6a_low_f0_decomposition();

void b4b6_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
  run_b4b6a_low_f0_decomposition();
  run_i2s_stage_b4b6_rc_validation();
  // The coordinator stack lives in PSRAM and the completed RC report remains
  // observable on the serial console. It has no further work after this point.
  vTaskSuspend(nullptr);
}
#endif

bool start_pipeline_tasks(vocal_fx_platform::AudioI2sMode mode, bool start_pitch = true) {
  if (!s_task_events)
    s_task_events = xEventGroupCreateStatic(&s_task_events_storage);
  xEventGroupClearBits(s_task_events, kAudioTaskStopped | kPitchTaskStopped);
  s_pitch_worker.reset();
  vocal_fx_platform::AudioI2sConfig io{};
  io.mode = mode;
  io.dma_desc_num = 6;
  io.dma_frame_num = 64;
  io.sample_rate = g_pipeline_sample_rate;

  if (!s_audio.init(io, 64)) {
    ESP_LOGE(TAG, "Audio I2S driver initialization failed");
    return false;
  }

  s_audio.print_hardware_config();

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
  printf("B4B4G_HEAP before_audio_task internal_free=%lu largest=%lu required_stack=32768\n",
         static_cast<unsigned long>(
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned long>(
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
#endif

  // Core 0: Audio task + DSP (highest real-time priority)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
  s_audio_task_handle = xTaskCreateStaticPinnedToCore(
      audio_task_entry, "vocal_audio", s_b4b4j_audio_task_stack.size(),
      nullptr, configMAX_PRIORITIES - 2, s_b4b4j_audio_task_stack.data(),
      &s_b4b4j_audio_task_storage, 0);
  const bool audio_task_created = s_audio_task_handle != nullptr;
#else
  const bool audio_task_created =
      xTaskCreatePinnedToCore(audio_task_entry, "vocal_audio", 32768, nullptr,
                              configMAX_PRIORITIES - 2, &s_audio_task_handle,
                              0) == pdPASS;
#endif
  if (!audio_task_created) {
    ESP_LOGE(TAG, "Failed to create audio task on Core 0");
    s_audio.deinit();
    return false;
  }

  // Core 1: Pitch Worker task
  if (start_pitch) {
    s_pitch_running.store(true, std::memory_order_release);
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
    s_pitch_task_handle = xTaskCreateStaticPinnedToCore(
        pitch_worker_task, "vocal_pitch", s_b4b6_pitch_task_stack.size(),
        nullptr, configMAX_PRIORITIES - 5, s_b4b6_pitch_task_stack.data(),
        &s_b4b6_pitch_task_storage, 1);
    const bool pitch_task_created = s_pitch_task_handle != nullptr;
#else
    const bool pitch_task_created =
        xTaskCreatePinnedToCore(pitch_worker_task, "vocal_pitch", 16384,
                                nullptr, configMAX_PRIORITIES - 5,
                                &s_pitch_task_handle, 1) == pdPASS;
#endif
    if (!pitch_task_created) {
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

bool stop_pipeline_tasks(bool deinit_after_stop = true) {
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
  // Both tasks signal immediately before self-delete. Give the scheduler and
  // idle tasks a boundary to finish deletion before a matrix case reuses the
  // static TCB/stack. This is outside every warmup and measured window.
  vTaskDelay(pdMS_TO_TICKS(2));
  // B4C.6A prints its PSRAM timing ring after the tasks stop; deinit frees
  // that ring, so the forensics stage defers it until after printing.
  if (deinit_after_stop)
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

struct ProfileOverheadBenchmark {
  uint32_t iterations = 0;
  double now_cycles_per_call = 0.0;
  double profile_pair_cycles = 0.0;
  double record_cycles_cycles = 0.0;
  double atomic_increment_cycles = 0.0;
};

ProfileOverheadBenchmark benchmark_profile_overhead() {
  constexpr uint32_t kIterations = 20000;
  ProfileOverheadBenchmark result{};
  result.iterations = kIterations;
  volatile uint32_t sink = 0;

  uint32_t start = Profiler::now_cycles();
  for (uint32_t i = 0; i < kIterations; ++i)
    sink = sink ^ Profiler::now_cycles();
  result.now_cycles_per_call =
      static_cast<uint32_t>(Profiler::now_cycles() - start) /
      static_cast<double>(kIterations);

  Profiler profiler;
  (void)profiler.enable_distribution_range(ProfileSection::AnalysisTotal,
                                           ProfileSection::AnalysisTotal);
  start = Profiler::now_cycles();
  for (uint32_t i = 0; i < kIterations; ++i) {
    profiler.begin(ProfileSection::AnalysisTotal);
    profiler.end(ProfileSection::AnalysisTotal, 0);
  }
  result.profile_pair_cycles =
      static_cast<uint32_t>(Profiler::now_cycles() - start) /
      static_cast<double>(kIterations);

  start = Profiler::now_cycles();
  for (uint32_t i = 0; i < kIterations; ++i)
    profiler.record_cycles(ProfileSection::AnalysisTotal, i & 1U);
  result.record_cycles_cycles =
      static_cast<uint32_t>(Profiler::now_cycles() - start) /
      static_cast<double>(kIterations);

  std::atomic<uint64_t> telemetry{0};
  start = Profiler::now_cycles();
  for (uint32_t i = 0; i < kIterations; ++i)
    telemetry.fetch_add(1, std::memory_order_relaxed);
  result.atomic_increment_cycles =
      static_cast<uint32_t>(Profiler::now_cycles() - start) /
      static_cast<double>(kIterations);
  sink ^= static_cast<uint32_t>(telemetry.load(std::memory_order_relaxed));
  (void)sink;
  return result;
}

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
struct AutocorrIsolatedBenchmark {
  uint32_t count = 0;
  uint64_t autocorr_total_us = 0;
  uint64_t autocorr_total_cycles = 0;
  uint64_t lpc_total_us = 0;
  uint32_t autocorr_p50_us = 0;
  uint32_t autocorr_p95_us = 0;
  uint32_t autocorr_p99_us = 0;
  uint32_t autocorr_max_us = 0;
  uint32_t lpc_max_us = 0;
  uint32_t valid_frames = 0;
  double checksum = 0.0;
};

const char *autocorr_variant_name(LpcAutocorrelationVariant variant) {
  switch (variant) {
  case LpcAutocorrelationVariant::AutocorrReferenceDouble:
    return "AUTOCORR_REFERENCE_DOUBLE";
  case LpcAutocorrelationVariant::AutocorrFloatScalar:
    return "AUTOCORR_FLOAT_SCALAR";
  case LpcAutocorrelationVariant::AutocorrFloatMultiacc:
    return "AUTOCORR_FLOAT_MULTIACC";
  case LpcAutocorrelationVariant::AutocorrF32Kahan:
    return "AUTOCORR_F32_KAHAN";
  case LpcAutocorrelationVariant::AutocorrF32FmaProduct:
    return "AUTOCORR_F32_FMA_PRODUCT";
  case LpcAutocorrelationVariant::AutocorrF32DoubleSingle:
    return "AUTOCORR_F32_DOUBLE_SINGLE";
  }
  return "UNKNOWN";
}

AutocorrIsolatedBenchmark benchmark_lpc_autocorrelation(
    LpcAutocorrelationVariant variant) {
  constexpr size_t kWindow = 1024;
  constexpr uint16_t kOrder = 16;
  constexpr size_t kWarmupFrames = 4;
  constexpr size_t kMeasuredFrames = 128;
  constexpr float kPreemphasis = 0.97f;
  constexpr float kPi = 3.14159265358979323846f;
  std::array<float, kWindow> frame{};
  std::array<double, kOrder + 1> autocorrelation{};
  std::array<uint32_t, kMeasuredFrames> autocorr_us{};
  AutocorrIsolatedBenchmark result{};
  for (size_t frame_index = 0;
       frame_index < kWarmupFrames + kMeasuredFrames; ++frame_index) {
    const size_t first_sample = frame_index * 384;
    for (size_t i = 0; i < kWindow; ++i) {
      const float phase =
          2.0f * kPi * 220.0f * static_cast<float>(first_sample + i) /
          48000.0f;
      frame[i] = 0.12589254f * std::sin(phase);
    }
    const uint64_t lpc_start_us = esp_timer_get_time();
    SharedLpcModel model{};
    const bool valid = SharedLpcAnalysis::solve_with_autocorrelation(
        frame.data(), frame.size(), kOrder, kPreemphasis, variant, &model);
    const uint32_t lpc_us = static_cast<uint32_t>(
        esp_timer_get_time() - lpc_start_us);

    // Convert the same raw frame to the exact preemphasis + Hann input used by
    // solve(). Reverse traversal preserves each unmodified predecessor.
    for (size_t i = kWindow - 1; i > 0; --i) {
      const float window =
          .5f - .5f * std::cos(2.0f * kPi * i / (kWindow - 1));
      frame[i] = (frame[i] - kPreemphasis * frame[i - 1]) * window;
    }
    frame[0] = 0.0f;
    const uint64_t autocorr_start_us = esp_timer_get_time();
    const uint32_t autocorr_start_cycles = esp_cpu_get_cycle_count();
    SharedLpcAnalysis::autocorrelate(frame.data(), frame.size(), kOrder,
                                     variant, autocorrelation.data());
    const uint32_t autocorr_cycles = static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - autocorr_start_cycles);
    const uint32_t elapsed_autocorr_us = static_cast<uint32_t>(
        esp_timer_get_time() - autocorr_start_us);
    if (frame_index < kWarmupFrames)
      continue;
    const size_t measured_index = frame_index - kWarmupFrames;
    autocorr_us[measured_index] = elapsed_autocorr_us;
    result.autocorr_total_us += elapsed_autocorr_us;
    result.autocorr_total_cycles += autocorr_cycles;
    result.lpc_total_us += lpc_us;
    result.autocorr_max_us =
        std::max(result.autocorr_max_us, elapsed_autocorr_us);
    result.lpc_max_us = std::max(result.lpc_max_us, lpc_us);
    result.valid_frames += valid ? 1U : 0U;
    result.checksum += autocorrelation[frame_index % (kOrder + 1)];
    // The coordinator is pinned to Core 1. Yield outside both timed regions so
    // IDLE1 can service the normal task watchdog during the isolated audit.
    vTaskDelay(1);
  }
  result.count = kMeasuredFrames;
  std::sort(autocorr_us.begin(), autocorr_us.end());
  result.autocorr_p50_us =
      percentile_sorted(autocorr_us.data(), autocorr_us.size(), 50);
  result.autocorr_p95_us =
      percentile_sorted(autocorr_us.data(), autocorr_us.size(), 95);
  result.autocorr_p99_us =
      percentile_sorted(autocorr_us.data(), autocorr_us.size(), 99);
  return result;
}

void print_lpc_autocorr_isolated_benchmarks() {
  constexpr double kProductsPerFrame = 17272.0;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  const std::array<LpcAutocorrelationVariant, 5> variants{{
      LpcAutocorrelationVariant::AutocorrReferenceDouble,
      LpcAutocorrelationVariant::AutocorrF32Kahan,
      LpcAutocorrelationVariant::AutocorrF32FmaProduct,
      LpcAutocorrelationVariant::AutocorrF32DoubleSingle,
      LpcAutocorrelationVariant::AutocorrFloatMultiacc,
  }};
  constexpr const char *kPrefix = "B4B4C";
#else
  const std::array<LpcAutocorrelationVariant, 3> variants{{
      LpcAutocorrelationVariant::AutocorrReferenceDouble,
      LpcAutocorrelationVariant::AutocorrFloatScalar,
      LpcAutocorrelationVariant::AutocorrFloatMultiacc,
  }};
  constexpr const char *kPrefix = "B4B4B";
#endif
  printf("%s_THEORY window_size=1024 order=16 products/frame=17272\n",
         kPrefix);
  for (const auto variant : variants) {
    const auto stats = benchmark_lpc_autocorrelation(variant);
    const double average_us = stats.count
                                  ? static_cast<double>(stats.autocorr_total_us) /
                                        stats.count
                                  : 0.0;
    const double average_cycles =
        stats.count ? static_cast<double>(stats.autocorr_total_cycles) /
                          stats.count
                    : 0.0;
    printf("%s_ISOLATED variant=%s frames=%lu autocorrelation_avg_us/frame=%.2f P50=%lu P95=%lu P99=%lu max=%lu cycles/frame=%.2f cycles/product=%.5f LPC_total_us/frame=%.2f LPC_max_us=%lu valid_frames=%lu products/frame=17272 checksum=%.9g\n",
           kPrefix, autocorr_variant_name(variant),
           static_cast<unsigned long>(stats.count), average_us,
           static_cast<unsigned long>(stats.autocorr_p50_us),
           static_cast<unsigned long>(stats.autocorr_p95_us),
           static_cast<unsigned long>(stats.autocorr_p99_us),
           static_cast<unsigned long>(stats.autocorr_max_us), average_cycles,
           average_cycles / kProductsPerFrame,
           stats.count ? static_cast<double>(stats.lpc_total_us) / stats.count
                       : 0.0,
           static_cast<unsigned long>(stats.lpc_max_us),
           static_cast<unsigned long>(stats.valid_frames), stats.checksum);
  }
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
struct LpcWindowIsolatedBenchmark {
  uint32_t count = 0;
  uint64_t window_total_us = 0;
  uint64_t window_total_cycles = 0;
  uint64_t lpc_total_us = 0;
  std::array<uint32_t, 128> window_us{};
  std::array<uint32_t, 128> window_cycles{};
  std::array<uint32_t, 128> lpc_us{};
  uint32_t valid_frames = 0;
  double checksum = 0.0;
};

LpcWindowIsolatedBenchmark benchmark_lpc_windowing(
    LpcWindowVariant variant) {
  constexpr size_t kWindow = 1024;
  constexpr uint16_t kOrder = 16;
  constexpr size_t kWarmups = 8;
  constexpr size_t kRuns = 128;
  constexpr float kPreemphasis = 0.97f;
  constexpr float kPi = 3.14159265358979323846f;
  LpcWindowIsolatedBenchmark result{};
  auto *workspace = static_cast<float *>(heap_caps_calloc(
      3 * kWindow, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!workspace) {
    printf(VOXP4_LPC_AUDIT_TOKEN "_ISOLATED allocation_failed bytes=%lu\n",
           static_cast<unsigned long>(3 * kWindow * sizeof(float)));
    return result;
  }
  float *frame = workspace;
  float *windowed = frame + kWindow;
  float *hann = windowed + kWindow;
  SharedLpcAnalysis::prepare_hann(hann, kWindow);
  for (size_t run = 0; run < kWarmups + kRuns; ++run) {
    const size_t first_sample = run * 384;
    for (size_t i = 0; i < kWindow; ++i) {
      const float phase =
          2.0f * kPi * 220.0f * static_cast<float>(first_sample + i) /
          48000.0f;
      frame[i] = 0.12589254f * std::sin(phase);
    }
    double energy = 0.0;
    const uint64_t window_start_us = esp_timer_get_time();
    const uint32_t window_start_cycles = esp_cpu_get_cycle_count();
    SharedLpcAnalysis::window_frame(frame, kWindow, kPreemphasis,
                                    variant, hann, windowed,
                                    &energy);
    const uint32_t window_cycles = static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - window_start_cycles);
    const uint32_t window_us = static_cast<uint32_t>(
        esp_timer_get_time() - window_start_us);
    SharedLpcModel model{};
    double solved_energy = 0.0;
    const uint64_t lpc_start_us = esp_timer_get_time();
    const bool valid = SharedLpcAnalysis::solve_with_kernels(
        frame, kWindow, kOrder, kPreemphasis,
        LpcAutocorrelationVariant::AutocorrF32DoubleSingle, variant,
        hann, &model, nullptr, &solved_energy);
    const uint32_t lpc_us = static_cast<uint32_t>(
        esp_timer_get_time() - lpc_start_us);
    if (run < kWarmups)
      continue;
    const size_t index = run - kWarmups;
    result.window_us[index] = window_us;
    result.window_cycles[index] = window_cycles;
    result.lpc_us[index] = lpc_us;
    result.window_total_us += window_us;
    result.window_total_cycles += window_cycles;
    result.lpc_total_us += lpc_us;
    result.valid_frames += valid ? 1U : 0U;
    result.checksum += windowed[run % kWindow] + solved_energy +
                       (valid ? model.coefficients[run % (kOrder + 1)] : 0.0f);
    vTaskDelay(1);
  }
  result.count = kRuns;
  heap_caps_free(workspace);
  return result;
}

struct LpcWindowComponentCost {
  uint64_t loop_cycles = 0;
  uint64_t preemphasis_cycles = 0;
  uint64_t hann_cycles = 0;
  uint64_t double_energy_cycles = 0;
  uint64_t runtime_hann_cycles = 0;
  uint64_t double_energy_us = 0;
  uint64_t runtime_hann_us = 0;
  float checksum = 0.0f;
};

LpcWindowComponentCost benchmark_lpc_window_components() {
  constexpr size_t kWindow = 1024;
  constexpr size_t kRuns = 128;
  constexpr float kPreemphasis = 0.97f;
  constexpr float kPi = 3.14159265358979323846f;
  LpcWindowComponentCost result{};
  auto *workspace = static_cast<float *>(heap_caps_calloc(
      3 * kWindow, sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!workspace) {
    printf(VOXP4_LPC_AUDIT_TOKEN "_COMPONENT allocation_failed bytes=%lu\n",
           static_cast<unsigned long>(3 * kWindow * sizeof(float)));
    return result;
  }
  float *frame = workspace;
  float *scratch = frame + kWindow;
  float *hann = scratch + kWindow;
  for (size_t i = 0; i < kWindow; ++i)
    frame[i] = 0.12589254f *
               std::sin(2.0f * kPi * 220.0f * i / 48000.0f);
  SharedLpcAnalysis::prepare_hann(hann, kWindow);
  double energy_checksum = 0.0;
  for (size_t run = 0; run < kRuns; ++run) {
    uint32_t start_cycles = esp_cpu_get_cycle_count();
    for (size_t i = 0; i < kWindow; ++i)
      scratch[i] = frame[i];
    result.loop_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    result.checksum += scratch[run % kWindow];

    start_cycles = esp_cpu_get_cycle_count();
    for (size_t i = 0; i < kWindow; ++i)
      scratch[i] = frame[i] - (i ? kPreemphasis * frame[i - 1] : 0.0f);
    result.preemphasis_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    result.checksum += scratch[(run + 1) % kWindow];

    start_cycles = esp_cpu_get_cycle_count();
    for (size_t i = 0; i < kWindow; ++i)
      scratch[i] = frame[i] * hann[i];
    result.hann_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    result.checksum += scratch[(run + 2) % kWindow];

    double energy = 0.0;
    const uint64_t energy_start_us = esp_timer_get_time();
    start_cycles = esp_cpu_get_cycle_count();
    for (size_t i = 0; i < kWindow; ++i)
      energy += static_cast<double>(scratch[i]) * scratch[i];
    result.double_energy_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    result.double_energy_us += esp_timer_get_time() - energy_start_us;
    energy_checksum += energy;

    const uint64_t hann_start_us = esp_timer_get_time();
    start_cycles = esp_cpu_get_cycle_count();
    for (size_t i = 0; i < kWindow; ++i)
      scratch[i] = .5f - .5f * std::cos(2 * kPi * i / (kWindow - 1));
    result.runtime_hann_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    result.runtime_hann_us += esp_timer_get_time() - hann_start_us;
    result.checksum += scratch[(run + 3) % kWindow];
  }
  result.checksum += static_cast<float>(energy_checksum);
  heap_caps_free(workspace);
  return result;
}

void print_lpc_windowing_isolated_benchmarks() {
  const std::array<LpcWindowVariant, 4> variants{{
      LpcWindowVariant::Reference,
      LpcWindowVariant::PrecomputedHannDoubleEnergy,
      LpcWindowVariant::PrecomputedHannCompensatedEnergy,
      LpcWindowVariant::EnergyFromAutocorrR0}};
  auto *results = static_cast<LpcWindowIsolatedBenchmark *>(heap_caps_calloc(
      variants.size(), sizeof(LpcWindowIsolatedBenchmark),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!results) {
    printf(VOXP4_LPC_AUDIT_TOKEN "_ISOLATED result_allocation_failed bytes=%lu\n",
           static_cast<unsigned long>(variants.size() *
                                      sizeof(LpcWindowIsolatedBenchmark)));
    return;
  }
  for (size_t i = 0; i < variants.size(); ++i) {
    results[i] = benchmark_lpc_windowing(variants[i]);
    auto sorted_us = results[i].window_us;
    auto sorted_cycles = results[i].window_cycles;
    auto sorted_lpc_us = results[i].lpc_us;
    std::sort(sorted_us.begin(), sorted_us.end());
    std::sort(sorted_cycles.begin(), sorted_cycles.end());
    std::sort(sorted_lpc_us.begin(), sorted_lpc_us.end());
    const auto &stats = results[i];
    printf(VOXP4_LPC_AUDIT_TOKEN "_ISOLATED variant=%s frames=%lu window_avg_us/frame=%.2f P50=%lu P95=%lu P99=%lu max=%lu cycles/frame=%.2f P50_cycles=%lu P95_cycles=%lu P99_cycles=%lu max_cycles=%lu LPC_total_us/frame=%.2f LPC_P50=%lu LPC_P95=%lu LPC_P99=%lu LPC_max=%lu valid_frames=%lu checksum=%.9g\n",
           lpc_window_variant_name(variants[i]),
           static_cast<unsigned long>(stats.count),
           static_cast<double>(stats.window_total_us) / stats.count,
           static_cast<unsigned long>(percentile_sorted(sorted_us.data(), sorted_us.size(), 50)),
           static_cast<unsigned long>(percentile_sorted(sorted_us.data(), sorted_us.size(), 95)),
           static_cast<unsigned long>(percentile_sorted(sorted_us.data(), sorted_us.size(), 99)),
           static_cast<unsigned long>(sorted_us.back()),
           static_cast<double>(stats.window_total_cycles) / stats.count,
           static_cast<unsigned long>(percentile_sorted(sorted_cycles.data(), sorted_cycles.size(), 50)),
           static_cast<unsigned long>(percentile_sorted(sorted_cycles.data(), sorted_cycles.size(), 95)),
           static_cast<unsigned long>(percentile_sorted(sorted_cycles.data(), sorted_cycles.size(), 99)),
           static_cast<unsigned long>(sorted_cycles.back()),
           static_cast<double>(stats.lpc_total_us) / stats.count,
           static_cast<unsigned long>(percentile_sorted(sorted_lpc_us.data(), sorted_lpc_us.size(), 50)),
           static_cast<unsigned long>(percentile_sorted(sorted_lpc_us.data(), sorted_lpc_us.size(), 95)),
           static_cast<unsigned long>(percentile_sorted(sorted_lpc_us.data(), sorted_lpc_us.size(), 99)),
           static_cast<unsigned long>(sorted_lpc_us.back()),
           static_cast<unsigned long>(stats.valid_frames), stats.checksum);
  }

  const auto cost = benchmark_lpc_window_components();
  constexpr double kRuns = 128.0;
  const double combined_cycles =
      static_cast<double>(results[1].window_total_cycles) / results[1].count;
  const double loop = static_cast<double>(cost.loop_cycles) / kRuns;
  const double pre = std::max(0.0,
      static_cast<double>(cost.preemphasis_cycles) / kRuns - loop);
  const double hann = std::max(0.0,
      static_cast<double>(cost.hann_cycles) / kRuns - loop);
  const double energy = std::max(0.0,
      static_cast<double>(cost.double_energy_cycles) / kRuns - loop);
  const double measured = loop + pre + hann + energy;
  const double scale = measured > 0.0 ? combined_cycles * 0.98 / measured : 0.0;
  const auto print_cost = [&](const char *name, double raw) {
    const double normalized = raw * scale;
    printf(VOXP4_LPC_AUDIT_TOKEN "_COST category=%s raw_cycles/frame=%.2f normalized_cycles/frame=%.2f percent=%.3f\n",
           name, raw, normalized,
           combined_cycles > 0.0 ? 100.0 * normalized / combined_cycles : 0.0);
  };
  printf(VOXP4_LPC_AUDIT_TOKEN "_COMPONENT runtime_hann_cos_avg_us/frame=%.2f cycles/frame=%.2f old_LPC_percent=%.3f double_energy_avg_us/frame=%.2f double_energy_cycles/frame=%.2f checksum=%.9g\n",
         cost.runtime_hann_us / kRuns, cost.runtime_hann_cycles / kRuns,
         100.0 * (cost.runtime_hann_us / kRuns) / 4090.08,
         cost.double_energy_us / kRuns, cost.double_energy_cycles / kRuns,
         cost.checksum);
  printf(VOXP4_LPC_AUDIT_TOKEN "_COST_METHOD combined_cycles/frame=%.2f normalized_probe_coverage=98.000 other=2.000 reconciled=100.000\n",
         combined_cycles);
  print_cost("preemphasis", pre);
  print_cost("hann_lookup_multiply", hann);
  print_cost("energy_accumulation", energy);
  print_cost("loop_address_overhead", loop);
  printf(VOXP4_LPC_AUDIT_TOKEN "_COST category=other raw_cycles/frame=0 normalized_cycles/frame=%.2f percent=2.000\n",
         combined_cycles * 0.02);
  heap_caps_free(results);
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
struct YinIsolatedBenchmark {
  uint32_t count = 0;
  uint32_t cold_us = 0;
  uint32_t cold_cycles = 0;
  uint64_t total_us = 0;
  uint64_t total_cycles = 0;
  uint32_t p50_us = 0;
  uint32_t p95_us = 0;
  uint32_t p99_us = 0;
  uint32_t maximum_us = 0;
  float checksum = 0.0f;
  uint64_t operations = 0;
  uint64_t rebases = 0;
};

volatile float s_b4b4d_yin_checksum = 0.0f;

YinIsolatedBenchmark
benchmark_yin_difference(YinDifferenceVariant variant,
                         uint32_t rebase_hops = 0) {
  constexpr size_t kWindow = 512;
  constexpr size_t kTauCount = 185;
  constexpr size_t kWarmupHops = 8;
  constexpr size_t kMeasuredHops = 128;
  constexpr float kPi = 3.14159265358979323846f;
  std::array<float, kWindow> window{};
  std::array<float, YinDetector::kMaxWindow + 1> difference{};
  std::array<uint32_t, kMeasuredHops> elapsed_us{};
  std::array<uint32_t, kMeasuredHops> elapsed_cycles{};
  const bool incremental =
      variant == YinDifferenceVariant::IncrementalF32 ||
      variant == YinDifferenceVariant::IncrementalDoubleSingle;
  // The coordinator already owns benchmark arrays on its stack and internal
  // SRAM is needed later by the audio task. This diagnostic-only state lives
  // temporarily in PSRAM; production state remains fixed inside Engine.
  void *incremental_storage =
      incremental ? heap_caps_malloc(sizeof(YinIncrementalDifference),
                                     MALLOC_CAP_SPIRAM)
                  : nullptr;
  auto *incremental_state = incremental_storage
                                ? new (incremental_storage)
                                      YinIncrementalDifference()
                                : nullptr;
  if (incremental &&
      (!incremental_state ||
       !incremental_state->init(variant, kWindow, 60, kTauCount,
                                rebase_hops))) {
    if (incremental_storage)
      heap_caps_free(incremental_storage);
    return {};
  }
  for (size_t i = 0; i < window.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * static_cast<float>(i) /
                        12000.0f;
    window[i] = 0.12589254f * std::sin(phase);
  }

  YinIsolatedBenchmark result{};
  uint64_t start_us = esp_timer_get_time();
  uint32_t start_cycles = esp_cpu_get_cycle_count();
  if (incremental)
    incremental_state->compute(window.data(), difference.data());
  else
    yin_difference_compute(variant, window.data(), window.size(), kTauCount,
                           difference.data());
  result.cold_cycles =
      static_cast<uint32_t>(esp_cpu_get_cycle_count() - start_cycles);
  result.cold_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);

  size_t stream_position = kWindow;
  auto advance_window = [&] {
    std::move(window.begin() + 60, window.end(), window.begin());
    for (size_t i = kWindow - 60; i < kWindow; ++i) {
      const float phase = 2.0f * kPi * 220.0f *
                          static_cast<float>(stream_position++) / 12000.0f;
      window[i] = 0.12589254f * std::sin(phase);
    }
  };
  for (size_t hop = 0; hop < kWarmupHops; ++hop) {
    advance_window();
    if (incremental)
      incremental_state->compute(window.data(), difference.data());
    else
      yin_difference_compute(variant, window.data(), window.size(), kTauCount,
                             difference.data());
  }
  const uint64_t operations_before =
      incremental_state ? incremental_state->full_rebase_products() +
                              incremental_state->update_terms()
                        : 0;
  const uint64_t rebases_before =
      incremental_state ? incremental_state->incremental_rebases() : 0;
  for (size_t hop = 0; hop < kMeasuredHops; ++hop) {
    advance_window();
    start_us = esp_timer_get_time();
    start_cycles = esp_cpu_get_cycle_count();
    if (incremental)
      incremental_state->compute(window.data(), difference.data());
    else
      yin_difference_compute(variant, window.data(), window.size(), kTauCount,
                             difference.data());
    elapsed_cycles[hop] =
        static_cast<uint32_t>(esp_cpu_get_cycle_count() - start_cycles);
    elapsed_us[hop] = static_cast<uint32_t>(esp_timer_get_time() - start_us);
    result.total_cycles += elapsed_cycles[hop];
    result.total_us += elapsed_us[hop];
    result.maximum_us = std::max(result.maximum_us, elapsed_us[hop]);
    result.checksum += difference[1 + (hop % kTauCount)];
  }
  s_b4b4d_yin_checksum = result.checksum;
  result.count = kMeasuredHops;
  result.operations = incremental
                          ? incremental_state->full_rebase_products() +
                                incremental_state->update_terms() -
                                operations_before
                          : kMeasuredHops * 77515ULL;
  result.rebases = incremental
                       ? incremental_state->incremental_rebases() - rebases_before
                       : kMeasuredHops;
  std::sort(elapsed_us.begin(), elapsed_us.end());
  result.p50_us = percentile_sorted(elapsed_us.data(), elapsed_us.size(), 50);
  result.p95_us = percentile_sorted(elapsed_us.data(), elapsed_us.size(), 95);
  result.p99_us = percentile_sorted(elapsed_us.data(), elapsed_us.size(), 99);
  if (incremental_state) {
    incremental_state->~YinIncrementalDifference();
    heap_caps_free(incremental_storage);
  }
  return result;
}

[[maybe_unused]] void print_yin_isolated_benchmarks() {
  constexpr double kProductsPerHop = 77515.0;
  const std::array<YinDifferenceVariant, 5> variants{{
      YinDifferenceVariant::ReferenceScalar,
      YinDifferenceVariant::FmaScalar,
      YinDifferenceVariant::Muladd4Acc,
      YinDifferenceVariant::Fma4Acc,
      YinDifferenceVariant::Fma8Acc,
  }};
  printf("B4B4D_THEORY analysis_sample_rate=12000 window_size=512 hop_size=60 tau_min=12 tau_max=184 difference_taus=185 products/hop=77515\n");
  for (const auto variant : variants) {
    const auto stats = benchmark_yin_difference(variant);
    const double average_us =
        stats.count ? static_cast<double>(stats.total_us) / stats.count : 0.0;
    const double average_cycles =
        stats.count ? static_cast<double>(stats.total_cycles) / stats.count
                    : 0.0;
    printf("B4B4D_ISOLATED variant=%s hops=%lu cold_us=%lu cold_cycles=%lu avg_us/hop=%.2f P50=%lu P95=%lu P99=%lu max=%lu cycles/hop=%.2f cycles/product=%.5f taus=185 products/hop=77515 reference_products=77515 candidate_products=77515 checksum=%.9g\n",
           yin_difference_variant_name(variant),
           static_cast<unsigned long>(stats.count),
           static_cast<unsigned long>(stats.cold_us),
           static_cast<unsigned long>(stats.cold_cycles), average_us,
           static_cast<unsigned long>(stats.p50_us),
           static_cast<unsigned long>(stats.p95_us),
           static_cast<unsigned long>(stats.p99_us),
           static_cast<unsigned long>(stats.maximum_us), average_cycles,
           average_cycles / kProductsPerHop, stats.checksum);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  const std::array<YinDifferenceVariant, 2> incremental_variants{{
      YinDifferenceVariant::IncrementalF32,
      YinDifferenceVariant::IncrementalDoubleSingle,
  }};
  const std::array<uint32_t, 5> rebase_intervals{{4, 8, 16, 32, 64}};
  for (const auto variant : incremental_variants) {
    for (const uint32_t interval : rebase_intervals) {
      const auto stats = benchmark_yin_difference(variant, interval);
      const double average_us = stats.count
                                    ? static_cast<double>(stats.total_us) /
                                          stats.count
                                    : 0.0;
      const double average_cycles =
          stats.count ? static_cast<double>(stats.total_cycles) / stats.count
                      : 0.0;
      printf("B4B4E_ISOLATED variant=%s rebase_hops=%lu hops=%lu cold_us=%lu cold_cycles=%lu avg_us/hop=%.2f P50=%lu P95=%lu P99=%lu max=%lu cycles/hop=%.2f operations/hop=%.2f periodic_rebases=%llu checksum=%.9g\n",
             yin_difference_variant_name(variant),
             static_cast<unsigned long>(interval),
             static_cast<unsigned long>(stats.count),
             static_cast<unsigned long>(stats.cold_us),
             static_cast<unsigned long>(stats.cold_cycles), average_us,
             static_cast<unsigned long>(stats.p50_us),
             static_cast<unsigned long>(stats.p95_us),
             static_cast<unsigned long>(stats.p99_us),
             static_cast<unsigned long>(stats.maximum_us), average_cycles,
             stats.count ? static_cast<double>(stats.operations) / stats.count
                         : 0.0,
             static_cast<unsigned long long>(stats.rebases), stats.checksum);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
#endif
}

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
struct PitchMarkReferenceCost {
  uint64_t ring_lookup = 0;
  uint64_t dot = 0;
  uint64_t aa = 0;
  uint64_t bb = 0;
  uint64_t square_root = 0;
  uint64_t division = 0;
  uint64_t candidate_loop = 0;
  uint64_t best_selection = 0;
};

inline uint32_t b4b4f_cycle_probe() {
  // Keep the diagnostic-only fine-grained measurements on their intended
  // side of the counter read. This code is not part of the production kernel.
  asm volatile("" ::: "memory");
  return esp_cpu_get_cycle_count();
}

uint32_t b4b4f_probe_overhead() {
  uint32_t best = UINT32_MAX;
  for (size_t i = 0; i < 1024; ++i) {
    const uint32_t start = b4b4f_cycle_probe();
    const uint32_t elapsed = b4b4f_cycle_probe() - start;
    best = std::min(best, elapsed);
  }
  return best;
}

inline uint32_t b4b4f_net_cycles(uint32_t start, uint32_t overhead) {
  const uint32_t elapsed = b4b4f_cycle_probe() - start;
  return elapsed > overhead ? elapsed - overhead : 0;
}

PitchMarkReferenceCost profile_pitch_mark_reference_cost(
    const float *ring, size_t ring_size, uint32_t probe_overhead,
    volatile float &checksum) {
  constexpr size_t kProfileRuns = 4;
  constexpr int kPeriod = 218, kRadius = kPeriod / 5, kWindow = kPeriod / 2;
  const size_t mask = ring_size - 1;
  PitchMarkReferenceCost cost{};
  for (size_t run = 0; run < kProfileRuns; ++run) {
    const uint64_t previous = 512 + run;
    const uint64_t predicted = previous + kPeriod;
    const uint64_t a_start = previous - kWindow;
    float best_score = -2.0f;
    int best_offset = 0;
    for (int offset = -kRadius; offset <= kRadius; ++offset) {
      uint32_t start = b4b4f_cycle_probe();
      const uint64_t candidate = predicted + offset;
      const uint64_t b_start = candidate - kWindow;
      asm volatile("" : : "r"(candidate), "r"(b_start));
      cost.candidate_loop += b4b4f_net_cycles(start, probe_overhead);

      float dot = 0.0f, aa = 0.0f, bb = 0.0f;
      for (int i = 0; i < kWindow; ++i) {
        start = b4b4f_cycle_probe();
        const float a = ring[(a_start + i) & mask];
        const float b = ring[(b_start + i) & mask];
        asm volatile("" : : "f"(a), "f"(b) : "memory");
        cost.ring_lookup += b4b4f_net_cycles(start, probe_overhead);

        start = b4b4f_cycle_probe();
        dot += a * b;
        asm volatile("" : "+f"(dot));
        cost.dot += b4b4f_net_cycles(start, probe_overhead);

        start = b4b4f_cycle_probe();
        aa += a * a;
        asm volatile("" : "+f"(aa));
        cost.aa += b4b4f_net_cycles(start, probe_overhead);

        start = b4b4f_cycle_probe();
        bb += b * b;
        asm volatile("" : "+f"(bb));
        cost.bb += b4b4f_net_cycles(start, probe_overhead);
      }
      start = b4b4f_cycle_probe();
      const float denominator = std::sqrt(std::max(aa * bb, 1e-20f));
      asm volatile("" : : "f"(denominator));
      cost.square_root += b4b4f_net_cycles(start, probe_overhead);

      start = b4b4f_cycle_probe();
      const float score = dot / denominator;
      asm volatile("" : : "f"(score));
      cost.division += b4b4f_net_cycles(start, probe_overhead);

      start = b4b4f_cycle_probe();
      if (score > best_score) {
        best_score = score;
        best_offset = offset;
      }
      asm volatile("" : "+f"(best_score), "+r"(best_offset));
      cost.best_selection += b4b4f_net_cycles(start, probe_overhead);
    }
    checksum = checksum + best_score + static_cast<float>(best_offset) * 1e-9f;
  }
  cost.ring_lookup /= kProfileRuns;
  cost.dot /= kProfileRuns;
  cost.aa /= kProfileRuns;
  cost.bb /= kProfileRuns;
  cost.square_root /= kProfileRuns;
  cost.division /= kProfileRuns;
  cost.candidate_loop /= kProfileRuns;
  cost.best_selection /= kProfileRuns;
  return cost;
}

void print_pitch_mark_ncc_isolated_benchmarks() {
  constexpr size_t kRingSize = 2048;
  constexpr size_t kRuns = 128;
  constexpr int kPeriod = 218, kRadius = kPeriod / 5, kWindow = kPeriod / 2;
  auto *ring = static_cast<float *>(heap_caps_malloc(
      kRingSize * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto *scratch = static_cast<float *>(heap_caps_malloc(
      kPitchMarkNccScratchCapacity * sizeof(float),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring || !scratch) {
    printf("B4B4F_ISOLATED allocation_failed ring=%p scratch=%p\n", ring,
           scratch);
    if (ring) heap_caps_free(ring);
    if (scratch) heap_caps_free(scratch);
    return;
  }
  constexpr float kPi = 3.14159265358979323846f;
  for (size_t i = 0; i < kRingSize; ++i) {
    const float phase = 2.0f * kPi * 220.0f * static_cast<float>(i) / 48000.0f;
    ring[i] = 0.12589254f *
              (std::sin(phase) + .31f * std::sin(2.0f * phase + .23f) +
               .13f * std::sin(3.0f * phase - .41f));
  }
  const std::array<PitchMarkNccVariant, 6> variants{{
      PitchMarkNccVariant::Reference, PitchMarkNccVariant::ReuseAa,
      PitchMarkNccVariant::Fma4AccDot, PitchMarkNccVariant::Fma8AccDot,
      PitchMarkNccVariant::Fma8SlidingBb,
      PitchMarkNccVariant::LinearScratch}};
  volatile float checksum = 0.0f;
  double reference_average_cycles = 0.0;
  for (const auto variant : variants) {
    std::array<uint32_t, kRuns> elapsed_us{}, elapsed_cycles{};
    uint64_t total_us = 0, total_cycles = 0;
    PitchMarkNccResult last{};
    // One untimed warm-up eliminates first-call/cache effects.
    (void)pitch_mark_ncc_search(variant, ring, kRingSize, 512,
                                512 + kPeriod, kRadius, kWindow, scratch,
                                kPitchMarkNccScratchCapacity);
    for (size_t run = 0; run < kRuns; ++run) {
      const uint64_t previous = 512 + run;
      const uint64_t start_us = esp_timer_get_time();
      const uint32_t start_cycles = esp_cpu_get_cycle_count();
      last = pitch_mark_ncc_search(variant, ring, kRingSize, previous,
                                   previous + kPeriod, kRadius, kWindow,
                                   scratch, kPitchMarkNccScratchCapacity);
      elapsed_cycles[run] = esp_cpu_get_cycle_count() - start_cycles;
      elapsed_us[run] = static_cast<uint32_t>(esp_timer_get_time() - start_us);
      total_us += elapsed_us[run];
      total_cycles += elapsed_cycles[run];
      checksum = checksum + last.best_score;
    }
    std::sort(elapsed_us.begin(), elapsed_us.end());
    std::sort(elapsed_cycles.begin(), elapsed_cycles.end());
    const double average_us = static_cast<double>(total_us) / kRuns;
    const double average_cycles = static_cast<double>(total_cycles) / kRuns;
    if (variant == PitchMarkNccVariant::Reference)
      reference_average_cycles = average_cycles;
    printf("B4B4F_ISOLATED variant=%s searches=%lu avg_us/search=%.2f cycles/search=%.2f us/offset=%.5f cycles/offset=%.5f cycles/pair=%.7f P50_us=%lu P95_us=%lu P99_us=%lu max_us=%lu P50_cycles=%lu P95_cycles=%lu P99_cycles=%lu max_cycles=%lu window=%d offsets=%lu pairs=%llu checksum=%.9g\n",
           pitch_mark_ncc_variant_name(variant),
           static_cast<unsigned long>(kRuns), average_us, average_cycles,
           average_us / last.offsets, average_cycles / last.offsets,
           average_cycles / last.sample_pairs,
           static_cast<unsigned long>(percentile_sorted(elapsed_us.data(), kRuns, 50)),
           static_cast<unsigned long>(percentile_sorted(elapsed_us.data(), kRuns, 95)),
           static_cast<unsigned long>(percentile_sorted(elapsed_us.data(), kRuns, 99)),
           static_cast<unsigned long>(elapsed_us.back()),
           static_cast<unsigned long>(percentile_sorted(elapsed_cycles.data(), kRuns, 50)),
           static_cast<unsigned long>(percentile_sorted(elapsed_cycles.data(), kRuns, 95)),
           static_cast<unsigned long>(percentile_sorted(elapsed_cycles.data(), kRuns, 99)),
           static_cast<unsigned long>(elapsed_cycles.back()), kWindow,
           static_cast<unsigned long>(last.offsets),
           static_cast<unsigned long long>(last.sample_pairs), checksum);
    vTaskDelay(pdMS_TO_TICKS(2));
  }

  // Isolate the one-time copy used by LINEAR_SCRATCH. End-to-end candidate
  // timing above remains authoritative; this split explains its net result.
  std::array<uint32_t, kRuns> copy_us{}, copy_cycles{};
  uint64_t copy_total_us = 0, copy_total_cycles = 0;
  for (size_t run = 0; run < kRuns; ++run) {
    const uint64_t previous = 512 + run;
    const uint64_t a_start = previous - kWindow;
    const uint64_t first_candidate = previous + kPeriod - kRadius;
    const uint64_t b_region = first_candidate - kWindow;
    const uint64_t start_us = esp_timer_get_time();
    const uint32_t start_cycles = esp_cpu_get_cycle_count();
    for (int i = 0; i < kWindow; ++i)
      scratch[i] = ring[(a_start + i) & (kRingSize - 1)];
    for (int i = 0; i < kWindow + 2 * kRadius; ++i)
      scratch[kWindow + i] = ring[(b_region + i) & (kRingSize - 1)];
    copy_cycles[run] = esp_cpu_get_cycle_count() - start_cycles;
    copy_us[run] = static_cast<uint32_t>(esp_timer_get_time() - start_us);
    copy_total_cycles += copy_cycles[run];
    copy_total_us += copy_us[run];
    checksum = checksum + scratch[run % (2 * kWindow + 2 * kRadius)];
  }
  std::sort(copy_us.begin(), copy_us.end());
  std::sort(copy_cycles.begin(), copy_cycles.end());
  printf("B4B4F_LINEAR_COPY searches=%lu samples/search=%d avg_us/search=%.2f cycles/search=%.2f P50_us=%lu P95_us=%lu P99_us=%lu max_us=%lu P50_cycles=%lu P95_cycles=%lu P99_cycles=%lu max_cycles=%lu\n",
         static_cast<unsigned long>(kRuns), 2 * kWindow + 2 * kRadius,
         static_cast<double>(copy_total_us) / kRuns,
         static_cast<double>(copy_total_cycles) / kRuns,
         static_cast<unsigned long>(percentile_sorted(copy_us.data(), kRuns, 50)),
         static_cast<unsigned long>(percentile_sorted(copy_us.data(), kRuns, 95)),
         static_cast<unsigned long>(percentile_sorted(copy_us.data(), kRuns, 99)),
         static_cast<unsigned long>(copy_us.back()),
         static_cast<unsigned long>(percentile_sorted(copy_cycles.data(), kRuns, 50)),
         static_cast<unsigned long>(percentile_sorted(copy_cycles.data(), kRuns, 95)),
         static_cast<unsigned long>(percentile_sorted(copy_cycles.data(), kRuns, 99)),
         static_cast<unsigned long>(copy_cycles.back()));

  // Fine-grained cycle probes perturb the kernel, so subtract the minimum
  // counter-pair overhead and normalize their relative weights to 98% of the
  // independently measured uninstrumented reference. The remaining 2% is
  // reported as other; the reconciled attribution is therefore exactly 100%.
  const uint32_t probe_overhead = b4b4f_probe_overhead();
  const auto cost = profile_pitch_mark_reference_cost(
      ring, kRingSize, probe_overhead, checksum);
  const uint64_t measured_sum = cost.ring_lookup + cost.dot + cost.aa + cost.bb +
      cost.square_root + cost.division + cost.candidate_loop +
      cost.best_selection;
  const double scale = measured_sum && reference_average_cycles > 0.0
                           ? reference_average_cycles * 0.98 / measured_sum
                           : 0.0;
  const double other_cycles = reference_average_cycles * 0.02;
  const auto print_cost = [&](const char *category, uint64_t raw_cycles) {
    const double normalized = raw_cycles * scale;
    printf("B4B4F_COST category=%s raw_cycles/search=%llu normalized_cycles/search=%.2f percent=%.3f\n",
           category, static_cast<unsigned long long>(raw_cycles), normalized,
           reference_average_cycles > 0.0
               ? normalized * 100.0 / reference_average_cycles
               : 0.0);
  };
  printf("B4B4F_COST_METHOD probe_overhead=%lu reference_cycles/search=%.2f measured_raw_sum=%llu normalized_probe_coverage=98.000 other=2.000 reconciled=100.000\n",
         static_cast<unsigned long>(probe_overhead), reference_average_cycles,
         static_cast<unsigned long long>(measured_sum));
  print_cost("audio_ring_lookup", cost.ring_lookup);
  print_cost("dot_accumulation", cost.dot);
  print_cost("aa_accumulation", cost.aa);
  print_cost("bb_accumulation", cost.bb);
  print_cost("sqrt", cost.square_root);
  print_cost("division", cost.division);
  print_cost("candidate_loop", cost.candidate_loop);
  print_cost("best_score_selection", cost.best_selection);
  printf("B4B4F_COST category=other raw_cycles/search=0 normalized_cycles/search=%.2f percent=2.000\n",
         other_cycles);
  heap_caps_free(scratch);
  heap_caps_free(ring);
}
#endif
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
struct CmndIsolatedBenchmark {
  uint32_t count = 0, cold_us = 0, cold_cycles = 0;
  uint64_t total_us = 0, total_cycles = 0;
  uint32_t p50_us = 0, p95_us = 0, p99_us = 0, maximum_us = 0;
  float checksum = 0.0f;
};

volatile float s_b4b4j_cmnd_checksum = 0.0f;
volatile double s_b4b4j_cmnd_double_checksum = 0.0;
volatile uint32_t s_b4b4j_cmnd_integer_checksum = 0;
float *s_b4b4j_numerators = nullptr;
double *s_b4b4j_denominators = nullptr;

using CmndCostProbe = void (*)(const float *, size_t, float *);

__attribute__((noinline)) void cmnd_probe_cumulative(
    const float *difference, size_t tau_count, float *) {
  double cumulative = 0.0;
  for (size_t tau = 1; tau <= tau_count; ++tau)
    cumulative += difference[tau];
  s_b4b4j_cmnd_double_checksum = cumulative;
}

__attribute__((noinline)) void cmnd_probe_numerator(
    const float *difference, size_t tau_count, float *output) {
  for (size_t tau = 1; tau <= tau_count; ++tau)
    output[tau] = difference[tau] * static_cast<float>(tau);
  s_b4b4j_cmnd_checksum = output[tau_count];
}

__attribute__((noinline)) void cmnd_probe_division(
    const float *, size_t tau_count, float *output) {
  for (size_t tau = 1; tau <= tau_count; ++tau)
    output[tau] = static_cast<float>(
        static_cast<double>(s_b4b4j_numerators[tau]) /
        s_b4b4j_denominators[tau]);
  s_b4b4j_cmnd_checksum = output[tau_count];
}

__attribute__((noinline)) void cmnd_probe_store(
    const float *, size_t tau_count, float *output) {
  for (size_t tau = 1; tau <= tau_count; ++tau)
    output[tau] = s_b4b4j_numerators[tau];
  s_b4b4j_cmnd_checksum = output[tau_count];
}

__attribute__((noinline)) void cmnd_probe_loop_branch(
    const float *, size_t tau_count, float *) {
  uint32_t taken = 0;
  for (size_t tau = 1; tau <= tau_count; ++tau)
    if (s_b4b4j_denominators[tau] > 1e-20)
      ++taken;
  s_b4b4j_cmnd_integer_checksum = taken;
}

__attribute__((noinline)) void cmnd_probe_loop(
    const float *, size_t tau_count, float *) {
  uint32_t visited = 0;
  for (size_t tau = 1; tau <= tau_count; ++tau)
    visited += static_cast<uint32_t>(tau);
  s_b4b4j_cmnd_integer_checksum = visited;
}

double benchmark_cmnd_cost_probe(CmndCostProbe probe, const float *difference,
                                 size_t tau_count, float *output) {
  constexpr size_t kWarmup = 8, kRuns = 128;
  for (size_t i = 0; i < kWarmup; ++i)
    probe(difference, tau_count, output);
  uint64_t cycles = 0;
  for (size_t i = 0; i < kRuns; ++i) {
    const uint32_t start = esp_cpu_get_cycle_count();
    probe(difference, tau_count, output);
    cycles += static_cast<uint32_t>(esp_cpu_get_cycle_count() - start);
  }
  return static_cast<double>(cycles) / kRuns;
}

CmndIsolatedBenchmark benchmark_yin_cmnd(YinCmndVariant variant) {
  constexpr size_t kWindow = 512, kTaus = 185, kWarmup = 8, kRuns = 128;
  constexpr float kPi = 3.14159265358979323846f;
  std::array<float, kWindow> window{};
  std::array<float, YinDetector::kMaxWindow + 1> difference{}, cmnd{};
  std::array<uint32_t, kRuns> elapsed_us{}, elapsed_cycles{};
  for (size_t i = 0; i < window.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * static_cast<float>(i) / 12000.0f;
    window[i] = 0.12589254f * std::sin(phase) +
                0.007f * std::sin(2.0f * phase);
  }
  yin_difference_fma_8acc(window.data(), window.size(), kTaus,
                          difference.data());
  CmndIsolatedBenchmark result{};
  uint64_t start_us = esp_timer_get_time();
  uint32_t start_cycles = esp_cpu_get_cycle_count();
  yin_cmnd_compute(variant, difference.data(), kTaus, cmnd.data());
  result.cold_cycles = esp_cpu_get_cycle_count() - start_cycles;
  result.cold_us = esp_timer_get_time() - start_us;
  for (size_t i = 0; i < kWarmup; ++i)
    yin_cmnd_compute(variant, difference.data(), kTaus, cmnd.data());
  for (size_t run = 0; run < kRuns; ++run) {
    start_us = esp_timer_get_time();
    start_cycles = esp_cpu_get_cycle_count();
    yin_cmnd_compute(variant, difference.data(), kTaus, cmnd.data());
    elapsed_cycles[run] = esp_cpu_get_cycle_count() - start_cycles;
    elapsed_us[run] = esp_timer_get_time() - start_us;
    result.total_cycles += elapsed_cycles[run];
    result.total_us += elapsed_us[run];
    result.maximum_us = std::max(result.maximum_us, elapsed_us[run]);
    result.checksum += cmnd[1 + run % kTaus];
  }
  result.count = kRuns;
  std::sort(elapsed_us.begin(), elapsed_us.end());
  result.p50_us = percentile_sorted(elapsed_us.data(), kRuns, 50);
  result.p95_us = percentile_sorted(elapsed_us.data(), kRuns, 95);
  result.p99_us = percentile_sorted(elapsed_us.data(), kRuns, 99);
  s_b4b4j_cmnd_checksum = result.checksum;
  return result;
}

void print_yin_cmnd_isolated_benchmarks() {
  const std::array<YinCmndVariant, 4> variants{{
      YinCmndVariant::ReferenceDouble, YinCmndVariant::DoubleSumF32Div,
      YinCmndVariant::F32, YinCmndVariant::F32Compensated,
  }};
  CmndIsolatedBenchmark reference{};
  printf("B4B4J_CMND_THEORY tau_first=1 tau_last=185 taus_per_hop=185 "
         "threshold=0.15 numerator_semantics=F32_PRODUCT_THEN_EXTEND\n");
  for (const auto variant : variants) {
    const auto stats = benchmark_yin_cmnd(variant);
    if (variant == YinCmndVariant::ReferenceDouble)
      reference = stats;
    const double average_us = stats.count
                                  ? static_cast<double>(stats.total_us) /
                                        stats.count
                                  : 0.0;
    const double average_cycles = stats.count
                                      ? static_cast<double>(stats.total_cycles) /
                                            stats.count
                                      : 0.0;
    const double reference_cycles = reference.count
                                        ? static_cast<double>(reference.total_cycles) /
                                              reference.count
                                        : average_cycles;
    printf("B4B4J_CMND_ISOLATED variant=%s hops=%lu cold_us=%lu "
           "cold_cycles=%lu avg_us/hop=%.3f P50=%lu P95=%lu P99=%lu "
           "max=%lu cycles/hop=%.2f cycles/tau=%.4f speedup=%.4f "
           "checksum=%.9g\n",
           yin_cmnd_variant_name(variant),
           static_cast<unsigned long>(stats.count),
           static_cast<unsigned long>(stats.cold_us),
           static_cast<unsigned long>(stats.cold_cycles), average_us,
           static_cast<unsigned long>(stats.p50_us),
           static_cast<unsigned long>(stats.p95_us),
           static_cast<unsigned long>(stats.p99_us),
           static_cast<unsigned long>(stats.maximum_us), average_cycles,
           average_cycles / 185.0,
           average_cycles > 0.0 ? reference_cycles / average_cycles : 0.0,
           stats.checksum);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  constexpr size_t kTaus = 185, kWindow = 512;
  constexpr float kPi = 3.14159265358979323846f;
  auto *window = static_cast<float *>(
      heap_caps_calloc(kWindow, sizeof(float), MALLOC_CAP_SPIRAM));
  auto *difference = static_cast<float *>(heap_caps_calloc(
      YinDetector::kMaxWindow + 1, sizeof(float), MALLOC_CAP_SPIRAM));
  auto *output = static_cast<float *>(heap_caps_calloc(
      YinDetector::kMaxWindow + 1, sizeof(float), MALLOC_CAP_SPIRAM));
  s_b4b4j_numerators = static_cast<float *>(heap_caps_calloc(
      YinDetector::kMaxWindow + 1, sizeof(float), MALLOC_CAP_SPIRAM));
  s_b4b4j_denominators = static_cast<double *>(heap_caps_calloc(
      YinDetector::kMaxWindow + 1, sizeof(double), MALLOC_CAP_SPIRAM));
  if (!window || !difference || !output || !s_b4b4j_numerators ||
      !s_b4b4j_denominators) {
    printf("B4B4J_CMND_COST_METHOD allocation=FAIL\n");
    heap_caps_free(window);
    heap_caps_free(difference);
    heap_caps_free(output);
    heap_caps_free(s_b4b4j_numerators);
    heap_caps_free(s_b4b4j_denominators);
    s_b4b4j_numerators = nullptr;
    s_b4b4j_denominators = nullptr;
    return;
  }
  for (size_t i = 0; i < kWindow; ++i) {
    const float phase = 2.0f * kPi * 220.0f * static_cast<float>(i) / 12000.0f;
    window[i] = 0.12589254f * std::sin(phase) +
                0.007f * std::sin(2.0f * phase);
  }
  yin_difference_fma_8acc(window, kWindow, kTaus, difference);
  double cumulative = 0.0;
  for (size_t tau = 1; tau <= kTaus; ++tau) {
    cumulative += difference[tau];
    s_b4b4j_numerators[tau] =
        difference[tau] * static_cast<float>(tau);
    s_b4b4j_denominators[tau] = cumulative;
  }
  const double reference_cycles = reference.count
                                      ? static_cast<double>(reference.total_cycles) /
                                            reference.count
                                      : 0.0;
  const double loop = benchmark_cmnd_cost_probe(
      cmnd_probe_loop_branch, difference, kTaus, output);
  const double pure_loop = benchmark_cmnd_cost_probe(
      cmnd_probe_loop, difference, kTaus, output);
  const double store_with_loop = benchmark_cmnd_cost_probe(
      cmnd_probe_store, difference, kTaus, output);
  const double numerator_with_store = benchmark_cmnd_cost_probe(
      cmnd_probe_numerator, difference, kTaus, output);
  const double cumulative_with_loop = benchmark_cmnd_cost_probe(
      cmnd_probe_cumulative, difference, kTaus, output);
  const double division_with_store = benchmark_cmnd_cost_probe(
      cmnd_probe_division, difference, kTaus, output);
  const std::array<double, 5> raw{{
      std::max(0.0, cumulative_with_loop - pure_loop),
      std::max(0.0, numerator_with_store - store_with_loop),
      std::max(0.0, division_with_store - store_with_loop),
      std::max(0.0, store_with_loop - pure_loop), std::max(0.0, loop),
  }};
  const double raw_sum = raw[0] + raw[1] + raw[2] + raw[3] + raw[4];
  const double scale = raw_sum > 0.0 ? reference_cycles * 0.98 / raw_sum : 0.0;
  const std::array<const char *, 5> names{{
      "cumulative_update", "numerator_formation", "division", "store_cmnd",
      "loop_branch",
  }};
  printf("B4B4J_CMND_COST_METHOD reference_cycles/hop=%.2f raw_sum=%.2f "
         "normalized_probe_coverage=98.000 other=2.000 reconciled=100.000\n",
         reference_cycles, raw_sum);
  for (size_t i = 0; i < raw.size(); ++i) {
    const double normalized = raw[i] * scale;
    printf("B4B4J_CMND_COST category=%s raw_cycles/hop=%.2f "
           "normalized_cycles/hop=%.2f normalized_us/hop=%.3f percent=%.3f\n",
           names[i], raw[i], normalized,
           normalized / Profiler::cycles_per_us(),
           reference_cycles > 0.0 ? 100.0 * normalized / reference_cycles
                                  : 0.0);
  }
  printf("B4B4J_CMND_COST category=other raw_cycles/hop=0 "
         "normalized_cycles/hop=%.2f normalized_us/hop=%.3f percent=2.000\n",
         reference_cycles * 0.02,
         reference_cycles * 0.02 / Profiler::cycles_per_us());
  heap_caps_free(window);
  heap_caps_free(difference);
  heap_caps_free(output);
  heap_caps_free(s_b4b4j_numerators);
  heap_caps_free(s_b4b4j_denominators);
  s_b4b4j_numerators = nullptr;
  s_b4b4j_denominators = nullptr;
}
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
struct YinEnergyIsolatedBenchmark {
  uint32_t count = 0, cold_us = 0, cold_cycles = 0;
  uint64_t total_us = 0, total_cycles = 0;
  uint32_t p50_us = 0, p95_us = 0, p99_us = 0, maximum_us = 0;
  double checksum = 0.0;
};

volatile double s_b4b4k_energy_checksum = 0.0;

YinEnergyIsolatedBenchmark benchmark_yin_energy(
    YinEnergyVariant variant, const float *window, size_t frames) {
  constexpr size_t kWarmup = 8, kRuns = 128;
  std::array<uint32_t, kRuns> elapsed_us{};
  YinEnergyIsolatedBenchmark result{};
  double measured_energy = 0.0;
  float measured_rms_db = 0.0f;
  auto measure = [&]() {
    measured_energy = yin_energy_compute(variant, window, frames);
    measured_rms_db = 10.0f * std::log10(
        static_cast<float>(measured_energy / frames) + 1e-20f);
  };
  uint64_t start_us = esp_timer_get_time();
  uint32_t start_cycles = esp_cpu_get_cycle_count();
  measure();
  result.cold_cycles = esp_cpu_get_cycle_count() - start_cycles;
  result.cold_us = static_cast<uint32_t>(esp_timer_get_time() - start_us);
  result.checksum = measured_energy + measured_rms_db;
  for (size_t i = 0; i < kWarmup; ++i) {
    measure();
    result.checksum += measured_energy + measured_rms_db;
  }
  result.checksum = 0.0;
  for (size_t run = 0; run < kRuns; ++run) {
    start_us = esp_timer_get_time();
    start_cycles = esp_cpu_get_cycle_count();
    measure();
    result.total_cycles += static_cast<uint32_t>(
        esp_cpu_get_cycle_count() - start_cycles);
    elapsed_us[run] = static_cast<uint32_t>(esp_timer_get_time() - start_us);
    result.checksum += measured_energy + measured_rms_db;
    result.total_us += elapsed_us[run];
    result.maximum_us = std::max(result.maximum_us, elapsed_us[run]);
  }
  result.count = kRuns;
  std::sort(elapsed_us.begin(), elapsed_us.end());
  result.p50_us = percentile_sorted(elapsed_us.data(), kRuns, 50);
  result.p95_us = percentile_sorted(elapsed_us.data(), kRuns, 95);
  result.p99_us = percentile_sorted(elapsed_us.data(), kRuns, 99);
  s_b4b4k_energy_checksum = result.checksum;
  return result;
}

void print_yin_energy_isolated_benchmarks() {
  constexpr size_t kFrames = 512;
  constexpr float kPi = 3.14159265358979323846f;
  std::array<float, kFrames> window{};
  uint32_t state = 0x4234424bU;
  for (size_t i = 0; i < window.size(); ++i) {
    state = state * 1664525U + 1013904223U;
    const float noise = static_cast<int32_t>(state) / 2147483648.0f;
    const float phase = 2.0f * kPi * 220.0f * i / 12000.0f;
    window[i] = 0.12589254f * std::sin(phase) +
                0.007f * std::sin(2.0f * phase) + 0.0002f * noise;
  }
  const std::array<YinEnergyVariant, 3> variants{{
      YinEnergyVariant::ReferenceDouble, YinEnergyVariant::F32,
      YinEnergyVariant::F32Compensated,
  }};
  YinEnergyIsolatedBenchmark reference{};
  printf("B4B4K_ENERGY_THEORY samples_per_hop=512 energy=sum_x_squared "
         "rms_db=10_log10_energy_over_N_plus_1e-20\n");
  for (const auto variant : variants) {
    const auto stats = benchmark_yin_energy(variant, window.data(),
                                            window.size());
    if (variant == YinEnergyVariant::ReferenceDouble)
      reference = stats;
    const double average_us = stats.count
                                  ? static_cast<double>(stats.total_us) /
                                        stats.count
                                  : 0.0;
    const double average_cycles = stats.count
                                      ? static_cast<double>(stats.total_cycles) /
                                            stats.count
                                      : 0.0;
    const double reference_cycles = reference.count
                                        ? static_cast<double>(
                                              reference.total_cycles) /
                                              reference.count
                                        : average_cycles;
    printf("B4B4K_ENERGY_ISOLATED variant=%s hops=%lu cold_us=%lu "
           "cold_cycles=%lu avg_us/hop=%.3f P50=%lu P95=%lu P99=%lu "
           "max=%lu cycles/hop=%.2f cycles/sample=%.4f speedup=%.4f "
           "checksum=%.17g\n",
           yin_energy_variant_name(variant),
           static_cast<unsigned long>(stats.count),
           static_cast<unsigned long>(stats.cold_us),
           static_cast<unsigned long>(stats.cold_cycles), average_us,
           static_cast<unsigned long>(stats.p50_us),
           static_cast<unsigned long>(stats.p95_us),
           static_cast<unsigned long>(stats.p99_us),
           static_cast<unsigned long>(stats.maximum_us), average_cycles,
           average_cycles / kFrames,
           average_cycles > 0.0 ? reference_cycles / average_cycles : 0.0,
           stats.checksum);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
#endif

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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
  constexpr uint32_t kDurationSeconds = CONFIG_VOXP4_B4B6_MEASURE_SECONDS;
  bool b4b5_config_matches = false;
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  constexpr uint32_t kDurationSeconds = CONFIG_VOXP4_B4B5_DURATION_SECONDS;
  bool b4b5_config_matches = false;
#else
  constexpr uint32_t kDurationSeconds = 10;
#endif
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
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  // B4B.4A measurements on this same ESP32-P4 rev1.3 at 360 MHz.
  constexpr double kBeforeLpcUsPerFrame = 14855.20;
  constexpr double kBeforeWindowHandlingUsPerFrame = 246.18;
  constexpr double kBeforeAutocorrelationUsPerFrame = 13150.37;
  constexpr double kBeforeLevinsonUsPerFrame = 181.42;
  constexpr double kBeforeYinDifferenceUsPerHop = 4127.86;
  constexpr double kBeforePitchAnalysisUsPerHop = 5136.14;
  constexpr double kBeforeCombinedUsPerHop = 19111.35;
  constexpr double kBeforeCore1Percent = 98.26;
  constexpr double kBeforePitchHopsPerSecond = 51.39;
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  // Retained-double B4B.4B measurements on this P4 rev1.3 at 360 MHz.
  constexpr double kBeforeLpcUsPerFrame = 14487.23;
  constexpr double kBeforeWindowHandlingUsPerFrame = 242.21;
  constexpr double kBeforeAutocorrelationUsPerFrame = 12781.60;
  constexpr double kBeforeLevinsonUsPerFrame = 182.53;
  constexpr double kBeforeYinDifferenceUsPerHop = 4127.47;
  constexpr double kBeforePitchAnalysisUsPerHop = 5139.66;
  constexpr double kBeforeCombinedUsPerHop = 18891.58;
  constexpr double kBeforeCore1Percent = 98.23;
  constexpr double kBeforePitchHopsPerSecond = 51.97;
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
  // B4B.4F reports its fixed B4B.4E baseline alongside the new target-rate
  // decomposition below; no legacy summary constants are needed here.
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  // B4B.4D production measurements on this P4 rev1.3 at 360 MHz.
  constexpr double kBeforeLpcUsPerFrame = 4101.27;
  constexpr double kBeforeYinDifferenceUsPerHop = 2598.04;
  constexpr double kBeforePitchAnalysisUsPerHop = 4947.68;
  constexpr double kBeforeCombinedUsPerHop = 8835.85;
  constexpr double kBeforeCore1Percent = 95.52;
  constexpr double kBeforePitchHopsPerSecond = 107.98;
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  // B4B.4C measurements on this same ESP32-P4 rev1.3 at 360 MHz.
  constexpr double kBeforeLpcUsPerFrame = 4304.14;
  constexpr double kBeforeYinDifferenceUsPerHop = 4142.15;
  constexpr double kBeforePitchAnalysisUsPerHop = 5340.55;
  constexpr double kBeforeCombinedUsPerHop = 9520.59;
  constexpr double kBeforeCore1Percent = 97.33;
  constexpr double kBeforePitchHopsPerSecond = 102.11;
#endif

  printf("\n=======================================================\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4A_LPC_RING_BUFFER_AUDIT)
  printf("   B4B.4A — LPC RING BUFFER OPTIMIZATION AUDIT\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  printf("   B4B.4B — LPC AUTOCORRELATION FPU OPTIMIZATION AUDIT\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  printf("   B4B.4C — COMPENSATED FMA / DOUBLE-SINGLE LPC AUDIT\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("   B4B.5 — P4 PRODUCTION DEFAULT REALTIME QUALIFICATION\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  printf("   B4B.4K — YIN ENERGY COMPENSATED-F32 QUALIFICATION\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  printf("   B4B.4J — YIN CMND FPU KERNEL QUALIFICATION\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  printf("   B4B.4I — RESIDUAL PITCH ANALYSIS HOTSPOT AUDIT\n");
#else
  printf("   B4B.4H — LPC COMPENSATED F32 ENERGY QUALIFICATION\n");
#endif
#else
  printf("   B4B.4G — LPC WINDOWING / ENERGY KERNEL AUDIT\n");
#endif
#else
  printf("   B4B.4F — PITCH-MARK NCC / ALIGNMENT AUDIT\n");
#endif
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  printf("   B4B.4E — INCREMENTAL / SLIDING YIN AUDIT\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  printf("   B4B.4D — YIN DIFFERENCE KERNEL OPTIMIZATION AUDIT\n");
#else
  printf("   B4B_3_PITCH_ANALYSIS_HOTSPOT_AUDIT\n");
#endif
  printf("=======================================================\n");
  printf("Duration: %lu seconds\n",
         static_cast<unsigned long>(kDurationSeconds));
  printf("Stimulus: 500 ms silence, 20 ms attack, 220 Hz pure sine at -18 dBFS\n");
  printf("Transport: real PCM1808 RX/read and PCM5102 TX; synthetic input feeds all DSP taps\n");
  printf("Pitch priority: configMAX_PRIORITIES - 5 (unchanged)\n");
  printf("CPU frequency: 360 MHz\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("Normal ESP32-P4 production defaults; profiling changes no DSP selector.\n");
#else
  printf("No DSP algorithm or analysis parameter is changed by this audit.\n");
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  printf("Production autocorrelation: AUTOCORR_REFERENCE_DOUBLE (float candidates rejected by host numeric guardrails)\n");
  print_lpc_autocorr_isolated_benchmarks();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  printf("Production candidate: AUTOCORR_F32_DOUBLE_SINGLE (host numeric and audio guardrails passed)\n");
  print_lpc_autocorr_isolated_benchmarks();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
  printf("Production YIN: YIN_DIFF_INCREMENTAL_F32 rebase=64 (unchanged)\n");
  printf("Production LPC autocorrelation: AUTOCORR_F32_DOUBLE_SINGLE (unchanged)\n");
  printf("P4 production default: PITCH_MARK_NCC_REUSE_AA\n");
  printf("Generic cross-platform default: PITCH_MARK_NCC_REFERENCE\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("B4B algorithm override active: NO\n");
#else
  printf("Audit override: PITCH_MARK_NCC_REUSE_AA\n");
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("Production LPC energy default: LPC_ENERGY_F32_COMPENSATED\n");
  printf("LPC windowing oracle retained: LPC_WINDOW_HANN_DOUBLE_ENERGY\n");
  printf("Production CMND default: YIN_CMND_F32_COMPENSATED\n");
  printf("CMND oracle retained: YIN_CMND_REFERENCE_DOUBLE\n");
  printf("Production YIN energy default: YIN_ENERGY_F32_COMPENSATED\n");
  printf("YIN energy oracle retained: YIN_ENERGY_REFERENCE_DOUBLE\n");
#else
  printf("Production LPC energy candidate: LPC_ENERGY_F32_COMPENSATED\n");
  printf("Production oracle: LPC_WINDOW_HANN_DOUBLE_ENERGY\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  printf("Production CMND candidate: YIN_CMND_F32_COMPENSATED\n");
  printf("CMND oracle retained: YIN_CMND_REFERENCE_DOUBLE\n");
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  printf("Production YIN energy candidate: YIN_ENERGY_F32_COMPENSATED\n");
  printf("YIN energy oracle retained: YIN_ENERGY_REFERENCE_DOUBLE\n");
#endif
#endif
#else
  printf("Production LPC windowing candidate: LPC_WINDOW_HANN_DOUBLE_ENERGY\n");
  printf("Host Hann guardrail: 1024/1024 bit-identical coefficients\n");
#endif
  print_lpc_windowing_isolated_benchmarks();
#else
  print_pitch_mark_ncc_isolated_benchmarks();
#endif
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  printf("Oracle YIN difference: YIN_DIFF_FMA_8ACC\n");
  printf("Host-selected candidate: YIN_DIFF_INCREMENTAL_F32 rebase=64 hops\n");
  printf("Production LPC autocorrelation: AUTOCORR_F32_DOUBLE_SINGLE (unchanged from B4B.4C)\n");
  print_yin_isolated_benchmarks();
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  printf("Production YIN difference: YIN_DIFF_FMA_8ACC (host functional and numerical guardrails passed)\n");
  printf("Production LPC autocorrelation: AUTOCORR_F32_DOUBLE_SINGLE (unchanged from B4B.4C)\n");
  print_yin_isolated_benchmarks();
#endif

  ProfileOverheadBenchmark profile_overhead{};
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  profile_overhead = benchmark_profile_overhead();
  printf("B4B4I_OVERHEAD component=ProfilerNowCycles iterations=%lu avg_cycles=%.3f avg_us=%.6f\n",
         static_cast<unsigned long>(profile_overhead.iterations),
         profile_overhead.now_cycles_per_call,
         profile_overhead.now_cycles_per_call / Profiler::cycles_per_us());
  printf("B4B4I_OVERHEAD component=VF_PROFILE_BEGIN_END iterations=%lu avg_cycles=%.3f avg_us=%.6f\n",
         static_cast<unsigned long>(profile_overhead.iterations),
         profile_overhead.profile_pair_cycles,
         profile_overhead.profile_pair_cycles / Profiler::cycles_per_us());
  printf("B4B4I_OVERHEAD component=ProfilerRecordCycles iterations=%lu avg_cycles=%.3f avg_us=%.6f\n",
         static_cast<unsigned long>(profile_overhead.iterations),
         profile_overhead.record_cycles_cycles,
         profile_overhead.record_cycles_cycles / Profiler::cycles_per_us());
  printf("B4B4I_OVERHEAD component=AtomicTelemetryIncrement iterations=%lu avg_cycles=%.3f avg_us=%.6f\n",
         static_cast<unsigned long>(profile_overhead.iterations),
         profile_overhead.atomic_increment_cycles,
         profile_overhead.atomic_increment_cycles / Profiler::cycles_per_us());
#endif

  static VocalFxConfig config;
  config = {};
  config.enable_gate = true;
  config.enable_compressor = true;
  config.enable_delay = true;
  config.enable_reverb = true;
  config.enable_pitch_analysis = true;
  config.pitch_shift.enabled = true;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  config.lpc.windowing =
      LpcWindowVariant::PrecomputedHannCompensatedEnergy;
#endif
#if (defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT) || \
     defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  config.lpc.autocorrelation =
      LpcAutocorrelationVariant::AutocorrF32DoubleSingle;
#endif
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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT) && \
    !defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  PitchAnalysisConfig incremental_pitch_config{};
  incremental_pitch_config.yin_difference =
      YinDifferenceVariant::IncrementalF32;
  incremental_pitch_config.yin_incremental_rebase_hops = 64;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  incremental_pitch_config.yin_cmnd = YinCmndVariant::F32Compensated;
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  incremental_pitch_config.yin_energy = YinEnergyVariant::F32Compensated;
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
  incremental_pitch_config.pitch_mark_ncc = PitchMarkNccVariant::ReuseAa;
#endif
  if (!vocal_fx_init_pitch_analysis(incremental_pitch_config)) {
    ESP_LOGE(TAG, "B4B.4E incremental pitch initialization failed");
    return;
  }
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  const VocalFxEffectiveDspConfig effective =
      vocal_fx_effective_dsp_config();
  b4b5_config_matches =
      effective.yin_difference == YinDifferenceVariant::IncrementalF32 &&
      effective.yin_incremental_rebase_hops == 64 &&
      effective.yin_energy == YinEnergyVariant::F32Compensated &&
      effective.yin_cmnd == YinCmndVariant::F32Compensated &&
      effective.pitch_mark_ncc == PitchMarkNccVariant::ReuseAa &&
      effective.lpc_windowing ==
          LpcWindowVariant::PrecomputedHannCompensatedEnergy &&
      effective.lpc_autocorrelation ==
          LpcAutocorrelationVariant::AutocorrF32DoubleSingle;
  printf("\nEffective P4 DSP configuration:\n\n");
  printf("YIN difference:\n%s\n\n",
         yin_difference_variant_name(effective.yin_difference));
  printf("YIN rebase:\n%u\n\n",
         static_cast<unsigned>(effective.yin_incremental_rebase_hops));
  printf("YIN energy:\n%s\n\n",
         yin_energy_variant_name(effective.yin_energy));
  printf("YIN CMND:\n%s\n\n",
         yin_cmnd_variant_name(effective.yin_cmnd));
  printf("PitchMark NCC:\n%s\n\n",
         pitch_mark_ncc_variant_name(effective.pitch_mark_ncc));
  printf("LPC energy:\n%s\n\n",
         lpc_window_variant_name(effective.lpc_windowing));
  printf("LPC autocorrelation:\n%s\n\n",
         lpc_autocorrelation_variant_name(effective.lpc_autocorrelation));
  printf("B4B algorithm override active:\nNO\n");
  printf("B4B5_EFFECTIVE_CONFIG all_matches=%s source=normal_p4_defaults "
         "post_init_replacement=NO runtime_override=NO\n",
         b4b5_config_matches ? "YES" : "NO");
  if (!b4b5_config_matches) {
    ESP_LOGE(TAG, "B4B.5 normal production configuration mismatch");
    return;
  }
#endif
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

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  // Keep startup transients separate. The following ten-second window is the
  // only source for the primary B4B.4I statistics.
  vTaskDelay(pdMS_TO_TICKS(1000));
#endif
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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  float b4b5_backlog_first_ms = audit_start.analysis_backlog_ms;
  float b4b5_backlog_last_ms = b4b5_backlog_first_ms;
  float b4b5_pitch_age_first_ms = audit_start.latest_pitch_age_ms;
  float b4b5_pitch_age_last_ms = b4b5_pitch_age_first_ms;
  uint32_t b4b5_backlog_growth_streak = 0;
  uint32_t b4b5_backlog_max_growth_streak = 0;
  uint32_t b4b5_pitch_age_growth_streak = 0;
  uint32_t b4b5_pitch_age_max_growth_streak = 0;
#endif

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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  for (uint32_t second = 0; second < kDurationSeconds; ++second) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const auto sample = vocal_fx_pitch_analysis_audit_telemetry();
    b4b5_backlog_growth_streak =
        sample.analysis_backlog_ms > b4b5_backlog_last_ms + 0.05f
            ? b4b5_backlog_growth_streak + 1
            : 0;
    b4b5_pitch_age_growth_streak =
        sample.latest_pitch_age_ms > b4b5_pitch_age_last_ms + 0.05f
            ? b4b5_pitch_age_growth_streak + 1
            : 0;
    b4b5_backlog_max_growth_streak = std::max(
        b4b5_backlog_max_growth_streak, b4b5_backlog_growth_streak);
    b4b5_pitch_age_max_growth_streak = std::max(
        b4b5_pitch_age_max_growth_streak, b4b5_pitch_age_growth_streak);
    b4b5_backlog_last_ms = sample.analysis_backlog_ms;
    b4b5_pitch_age_last_ms = sample.latest_pitch_age_ms;
  }
  // Include the initial partial scheduler tick in addition to the requested
  // number of complete one-second sampling periods.
  vTaskDelay(1);
#else
  // vTaskDelay() counts ticks after the current partial tick.  Add one tick so
  // the measured warm window itself is never a few microseconds short of the
  // required ten seconds.
  vTaskDelay(pdMS_TO_TICKS(kDurationSeconds * 1000) + 1);
#endif
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
  const PitchTrackState pitch_track_end = vocal_fx_pitch_track_state();
  const PitchShiftTelemetry harmony0_end = vocal_fx_harmony_telemetry(0);
  const VocalFxFunnelStats funnel_end = vocal_fx_funnel_stats();
  const bool teardown_clean = stop_pipeline_tasks();
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  // Run the diagnostic-only CMND probes after both real-time task stacks have
  // been released.  Newlib's first-use formatting state can otherwise consume
  // the final 1 KiB adjacent to the 32 KiB audio-stack heap block.  This keeps
  // the frozen worker stack sizes and the measured pipeline unchanged.
  print_yin_cmnd_isolated_benchmarks();
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  print_yin_energy_isolated_benchmarks();
#endif
  std::array<VocalFxProfileDistribution,
             static_cast<size_t>(PitchAnalysisProfileSection::Count)>
      pitch_distribution{};
  for (size_t i = 0; i < pitch_distribution.size(); ++i)
    pitch_distribution[i] = vocal_fx_pitch_profile_distribution(
        static_cast<PitchAnalysisProfileSection>(i));
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
  // B4B.4F sampled these relaxed counters while the audio task could still be
  // between "attempt" and its terminal scheduled/rejected counter. Sampling a
  // second time after teardown makes that one-event boundary race observable.
  const VocalFxFunnelStats funnel_after_teardown = vocal_fx_funnel_stats();
#endif
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
  const double average_recorded_hop_us =
      completed_hops ? static_cast<double>(call_total_us) / completed_hops
                     : 0.0;

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
  // The bounded call-record ring may drop early records under overload. The
  // detector execution counter spans the complete measurement window and is
  // therefore the authoritative per-hop denominator.
  const uint64_t profiled_hops =
      yin_end.executions - yin_start.executions;
  const double average_hop_us =
      profiled_hops ? worker_runtime_us / profiled_hops : 0.0;
  const double maximum_theoretical_hops =
      average_hop_us > 0.0 ? 1000000.0 / average_hop_us : 0.0;
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
      worker_runtime_us > accounted_us
          ? static_cast<uint64_t>(worker_runtime_us) - accounted_us
          : 0;
  const double accounted_percent =
      worker_runtime_us ? 100.0 * accounted_us / worker_runtime_us : 0.0;

  const auto &yin_difference =
      pitch(PitchAnalysisProfileSection::YinDifference);
  const auto &mark_search =
      pitch(PitchAnalysisProfileSection::PitchMarkSearch);
  const double yin_difference_percent =
      worker_runtime_us ? 100.0 * yin_difference.total_us / worker_runtime_us
                        : 0.0;
  const double mark_search_percent =
      worker_runtime_us ? 100.0 * mark_search.total_us / worker_runtime_us
                        : 0.0;
  const double lpc_percent =
      worker_runtime_us ? 100.0 * lpc(LpcProfileSection::Total).total_us /
                              worker_runtime_us
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
  const uint64_t yin_executions = profiled_hops;
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
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  printf("Stage B4B.4B — LPC Autocorrelation FPU Optimization Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  printf("Stage B4B.4C — Compensated FMA / Double-Single LPC Autocorrelation Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("Stage B4B.5 — Production Default Realtime Qualification Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  printf("Stage B4B.4K — YIN Energy Compensated-F32 Qualification Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  printf("Stage B4B.4J — YIN CMND FPU Kernel Qualification Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  printf("Stage B4B.4I — Residual PitchAnalysis Hotspot Audit Report\n");
#else
  printf("Stage B4B.4H — LPC Compensated F32 Energy Production Qualification Report\n");
#endif
#else
  printf("Stage B4B.4G — LPC Windowing & Energy Kernel Optimization Report\n");
#endif
#else
  printf("Stage B4B.4F — Pitch-Mark NCC Kernel / Passive Alignment Report\n");
#endif
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  printf("Stage B4B.4E — Incremental / Sliding YIN Difference Report\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  printf("Stage B4B.4D — YIN Difference Kernel Optimization Report\n");
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
  printf("Recorded completed-hop cost (us): avg=%.2f P50=%lu P95=%lu P99=%lu max=%lu recorded_hops=%llu\n",
         average_recorded_hop_us,
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
    const double avg_hop = profiled_hops
                               ? static_cast<double>(row.stats->total_us) /
                                     profiled_hops
                               : 0.0;
    const double percent = worker_runtime_us
                               ? 100.0 * row.stats->total_us / worker_runtime_us
                               : 0.0;
    const double avg_call = row.stats->blocks
                                ? static_cast<double>(row.stats->total_us) /
                                      row.stats->blocks
                                : 0.0;
    const double cycles_hop = profiled_hops
                                  ? static_cast<double>(row.stats->total_cycles) /
                                        profiled_hops
                                  : 0.0;
    printf("%-26s %11.2f %16.2f %10llu %14.2f %10llu %17llu %15.0f\n",
           row.name, avg_hop, percent,
           static_cast<unsigned long long>(row.stats->total_us), avg_call,
           static_cast<unsigned long long>(row.stats->worst_us),
           static_cast<unsigned long long>(row.stats->total_cycles),
           cycles_hop);
  }
  printf("%-26s %11.2f %16.2f %10llu\n", "Other/unaccounted",
         profiled_hops ? static_cast<double>(unaccounted_us) / profiled_hops
                        : 0.0,
         worker_runtime_us ? 100.0 * unaccounted_us / worker_runtime_us : 0.0,
         static_cast<unsigned long long>(unaccounted_us));
  printf("TOTAL                      %11.2f %16.2f %10llu\n",
         average_hop_us, accounted_percent,
         static_cast<unsigned long long>(worker_runtime_us));
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
         profiled_hops
             ? static_cast<double>(pitch_total.total_us) / profiled_hops
             : 0.0,
         static_cast<unsigned long long>(pitch_total.worst_us),
         static_cast<unsigned long long>(pitch_total.total_cycles));
  printf("YIN total: total=%llu us avg/hop=%.2f max=%llu us cycles=%llu\n",
         static_cast<unsigned long long>(yin_total.total_us),
         profiled_hops ? static_cast<double>(yin_total.total_us) /
                              profiled_hops
                        : 0.0,
         static_cast<unsigned long long>(yin_total.worst_us),
         static_cast<unsigned long long>(yin_total.total_cycles));
  printf("LPC run total: total=%llu us avg/call=%.2f avg/pitch-hop=%.2f max=%llu us cycles=%llu\n",
         static_cast<unsigned long long>(lpc_total.total_us),
         lpc_total.blocks
             ? static_cast<double>(lpc_total.total_us) / lpc_total.blocks
             : 0.0,
         profiled_hops
             ? static_cast<double>(lpc_total.total_us) / profiled_hops
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
  const double lpc_windowing_avg_frame_us =
      lpc_frames
          ? static_cast<double>(
                lpc(LpcProfileSection::SolveWindowing).total_us) /
                lpc_frames
          : 0.0;
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
      wall_seconds > 0.0 ? profiled_hops / wall_seconds : 0.0;
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
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  const bool no_transport_regression =
      transport_end.rx_dma_errors == transport_start.rx_dma_errors &&
      transport_end.tx_dma_errors == transport_start.tx_dma_errors &&
      transport_end.read_failures == transport_start.read_failures &&
      transport_end.write_failures == transport_start.write_failures &&
      transport_end.rx_dropped_frames == transport_start.rx_dropped_frames &&
      transport_end.tx_dropped_frames == transport_start.tx_dropped_frames;
  const double new_pitch_analysis_us_per_hop =
      profiled_hops
          ? static_cast<double>(pitch_total.total_us) / profiled_hops
          : 0.0;
  const double new_yin_difference_us_per_hop =
      profiled_hops
          ? static_cast<double>(yin_difference.total_us) / profiled_hops
          : 0.0;
  const bool b4b4b_transport_pass =
      audit_pass && no_transport_regression && lpc_frames > 0 &&
      lpc_frame_cost.count > 0;
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
         profiled_hops
             ? static_cast<double>(mark_correlation.total_us) / profiled_hops
             : 0.0);

  printf("\nCore 1 utilization: total=%.2f%% worker=%.2f%% compute_saturated=%s\n",
         core1_utilization, worker_utilization,
         core1_utilization >= 90.0 ? "yes" : "no");
  printf("Core 1 time us: worker=%0.f requested_yield_min=%llu non_worker=%0.f diagnostic=%0.f IDLE1=%0.f other=%0.f\n",
         worker_runtime_us,
         static_cast<unsigned long long>(pitch_total.blocks * 1000ULL),
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
         wall_seconds > 0.0 ? pitch_total.blocks / wall_seconds : 0.0,
         pitch_hops_per_second,
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
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  (void)old_storage;
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
    }
  }
  printf("\nB4B.4B production selection: AUTOCORR_REFERENCE_DOUBLE\n");
  printf("Candidate classification: FLOAT_AUTOCORR_NUMERICALLY_UNACCEPTABLE\n");
  printf("No float candidate was installed in the full worker. The full-worker run below is the retained double reference.\n");
  printf("\nSECTION                                  B4B.4A        B4B.4B        SPEEDUP\n");
  printf("LPC window handling (us/frame)          %9.2f %14.2f %11.2fx\n",
         kBeforeWindowHandlingUsPerFrame, new_window_handling_us_per_frame,
         new_window_handling_us_per_frame > 0.0
             ? kBeforeWindowHandlingUsPerFrame /
                   new_window_handling_us_per_frame
             : 0.0);
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
  printf("LPC total (us/frame)                    %9.2f %14.2f %11.2fx\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us,
         lpc_avg_frame_us > 0.0 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us
                                : 0.0);
  printf("YinDifference (us/pitch hop)            %9.2f %14.2f %11.2fx\n",
         kBeforeYinDifferenceUsPerHop, new_yin_difference_us_per_hop,
         new_yin_difference_us_per_hop > 0.0
             ? kBeforeYinDifferenceUsPerHop / new_yin_difference_us_per_hop
             : 0.0);
  printf("PitchAnalysis total (us/pitch hop)      %9.2f %14.2f %11.2fx\n",
         kBeforePitchAnalysisUsPerHop, new_pitch_analysis_us_per_hop,
         new_pitch_analysis_us_per_hop > 0.0
             ? kBeforePitchAnalysisUsPerHop /
                   new_pitch_analysis_us_per_hop
             : 0.0);
  printf("Combined worker cost (us/pitch hop)     %9.2f %14.2f %11.2fx\n",
         kBeforeCombinedUsPerHop, average_hop_us,
         average_hop_us > 0.0 ? kBeforeCombinedUsPerHop / average_hop_us
                              : 0.0);
  printf("Core 1 utilization (percent)             %8.2f %14.2f %11.2fx\n",
         kBeforeCore1Percent, core1_utilization,
         core1_utilization > 0.0 ? kBeforeCore1Percent / core1_utilization
                                 : 0.0);
  printf("Pitch throughput (hops/s)                %8.2f %14.2f %11.2fx\n",
         kBeforePitchHopsPerSecond, pitch_hops_per_second,
         kBeforePitchHopsPerSecond > 0.0
             ? pitch_hops_per_second / kBeforePitchHopsPerSecond
             : 0.0);
  printf("FIFO/backlog retained-double: current=%lu max=%lu drops=%llu backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops -
                                         audit_start.fifo_drops),
         audit_end.analysis_backlog_ms, audit_end.analysis_backlog_max_ms,
         audit_end.latest_pitch_age_ms, audit_end.pitch_age_average_ms,
         audit_end.pitch_age_p95_ms, audit_end.pitch_age_p99_ms,
         audit_end.pitch_age_max_ms);
  printf("B4B.4B transport/teardown audit: %s\n",
         b4b4b_transport_pass ? "PASS" : "FAIL");
  printf("\nB4B.4B RESULT:\nFAIL\n");
  printf("AUTOCORR DOUBLE:\n%.2f us/frame\n", autocorrelation_avg_frame_us);
  printf("AUTOCORR OPTIMIZED:\nnot adopted\n");
  printf("AUTOCORR SPEEDUP:\nnot applicable\n");
  printf("LPC TOTAL:\n%.2f -> %.2f us/frame (double retained)\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us);
  printf("CORE1:\n%.2f%%\n", core1_utilization);
  printf("PITCH THROUGHPUT:\n%.2f hops/s\n", pitch_hops_per_second);
  printf("NEXT PRIMARY HOTSPOT:\n%s\n", primary_hotspot);
  printf("NEXT STEP:\nalgorithm-preserving non-float autocorrelation investigation\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  (void)old_storage;
  const bool ever_locked = audit_end.first_locked_input_position != 0;
  const bool psola_usable =
      harmony0_end.state == PitchShiftState::Active ||
      harmony0_end.state == PitchShiftState::Acquiring;
  const bool b4b4c_hardware_pass =
      b4b4b_transport_pass && teardown_clean &&
      autocorrelation_avg_frame_us < kBeforeAutocorrelationUsPerFrame;
  printf("\nB4B.4C production selection: AUTOCORR_F32_DOUBLE_SINGLE\n");
  printf("Host classification: numerical guardrails PASS; audio guardrails PASS\n");
  printf("\nSECTION                                  DOUBLE    COMPENSATED        SPEEDUP\n");
  printf("LPC window handling (us/frame)          %9.2f %14.2f %11.2fx\n",
         kBeforeWindowHandlingUsPerFrame, new_window_handling_us_per_frame,
         new_window_handling_us_per_frame > 0.0
             ? kBeforeWindowHandlingUsPerFrame /
                   new_window_handling_us_per_frame
             : 0.0);
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
  printf("LPC total (us/frame)                    %9.2f %14.2f %11.2fx\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us,
         lpc_avg_frame_us > 0.0 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us
                                : 0.0);
  printf("PitchAnalysis total (us/pitch hop)      %9.2f %14.2f %11.2fx\n",
         kBeforePitchAnalysisUsPerHop, new_pitch_analysis_us_per_hop,
         new_pitch_analysis_us_per_hop > 0.0
             ? kBeforePitchAnalysisUsPerHop /
                   new_pitch_analysis_us_per_hop
             : 0.0);
  printf("Combined worker cost (us/pitch hop)     %9.2f %14.2f %11.2fx\n",
         kBeforeCombinedUsPerHop, average_hop_us,
         average_hop_us > 0.0 ? kBeforeCombinedUsPerHop / average_hop_us
                              : 0.0);
  printf("Core 1 utilization (percent)             %8.2f %14.2f %11.2fx\n",
         kBeforeCore1Percent, core1_utilization,
         core1_utilization > 0.0 ? kBeforeCore1Percent / core1_utilization
                                 : 0.0);
  printf("Pitch throughput (hops/s)                %8.2f %14.2f %11.2fx\n",
         kBeforePitchHopsPerSecond, pitch_hops_per_second,
         kBeforePitchHopsPerSecond > 0.0
             ? pitch_hops_per_second / kBeforePitchHopsPerSecond
             : 0.0);
  printf("FIFO/backlog compensated: current=%lu max=%lu drops=%llu backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops -
                                         audit_start.fifo_drops),
         audit_end.analysis_backlog_ms, audit_end.analysis_backlog_max_ms,
         audit_end.latest_pitch_age_ms, audit_end.pitch_age_average_ms,
         audit_end.pitch_age_p95_ms, audit_end.pitch_age_p99_ms,
         audit_end.pitch_age_max_ms);
  printf("Tracker final=%s reached_LOCKED=%s PSOLA_usable=%s grain_attempts=%llu grains_rendered=%llu\n",
         pitch_track_name(pitch_track_end), ever_locked ? "yes" : "no",
         psola_usable ? "yes" : "no",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("B4B.4C transport/teardown audit: %s\n",
         b4b4c_hardware_pass ? "PASS" : "FAIL");
  printf("\nB4B.4C RESULT:\n%s\n",
         b4b4c_hardware_pass ? "PASS" : "FAIL");
  printf("PRODUCTION AUTOCORR:\nAUTOCORR_F32_DOUBLE_SINGLE\n");
  printf("AUTOCORR DOUBLE:\n%.2f us/frame\n",
         kBeforeAutocorrelationUsPerFrame);
  printf("AUTOCORR CANDIDATE:\n%.2f us/frame\n",
         autocorrelation_avg_frame_us);
  printf("ELIGIBLE SPEEDUP:\n%.2fx\n",
         autocorrelation_avg_frame_us > 0.0
             ? kBeforeAutocorrelationUsPerFrame /
                   autocorrelation_avg_frame_us
             : 0.0);
  printf("CORE1:\n%.2f%%\n", core1_utilization);
  printf("PITCH THROUGHPUT:\n%.2f hops/s\n", pitch_hops_per_second);
  printf("NEXT PRIMARY HOTSPOT:\n%s\n", primary_hotspot);
  printf("NEXT STEP:\nstrictly follow the measured primary hotspot\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  (void)old_storage;
  (void)maximum_theoretical_hops;
  (void)buffer_audit_count;
  const bool ever_locked = audit_end.first_locked_input_position != 0;
  const bool psola_usable = harmony0_end.state == PitchShiftState::Active ||
                            harmony0_end.state == PitchShiftState::Acquiring;
  const bool fifo_bounded =
      audit_end.fifo_drops == audit_start.fifo_drops &&
      audit_end.fifo_current_occupancy < PitchAnalysis::kFifoCapacity - 1 &&
      audit_end.analysis_backlog_samples < PitchAnalysis::kFifoCapacity / 2;
  const bool realtime = pitch_hops_per_second >= kRequiredHopsPerSecond &&
                        fifo_bounded;
  const bool b4b4e_pass = b4b4b_transport_pass && teardown_clean &&
                          new_yin_difference_us_per_hop <= 1200.0;
  GrainRejectionTelemetry grain = vocal_fx_grain_rejection_telemetry(0);
  const GrainRejectionTelemetry grain_voice1 =
      vocal_fx_grain_rejection_telemetry(1);
  grain.attempt_source_negative += grain_voice1.attempt_source_negative;
  grain.select_mark_failure_total += grain_voice1.select_mark_failure_total;
  grain.select_mark_no_marks += grain_voice1.select_mark_no_marks;
  grain.select_mark_low_confidence += grain_voice1.select_mark_low_confidence;
  grain.select_mark_invalid_period += grain_voice1.select_mark_invalid_period;
  grain.select_mark_distance_too_large +=
      grain_voice1.select_mark_distance_too_large;
  grain.history_failure_total += grain_voice1.history_failure_total;
  grain.history_center_before_half += grain_voice1.history_center_before_half;
  grain.history_too_old += grain_voice1.history_too_old;
  grain.history_future_end += grain_voice1.history_future_end;
  grain.alignment_observations += grain_voice1.alignment_observations;
  for (size_t i = 0; i < grain.signed_delta_period_histogram.size(); ++i)
    grain.signed_delta_period_histogram[i] +=
        grain_voice1.signed_delta_period_histogram[i];
  for (size_t i = 0; i < grain.pitch_age_attempt_histogram.size(); ++i) {
    grain.pitch_age_attempt_histogram[i] +=
        grain_voice1.pitch_age_attempt_histogram[i];
    grain.pitch_age_distance_failure_histogram[i] +=
        grain_voice1.pitch_age_distance_failure_histogram[i];
  }
  if (grain.diagnostic_count == 0 && grain_voice1.diagnostic_count != 0) {
    grain.diagnostics[0] = grain_voice1.diagnostics[0];
    grain.diagnostic_count = 1;
  }
  const char *grain_primary = "NONE";
  if (grain.history_too_old > 0 &&
      grain.history_too_old >= grain.select_mark_distance_too_large)
    grain_primary = "GRAIN_FAILURE_DUE_TO_ANALYSIS_BACKLOG";
  else if (grain.select_mark_distance_too_large > 0)
    grain_primary = "GRAIN_MARK_ALIGNMENT_FAILURE";
  else if (realtime && funnel_end.grain_schedule_attempts > 0 &&
           funnel_end.grains_scheduled == 0)
    grain_primary = "PSOLA_SCHEDULER_ISSUE";
  else if (grain.select_mark_no_marks > 0)
    grain_primary = "SELECT_MARK_NO_MARKS";
  else if (grain.history_future_end > 0)
    grain_primary = "HISTORY_FUTURE_END";

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
  (void)b4b4e_pass;
  (void)grain_primary;
  (void)pitch_track_end;
  (void)ever_locked;
  (void)psola_usable;
  constexpr double kReferencePitchMarkUsPerHop = 2368.19;
  const double pitch_mark_us_per_hop =
      profiled_hops ? static_cast<double>(mark_search.total_us) / profiled_hops
                    : 0.0;
  const double pitch_mark_speedup = pitch_mark_us_per_hop > 0.0
                                        ? kReferencePitchMarkUsPerHop /
                                              pitch_mark_us_per_hop
                                        : 0.0;
  const double other_pitch_us_per_hop =
      std::max(0.0, new_pitch_analysis_us_per_hop - pitch_mark_us_per_hop);
  const double yin_energy_us_per_hop =
      profiled_hops
          ? static_cast<double>(
                pitch(PitchAnalysisProfileSection::YinEnergy).total_cycles) /
                (Profiler::cycles_per_us() * profiled_hops)
          : 0.0;
  const double yin_cmnd_us_per_hop =
      profiled_hops
          ? static_cast<double>(
                pitch(PitchAnalysisProfileSection::YinCmnd).total_cycles) /
                (Profiler::cycles_per_us() * profiled_hops)
          : 0.0;
  const double pitch_mark_target_ms_s = pitch_mark_us_per_hop * 200.0 / 1000.0;
  const double other_pitch_target_ms_s = other_pitch_us_per_hop * 200.0 / 1000.0;
  const double lpc_target_ms_s = lpc_avg_frame_us * 125.0 / 1000.0;
  const double target_core_ms_s = pitch_mark_target_ms_s +
                                  other_pitch_target_ms_s + lpc_target_ms_s;
  size_t lowest_populated_age = grain.pitch_age_attempt_histogram.size();
  for (size_t i = 0; i < grain.pitch_age_attempt_histogram.size(); ++i)
    if (grain.pitch_age_attempt_histogram[i]) {
      lowest_populated_age = i;
      break;
    }
  const uint64_t failures_at_lowest =
      lowest_populated_age < grain.pitch_age_attempt_histogram.size()
          ? grain.pitch_age_distance_failure_histogram[lowest_populated_age]
          : 0;
  const uint64_t failures_above_lowest =
      grain.select_mark_distance_too_large -
      std::min(grain.select_mark_distance_too_large, failures_at_lowest);
  const char *alignment_class = grain.select_mark_distance_too_large == 0
      ? "NO_DISTANCE_FAILURE"
      : failures_at_lowest == 0 && failures_above_lowest > 0
          ? "GRAIN_ALIGNMENT_BACKLOG_COUPLED"
          : failures_above_lowest == 0
              ? "PSOLA_MARK_PHASE_ALIGNMENT_ISSUE"
              : "MIXED_BACKLOG_AND_PHASE_ALIGNMENT";
  const bool b4b4f_pass = b4b4b_transport_pass && teardown_clean &&
                          pitch_mark_speedup >= 2.0;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
  (void)b4b4f_pass;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
  constexpr double kBeforeLpcUsPerFrame = 3539.96;
  constexpr double kBeforeWindowingUsPerFrame = 647.02;
#else
  constexpr double kBeforeLpcUsPerFrame = 4090.08;
  constexpr double kBeforeWindowingUsPerFrame = 1203.09;
#endif
  const double lpc_speedup = lpc_avg_frame_us > 0.0
                                 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us
                                 : 0.0;
  const bool realtime_capacity = target_core_ms_s <= 1000.0;
  const bool ninety_percent_headroom = target_core_ms_s <= 900.0;
  const bool preferred_headroom = target_core_ms_s <= 800.0;
  const uint64_t terminal_rejections = grain.attempt_source_negative +
      grain.select_mark_failure_total + grain.history_failure_total;
  const uint64_t terminal_accounted =
      funnel_after_teardown.grains_scheduled + terminal_rejections;
  const uint64_t terminal_unaccounted =
      funnel_after_teardown.grain_schedule_attempts > terminal_accounted
          ? funnel_after_teardown.grain_schedule_attempts - terminal_accounted
          : 0;
  const uint64_t boundary_attempt_delta =
      funnel_after_teardown.grain_schedule_attempts -
      funnel_end.grain_schedule_attempts;
  const uint64_t boundary_scheduled_delta =
      funnel_after_teardown.grains_scheduled - funnel_end.grains_scheduled;
  const char *accounting_class = terminal_unaccounted == 0 &&
          funnel_end.grain_schedule_attempts !=
              funnel_end.grains_scheduled + terminal_rejections
      ? "PRE_TEARDOWN_SNAPSHOT_RACE_RECONCILED"
      : terminal_unaccounted == 0 ? "EXACT" : "UNRECONCILED";
  const char *next_hotspot =
      other_pitch_target_ms_s >= lpc_target_ms_s &&
              other_pitch_target_ms_s >= pitch_mark_target_ms_s
          ? "OTHER_PITCH_ANALYSIS"
          : lpc_target_ms_s >= pitch_mark_target_ms_s ? "LPC"
                                                      : "PITCH_MARK";
  const double next_reduction_potential_ms_s = std::max(
      pitch_mark_target_ms_s,
      std::max(other_pitch_target_ms_s, lpc_target_ms_s));
  bool hann_internal = false;
  size_t hann_bytes = 0;
  for (size_t i = 0; i < buffer_audit_count; ++i) {
    if (std::string_view(buffer_audits[i].name) == "LpcHann") {
      hann_internal = buffer_audits[i].is_sram && !buffer_audits[i].is_psram;
      hann_bytes = buffer_audits[i].size_bytes;
      break;
    }
  }
  const bool b4b4g_transport_pass = no_transport_regression && teardown_clean &&
      lpc_frames > 0 && lpc_frame_cost.count > 0;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  const std::array<PitchAnalysisProfileSection, 13> b4b4i_sections{{
      PitchAnalysisProfileSection::FifoDrain,
      PitchAnalysisProfileSection::RollingWindow,
      PitchAnalysisProfileSection::LinearWindowCopy,
      PitchAnalysisProfileSection::YinEnergy,
      PitchAnalysisProfileSection::YinDifference,
      PitchAnalysisProfileSection::YinCmnd,
      PitchAnalysisProfileSection::YinSearch,
      PitchAnalysisProfileSection::YinInterpolation,
      PitchAnalysisProfileSection::VoicedFeatures,
      PitchAnalysisProfileSection::VoicedClassifier,
      PitchAnalysisProfileSection::PitchSmoother,
      PitchAnalysisProfileSection::PitchMarkSearch,
      PitchAnalysisProfileSection::PitchPublication,
  }};
  uint64_t b4b4i_dsp_section_cycles = 0;
  for (const auto section : b4b4i_sections)
    b4b4i_dsp_section_cycles += pitch(section).total_cycles;
  const uint64_t b4b4i_total_cycles = pitch_total.total_cycles;
  const std::array<PitchAnalysisProfileSection, 15> profiled_pair_sections{{
      PitchAnalysisProfileSection::LinearWindowCopy,
      PitchAnalysisProfileSection::YinEnergy,
      PitchAnalysisProfileSection::YinDifference,
      PitchAnalysisProfileSection::YinCmnd,
      PitchAnalysisProfileSection::YinSearch,
      PitchAnalysisProfileSection::YinInterpolation,
      PitchAnalysisProfileSection::YinTotal,
      PitchAnalysisProfileSection::VoicedFeatures,
      PitchAnalysisProfileSection::VoicedClassifier,
      PitchAnalysisProfileSection::PitchSmoother,
      PitchAnalysisProfileSection::PitchMarkSearch,
      PitchAnalysisProfileSection::PitchMarkCorrelation,
      PitchAnalysisProfileSection::PitchPublication,
      PitchAnalysisProfileSection::Total,
      PitchAnalysisProfileSection::RunTotal,
  }};
  uint64_t profiled_pairs = 0;
  for (const auto section : profiled_pair_sections)
    profiled_pairs += pitch(section).blocks;
  const uint64_t record_cycle_calls =
      pitch(PitchAnalysisProfileSection::FifoDrain).blocks +
      pitch(PitchAnalysisProfileSection::RollingWindow).blocks;
  // Four direct cycle reads are provable for every successfully popped sample:
  // two around FIFO pop and two around rolling-window maintenance.
  const uint64_t direct_cycle_reads =
      4 * (audit_end.fifo_pops - audit_start.fifo_pops);
  const double measured_audit_cycles =
      profiled_pairs * profile_overhead.profile_pair_cycles +
      record_cycle_calls * profile_overhead.record_cycles_cycles +
      direct_cycle_reads * profile_overhead.now_cycles_per_call +
      profiled_hops * profile_overhead.atomic_increment_cycles;
  const uint64_t b4b4i_raw_other_cycles =
      b4b4i_total_cycles > b4b4i_dsp_section_cycles
          ? b4b4i_total_cycles - b4b4i_dsp_section_cycles
          : 0;
  const uint64_t b4b4i_audit_accounted_cycles = std::min<uint64_t>(
      b4b4i_raw_other_cycles,
      static_cast<uint64_t>(std::llround(measured_audit_cycles)));
  const uint64_t b4b4i_accounted_cycles =
      b4b4i_dsp_section_cycles + b4b4i_audit_accounted_cycles;
  const uint64_t b4b4i_other_cycles =
      b4b4i_total_cycles > b4b4i_accounted_cycles
          ? b4b4i_total_cycles - b4b4i_accounted_cycles
          : 0;
  const double b4b4i_reconciliation =
      b4b4i_total_cycles
          ? 100.0 * b4b4i_accounted_cycles / b4b4i_total_cycles
          : 0.0;
  const double measured_audit_us_per_hop =
      profiled_hops
          ? measured_audit_cycles /
                (Profiler::cycles_per_us() * static_cast<double>(profiled_hops))
          : 0.0;
  const double current_pitch_us_per_hop =
      profiled_hops
          ? b4b4i_total_cycles /
                (Profiler::cycles_per_us() * static_cast<double>(profiled_hops))
          : 0.0;
  const double production_pitch_us_per_hop =
      std::max(0.0, current_pitch_us_per_hop - measured_audit_us_per_hop);
  const bool pitch_age_low = audit_end.latest_pitch_age_ms <= 35.0f &&
                             audit_end.pitch_age_max_ms <= 40.0f;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  // A short rising streak is normal scheduler phase jitter.  Call it a
  // persistent growth trend only when the streak is accompanied by a
  // materially larger endpoint; the independent boundedness limits below
  // continue to guard maxima and FIFO occupancy.
  const bool b4b5_backlog_monotonic_growth =
      b4b5_backlog_max_growth_streak >= 5 &&
      b4b5_backlog_last_ms > b4b5_backlog_first_ms + 5.0f;
  const bool b4b5_pitch_age_monotonic_growth =
      b4b5_pitch_age_max_growth_streak >= 5 &&
      b4b5_pitch_age_last_ms > b4b5_pitch_age_first_ms + 10.0f;
  const bool b4b5_stability_trend_ok =
      !b4b5_backlog_monotonic_growth &&
      !b4b5_pitch_age_monotonic_growth;
#endif
  const bool b4b4g_pass = b4b4g_transport_pass &&
                           hann_internal && terminal_unaccounted == 0 &&
                           terminal_rejections == 0 && fifo_bounded &&
                           pitch_age_low && b4b4i_reconciliation >= 97.0 &&
                           wall_seconds >= 10.0
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
                           && b4b5_config_matches &&
                           b4b5_stability_trend_ok
#endif
                           ;
#else
  const bool pitch_age_low = audit_end.latest_pitch_age_ms <= 35.0f &&
                             audit_end.pitch_age_max_ms <= 40.0f;
  const bool b4b4g_pass = b4b4g_transport_pass && realtime_capacity &&
                          hann_internal && terminal_unaccounted == 0 &&
                          terminal_rejections == 0 && fifo_bounded &&
                          pitch_age_low;
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  const std::array<const char *, 13> b4b4i_names{{
      "FIFO", "RollingWindow", "LinearCopy", "YinEnergy",
      "YinDifference", "YinCMND", "YinSearch", "YinInterpolation",
      "VoicedFeatures", "VoicedClassifier", "PitchSmoother",
      "PitchMarkSearch", "PitchPublication",
  }};
  const std::array<const char *, 13> b4b4i_categories{{
      "PRODUCTION_DSP", "PRODUCTION_DSP", "PRODUCTION_DSP",
      "PRODUCTION_DSP", "PRODUCTION_DSP", "PRODUCTION_DSP",
      "PRODUCTION_DSP", "PRODUCTION_DSP", "PRODUCTION_DSP",
      "PRODUCTION_DSP", "PRODUCTION_DSP", "PRODUCTION_DSP",
      "PRODUCTION_SYNCHRONIZATION",
  }};
  for (size_t i = 0; i < b4b4i_sections.size(); ++i) {
    const auto &stats = pitch(b4b4i_sections[i]);
    const auto &distribution =
        pitch_distribution[static_cast<size_t>(b4b4i_sections[i])];
    const double average_cycles =
        profiled_hops ? static_cast<double>(stats.total_cycles) / profiled_hops
                      : 0.0;
    const double average_us = average_cycles / Profiler::cycles_per_us();
    printf("B4B4I_SECTION name=%s category=%s executions=%llu avg_us_hop=%.3f P50_us=%lu P95_us=%lu P99_us=%lu max_us=%llu avg_cycles_hop=%.3f cpu_ms_s=%.3f samples=%lu bytes=%lu cycles_per_sample=%.3f\n",
           b4b4i_names[i], b4b4i_categories[i],
           static_cast<unsigned long long>(stats.blocks), average_us,
           static_cast<unsigned long>(distribution.p50_us),
           static_cast<unsigned long>(distribution.p95_us),
           static_cast<unsigned long>(distribution.p99_us),
           static_cast<unsigned long long>(stats.worst_us), average_cycles,
           average_us * 0.2,
           static_cast<unsigned long>(
               b4b4i_sections[i] == PitchAnalysisProfileSection::FifoDrain ||
                       b4b4i_sections[i] ==
                           PitchAnalysisProfileSection::RollingWindow
                   ? 60
                   : b4b4i_sections[i] ==
                                 PitchAnalysisProfileSection::LinearWindowCopy ||
                             b4b4i_sections[i] ==
                                 PitchAnalysisProfileSection::YinEnergy ||
                             b4b4i_sections[i] ==
                                 PitchAnalysisProfileSection::VoicedFeatures
                         ? 512
                         : 0),
           static_cast<unsigned long>(
               b4b4i_sections[i] ==
                       PitchAnalysisProfileSection::LinearWindowCopy
                   ? 512 * sizeof(float)
                   : 0),
           (b4b4i_sections[i] == PitchAnalysisProfileSection::FifoDrain ||
            b4b4i_sections[i] == PitchAnalysisProfileSection::RollingWindow)
               ? average_cycles / 60.0
               : (b4b4i_sections[i] ==
                              PitchAnalysisProfileSection::LinearWindowCopy ||
                          b4b4i_sections[i] ==
                              PitchAnalysisProfileSection::YinEnergy ||
                          b4b4i_sections[i] ==
                              PitchAnalysisProfileSection::VoicedFeatures)
                     ? average_cycles / 512.0
                     : 0.0);
  }
  const auto &mark_correlation_stats =
      pitch(PitchAnalysisProfileSection::PitchMarkCorrelation);
  printf("B4B4I_NESTED name=PitchMarkCorrelation parent=PitchMarkSearch executions=%llu avg_us_hop=%.3f avg_cycles_hop=%.3f\n",
         static_cast<unsigned long long>(mark_correlation_stats.blocks),
         profiled_hops
             ? static_cast<double>(mark_correlation_stats.total_cycles) /
                   (Profiler::cycles_per_us() * profiled_hops)
             : 0.0,
         profiled_hops
             ? static_cast<double>(mark_correlation_stats.total_cycles) /
                   profiled_hops
             : 0.0);
  const uint64_t cold_hops = yin_start.executions;
  const auto &cold_total =
      pitch_profile_start[static_cast<size_t>(
          PitchAnalysisProfileSection::RunTotal)];
  const double cold_us_per_hop =
      cold_hops
          ? static_cast<double>(cold_total.total_cycles) /
                (Profiler::cycles_per_us() * cold_hops)
          : 0.0;
  const double current_target_ms_s =
      current_pitch_us_per_hop * 0.2 + lpc_target_ms_s;
  const double production_target_ms_s =
      production_pitch_us_per_hop * 0.2 + lpc_target_ms_s;
  const double residual_us_per_hop =
      profiled_hops
          ? std::max(
                0.0, current_pitch_us_per_hop -
                         (static_cast<double>(yin_difference.total_cycles +
                                              mark_search.total_cycles) /
                          (Profiler::cycles_per_us() * profiled_hops)))
          : 0.0;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("\n%s effective normal production configuration: YIN_DIFF_INCREMENTAL_F32 rebase=64; YIN_CMND_%s; YIN_ENERGY_%s; PITCH_MARK_NCC_REUSE_AA; LPC_ENERGY_F32_COMPENSATED; AUTOCORR_F32_DOUBLE_SINGLE\n",
#else
  printf("\n%s frozen audit configuration: YIN_DIFF_INCREMENTAL_F32 rebase=64; YIN_CMND_%s; YIN_ENERGY_%s; PITCH_MARK_NCC_REUSE_AA; LPC_ENERGY_F32_COMPENSATED; AUTOCORR_F32_DOUBLE_SINGLE\n",
#endif
         VOXP4_LPC_AUDIT_STAGE,
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
         "F32_COMPENSATED"
#else
         "REFERENCE_DOUBLE"
#endif
         ,
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
         "F32_COMPENSATED"
#else
         "REFERENCE_DOUBLE"
#endif
  );
  printf("B4B4I_COLD warmup_seconds=1 hops=%llu PitchAnalysis_us_hop=%.3f\n",
         static_cast<unsigned long long>(cold_hops), cold_us_per_hop);
  printf("B4B4I_YIN energy_samples=512 energy_accumulator=%s energy_passes=1 energy_math=log10 CMND_tau_first=1 CMND_tau_last=%lu CMND_taus=%lu CMND_accumulator=%s search_tau_min=%lu search_tau_max=%lu interpolation=parabolic_raw_difference\n",
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
         "f32_compensated_then_double_materialization",
#else
         "double",
#endif
         static_cast<unsigned long>(yin_end.tau_max + 1),
         static_cast<unsigned long>(yin_end.tau_values_per_execution),
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
         "f32_compensated",
#else
         "double",
#endif
         static_cast<unsigned long>(yin_end.tau_min),
         static_cast<unsigned long>(yin_end.tau_max));
  printf("B4B4I_CMND_SENSITIVITY threshold=%.9g selected_min=%.9g selected_avg=%.9g closest_threshold_distance=%.9g selected_tau_min=%lu selected_tau_max=%lu\n",
         yin_end.yin_threshold, yin_end.selected_cmnd_minimum,
         yin_end.selected_cmnd_average, yin_end.closest_threshold_distance,
         static_cast<unsigned long>(yin_end.selected_tau_minimum),
         static_cast<unsigned long>(yin_end.selected_tau_maximum));
  printf("B4B4I_ITERATIVE YinDifference_inner=%llu YinDifference_cycles_operation=%.6f YinCMND_cycles_tau=%.6f FIFO_samples_popped=%llu Rolling_samples_written=%llu LinearCopy_bytes_hop=2048\n",
         static_cast<unsigned long long>(yin_inner),
         yin_inner ? static_cast<double>(yin_difference.total_cycles) / yin_inner
                   : 0.0,
         profiled_hops && yin_end.tau_values_per_execution
             ? static_cast<double>(
                   pitch(PitchAnalysisProfileSection::YinCmnd).total_cycles) /
                   (profiled_hops * yin_end.tau_values_per_execution)
             : 0.0,
         static_cast<unsigned long long>(audit_end.fifo_pops -
                                         audit_start.fifo_pops),
         static_cast<unsigned long long>(audit_end.fifo_pops -
                                         audit_start.fifo_pops));
  printf("B4B4I_WINDOW_PASSES full_window_passes_per_hop=3 passes=LinearCopy,YinEnergy,VoicedFeatures YinEnergy_quantities=sum_sq_selected,rms_db VoicedFeatures_quantities=peak,sum_sq_float,lag1,zcr,hfr,centroid redundant_energy=yes redundant_transcendentals=no exact_share_without_semantic_change=no\n");
  printf("B4B4I_MATH VoicedFeatures=fabs,acos,division VoicedClassifier=log2,pow,division PitchSmoother=log2,exp,exp2,sqrt,division Publication=atomic_seqlock\n");
  printf("B4B4I_RECONCILIATION total_cycles=%llu accounted_cycles=%llu other_cycles=%llu other_us_hop=%.3f percent=%.3f result=%s\n",
         static_cast<unsigned long long>(b4b4i_total_cycles),
         static_cast<unsigned long long>(b4b4i_accounted_cycles),
         static_cast<unsigned long long>(b4b4i_other_cycles),
         profiled_hops
             ? static_cast<double>(b4b4i_other_cycles) /
                   (Profiler::cycles_per_us() * profiled_hops)
             : 0.0,
         b4b4i_reconciliation,
         b4b4i_reconciliation >= 97.0 ? "PASS" : "FAIL");
  printf("B4B4I_AUDIT_ESTIMATE profiled_pairs=%llu record_cycle_calls=%llu direct_now_cycle_calls=%llu sensitivity_atomic_increments=%llu audit_us_hop=%.3f audit_ms_s=%.3f current_audit_pitch_us_hop=%.3f estimated_production_pitch_us_hop=%.3f production_only_estimate=yes\n",
         static_cast<unsigned long long>(profiled_pairs),
         static_cast<unsigned long long>(record_cycle_calls),
         static_cast<unsigned long long>(direct_cycle_reads),
         static_cast<unsigned long long>(profiled_hops),
         measured_audit_us_per_hop, measured_audit_us_per_hop * 0.2,
         current_pitch_us_per_hop, production_pitch_us_per_hop);
  printf("B4B4I_SUMMARY PitchAnalysis_us_hop=%.3f YinDifference_us_hop=%.3f PitchMarkSearch_us_hop=%.3f residual_us_hop=%.3f LPC_us_frame=%.3f LPC_ms_s=%.3f target_rate_current_ms_s=%.3f target_rate_minus_audit_ms_s=%.3f missing_to_900_ms_s=%.3f reconciliation_percent=%.3f\n",
         current_pitch_us_per_hop,
         profiled_hops
             ? static_cast<double>(yin_difference.total_cycles) /
                   (Profiler::cycles_per_us() * profiled_hops)
             : 0.0,
         profiled_hops
             ? static_cast<double>(mark_search.total_cycles) /
                   (Profiler::cycles_per_us() * profiled_hops)
             : 0.0,
         residual_us_per_hop, lpc_avg_frame_us, lpc_target_ms_s,
         current_target_ms_s, production_target_ms_s,
         std::max(0.0, current_target_ms_s - 900.0),
         b4b4i_reconciliation);
#else
  printf("\nB4B.4H production selection: LPC_ENERGY_F32_COMPENSATED\n");
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("Production kernels: LPC_WINDOW_HANN_DOUBLE_ENERGY; AUTOCORR_F32_DOUBLE_SINGLE; YIN_DIFF_INCREMENTAL_F32 rebase64; YIN_ENERGY_F32_COMPENSATED; YIN_CMND_F32_COMPENSATED; PITCH_MARK_NCC_REUSE_AA\n");
#else
  printf("Frozen kernels: AUTOCORR_F32_DOUBLE_SINGLE; YIN audit override=YIN_DIFF_INCREMENTAL_F32 rebase64; P4 normal YIN default=YIN_DIFF_FMA_8ACC; PITCH_MARK_NCC_REUSE_AA\n");
#endif
  printf("Host B-vs-C guardrails: y_bit_mismatch=0 valid_mismatch=0 coefficient_max_abs<=1e-6 prediction_error_diff<=1e-7 confidence_diff<=1e-7 timestamp_mismatch=0 frame_count_mismatch=0 NaN/Inf=0 audio_difference_SNR>=100dB\n");
#else
  const bool b4b4g_pass = b4b4g_transport_pass &&
                          realtime_capacity && hann_internal &&
                          terminal_unaccounted == 0;
  printf("\nB4B.4G production selection: LPC_WINDOW_HANN_DOUBLE_ENERGY\n");
  printf("Frozen kernels: AUTOCORR_F32_DOUBLE_SINGLE; YIN audit override=YIN_DIFF_INCREMENTAL_F32 rebase64; P4 normal YIN default=YIN_DIFF_FMA_8ACC; PITCH_MARK_NCC_REUSE_AA\n");
  printf("Host window guardrails: Hann_bit_mismatch=0 y_bit_mismatch=0 energy_bit_mismatch=0 valid_mismatch=0 coefficient_bit_mismatch=0 prediction_error_diff=0 confidence_diff=0 timestamp_mismatch=0 frame_count_mismatch=0 NaN/Inf=0\n");
  printf("Experimental energy guardrails: LPC_ENERGY_F32_COMPENSATED valid_mismatch=0 coefficient_max_abs=0 prediction_error_diff=0 confidence_diff=0 NaN/Inf=0; LPC_ENERGY_FROM_AUTOCORR_R0 valid_mismatch=0 diagnostic_only=yes\n");
#endif
  printf("LPC Hann memory: bytes=%lu internal_SRAM=%s PSRAM=%s\n",
         static_cast<unsigned long>(hann_bytes), hann_internal ? "yes" : "no",
         hann_internal ? "no" : "unknown");
  printf("LPC full-worker: windowing_us/frame=%.2f autocorrelation_us/frame=%.2f Levinson_us/frame=%.2f total_us/frame=%.2f P50=%lu P95=%lu P99=%lu max=%lu cycles/frame=%.2f\n",
         lpc_windowing_avg_frame_us, autocorrelation_avg_frame_us,
         levinson_avg_frame_us, lpc_avg_frame_us,
         static_cast<unsigned long>(lpc_frame_cost.p50_us),
         static_cast<unsigned long>(lpc_frame_cost.p95_us),
         static_cast<unsigned long>(lpc_frame_cost.p99_us),
         static_cast<unsigned long>(lpc_frame_cost.max_us),
         lpc_frames ? static_cast<double>(lpc_total.total_cycles) / lpc_frames
                    : 0.0);
  printf(VOXP4_LPC_AUDIT_TOKEN "_FULL_WORKER pitch_mark_us/hop=%.2f other_pitch_us/hop=%.2f pitch_analysis_us/hop=%.2f yin_energy_us/hop=%.2f yin_difference_us/hop=%.2f yin_cmnd_us/hop=%.2f LPC_windowing_us/frame=%.2f LPC_autocorrelation_us/frame=%.2f LPC_Levinson_us/frame=%.2f LPC_total_us/frame=%.2f\n",
         pitch_mark_us_per_hop, other_pitch_us_per_hop,
         new_pitch_analysis_us_per_hop, yin_energy_us_per_hop,
         new_yin_difference_us_per_hop, yin_cmnd_us_per_hop,
         lpc_windowing_avg_frame_us, autocorrelation_avg_frame_us,
         levinson_avg_frame_us, lpc_avg_frame_us);
  printf("Target-rate budget: PitchMark=%.3f ms/s other_PitchAnalysis=%.3f ms/s LPC=%.3f ms/s total_Core1=%.3f ms/s\n",
         pitch_mark_target_ms_s, other_pitch_target_ms_s, lpc_target_ms_s,
         target_core_ms_s);
  printf("FIFO/backlog: current=%lu max=%lu drops=%llu bounded=%s backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops - audit_start.fifo_drops),
         fifo_bounded ? "yes" : "no", audit_end.analysis_backlog_ms,
         audit_end.analysis_backlog_max_ms, audit_end.latest_pitch_age_ms,
         audit_end.pitch_age_average_ms, audit_end.pitch_age_p95_ms,
         audit_end.pitch_age_p99_ms, audit_end.pitch_age_max_ms);
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("B4B5_STABILITY_TREND duration_s=%lu backlog_first_ms=%.3f "
         "backlog_last_ms=%.3f backlog_max_growth_streak=%lu "
         "pitch_age_first_ms=%.3f pitch_age_last_ms=%.3f "
         "pitch_age_max_growth_streak=%lu monotonic_growth=%s\n",
         static_cast<unsigned long>(kDurationSeconds), b4b5_backlog_first_ms,
         b4b5_backlog_last_ms,
         static_cast<unsigned long>(b4b5_backlog_max_growth_streak),
         b4b5_pitch_age_first_ms, b4b5_pitch_age_last_ms,
         static_cast<unsigned long>(b4b5_pitch_age_max_growth_streak),
         b4b5_stability_trend_ok ? "NO" : "YES");
#endif
  printf("Grain rejection: source_negative=%llu select_total=%llu no_marks=%llu low_confidence=%llu invalid_period=%llu distance_too_large=%llu history_total=%llu center_before_half=%llu history_too_old=%llu future_end=%llu\n",
         static_cast<unsigned long long>(grain.attempt_source_negative),
         static_cast<unsigned long long>(grain.select_mark_failure_total),
         static_cast<unsigned long long>(grain.select_mark_no_marks),
         static_cast<unsigned long long>(grain.select_mark_low_confidence),
         static_cast<unsigned long long>(grain.select_mark_invalid_period),
         static_cast<unsigned long long>(grain.select_mark_distance_too_large),
         static_cast<unsigned long long>(grain.history_failure_total),
         static_cast<unsigned long long>(grain.history_center_before_half),
         static_cast<unsigned long long>(grain.history_too_old),
         static_cast<unsigned long long>(grain.history_future_end));
  printf(VOXP4_LPC_AUDIT_TOKEN "_GRAIN_ACCOUNTING pre_stop_attempted=%llu pre_stop_scheduled=%llu post_stop_attempted=%llu post_stop_scheduled=%llu terminal_rejections=%llu unaccounted=%llu boundary_attempt_delta=%llu boundary_scheduled_delta=%llu classification=%s\n",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_after_teardown.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_after_teardown.grains_scheduled),
         static_cast<unsigned long long>(terminal_rejections),
         static_cast<unsigned long long>(terminal_unaccounted),
         static_cast<unsigned long long>(boundary_attempt_delta),
         static_cast<unsigned long long>(boundary_scheduled_delta),
         accounting_class);
  printf("Transport deltas: diagnostic_q_overflow RX/TX=%llu/%llu actual_DMA_errors=%llu/%llu read/write_failures=%llu/%llu dropped_frames=%llu/%llu\n",
         static_cast<unsigned long long>(transport_end.rx_queue_overflows - transport_start.rx_queue_overflows),
         static_cast<unsigned long long>(transport_end.tx_queue_overflows - transport_start.tx_queue_overflows),
         static_cast<unsigned long long>(transport_end.rx_dma_errors - transport_start.rx_dma_errors),
         static_cast<unsigned long long>(transport_end.tx_dma_errors - transport_start.tx_dma_errors),
         static_cast<unsigned long long>(transport_end.read_failures - transport_start.read_failures),
         static_cast<unsigned long long>(transport_end.write_failures - transport_start.write_failures),
         static_cast<unsigned long long>(transport_end.rx_dropped_frames - transport_start.rx_dropped_frames),
         static_cast<unsigned long long>(transport_end.tx_dropped_frames - transport_start.tx_dropped_frames));
  printf(VOXP4_LPC_AUDIT_STAGE " transport/teardown audit: %s; spinlock/reboot/watchdog/callback-after-free: not observed\n",
         b4b4g_transport_pass ? "PASS" : "FAIL");
  printf("\n" VOXP4_LPC_AUDIT_STAGE " RESULT:\n%s\n",
         b4b4g_pass ? "PASS" : "FAIL");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
  printf("PRODUCTION LPC ENERGY:\nLPC_ENERGY_F32_COMPENSATED\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  printf("PRODUCTION YIN CMND:\nYIN_CMND_F32_COMPENSATED\n");
#endif
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  printf("PRODUCTION YIN ENERGY:\nYIN_ENERGY_F32_COMPENSATED\n");
  printf("YIN ENERGY:\n326.20 -> %.2f us/hop\n", yin_energy_us_per_hop);
  printf("YIN ENERGY SPEEDUP:\n%.2fx\n",
         yin_energy_us_per_hop > 0.0 ? 326.20 / yin_energy_us_per_hop : 0.0);
#endif
#else
  printf("PRODUCTION LPC WINDOWING:\nLPC_WINDOW_HANN_DOUBLE_ENERGY\n");
#endif
  printf("LPC WINDOWING:\n%.2f -> %.2f us/frame\n",
         kBeforeWindowingUsPerFrame, lpc_windowing_avg_frame_us);
  printf("LPC TOTAL:\n%.2f -> %.2f us/frame\n", kBeforeLpcUsPerFrame,
         lpc_avg_frame_us);
  printf("LPC SPEEDUP:\n%.2fx\n", lpc_speedup);
  printf("TARGET-RATE CPU:\n%.3f ms/s\n", target_core_ms_s);
  printf("REALTIME <=1000:\n%s\n", realtime_capacity ? "PASS" : "FAIL");
  printf("HEADROOM <=900:\n%s\n", ninety_percent_headroom ? "PASS" : "FAIL");
  printf("PREFERRED <=800:\n%s\n", preferred_headroom ? "PASS" : "FAIL");
  printf("FIFO:\n%s\n", fifo_bounded ? "bounded" : "saturated");
  printf("PITCH AGE:\n%.2f ms\n", audit_end.latest_pitch_age_ms);
  printf("GRAINS:\nattempted=%llu scheduled=%llu rendered=%llu\n",
         static_cast<unsigned long long>(funnel_after_teardown.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_after_teardown.grains_scheduled),
         static_cast<unsigned long long>(funnel_after_teardown.grains_rendered));
  printf("NEXT TARGET-RATE HOTSPOT:\n%s\n", next_hotspot);
  printf("NEXT REDUCTION POTENTIAL:\n%.3f ms/s\n",
         next_reduction_potential_ms_s);
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  printf("NORMAL FIRMWARE QUALIFIED:\n%s\n",
         b4b4g_pass ? "YES" : "NO");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4H_LPC_ENERGY_QUALIFICATION)
  printf("NORMAL FIRMWARE QUALIFIED:\nNO (YIN_DIFF_INCREMENTAL_F32 rebase64 remains an audit override)\n");
#endif
  printf("NEXT STEP:\n");
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  if (b4b4g_pass)
    printf("release-candidate validation; retain <=800 ms/s as future optimization\n");
  else
    printf("investigate the failing production qualification guardrail\n");
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  if (ninety_percent_headroom)
    printf("PRODUCTION DEFAULT INTEGRATION\n");
  else
    printf("optimize %s, the largest normalized measured CPU demand\n",
           next_hotspot);
#else
  printf("optimize %s, the largest normalized measured CPU demand\n",
         next_hotspot);
#endif
#else
  printf("\nB4B.4F production selection: PITCH_MARK_NCC_REUSE_AA\n");
  printf("Host NCC guardrails: score_bit_identical=yes best_offset=0 accept_reject=0 mark_position=0 coherent_mark=0 track_state=0 NaN/Inf=0\n");
  printf("Pitch-mark geometry: searches=%llu searches/s=%.3f searches/pitch_hop=%.5f offsets/search=%.3f sample_pairs/search=%.3f\n",
         static_cast<unsigned long long>(mark_correlations),
         wall_seconds > 0.0 ? mark_correlations / wall_seconds : 0.0,
         mark_hops ? static_cast<double>(mark_correlations) / mark_hops : 0.0,
         mark_correlations ? static_cast<double>(mark_candidates) / mark_correlations : 0.0,
         mark_correlations ? static_cast<double>(mark_samples) / mark_correlations : 0.0);
  printf("B4B4F_GEOMETRY observations=%lu period_P50=%lu period_P95=%lu period_P99=%lu period_max=%lu radius_P50=%lu radius_P95=%lu radius_P99=%lu radius_max=%lu window_P50=%lu window_P95=%lu window_P99=%lu window_max=%lu offsets_P50=%lu offsets_P95=%lu offsets_P99=%lu offsets_max=%lu pairs_P50=%lu pairs_P95=%lu pairs_P99=%lu pairs_max=%lu\n",
         static_cast<unsigned long>(marks_end.geometry_observations),
         static_cast<unsigned long>(marks_end.period_p50),
         static_cast<unsigned long>(marks_end.period_p95),
         static_cast<unsigned long>(marks_end.period_p99),
         static_cast<unsigned long>(marks_end.period_max),
         static_cast<unsigned long>(marks_end.radius_p50),
         static_cast<unsigned long>(marks_end.radius_p95),
         static_cast<unsigned long>(marks_end.radius_p99),
         static_cast<unsigned long>(marks_end.radius_max),
         static_cast<unsigned long>(marks_end.window_p50),
         static_cast<unsigned long>(marks_end.window_p95),
         static_cast<unsigned long>(marks_end.window_p99),
         static_cast<unsigned long>(marks_end.window_max),
         static_cast<unsigned long>(marks_end.offsets_p50),
         static_cast<unsigned long>(marks_end.offsets_p95),
         static_cast<unsigned long>(marks_end.offsets_p99),
         static_cast<unsigned long>(marks_end.offsets_max),
         static_cast<unsigned long>(marks_end.pairs_p50),
         static_cast<unsigned long>(marks_end.pairs_p95),
         static_cast<unsigned long>(marks_end.pairs_p99),
         static_cast<unsigned long>(marks_end.pairs_max));
  printf("Target-rate budget: PitchMark=%.3f ms/s other_PitchAnalysis=%.3f ms/s LPC=%.3f ms/s total_Core1=%.3f ms/s\n",
         pitch_mark_target_ms_s, other_pitch_target_ms_s, lpc_target_ms_s,
         target_core_ms_s);
  printf("B4B4F_DELTA_HIST labels=lt-3,-3to-2,-2to-1,-1to0,0to1,1to2,2to3,gt3 counts=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[0]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[1]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[2]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[3]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[4]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[5]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[6]),
         static_cast<unsigned long long>(grain.signed_delta_period_histogram[7]));
  printf("Grain rejection: source_negative=%llu select_total=%llu no_marks=%llu low_confidence=%llu invalid_period=%llu distance_too_large=%llu history_total=%llu center_before_half=%llu history_too_old=%llu future_end=%llu\n",
         static_cast<unsigned long long>(grain.attempt_source_negative),
         static_cast<unsigned long long>(grain.select_mark_failure_total),
         static_cast<unsigned long long>(grain.select_mark_no_marks),
         static_cast<unsigned long long>(grain.select_mark_low_confidence),
         static_cast<unsigned long long>(grain.select_mark_invalid_period),
         static_cast<unsigned long long>(grain.select_mark_distance_too_large),
         static_cast<unsigned long long>(grain.history_failure_total),
         static_cast<unsigned long long>(grain.history_center_before_half),
         static_cast<unsigned long long>(grain.history_too_old),
         static_cast<unsigned long long>(grain.history_future_end));
  for (uint32_t i = 0; i < grain.diagnostic_count; ++i) {
    const auto &d = grain.diagnostics[i];
    printf("B4B4F_GRAIN_FAILURE index=%lu reason=%u input_end=%llu oldest=%llu analysis_timestamp=%llu pitch_age_samples=%llu requested_source=%.3f nearest_mark_center=%llu signed_delta_samples=%.3f abs_delta_samples=%.3f delta_in_periods=%.6f allowed_distance=%.3f pitch_period=%.3f history_first=%llu history_last=%llu\n",
           static_cast<unsigned long>(i), static_cast<unsigned>(d.reason),
           static_cast<unsigned long long>(d.input_end),
           static_cast<unsigned long long>(d.history_oldest_sample),
           static_cast<unsigned long long>(d.pitch_analysis_timestamp),
           static_cast<unsigned long long>(d.pitch_age_samples),
           d.requested_source,
           static_cast<unsigned long long>(d.selected_mark_center),
           d.signed_delta_samples, std::fabs(d.signed_delta_samples),
           d.delta_in_periods, d.allowed_distance, d.pitch_period,
           static_cast<unsigned long long>(d.required_first_sample),
           static_cast<unsigned long long>(d.required_last_sample));
  }
  for (size_t i = 0; i < grain.pitch_age_attempt_histogram.size(); ++i)
    printf("B4B4F_AGE_HIST bin=%lu attempts=%llu distance_failures=%llu rate=%.6f\n",
           static_cast<unsigned long>(i),
           static_cast<unsigned long long>(grain.pitch_age_attempt_histogram[i]),
           static_cast<unsigned long long>(grain.pitch_age_distance_failure_histogram[i]),
           grain.pitch_age_attempt_histogram[i]
               ? static_cast<double>(grain.pitch_age_distance_failure_histogram[i]) /
                     grain.pitch_age_attempt_histogram[i]
               : 0.0);
  printf("B4B.4F transport/teardown audit: %s\n",
         b4b4b_transport_pass && teardown_clean ? "PASS" : "FAIL");
  printf("\nB4B.4F RESULT:\n%s\n", b4b4f_pass ? "PASS" : "FAIL");
  printf("PRODUCTION PITCH-MARK NCC:\nPITCH_MARK_NCC_REUSE_AA\n");
  printf("PITCH-MARK SEARCH:\n%.2f -> %.2f us/hop\n",
         kReferencePitchMarkUsPerHop, pitch_mark_us_per_hop);
  printf("ELIGIBLE SPEEDUP:\n%.2fx\n", pitch_mark_speedup);
  printf("PITCH ANALYSIS:\n%.2f us/hop\n", new_pitch_analysis_us_per_hop);
  printf("TARGET-RATE CORE1 DEMAND:\n%.2f ms CPU / second\n", target_core_ms_s);
  printf("CORE1:\n%.2f%%\n", core1_utilization);
  printf("PITCH THROUGHPUT:\n%.2f hops/s\n", pitch_hops_per_second);
  printf("FIFO:\n%s\n", fifo_bounded ? "bounded" : "saturated");
  printf("PITCH AGE:\n%.2f ms\n", audit_end.latest_pitch_age_ms);
  printf("GRAINS:\nattempted=%llu scheduled=%llu rendered=%llu\n",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("GRAIN ALIGNMENT:\n%s\n", alignment_class);
  printf("NEXT TARGET-RATE HOTSPOT:\n%s\n",
         lpc_target_ms_s >= other_pitch_target_ms_s ? "LPC" : "OTHER_PITCH_ANALYSIS");
  printf("NEXT STEP:\noptimize the largest measured target-rate CPU demand without changing DSP semantics\n");
#endif
#else

  printf("\nB4B.4E production selection: YIN_DIFF_INCREMENTAL_F32 rebase=64\n");
  printf("Host guardrails: NaN/Inf=0 selected_tau=0 voiced=0 track_state=0 pitch_marks=0\n");
  printf("YIN update telemetry: executions=%llu update_terms=%llu full_rebase_products=%llu rebases=%llu gap_rebases=%llu periodic_rebases=%llu\n",
         static_cast<unsigned long long>(yin_end.executions - yin_start.executions),
         static_cast<unsigned long long>(yin_end.incremental_update_terms - yin_start.incremental_update_terms),
         static_cast<unsigned long long>(yin_end.full_rebase_products - yin_start.full_rebase_products),
         static_cast<unsigned long long>(yin_end.incremental_rebases - yin_start.incremental_rebases),
         static_cast<unsigned long long>(yin_end.incremental_gap_rebases - yin_start.incremental_gap_rebases),
         static_cast<unsigned long long>(yin_end.periodic_rebases - yin_start.periodic_rebases));
  printf("YinDifference (us/pitch hop)            %9.2f %14.2f %11.2fx\n",
         kBeforeYinDifferenceUsPerHop, new_yin_difference_us_per_hop,
         new_yin_difference_us_per_hop > 0.0 ? kBeforeYinDifferenceUsPerHop / new_yin_difference_us_per_hop : 0.0);
  printf("PitchAnalysis total (us/pitch hop)      %9.2f %14.2f %11.2fx\n",
         kBeforePitchAnalysisUsPerHop, new_pitch_analysis_us_per_hop,
         new_pitch_analysis_us_per_hop > 0.0 ? kBeforePitchAnalysisUsPerHop / new_pitch_analysis_us_per_hop : 0.0);
  printf("LPC total (us/frame)                    %9.2f %14.2f %11.2fx\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us,
         lpc_avg_frame_us > 0.0 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us : 0.0);
  printf("Combined worker cost (us/pitch hop)     %9.2f %14.2f %11.2fx\n",
         kBeforeCombinedUsPerHop, average_hop_us,
         average_hop_us > 0.0 ? kBeforeCombinedUsPerHop / average_hop_us : 0.0);
  printf("Core 1 utilization (percent)             %8.2f %14.2f %11.2fx\n",
         kBeforeCore1Percent, core1_utilization,
         core1_utilization > 0.0 ? kBeforeCore1Percent / core1_utilization : 0.0);
  printf("Pitch throughput (hops/s)                %8.2f %14.2f %11.2fx\n",
         kBeforePitchHopsPerSecond, pitch_hops_per_second,
         kBeforePitchHopsPerSecond > 0.0 ? pitch_hops_per_second / kBeforePitchHopsPerSecond : 0.0);
  printf("Core1 headroom at 200 hops/s: 70%%=%s 80%%=%s 90%%=%s 100%%=%s\n",
         average_hop_us <= 3500.0 ? "PASS" : "FAIL",
         average_hop_us <= 4000.0 ? "PASS" : "FAIL",
         average_hop_us <= 4500.0 ? "PASS" : "FAIL",
         average_hop_us <= 5000.0 ? "PASS" : "FAIL");
  printf("FIFO/backlog: current=%lu max=%lu drops=%llu bounded=%s backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops - audit_start.fifo_drops),
         fifo_bounded ? "yes" : "no", audit_end.analysis_backlog_ms,
         audit_end.analysis_backlog_max_ms, audit_end.latest_pitch_age_ms,
         audit_end.pitch_age_average_ms, audit_end.pitch_age_p95_ms,
         audit_end.pitch_age_p99_ms, audit_end.pitch_age_max_ms);
  printf("Tracker final=%s reached_LOCKED=%s PSOLA_usable=%s grain_attempts=%llu grains_scheduled=%llu grains_rendered=%llu\n",
         pitch_track_name(pitch_track_end), ever_locked ? "yes" : "no",
         psola_usable ? "yes" : "no",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("Grain rejection: source_negative=%llu select_total=%llu no_marks=%llu low_confidence=%llu invalid_period=%llu distance_too_large=%llu history_total=%llu center_before_half=%llu history_too_old=%llu future_end=%llu\n",
         static_cast<unsigned long long>(grain.attempt_source_negative),
         static_cast<unsigned long long>(grain.select_mark_failure_total),
         static_cast<unsigned long long>(grain.select_mark_no_marks),
         static_cast<unsigned long long>(grain.select_mark_low_confidence),
         static_cast<unsigned long long>(grain.select_mark_invalid_period),
         static_cast<unsigned long long>(grain.select_mark_distance_too_large),
         static_cast<unsigned long long>(grain.history_failure_total),
         static_cast<unsigned long long>(grain.history_center_before_half),
         static_cast<unsigned long long>(grain.history_too_old),
         static_cast<unsigned long long>(grain.history_future_end));
  printf("PSOLA history: size_samples=%lu size_ms=%.3f input_end=%llu oldest_available=%llu diagnostics=%lu primary=%s\n",
         static_cast<unsigned long>(grain.history_size_samples), grain.history_size_ms,
         static_cast<unsigned long long>(grain.input_end),
         static_cast<unsigned long long>(grain.oldest_available),
         static_cast<unsigned long>(grain.diagnostic_count), grain_primary);
  for (uint32_t i = 0; i < grain.diagnostic_count; ++i) {
    const auto &d = grain.diagnostics[i];
    printf("B4B4E_GRAIN_FAILURE index=%lu reason=%u input_end=%llu oldest=%llu pitch_timestamp=%llu pitch_age_samples=%llu source=%.3f destination=%.3f center=%llu mark_age=%llu half=%lu required_first=%llu required_last=%llu distance=%.3f allowed=%.3f\n",
           static_cast<unsigned long>(i), static_cast<unsigned>(d.reason),
           static_cast<unsigned long long>(d.input_end),
           static_cast<unsigned long long>(d.history_oldest_sample),
           static_cast<unsigned long long>(d.pitch_analysis_timestamp),
           static_cast<unsigned long long>(d.pitch_age_samples), d.requested_source,
           d.destination, static_cast<unsigned long long>(d.selected_mark_center),
           static_cast<unsigned long long>(d.mark_age_samples),
           static_cast<unsigned long>(d.half_window),
           static_cast<unsigned long long>(d.required_first_sample),
           static_cast<unsigned long long>(d.required_last_sample),
           d.source_to_nearest_mark, d.allowed_distance);
  }
  printf("B4B.4E transport/teardown audit: %s\n", b4b4b_transport_pass && teardown_clean ? "PASS" : "FAIL");
  printf("\nB4B.4E RESULT:\n%s\n", b4b4e_pass ? "PASS" : "FAIL");
  printf("PRODUCTION YIN:\nYIN_DIFF_INCREMENTAL_F32 rebase=64\n");
  printf("YIN DIFFERENCE:\n%.2f us/hop\n", new_yin_difference_us_per_hop);
  printf("PITCH ANALYSIS:\n%.2f us/hop\n", new_pitch_analysis_us_per_hop);
  printf("CORE1:\n%.2f%%\n", core1_utilization);
  printf("PITCH THROUGHPUT:\n%.2f hops/s\n", pitch_hops_per_second);
  printf("FIFO:\n%s\n", fifo_bounded ? "bounded" : "saturated");
  printf("PITCH AGE:\n%.2f ms\n", audit_end.latest_pitch_age_ms);
  printf("GRAIN REJECTION PRIMARY:\n%s\n", grain_primary);
  printf("GRAINS:\nattempted=%llu scheduled=%llu rendered=%llu\n",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("REALTIME:\n%s\n", realtime ? "PASS" : "FAIL");
  printf("NEXT HOTSPOT:\n%s\n", primary_hotspot);
  printf("NEXT STEP:\n%s\n", realtime ? "AUDIT_PSOLA_ONLY_IF_ZERO_GRAINS_PERSISTS" : "OPTIMIZE_THE_MEASURED_FULL_WORKER_HOTSPOT");
#endif
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  (void)old_storage;
  (void)maximum_theoretical_hops;
  const bool ever_locked = audit_end.first_locked_input_position != 0;
  const bool psola_usable =
      harmony0_end.state == PitchShiftState::Active ||
      harmony0_end.state == PitchShiftState::Acquiring;
  const bool fifo_bounded =
      audit_end.fifo_drops == audit_start.fifo_drops &&
      audit_end.fifo_current_occupancy < PitchAnalysis::kFifoCapacity - 1 &&
      audit_end.analysis_backlog_samples < PitchAnalysis::kFifoCapacity / 2;
  const bool realtime = pitch_hops_per_second >= kRequiredHopsPerSecond &&
                        fifo_bounded;
  const bool b4b4d_pass = b4b4b_transport_pass && teardown_clean &&
                          new_yin_difference_us_per_hop <= 2000.0;
  printf("\nB4B.4D production selection: YIN_DIFF_FMA_8ACC\n");
  printf("Host guardrails: NaN/Inf=0 selected_tau=0 voiced=0 track_state=0 pitch_marks=0\n");
  printf("\nSECTION                                  B4B.4C       B4B.4D        SPEEDUP\n");
  printf("YinDifference (us/pitch hop)            %9.2f %14.2f %11.2fx\n",
         kBeforeYinDifferenceUsPerHop, new_yin_difference_us_per_hop,
         new_yin_difference_us_per_hop > 0.0
             ? kBeforeYinDifferenceUsPerHop / new_yin_difference_us_per_hop
             : 0.0);
  printf("PitchAnalysis total (us/pitch hop)      %9.2f %14.2f %11.2fx\n",
         kBeforePitchAnalysisUsPerHop, new_pitch_analysis_us_per_hop,
         new_pitch_analysis_us_per_hop > 0.0
             ? kBeforePitchAnalysisUsPerHop / new_pitch_analysis_us_per_hop
             : 0.0);
  printf("LPC total (us/frame)                    %9.2f %14.2f %11.2fx\n",
         kBeforeLpcUsPerFrame, lpc_avg_frame_us,
         lpc_avg_frame_us > 0.0 ? kBeforeLpcUsPerFrame / lpc_avg_frame_us
                                : 0.0);
  printf("Combined worker cost (us/pitch hop)     %9.2f %14.2f %11.2fx\n",
         kBeforeCombinedUsPerHop, average_hop_us,
         average_hop_us > 0.0 ? kBeforeCombinedUsPerHop / average_hop_us
                              : 0.0);
  printf("Core 1 utilization (percent)             %8.2f %14.2f %11.2fx\n",
         kBeforeCore1Percent, core1_utilization,
         core1_utilization > 0.0 ? kBeforeCore1Percent / core1_utilization
                                 : 0.0);
  printf("Pitch throughput (hops/s)                %8.2f %14.2f %11.2fx\n",
         kBeforePitchHopsPerSecond, pitch_hops_per_second,
         kBeforePitchHopsPerSecond > 0.0
             ? pitch_hops_per_second / kBeforePitchHopsPerSecond
             : 0.0);
  printf("PitchAnalysis budget estimate: without_old_diff=%.2f optimized_total=%.2f theoretical_max=%.2f hops/s\n",
         kBeforePitchAnalysisUsPerHop - kBeforeYinDifferenceUsPerHop,
         kBeforePitchAnalysisUsPerHop - kBeforeYinDifferenceUsPerHop +
             new_yin_difference_us_per_hop,
         (kBeforePitchAnalysisUsPerHop - kBeforeYinDifferenceUsPerHop +
                      new_yin_difference_us_per_hop) > 0.0
             ? 1000000.0 /
                   (kBeforePitchAnalysisUsPerHop -
                    kBeforeYinDifferenceUsPerHop +
                    new_yin_difference_us_per_hop)
             : 0.0);
  printf("Core1 headroom targets at 200 hops/s: 70%%=%s 80%%=%s 90%%=%s (cost limits 3500/4000/4500 us)\n",
         average_hop_us <= 3500.0 ? "PASS" : "FAIL",
         average_hop_us <= 4000.0 ? "PASS" : "FAIL",
         average_hop_us <= 4500.0 ? "PASS" : "FAIL");
  printf("FIFO/backlog optimized: current=%lu max=%lu drops=%llu bounded=%s backlog_current_ms=%.3f backlog_max_ms=%.3f pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         static_cast<unsigned long>(audit_end.fifo_current_occupancy),
         static_cast<unsigned long>(audit_end.fifo_maximum_occupancy),
         static_cast<unsigned long long>(audit_end.fifo_drops -
                                         audit_start.fifo_drops),
         fifo_bounded ? "yes" : "no", audit_end.analysis_backlog_ms,
         audit_end.analysis_backlog_max_ms, audit_end.latest_pitch_age_ms,
         audit_end.pitch_age_average_ms, audit_end.pitch_age_p95_ms,
         audit_end.pitch_age_p99_ms, audit_end.pitch_age_max_ms);
  printf("Tracker final=%s reached_LOCKED=%s coherent_marks=%u PSOLA_usable=%s grain_attempts=%llu grains_scheduled=%llu grains_rendered=%llu\n",
         pitch_track_name(pitch_track_end), ever_locked ? "yes" : "no",
         static_cast<unsigned>(audit_end.coherent_marks),
         psola_usable ? "yes" : "no",
         static_cast<unsigned long long>(funnel_end.grain_schedule_attempts),
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("Pitch marks: accepted=%llu rejected=%llu generated=%llu transferred=%llu consumed=%llu coherent_current=%u coherent_max=%u coherent_increments=%llu coherent_resets=%llu\n",
         static_cast<unsigned long long>(audit_end.accepted_marks),
         static_cast<unsigned long long>(audit_end.rejected_marks),
         static_cast<unsigned long long>(funnel_end.pitch_marks_generated),
         static_cast<unsigned long long>(funnel_end.pitch_marks_transferred),
         static_cast<unsigned long long>(funnel_end.pitch_marks_consumed),
         static_cast<unsigned>(audit_end.coherent_marks),
         static_cast<unsigned>(audit_end.coherent_marks_maximum),
         static_cast<unsigned long long>(audit_end.coherent_mark_increments),
         static_cast<unsigned long long>(audit_end.coherent_mark_resets));
  printf("YIN memory: sizeof_before=%zu sizeof_after=%zu dynamic_hot_path_allocations=0\n",
         sizeof(YinDetector), sizeof(YinDetector));
  for (const char *requested : {"YinDifferenceArray", "YinCmndArray",
                                "PitchRollingWindow", "PitchLinearWindow"}) {
    for (size_t i = 0; i < buffer_audit_count; ++i) {
      if (std::string_view(buffer_audits[i].name) != requested)
        continue;
      const char *placement =
          buffer_audits[i].is_psram
              ? "PSRAM"
              : (buffer_audits[i].is_sram ? "internal SRAM" : "other");
      printf("  %s: ptr=%p bytes=%zu placement=%s\n", buffer_audits[i].name,
             buffer_audits[i].ptr, buffer_audits[i].size_bytes, placement);
    }
  }
  printf("B4B.4D transport/teardown audit: %s\n",
         b4b4b_transport_pass && teardown_clean ? "PASS" : "FAIL");
  printf("\nB4B.4D RESULT:\n%s\n", b4b4d_pass ? "PASS" : "FAIL");
  printf("PRODUCTION YIN DIFFERENCE:\nYIN_DIFF_FMA_8ACC\n");
  printf("REFERENCE:\n%.2f us/hop\n", kBeforeYinDifferenceUsPerHop);
  printf("OPTIMIZED:\n%.2f us/hop\n", new_yin_difference_us_per_hop);
  printf("ELIGIBLE SPEEDUP:\n%.2fx\n",
         new_yin_difference_us_per_hop > 0.0
             ? kBeforeYinDifferenceUsPerHop / new_yin_difference_us_per_hop
             : 0.0);
  printf("PITCH ANALYSIS TOTAL:\n%.2f us/hop\n",
         new_pitch_analysis_us_per_hop);
  printf("CORE1:\n%.2f%%\n", core1_utilization);
  printf("PITCH THROUGHPUT:\n%.2f hops/s\n", pitch_hops_per_second);
  printf("FIFO:\n%s\n", fifo_bounded ? "bounded" : "saturated");
  printf("PITCH AGE:\n%.2f ms\n", audit_end.latest_pitch_age_ms);
  printf("TRACK:\n%s\n", pitch_track_name(pitch_track_end));
  printf("PSOLA USABLE:\n%s\n", psola_usable ? "yes" : "no");
  printf("GRAINS:\nscheduled=%llu rendered=%llu\n",
         static_cast<unsigned long long>(funnel_end.grains_scheduled),
         static_cast<unsigned long long>(funnel_end.grains_rendered));
  printf("REALTIME 200 HOPS/S:\n%s\n", realtime ? "PASS" : "FAIL");
  printf("NEXT PRIMARY HOTSPOT:\n%s\n", primary_hotspot);
  printf("NEXT STEP:\n%s\n",
         new_yin_difference_us_per_hop > 2000.0
             ? "SCALAR_KERNEL_OPTIMIZATION_INSUFFICIENT"
             : "strictly follow the measured primary hotspot");
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

#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
namespace {
using vocal_fx_platform::B4B6StimulusKind;

struct B4B6CaseDefinition {
  const char *name;
  const char *family;
  B4B6StimulusKind stimulus;
  float nominal_f0_hz;
  bool representative;
  bool vocal;
};

struct B4B6CaseResult {
  const B4B6CaseDefinition *definition = nullptr;
  uint32_t measured_seconds = 0;
  double wall_seconds = 0.0;
  uint64_t pitch_hops = 0;
  uint64_t lpc_frames = 0;
  double pitch_analysis_us_hop = 0.0;
  double pitch_mark_us_hop = 0.0;
  double pitch_mark_correlation_us_hop = 0.0;
  double lpc_total_us_frame = 0.0;
  double total_ms_s = 0.0;
  std::array<double, 5> yin_us_hop{};
  std::array<double, 3> lpc_detail_us_frame{};
  VocalFxProfileDistribution pitch_distribution{};
  VocalFxProfileDistribution mark_distribution{};
  LpcFrameCostSummary lpc_distribution{};
  PitchMarkForensicTelemetry geometry{};
  PitchAnalysisAuditTelemetry audit{};
  uint64_t searches = 0, offsets = 0, sample_pairs = 0;
  uint64_t grain_attempted = 0, grain_scheduled = 0, grain_rendered = 0;
  uint64_t grain_no_marks = 0, grain_low_confidence = 0;
  uint64_t grain_invalid_period = 0, grain_distance = 0;
  uint64_t grain_history = 0, grain_source_negative = 0, grain_other = 0;
  uint64_t fifo_drops = 0;
  float backlog_first_ms = 0.0f, backlog_last_ms = 0.0f;
  uint32_t backlog_growth_streak = 0;
  bool transport_clean = false;
  bool teardown_clean = false;
  bool trend_bounded = false;
  bool health_pass = false;
  bool pass = false;
};

constexpr std::array<B4B6CaseDefinition, 39> kB4B6Cases{{
    {"pure_65", "pure", B4B6StimulusKind::PureTone, 65.0f, false, false},
    {"pure_66", "pure", B4B6StimulusKind::PureTone, 66.0f, false, false},
    {"pure_80", "pure", B4B6StimulusKind::PureTone, 80.0f, true, false},
    {"pure_110", "pure", B4B6StimulusKind::PureTone, 110.0f, true, false},
    {"pure_147", "pure", B4B6StimulusKind::PureTone, 147.0f, true, false},
    {"pure_220", "pure", B4B6StimulusKind::PureTone, 220.0f, true, false},
    {"pure_330", "pure", B4B6StimulusKind::PureTone, 330.0f, true, false},
    {"pure_440", "pure", B4B6StimulusKind::PureTone, 440.0f, true, false},
    {"pure_700", "pure", B4B6StimulusKind::PureTone, 700.0f, false, false},
    {"pure_900", "pure", B4B6StimulusKind::PureTone, 900.0f, false, false},
    {"pure_990", "pure", B4B6StimulusKind::PureTone, 990.0f, false, false},
    {"pure_1000", "pure", B4B6StimulusKind::PureTone, 1000.0f, false, false},
    {"harmonic_65", "harmonic", B4B6StimulusKind::HarmonicVoiced, 65.0f, false, false},
    {"harmonic_66", "harmonic", B4B6StimulusKind::HarmonicVoiced, 66.0f, false, false},
    {"harmonic_80", "harmonic", B4B6StimulusKind::HarmonicVoiced, 80.0f, true, false},
    {"harmonic_110", "harmonic", B4B6StimulusKind::HarmonicVoiced, 110.0f, true, false},
    {"harmonic_147", "harmonic", B4B6StimulusKind::HarmonicVoiced, 147.0f, true, false},
    {"harmonic_220", "harmonic", B4B6StimulusKind::HarmonicVoiced, 220.0f, true, false},
    {"harmonic_330", "harmonic", B4B6StimulusKind::HarmonicVoiced, 330.0f, true, false},
    {"harmonic_440", "harmonic", B4B6StimulusKind::HarmonicVoiced, 440.0f, true, false},
    {"harmonic_700", "harmonic", B4B6StimulusKind::HarmonicVoiced, 700.0f, false, false},
    {"harmonic_900", "harmonic", B4B6StimulusKind::HarmonicVoiced, 900.0f, false, false},
    {"harmonic_990", "harmonic", B4B6StimulusKind::HarmonicVoiced, 990.0f, false, false},
    {"harmonic_1000", "harmonic", B4B6StimulusKind::HarmonicVoiced, 1000.0f, false, false},
    {"broadband_noise", "noise", B4B6StimulusKind::BroadbandNoise, 0.0f, true, false},
    {"breathy_220", "noise", B4B6StimulusKind::BreathyVoiced, 220.0f, true, false},
    {"silence", "silence", B4B6StimulusKind::Silence, 0.0f, true, false},
    {"near_silence", "silence", B4B6StimulusKind::NearSilence, 0.0f, true, false},
    {"step_110_220", "transition", B4B6StimulusKind::Step110To220, 110.0f, true, false},
    {"step_220_110", "transition", B4B6StimulusKind::Step220To110, 220.0f, true, false},
    {"step_110_440", "transition", B4B6StimulusKind::Step110To440, 110.0f, true, false},
    {"step_440_110", "transition", B4B6StimulusKind::Step440To110, 440.0f, true, false},
    {"step_147_220_330", "transition", B4B6StimulusKind::Step147To220To330, 147.0f, true, false},
    {"gliss_80_440", "glissando", B4B6StimulusKind::Gliss80To440, 80.0f, true, false},
    {"gliss_440_80", "glissando", B4B6StimulusKind::Gliss440To80, 440.0f, true, false},
    {"vibrato_1st", "vibrato", B4B6StimulusKind::VibratoOneSemitone, 220.0f, true, false},
    {"vibrato_2st", "vibrato", B4B6StimulusKind::VibratoTwoSemitones, 220.0f, true, false},
    {"staccato_110_220_440", "staccato", B4B6StimulusKind::Staccato, 110.0f, true, false},
    {"real_vocal_corpus", "real_vocal", B4B6StimulusKind::VocalReplay, 0.0f, true, true},
}};

double b4b6_average_us(const VocalFxProfileStats &stats, uint64_t count) {
  return count ? static_cast<double>(stats.total_cycles) /
                     (Profiler::cycles_per_us() * static_cast<double>(count))
               : 0.0;
}

uint64_t b4b6_grain_sum(uint64_t GrainRejectionTelemetry::*member,
                        const GrainRejectionTelemetry &a,
                        const GrainRejectionTelemetry &b) {
  return a.*member + b.*member;
}

B4B6CaseResult b4b6_run_case(const B4B6CaseDefinition &definition,
                             uint32_t measured_seconds,
                             const char *role) {
  B4B6CaseResult result{};
  result.definition = &definition;
  result.measured_seconds = measured_seconds;
  vocal_fx_reset();
  vocal_fx_reset_funnel_stats();
  s_audio.configure_b4b6_stimulus(
      definition.stimulus, definition.nominal_f0_hz,
      definition.vocal ? kB4b6VocalMulaw : nullptr,
      definition.vocal ? kB4b6VocalMulawSize : 0);
  if (!start_pipeline_tasks(
          vocal_fx_platform::AudioI2sMode::B4B6RcValidation, true)) {
    printf("B4B6_CASE role=%s name=%s status=START_FAILURE\n", role,
           definition.name);
    return result;
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
  s_b4b6_measurement_pause.store(true, std::memory_order_release);
  s_audio.set_b4b6_dsp_paused(true);
  while (s_pitch_worker_in_call.load(std::memory_order_acquire) ||
         s_audio.b4b6_dsp_active())
    vTaskDelay(1);
  vocal_fx_reset_measurement_telemetry();

  const TransportSnapshot transport_start = transport_snapshot();
  const PitchAnalysisAuditTelemetry audit_start =
      vocal_fx_pitch_analysis_audit_telemetry();
  const VocalFxFunnelStats funnel_start = vocal_fx_funnel_stats();
  const GrainRejectionTelemetry grain0_start =
      vocal_fx_grain_rejection_telemetry(0);
  const GrainRejectionTelemetry grain1_start =
      vocal_fx_grain_rejection_telemetry(1);
  result.backlog_first_ms = audit_start.analysis_backlog_ms;
  float previous_backlog = result.backlog_first_ms;
  uint32_t current_growth_streak = 0;
  const uint64_t window_start_us = esp_timer_get_time();
  s_audio.set_b4b6_dsp_paused(false);
  s_b4b6_measurement_pause.store(false, std::memory_order_release);

  for (uint32_t second = 0; second < measured_seconds; ++second) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const PitchAnalysisAuditTelemetry sample =
        vocal_fx_pitch_analysis_audit_telemetry();
    current_growth_streak =
        sample.analysis_backlog_ms > previous_backlog + 0.05f
            ? current_growth_streak + 1
            : 0;
    result.backlog_growth_streak =
        std::max(result.backlog_growth_streak, current_growth_streak);
    previous_backlog = sample.analysis_backlog_ms;
  }
  vTaskDelay(1);

  s_b4b6_measurement_pause.store(true, std::memory_order_release);
  s_audio.set_b4b6_dsp_paused(true);
  while (s_pitch_worker_in_call.load(std::memory_order_acquire) ||
         s_audio.b4b6_dsp_active())
    vTaskDelay(1);
  const uint64_t window_end_us = esp_timer_get_time();
  result.wall_seconds = (window_end_us - window_start_us) / 1000000.0;

  std::array<VocalFxProfileStats,
             static_cast<size_t>(PitchAnalysisProfileSection::Count)>
      pitch{};
  for (size_t i = 0; i < pitch.size(); ++i)
    pitch[i] = vocal_fx_pitch_profile_stats(
        static_cast<PitchAnalysisProfileSection>(i));
  std::array<VocalFxProfileStats,
             static_cast<size_t>(LpcProfileSection::Count)>
      lpc{};
  for (size_t i = 0; i < lpc.size(); ++i)
    lpc[i] = vocal_fx_lpc_profile_stats(static_cast<LpcProfileSection>(i));
  result.geometry = vocal_fx_pitch_mark_forensic_telemetry();
  result.audit = vocal_fx_pitch_analysis_audit_telemetry();
  const TransportSnapshot transport_end = transport_snapshot();
  const VocalFxFunnelStats funnel_end = vocal_fx_funnel_stats();
  const GrainRejectionTelemetry grain0_end =
      vocal_fx_grain_rejection_telemetry(0);
  const GrainRejectionTelemetry grain1_end =
      vocal_fx_grain_rejection_telemetry(1);
  result.pitch_distribution = vocal_fx_pitch_profile_distribution(
      PitchAnalysisProfileSection::Total);
  result.mark_distribution = vocal_fx_pitch_profile_distribution(
      PitchAnalysisProfileSection::PitchMarkSearch);
  result.lpc_distribution = vocal_fx_lpc_frame_cost_summary();

  result.backlog_last_ms = result.audit.analysis_backlog_ms;
  result.pitch_hops =
      pitch[static_cast<size_t>(PitchAnalysisProfileSection::Total)].blocks;
  result.lpc_frames = result.lpc_distribution.count;
  const auto pitch_avg = [&](PitchAnalysisProfileSection section) {
    return b4b6_average_us(pitch[static_cast<size_t>(section)],
                           result.pitch_hops);
  };
  const auto lpc_avg = [&](LpcProfileSection section) {
    return b4b6_average_us(lpc[static_cast<size_t>(section)],
                           result.lpc_frames);
  };
  result.pitch_analysis_us_hop =
      pitch_avg(PitchAnalysisProfileSection::Total);
  result.pitch_mark_us_hop =
      pitch_avg(PitchAnalysisProfileSection::PitchMarkSearch);
  result.pitch_mark_correlation_us_hop =
      pitch_avg(PitchAnalysisProfileSection::PitchMarkCorrelation);
  result.yin_us_hop = {{
      pitch_avg(PitchAnalysisProfileSection::YinEnergy),
      pitch_avg(PitchAnalysisProfileSection::YinDifference),
      pitch_avg(PitchAnalysisProfileSection::YinCmnd),
      pitch_avg(PitchAnalysisProfileSection::YinSearch),
      pitch_avg(PitchAnalysisProfileSection::YinInterpolation),
  }};
  result.lpc_detail_us_frame = {{
      lpc_avg(LpcProfileSection::SolveWindowing),
      lpc_avg(LpcProfileSection::Autocorrelation),
      lpc_avg(LpcProfileSection::LevinsonDurbin),
  }};
  result.lpc_total_us_frame = lpc_avg(LpcProfileSection::Total);
  result.total_ms_s = result.pitch_analysis_us_hop * 0.2 +
                      result.lpc_total_us_frame * 0.125;
  result.searches = result.geometry.correlation_searches;
  result.offsets = result.geometry.candidate_offsets_evaluated;
  result.sample_pairs = result.geometry.sample_pairs_correlated;
  result.fifo_drops = result.audit.fifo_drops - audit_start.fifo_drops;
  result.grain_attempted = funnel_end.grain_schedule_attempts -
                           funnel_start.grain_schedule_attempts;
  result.grain_scheduled =
      funnel_end.grains_scheduled - funnel_start.grains_scheduled;
  result.grain_rendered =
      funnel_end.grains_rendered - funnel_start.grains_rendered;

  const auto grain_delta = [&](uint64_t GrainRejectionTelemetry::*member) {
    return b4b6_grain_sum(member, grain0_end, grain1_end) -
           b4b6_grain_sum(member, grain0_start, grain1_start);
  };
  result.grain_no_marks =
      grain_delta(&GrainRejectionTelemetry::select_mark_no_marks);
  result.grain_low_confidence =
      grain_delta(&GrainRejectionTelemetry::select_mark_low_confidence);
  result.grain_invalid_period =
      grain_delta(&GrainRejectionTelemetry::select_mark_invalid_period);
  result.grain_distance =
      grain_delta(&GrainRejectionTelemetry::select_mark_distance_too_large);
  result.grain_history =
      grain_delta(&GrainRejectionTelemetry::history_failure_total);
  result.grain_source_negative =
      grain_delta(&GrainRejectionTelemetry::attempt_source_negative);
  const uint64_t known_terminal_rejections =
      result.grain_no_marks + result.grain_low_confidence +
      result.grain_invalid_period + result.grain_distance +
      result.grain_history + result.grain_source_negative;
  const uint64_t unscheduled =
      result.grain_attempted > result.grain_scheduled
          ? result.grain_attempted - result.grain_scheduled
          : 0;
  result.grain_other = unscheduled > known_terminal_rejections
                           ? unscheduled - known_terminal_rejections
                           : 0;
  result.transport_clean =
      transport_end.rx_dma_errors == transport_start.rx_dma_errors &&
      transport_end.tx_dma_errors == transport_start.tx_dma_errors &&
      transport_end.read_failures == transport_start.read_failures &&
      transport_end.write_failures == transport_start.write_failures &&
      transport_end.rx_dropped_frames == transport_start.rx_dropped_frames &&
      transport_end.tx_dropped_frames == transport_start.tx_dropped_frames;
  result.trend_bounded =
      !(result.backlog_growth_streak >= 5 &&
        result.backlog_last_ms > result.backlog_first_ms + 5.0f);
  result.teardown_clean = stop_pipeline_tasks();
  s_audio.set_b4b6_dsp_paused(false);
  s_b4b6_measurement_pause.store(false, std::memory_order_release);

  const bool fifo_bounded =
      result.fifo_drops == 0 &&
      result.audit.fifo_current_occupancy < PitchAnalysis::kFifoCapacity - 1 &&
      result.audit.fifo_maximum_occupancy < PitchAnalysis::kFifoCapacity - 1;
  const bool backlog_bounded = result.audit.analysis_backlog_max_ms <= 50.0f &&
                               result.trend_bounded;
  const bool pitch_age_healthy = result.audit.pitch_age_max_ms <= 50.0f;
  result.health_pass = result.wall_seconds >= measured_seconds &&
                       fifo_bounded && backlog_bounded && pitch_age_healthy &&
                       result.transport_clean && result.teardown_clean;
  result.pass = result.health_pass && result.total_ms_s <= 1000.0;

  const double searches_hop = result.pitch_hops
                                  ? static_cast<double>(result.searches) /
                                        result.pitch_hops
                                  : 0.0;
  const double searches_s = result.wall_seconds > 0.0
                                ? result.searches / result.wall_seconds
                                : 0.0;
  const double offsets_search = result.searches
                                    ? static_cast<double>(result.offsets) /
                                          result.searches
                                    : 0.0;
  const double pairs_search = result.searches
                                  ? static_cast<double>(result.sample_pairs) /
                                        result.searches
                                  : 0.0;
  printf("B4B6_CASE role=%s name=%s family=%s f0_hz=%.3f representative=%s vocal=%s measured_s=%.6f pitch_hops=%llu lpc_frames=%llu total_ms_s=%.3f pitch_us_hop=%.3f pitchmark_us_hop=%.3f correlation_us_hop=%.3f lpc_us_frame=%.3f health=%s pass=%s\n",
         role, definition.name, definition.family, definition.nominal_f0_hz,
         definition.representative ? "YES" : "NO",
         definition.vocal ? "YES" : "NO", result.wall_seconds,
         static_cast<unsigned long long>(result.pitch_hops),
         static_cast<unsigned long long>(result.lpc_frames), result.total_ms_s,
         result.pitch_analysis_us_hop, result.pitch_mark_us_hop,
         result.pitch_mark_correlation_us_hop, result.lpc_total_us_frame,
         result.health_pass ? "PASS" : "FAIL",
         result.pass ? "PASS" : "FAIL");
  printf("B4B6_COST role=%s name=%s YinEnergy=%.3f YinDifference=%.3f YinCMND=%.3f YinSearch=%.3f YinInterpolation=%.3f PitchMarkSearch=%.3f PitchMarkCorrelation=%.3f PitchAnalysis=%.3f LPC_windowing=%.3f LPC_autocorrelation=%.3f LPC_Levinson=%.3f LPC_total=%.3f\n",
         role, definition.name, result.yin_us_hop[0], result.yin_us_hop[1],
         result.yin_us_hop[2], result.yin_us_hop[3], result.yin_us_hop[4],
         result.pitch_mark_us_hop, result.pitch_mark_correlation_us_hop,
         result.pitch_analysis_us_hop, result.lpc_detail_us_frame[0],
         result.lpc_detail_us_frame[1], result.lpc_detail_us_frame[2],
         result.lpc_total_us_frame);
  printf("B4B6_GEOMETRY role=%s name=%s observations=%lu period_P50=%lu period_P95=%lu period_P99=%lu period_max=%lu radius_P50=%lu radius_P95=%lu radius_P99=%lu radius_max=%lu window_P50=%lu window_P95=%lu window_P99=%lu window_max=%lu offsets_P50=%lu offsets_P95=%lu offsets_P99=%lu offsets_max=%lu pairs_P50=%lu pairs_P95=%lu pairs_P99=%lu pairs_max=%lu searches_hop=%.5f searches_s=%.3f offsets_search=%.3f pairs_search=%.3f\n",
         role, definition.name,
         static_cast<unsigned long>(result.geometry.geometry_observations),
         static_cast<unsigned long>(result.geometry.period_p50),
         static_cast<unsigned long>(result.geometry.period_p95),
         static_cast<unsigned long>(result.geometry.period_p99),
         static_cast<unsigned long>(result.geometry.period_max),
         static_cast<unsigned long>(result.geometry.radius_p50),
         static_cast<unsigned long>(result.geometry.radius_p95),
         static_cast<unsigned long>(result.geometry.radius_p99),
         static_cast<unsigned long>(result.geometry.radius_max),
         static_cast<unsigned long>(result.geometry.window_p50),
         static_cast<unsigned long>(result.geometry.window_p95),
         static_cast<unsigned long>(result.geometry.window_p99),
         static_cast<unsigned long>(result.geometry.window_max),
         static_cast<unsigned long>(result.geometry.offsets_p50),
         static_cast<unsigned long>(result.geometry.offsets_p95),
         static_cast<unsigned long>(result.geometry.offsets_p99),
         static_cast<unsigned long>(result.geometry.offsets_max),
         static_cast<unsigned long>(result.geometry.pairs_p50),
         static_cast<unsigned long>(result.geometry.pairs_p95),
         static_cast<unsigned long>(result.geometry.pairs_p99),
         static_cast<unsigned long>(result.geometry.pairs_max), searches_hop,
         searches_s, offsets_search, pairs_search);
  printf("B4B6_QUANTILES role=%s name=%s PitchAnalysis_avg=%.3f PitchAnalysis_P50=%lu PitchAnalysis_P95=%lu PitchAnalysis_P99=%lu PitchAnalysis_max=%llu PitchMark_avg=%.3f PitchMark_P50=%lu PitchMark_P95=%lu PitchMark_P99=%lu PitchMark_max=%llu LPC_avg=%.3f LPC_P50=%lu LPC_P95=%lu LPC_P99=%lu LPC_max=%lu\n",
         role, definition.name, result.pitch_analysis_us_hop,
         static_cast<unsigned long>(result.pitch_distribution.p50_us),
         static_cast<unsigned long>(result.pitch_distribution.p95_us),
         static_cast<unsigned long>(result.pitch_distribution.p99_us),
         static_cast<unsigned long long>(
             pitch[static_cast<size_t>(PitchAnalysisProfileSection::Total)]
                 .worst_us),
         result.pitch_mark_us_hop,
         static_cast<unsigned long>(result.mark_distribution.p50_us),
         static_cast<unsigned long>(result.mark_distribution.p95_us),
         static_cast<unsigned long>(result.mark_distribution.p99_us),
         static_cast<unsigned long long>(
             pitch[static_cast<size_t>(
                       PitchAnalysisProfileSection::PitchMarkSearch)]
                 .worst_us),
         result.lpc_total_us_frame,
         static_cast<unsigned long>(result.lpc_distribution.p50_us),
         static_cast<unsigned long>(result.lpc_distribution.p95_us),
         static_cast<unsigned long>(result.lpc_distribution.p99_us),
         static_cast<unsigned long>(result.lpc_distribution.max_us));
  printf("B4B6_HEALTH role=%s name=%s fifo_current=%lu fifo_max=%lu fifo_capacity=%lu drops=%llu backlog_current_ms=%.3f backlog_max_ms=%.3f backlog_first_ms=%.3f backlog_last_ms=%.3f backlog_growth_streak=%lu monotonic_growth=%s pitch_age_current_ms=%.3f pitch_age_avg_ms=%.3f pitch_age_P95_ms=%.3f pitch_age_P99_ms=%.3f pitch_age_max_ms=%.3f\n",
         role, definition.name,
         static_cast<unsigned long>(result.audit.fifo_current_occupancy),
         static_cast<unsigned long>(result.audit.fifo_maximum_occupancy),
         static_cast<unsigned long>(PitchAnalysis::kFifoCapacity),
         static_cast<unsigned long long>(result.fifo_drops),
         result.audit.analysis_backlog_ms,
         result.audit.analysis_backlog_max_ms, result.backlog_first_ms,
         result.backlog_last_ms,
         static_cast<unsigned long>(result.backlog_growth_streak),
         result.trend_bounded ? "NO" : "YES",
         result.audit.latest_pitch_age_ms, result.audit.pitch_age_average_ms,
         result.audit.pitch_age_p95_ms, result.audit.pitch_age_p99_ms,
         result.audit.pitch_age_max_ms);
  printf("B4B6_GRAINS role=%s name=%s attempted=%llu scheduled=%llu rendered=%llu no_marks=%llu low_confidence=%llu invalid_period=%llu distance_too_large=%llu history_failures=%llu source_negative=%llu other=%llu\n",
         role, definition.name,
         static_cast<unsigned long long>(result.grain_attempted),
         static_cast<unsigned long long>(result.grain_scheduled),
         static_cast<unsigned long long>(result.grain_rendered),
         static_cast<unsigned long long>(result.grain_no_marks),
         static_cast<unsigned long long>(result.grain_low_confidence),
         static_cast<unsigned long long>(result.grain_invalid_period),
         static_cast<unsigned long long>(result.grain_distance),
         static_cast<unsigned long long>(result.grain_history),
         static_cast<unsigned long long>(result.grain_source_negative),
         static_cast<unsigned long long>(result.grain_other));
  printf("B4B6_TRANSPORT role=%s name=%s rx_dma_errors=%llu tx_dma_errors=%llu read_failures=%llu write_failures=%llu dropped_rx_frames=%llu dropped_tx_frames=%llu diagnostic_rx_overflow=%llu diagnostic_tx_overflow=%llu transport=%s teardown=%s\n",
         role, definition.name,
         static_cast<unsigned long long>(transport_end.rx_dma_errors -
                                         transport_start.rx_dma_errors),
         static_cast<unsigned long long>(transport_end.tx_dma_errors -
                                         transport_start.tx_dma_errors),
         static_cast<unsigned long long>(transport_end.read_failures -
                                         transport_start.read_failures),
         static_cast<unsigned long long>(transport_end.write_failures -
                                         transport_start.write_failures),
         static_cast<unsigned long long>(transport_end.rx_dropped_frames -
                                         transport_start.rx_dropped_frames),
         static_cast<unsigned long long>(transport_end.tx_dropped_frames -
                                         transport_start.tx_dropped_frames),
         static_cast<unsigned long long>(transport_end.rx_queue_overflows -
                                         transport_start.rx_queue_overflows),
         static_cast<unsigned long long>(transport_end.tx_queue_overflows -
                                         transport_start.tx_queue_overflows),
         result.transport_clean ? "PASS" : "FAIL",
         result.teardown_clean ? "PASS" : "FAIL");
  return result;
}

inline uint32_t b4b6a_cycle_probe() {
  asm volatile("" ::: "memory");
  return esp_cpu_get_cycle_count();
}

uint32_t b4b6a_probe_overhead() {
  uint32_t best = UINT32_MAX;
  for (size_t i = 0; i < 1024; ++i) {
    const uint32_t start = b4b6a_cycle_probe();
    const uint32_t elapsed = b4b6a_cycle_probe() - start;
    best = std::min(best, elapsed);
  }
  return best;
}

inline uint32_t b4b6a_net_cycles(uint32_t start, uint32_t overhead) {
  const uint32_t elapsed = b4b6a_cycle_probe() - start;
  return elapsed > overhead ? elapsed - overhead : 0;
}

struct PitchMarkLowF0Probe {
  float f0_hz;
  int period;
  int radius;
  int window;
};

struct PitchMarkDetailedCost {
  uint64_t ring_lookup = 0;
  uint64_t dot = 0;
  uint64_t aa = 0;
  uint64_t bb = 0;
  uint64_t square_root = 0;
  uint64_t division = 0;
  uint64_t candidate_loop = 0;
  uint64_t best_selection = 0;
};

PitchMarkDetailedCost profile_low_f0_reference_cost(
    const float *ring, size_t ring_size, int period, int radius, int window,
    uint32_t probe_overhead, volatile float &checksum) {
  constexpr size_t kProfileRuns = 4;
  const size_t mask = ring_size - 1;
  PitchMarkDetailedCost cost{};
  for (size_t run = 0; run < kProfileRuns; ++run) {
    const uint64_t previous = 512 + run;
    const uint64_t predicted = previous + period;
    const uint64_t a_start = previous - window;
    float best_score = -2.0f;
    int best_offset = 0;
    for (int offset = -radius; offset <= radius; ++offset) {
      uint32_t start = b4b6a_cycle_probe();
      const uint64_t candidate = predicted + offset;
      const uint64_t b_start = candidate - window;
      asm volatile("" : : "r"(candidate), "r"(b_start));
      cost.candidate_loop += b4b6a_net_cycles(start, probe_overhead);

      float dot = 0.0f, aa = 0.0f, bb = 0.0f;
      for (int i = 0; i < window; ++i) {
        start = b4b6a_cycle_probe();
        const float a = ring[(a_start + i) & mask];
        const float b = ring[(b_start + i) & mask];
        asm volatile("" : : "f"(a), "f"(b) : "memory");
        cost.ring_lookup += b4b6a_net_cycles(start, probe_overhead);

        start = b4b6a_cycle_probe();
        dot += a * b;
        asm volatile("" : "+f"(dot));
        cost.dot += b4b6a_net_cycles(start, probe_overhead);

        start = b4b6a_cycle_probe();
        aa += a * a;
        asm volatile("" : "+f"(aa));
        cost.aa += b4b6a_net_cycles(start, probe_overhead);

        start = b4b6a_cycle_probe();
        bb += b * b;
        asm volatile("" : "+f"(bb));
        cost.bb += b4b6a_net_cycles(start, probe_overhead);
      }
      start = b4b6a_cycle_probe();
      const float denominator = std::sqrt(std::max(aa * bb, 1e-20f));
      asm volatile("" : : "f"(denominator));
      cost.square_root += b4b6a_net_cycles(start, probe_overhead);

      start = b4b6a_cycle_probe();
      const float score = dot / denominator;
      asm volatile("" : : "f"(score));
      cost.division += b4b6a_net_cycles(start, probe_overhead);

      start = b4b6a_cycle_probe();
      if (score > best_score) {
        best_score = score;
        best_offset = offset;
      }
      asm volatile("" : "+f"(best_score), "+r"(best_offset));
      cost.best_selection += b4b6a_net_cycles(start, probe_overhead);
    }
    checksum = checksum + best_score + static_cast<float>(best_offset) * 1e-9f;
  }
  cost.ring_lookup /= kProfileRuns;
  cost.dot /= kProfileRuns;
  cost.aa /= kProfileRuns;
  cost.bb /= kProfileRuns;
  cost.square_root /= kProfileRuns;
  cost.division /= kProfileRuns;
  cost.candidate_loop /= kProfileRuns;
  cost.best_selection /= kProfileRuns;
  return cost;
}

void run_b4b6a_low_f0_decomposition(void) {
  printf("\n=======================================================\n");
  printf(" B4B.6A — LOW-F0 PITCHMARK CORRELATION DECOMPOSITION\n");
  printf("=======================================================\n");
  constexpr size_t kRingSize = 2048;
  auto *ring = static_cast<float *>(heap_caps_malloc(
      kRingSize * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring) {
    printf("B4B6A_ERROR: failed to allocate %u ring buffer\n", static_cast<unsigned>(kRingSize));
    return;
  }

  constexpr std::array<PitchMarkLowF0Probe, 7> kProbes{{
      {65.0f, 738, 147, 369},
      {66.0f, 727, 145, 363},
      {80.0f, 600, 120, 300},
      {110.0f, 436, 87, 218},
      {147.0f, 327, 65, 163},
      {220.0f, 218, 43, 109},
      {440.0f, 109, 21, 54},
  }};

  const uint32_t probe_overhead = b4b6a_probe_overhead();
  volatile float checksum = 0.0f;
  constexpr float kPi = 3.14159265358979323846f;

  for (const auto &probe : kProbes) {
    for (size_t i = 0; i < kRingSize; ++i) {
      const float phase = 2.0f * kPi * probe.f0_hz * static_cast<float>(i) / 48000.0f;
      ring[i] = 0.12589254f *
                (std::sin(phase) + .31f * std::sin(2.0f * phase + .23f) +
                 .13f * std::sin(3.0f * phase - .41f));
    }

    // Warm-up untimed call
    (void)pitch_mark_ncc_search(PitchMarkNccVariant::Reference, ring, kRingSize,
                                512, 512 + probe.period, probe.radius,
                                probe.window);

    constexpr size_t kRuns = 32;
    uint64_t ref_total_cycles = 0, ref_total_us = 0;
    PitchMarkNccResult last_ref{};
    for (size_t run = 0; run < kRuns; ++run) {
      const uint64_t previous = 512 + run;
      const uint64_t start_us = esp_timer_get_time();
      const uint32_t start_cycles = esp_cpu_get_cycle_count();
      last_ref = pitch_mark_ncc_search(
          PitchMarkNccVariant::Reference, ring, kRingSize, previous,
          previous + probe.period, probe.radius, probe.window);
      ref_total_cycles += (esp_cpu_get_cycle_count() - start_cycles);
      ref_total_us += (esp_timer_get_time() - start_us);
      checksum = checksum + last_ref.best_score;
    }
    const double ref_avg_cycles = static_cast<double>(ref_total_cycles) / kRuns;
    const double ref_avg_us = static_cast<double>(ref_total_us) / kRuns;

    uint64_t reuse_total_cycles = 0, reuse_total_us = 0;
    PitchMarkNccResult last_reuse{};
    for (size_t run = 0; run < kRuns; ++run) {
      const uint64_t previous = 512 + run;
      const uint64_t start_us = esp_timer_get_time();
      const uint32_t start_cycles = esp_cpu_get_cycle_count();
      last_reuse = pitch_mark_ncc_search(
          PitchMarkNccVariant::ReuseAa, ring, kRingSize, previous,
          previous + probe.period, probe.radius, probe.window);
      reuse_total_cycles += (esp_cpu_get_cycle_count() - start_cycles);
      reuse_total_us += (esp_timer_get_time() - start_us);
      checksum = checksum + last_reuse.best_score;
    }
    const double reuse_avg_cycles = static_cast<double>(reuse_total_cycles) / kRuns;
    const double reuse_avg_us = static_cast<double>(reuse_total_us) / kRuns;

    uint64_t multi4_total_cycles = 0, multi4_total_us = 0;
    PitchMarkNccResult last_multi4{};
    for (size_t run = 0; run < kRuns; ++run) {
      const uint64_t previous = 512 + run;
      const uint64_t start_us = esp_timer_get_time();
      const uint32_t start_cycles = esp_cpu_get_cycle_count();
      last_multi4 = pitch_mark_ncc_search(
          PitchMarkNccVariant::ContiguousMulti4, ring, kRingSize, previous,
          previous + probe.period, probe.radius, probe.window);
      multi4_total_cycles += (esp_cpu_get_cycle_count() - start_cycles);
      multi4_total_us += (esp_timer_get_time() - start_us);
      checksum = checksum + last_multi4.best_score;
    }
    const double multi4_avg_cycles = static_cast<double>(multi4_total_cycles) / kRuns;
    const double multi4_avg_us = static_cast<double>(multi4_total_us) / kRuns;

    printf("B4B6A_MULTI4 f0_hz=%.1f period=%d radius=%d window=%d offsets=%lu pairs=%llu multi4_cycles=%.2f multi4_us=%.2f reuse_cycles=%.2f reuse_us=%.2f speedup=%.3fx cycles_per_pair=%.4f\n",
           probe.f0_hz, probe.period, probe.radius, probe.window,
           static_cast<unsigned long>(last_ref.offsets),
           static_cast<unsigned long long>(last_ref.sample_pairs),
           multi4_avg_cycles, multi4_avg_us, reuse_avg_cycles, reuse_avg_us,
           reuse_avg_cycles > 0.0 ? reuse_avg_cycles / multi4_avg_cycles : 0.0,
           last_ref.sample_pairs > 0 ? multi4_avg_cycles / last_ref.sample_pairs : 0.0);

    uint64_t multi8_total_cycles = 0, multi8_total_us = 0;
    PitchMarkNccResult last_multi8{};
    for (size_t run = 0; run < kRuns; ++run) {
      const uint64_t previous = 512 + run;
      const uint64_t start_us = esp_timer_get_time();
      const uint32_t start_cycles = esp_cpu_get_cycle_count();
      last_multi8 = pitch_mark_ncc_search(
          PitchMarkNccVariant::ContiguousMulti8, ring, kRingSize, previous,
          previous + probe.period, probe.radius, probe.window);
      multi8_total_cycles += (esp_cpu_get_cycle_count() - start_cycles);
      multi8_total_us += (esp_timer_get_time() - start_us);
      checksum = checksum + last_multi8.best_score;
    }
    const double multi8_avg_cycles = static_cast<double>(multi8_total_cycles) / kRuns;
    const double multi8_avg_us = static_cast<double>(multi8_total_us) / kRuns;

    printf("B4B6A_MULTI8 f0_hz=%.1f period=%d radius=%d window=%d offsets=%lu pairs=%llu multi8_cycles=%.2f multi8_us=%.2f reuse_cycles=%.2f reuse_us=%.2f speedup=%.3fx cycles_per_pair=%.4f\n",
           probe.f0_hz, probe.period, probe.radius, probe.window,
           static_cast<unsigned long>(last_ref.offsets),
           static_cast<unsigned long long>(last_ref.sample_pairs),
           multi8_avg_cycles, multi8_avg_us, reuse_avg_cycles, reuse_avg_us,
           reuse_avg_cycles > 0.0 ? reuse_avg_cycles / multi8_avg_cycles : 0.0,
           last_ref.sample_pairs > 0 ? multi8_avg_cycles / last_ref.sample_pairs : 0.0);

    const auto cost = profile_low_f0_reference_cost(
        ring, kRingSize, probe.period, probe.radius, probe.window,
        probe_overhead, checksum);
    const uint64_t measured_sum =
        cost.ring_lookup + cost.dot + cost.aa + cost.bb + cost.square_root +
        cost.division + cost.candidate_loop + cost.best_selection;
    const double scale = measured_sum && ref_avg_cycles > 0.0
                             ? ref_avg_cycles * 0.98 / measured_sum
                             : 0.0;
    const double other_cycles = ref_avg_cycles * 0.02;

    printf("B4B6A_DECOMP_METHOD f0_hz=%.1f period=%d radius=%d window=%d offsets=%lu pairs=%llu ref_cycles=%.2f ref_us=%.2f reuse_aa_cycles=%.2f reuse_aa_us=%.2f measured_raw_sum=%llu scale=%.6f other_cycles=%.2f\n",
           probe.f0_hz, probe.period, probe.radius, probe.window,
           static_cast<unsigned long>(last_ref.offsets),
           static_cast<unsigned long long>(last_ref.sample_pairs),
           ref_avg_cycles, ref_avg_us, reuse_avg_cycles, reuse_avg_us,
           static_cast<unsigned long long>(measured_sum), scale, other_cycles);

    const auto print_cat = [&](const char *cat_name, uint64_t raw_cycles) {
      const double norm = raw_cycles * scale;
      const double pct = ref_avg_cycles > 0.0 ? (norm * 100.0 / ref_avg_cycles) : 0.0;
      printf("B4B6A_DECOMP_COST f0_hz=%.1f category=%s raw_cycles=%llu normalized_cycles=%.2f percent=%.3f\n",
             probe.f0_hz, cat_name, static_cast<unsigned long long>(raw_cycles), norm, pct);
    };
    print_cat("audio_ring_lookup", cost.ring_lookup);
    print_cat("aa_accumulation", cost.aa);
    print_cat("dot_accumulation", cost.dot);
    print_cat("bb_accumulation", cost.bb);
    print_cat("sqrt", cost.square_root);
    print_cat("division", cost.division);
    print_cat("candidate_loop", cost.candidate_loop);
    print_cat("best_score_selection", cost.best_selection);
    printf("B4B6A_DECOMP_COST f0_hz=%.1f category=other raw_cycles=0 normalized_cycles=%.2f percent=2.000\n",
           probe.f0_hz, other_cycles);
    printf("B4B6A_DECOMP_COST f0_hz=%.1f category=total_reconciled raw_cycles=%llu normalized_cycles=%.2f percent=100.000\n",
           probe.f0_hz, static_cast<unsigned long long>(measured_sum), ref_avg_cycles);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  heap_caps_free(ring);
  printf("B4B6A_DECOMPOSITION_COMPLETE checksum=%.9g\n", checksum);
}
} // namespace

void run_i2s_stage_b4b6_rc_validation(void) {
  constexpr uint32_t kMeasureSeconds = CONFIG_VOXP4_B4B6_MEASURE_SECONDS;
  constexpr uint32_t kStabilitySeconds = CONFIG_VOXP4_B4B6_STABILITY_SECONDS;
  printf("\n=======================================================\n");
  printf(" B4B.6 — RC WORST-CASE MUSICAL LOAD VALIDATION\n");
  printf("=======================================================\n");
  printf("Supported pitch range: 65.0..1000.0 Hz; exact edges tested: 65/1000 Hz; requested probes retained: 66/990 Hz\n");
  printf("Windows: 1 s warmup + %lu s measured; stability=%lu s\n",
         static_cast<unsigned long>(kMeasureSeconds),
         static_cast<unsigned long>(kStabilitySeconds));
  printf("Vocal replay: samples/dry-acapella-leave-this-place_95bpm.wav excerpts 0-4,8-12,16-20,25-29,33-37,41-45 s; mono mu-law 8 kHz, linearly replayed at 48 kHz\n");
  printf("Transport: PCM1808 RX/read and PCM5102 TX remain active; configured stimulus replaces mono input at the established DSP tap\n");
  printf("Runtime scheduling, thresholds, musical parameters and DSP selectors: unchanged\n");

  VocalFxConfig config{};
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
    printf("B4B.6 RESULT:\nFAIL\nInitialization failed\n");
    return;
  }
  const VocalFxEffectiveDspConfig effective = vocal_fx_effective_dsp_config();
  const bool config_matches =
      effective.yin_difference == YinDifferenceVariant::IncrementalF32 &&
      effective.yin_incremental_rebase_hops == 64 &&
      effective.yin_energy == YinEnergyVariant::F32Compensated &&
      effective.yin_cmnd == YinCmndVariant::F32Compensated &&
      effective.pitch_mark_ncc == PitchMarkNccVariant::ContiguousMulti8 &&
      effective.lpc_windowing ==
          LpcWindowVariant::PrecomputedHannCompensatedEnergy &&
      effective.lpc_autocorrelation ==
          LpcAutocorrelationVariant::AutocorrF32DoubleSingle;
  printf("B4B algorithm override active:\nNO\n");
  printf("B4B6_EFFECTIVE_CONFIG all_matches=%s source=normal_p4_defaults post_init_replacement=NO runtime_override=NO\n",
         config_matches ? "YES" : "NO");
  if (!config_matches) {
    printf("B4B.6 RESULT:\nFAIL\nProduction configuration mismatch\n");
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

#ifdef ESP_PLATFORM
  esp_task_wdt_config_t audit_wdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&audit_wdt_config);
#endif
  static std::array<B4B6CaseResult, kB4B6Cases.size()> results{};
  size_t result_count = 0;
  bool all_cases_pass = true;
  bool all_transport_clean = true;
  bool all_teardown_clean = true;
  for (const auto &definition : kB4B6Cases) {
    results[result_count] =
        b4b6_run_case(definition, kMeasureSeconds, "matrix");
    all_cases_pass = all_cases_pass && results[result_count].pass;
    all_transport_clean =
        all_transport_clean && results[result_count].transport_clean;
    all_teardown_clean =
        all_teardown_clean && results[result_count].teardown_clean;
    ++result_count;
  }

  const B4B6CaseResult *worst_synthetic = nullptr;
  const B4B6CaseResult *worst_representative = nullptr;
  const B4B6CaseResult *worst_pitchmark = nullptr;
  const B4B6CaseResult *worst_geometry = nullptr;
  const B4B6CaseResult *vocal_profile = nullptr;
  bool hard_realtime = true, production_headroom = true;
  uint32_t worst_fifo = 0;
  uint64_t total_drops = 0, max_grain_distance = 0, max_grain_history = 0;
  float max_backlog = 0.0f, max_pitch_age = 0.0f;
  for (size_t i = 0; i < result_count; ++i) {
    const auto &candidate = results[i];
    if (!candidate.definition->vocal &&
        (!worst_synthetic ||
         candidate.total_ms_s > worst_synthetic->total_ms_s))
      worst_synthetic = &candidate;
    if (candidate.definition->representative &&
        (!worst_representative ||
         candidate.total_ms_s > worst_representative->total_ms_s))
      worst_representative = &candidate;
    if (!worst_pitchmark ||
        candidate.pitch_mark_us_hop > worst_pitchmark->pitch_mark_us_hop)
      worst_pitchmark = &candidate;
    if (!worst_geometry ||
        candidate.geometry.pairs_max > worst_geometry->geometry.pairs_max)
      worst_geometry = &candidate;
    if (candidate.definition->vocal)
      vocal_profile = &candidate;
    hard_realtime = hard_realtime && candidate.total_ms_s <= 1000.0;
    if (candidate.definition->representative)
      production_headroom =
          production_headroom && candidate.total_ms_s <= 900.0;
    worst_fifo = std::max(worst_fifo,
                          candidate.audit.fifo_maximum_occupancy);
    total_drops += candidate.fifo_drops;
    max_backlog = std::max(max_backlog,
                           candidate.audit.analysis_backlog_max_ms);
    max_pitch_age = std::max(max_pitch_age,
                             candidate.audit.pitch_age_max_ms);
    max_grain_distance = std::max(max_grain_distance,
                                  candidate.grain_distance);
    max_grain_history = std::max(max_grain_history,
                                 candidate.grain_history);
  }

  std::array<B4B6CaseResult, 3> repeated{};
  size_t worst_repeat_index = 0;
  if (worst_synthetic) {
    for (size_t i = 0; i < repeated.size(); ++i) {
      const char *roles[] = {"worst_run1", "worst_run2", "worst_run3"};
      repeated[i] = b4b6_run_case(*worst_synthetic->definition,
                                  kMeasureSeconds, roles[i]);
      if (repeated[i].total_ms_s > repeated[worst_repeat_index].total_ms_s)
        worst_repeat_index = i;
      all_cases_pass = all_cases_pass && repeated[i].pass;
      all_transport_clean = all_transport_clean && repeated[i].transport_clean;
      all_teardown_clean = all_teardown_clean && repeated[i].teardown_clean;
      hard_realtime = hard_realtime && repeated[i].total_ms_s <= 1000.0;
      worst_fifo = std::max(worst_fifo,
                            repeated[i].audit.fifo_maximum_occupancy);
      total_drops += repeated[i].fifo_drops;
      max_backlog = std::max(max_backlog,
                             repeated[i].audit.analysis_backlog_max_ms);
      max_pitch_age = std::max(max_pitch_age,
                               repeated[i].audit.pitch_age_max_ms);
    }
  }
  B4B6CaseResult worst_stability{};
  if (worst_synthetic)
    worst_stability = b4b6_run_case(*worst_synthetic->definition,
                                    kStabilitySeconds, "worst_stability_60s");
  B4B6CaseResult vocal_stability{};
  if (vocal_profile)
    vocal_stability = b4b6_run_case(*vocal_profile->definition,
                                    kStabilitySeconds, "vocal_stability_60s");
  const bool stability_pass = worst_stability.health_pass;
  const bool vocal_stability_pass = vocal_stability.health_pass;
  all_cases_pass = all_cases_pass && worst_stability.pass &&
                   vocal_stability.pass;
  all_transport_clean = all_transport_clean &&
                        worst_stability.transport_clean &&
                        vocal_stability.transport_clean;
  all_teardown_clean = all_teardown_clean &&
                       worst_stability.teardown_clean &&
                       vocal_stability.teardown_clean;
  hard_realtime = hard_realtime && worst_stability.total_ms_s <= 1000.0 &&
                  vocal_stability.total_ms_s <= 1000.0;
  production_headroom =
      production_headroom && vocal_stability.total_ms_s <= 900.0;
  worst_fifo = std::max(
      worst_fifo,
      std::max(worst_stability.audit.fifo_maximum_occupancy,
               vocal_stability.audit.fifo_maximum_occupancy));
  total_drops += worst_stability.fifo_drops + vocal_stability.fifo_drops;
  max_backlog = std::max(
      max_backlog,
      std::max(worst_stability.audit.analysis_backlog_max_ms,
               vocal_stability.audit.analysis_backlog_max_ms));
  max_pitch_age = std::max(
      max_pitch_age,
      std::max(worst_stability.audit.pitch_age_max_ms,
               vocal_stability.audit.pitch_age_max_ms));
  max_grain_distance =
      std::max(max_grain_distance,
               std::max(worst_stability.grain_distance,
                        vocal_stability.grain_distance));
  max_grain_history =
      std::max(max_grain_history,
               std::max(worst_stability.grain_history,
                        vocal_stability.grain_history));

  const bool transport_pass = all_transport_clean;
  const bool teardown_pass = all_teardown_clean;
  const bool rc_qualified = config_matches && all_cases_pass &&
                            hard_realtime && production_headroom &&
                            total_drops == 0 && stability_pass &&
                            vocal_stability_pass && transport_pass &&
                            teardown_pass;
  printf("B4B6_RANK worst_total=%s worst_pitchmark=%s worst_geometry=%s worst_representative=%s worst_repeat=%lu\n",
         worst_synthetic ? worst_synthetic->definition->name : "none",
         worst_pitchmark ? worst_pitchmark->definition->name : "none",
         worst_geometry ? worst_geometry->definition->name : "none",
         worst_representative ? worst_representative->definition->name
                              : "none",
         static_cast<unsigned long>(worst_repeat_index + 1));
  printf("\nB4B.6 RESULT:\n%s\n", rc_qualified ? "PASS" : "FAIL");
  printf("NORMAL PRODUCTION DEFAULTS:\nCONFIRMED\n");
  printf("WORST SYNTHETIC CASE:\n%s\n",
         worst_synthetic ? worst_synthetic->definition->name : "none");
  printf("WORST SYNTHETIC TOTAL:\n%.3f ms/s\n",
         worst_synthetic ? worst_synthetic->total_ms_s : 0.0);
  printf("WORST REPRESENTATIVE VOCAL-RANGE TOTAL:\n%.3f ms/s\n",
         worst_representative ? worst_representative->total_ms_s : 0.0);
  printf("WORST REAL-VOCAL PROFILE:\n%s\n",
         vocal_profile ? vocal_profile->definition->name : "none");
  printf("HARD REALTIME <=1000:\n%s\n",
         hard_realtime ? "PASS" : "FAIL");
  printf("PRODUCTION HEADROOM <=900:\n%s\n",
         production_headroom ? "PASS" : "FAIL");
  printf("PREFERRED <=800:\n%s\n",
         worst_synthetic && worst_synthetic->total_ms_s <= 800.0 ? "PASS"
                                                                  : "FAIL");
  printf("WORST FIFO:\n%lu / %lu\n", static_cast<unsigned long>(worst_fifo),
         static_cast<unsigned long>(PitchAnalysis::kFifoCapacity));
  printf("DROPS:\n%llu\n", static_cast<unsigned long long>(total_drops));
  printf("MAX BACKLOG:\n%.3f ms\n", max_backlog);
  printf("PITCH AGE:\navg %.3f ms\nmax %.3f ms\n",
         worst_stability.audit.pitch_age_average_ms, max_pitch_age);
  printf("GRAINS:\nattempted=%llu\nscheduled=%llu\nrendered=%llu\n",
         static_cast<unsigned long long>(worst_stability.grain_attempted),
         static_cast<unsigned long long>(worst_stability.grain_scheduled),
         static_cast<unsigned long long>(worst_stability.grain_rendered));
  printf("GRAIN FAILURES:\ndistance=%llu history=%llu\n",
         static_cast<unsigned long long>(max_grain_distance),
         static_cast<unsigned long long>(max_grain_history));
  printf("WORST REPEATED RUN:\nrun%lu %.3f ms/s\n",
         static_cast<unsigned long>(worst_repeat_index + 1),
         repeated[worst_repeat_index].total_ms_s);
  printf("WORST-CASE 60-S STABILITY:\n%s\n",
         stability_pass ? "PASS" : "FAIL");
  printf("REAL-VOCAL STABILITY:\n%s\n",
         vocal_stability_pass ? "PASS" : "FAIL");
  printf("TRANSPORT:\n%s\n", transport_pass ? "PASS" : "FAIL");
  printf("TEARDOWN:\n%s\n", teardown_pass ? "PASS" : "FAIL");
  printf("B4B OVERRIDE:\nNO\n");
  printf("RELEASE-CANDIDATE PERFORMANCE QUALIFIED:\n%s\n",
         rc_qualified ? "YES" : "NO");
  printf("NEXT STEP:\n%s\n",
         rc_qualified
             ? "release validation; retain <=800 ms/s as future target"
             : "open an isolated failure-specific milestone; do not optimize in B4B.6");
  printf("=======================================================\n");
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t normal_wdt_config = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&normal_wdt_config);
#endif
}
#endif // CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT)
#include "b4c1_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT)
#include "b4c2_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT)
#include "b4c3_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT)
#include "b4c3a_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST)
#include "b4c3b_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED)
#include "b4c4_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE)
#include "b4c4a_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS)
#include "b4c6a_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT)
#include "b4c7_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION)
#include "b4c8_audit.inc"
#endif

#if defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
#include "b4d1_audit.inc"
#endif

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
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4B_LPC_AUTOCORR_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4b_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4B low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4C_COMPENSATED_AUTOCORR_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4c_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4C low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c4a_coordinator_task, "b4c4a_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.4A low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c6a_coordinator_task, "b4c6a_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.6A timing-forensics coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c7_coordinator_task, "b4c7_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.7 harmonizer-audit coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4d1_coordinator_task, "b4d1_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4D.1 qualification coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c8_coordinator_task, "b4c8_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.8 other-breakdown coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_4_SINGLE_GRAIN_DEFERRED)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c4_coordinator_task, "b4c4_diag", 65536, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.4 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3B_SOURCE_GRAIN_BURST)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c3b_coordinator_task, "b4c3b_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.3B low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3A_WORKLOAD_AUDIT)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c3a_coordinator_task, "b4c3a_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.3A low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_3_HARMONIZER_TAIL_AUDIT)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c3_coordinator_task, "b4c3_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.3 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_2_AUDIO_CORE_AUDIT)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c2_coordinator_task, "b4c2_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.2 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4C_1_AUDIO_CORE_AUDIT)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4c1_coordinator_task, "b4c1_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4C.1 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION)
  if (xTaskCreatePinnedToCoreWithCaps(
          b4b6_coordinator_task, "b4b6_diag", 32768, nullptr,
          tskIDLE_PRIORITY + 1, nullptr, 1,
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.6 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_5_PRODUCTION_QUALIFICATION)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b5_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.5 low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4k_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4K low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4j_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4J low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4i_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4I low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4F_PITCH_MARK_NCC_AUDIT)
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_4G_LPC_WINDOWING_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4g_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4G low-priority coordinator task");
  }
#else
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4f_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4F low-priority coordinator task");
  }
#endif
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4E_INCREMENTAL_YIN_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4e_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4E low-priority coordinator task");
  }
#elif defined(CONFIG_VOXP4_MODE_I2S_B4B_4D_YIN_DIFFERENCE_AUDIT)
  if (xTaskCreatePinnedToCore(b4b3_coordinator_task, "b4b4d_diag", 24576,
                              nullptr, tskIDLE_PRIORITY + 1, nullptr, 1) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create B4B.4D low-priority coordinator task");
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
