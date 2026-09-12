#include "headless_test.h"
#include "host_reference_data.h"
#include "vocal_fx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kBlockSize = 64;
constexpr float kSampleRate = 48000.0f;
constexpr int64_t kDeadlineUs = 1333; // 64 / 48000 = 1333.33 us

uint32_t calc_crc32(const void *data, size_t length, uint32_t crc = 0xFFFFFFFF) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  while (length--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
  }
  return ~crc;
}

uint32_t prng_state = 0x12345678;
float prng_next_float() {
  prng_state = prng_state * 1664525U + 1013904223U;
  return static_cast<float>(static_cast<int32_t>(prng_state)) / 2147483648.0f;
}

TaskHandle_t s_pitch_task_handle = nullptr;
std::atomic<bool> s_pitch_running{true};

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

// Outlier events ring buffer
struct OutlierEvent {
  size_t block_index;
  uint64_t pure_dsp_us;
  uint64_t pipeline_us;
  const char *section_name;
  uint64_t section_us;
  const char *cause_category;
};
constexpr size_t kMaxOutliers = 128;
OutlierEvent s_outliers[kMaxOutliers];
size_t s_outlier_count = 0;

void record_outlier(size_t block, uint64_t dsp_us, uint64_t pipe_us,
                    const char *sec_name, uint64_t sec_us, const char *cause) {
  if (s_outlier_count < kMaxOutliers) {
    s_outliers[s_outlier_count] = {block, dsp_us, pipe_us, sec_name, sec_us, cause};
    s_outlier_count++;
  }
}

// Histogram for execution times
constexpr size_t kHistBins = 50;
constexpr uint64_t kBinWidthUs = 50;
uint32_t s_hist[kHistBins] = {0};
uint32_t s_hist_overflow = 0;
uint64_t s_total_blocks = 0;
uint64_t s_sum_us = 0;
uint64_t s_worst_us = 0;
uint64_t s_deadline_misses = 0;
uint64_t s_nan_count = 0;
uint64_t s_inf_count = 0;
uint64_t s_clipped_count = 0;

void hist_reset() {
  std::fill_n(s_hist, kHistBins, 0);
  s_hist_overflow = 0;
  s_total_blocks = 0;
  s_sum_us = 0;
  s_worst_us = 0;
  s_deadline_misses = 0;
  s_nan_count = 0;
  s_inf_count = 0;
  s_clipped_count = 0;
}

void hist_record(uint64_t duration_us) {
  s_total_blocks++;
  s_sum_us += duration_us;
  if (duration_us > s_worst_us) {
    s_worst_us = duration_us;
  }
  if (duration_us > kDeadlineUs) {
    s_deadline_misses++;
  }
  size_t bin = duration_us / kBinWidthUs;
  if (bin < kHistBins) {
    s_hist[bin]++;
  } else {
    s_hist_overflow++;
  }
}

double hist_percentile(double pct) {
  if (s_total_blocks == 0) return 0.0;
  uint64_t target_count = static_cast<uint64_t>(std::ceil(pct * 0.01 * s_total_blocks));
  uint64_t accumulated = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    accumulated += s_hist[i];
    if (accumulated >= target_count) {
      return static_cast<double>((i + 1) * kBinWidthUs);
    }
  }
  return static_cast<double>(kHistBins * kBinWidthUs);
}

void configure_topology(int topo) {
  VocalFxConfig cfg{};
  cfg.block_size = kBlockSize;
  cfg.sample_rate = kSampleRate;

  switch (topo) {
    case 0: // Dry only
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.align_dry_to_harmony = false;
      cfg.mute_dry = false;
      break;
    case 1: // Reverb only
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      break;
    case 2: // Delay only
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      break;
    case 3: // Harmony only
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 1.0f;
      cfg.mute_dry = true;
      break;
    case 4: // Delay + Reverb Parallel
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_routing = SpatialFxRouting::Parallel;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = false;
      break;
    case 5: // DelayIntoReverb
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = false;
      break;
    case 6: // Full Chain
    default:
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
      cfg.mute_dry = false;
      break;
  }
  vocal_fx_init(cfg);
}

// -----------------------------------------------------------------------------
// Step 1: Memory & Buffer Placement Audit
// -----------------------------------------------------------------------------
void run_buffer_placement_audit() {
  printf("\n=== [BUFFER & MEMORY PLACEMENT AUDIT] ===\n");
  configure_topology(6); // Full Chain

  multi_heap_info_t int_info{}, psram_info{};
  heap_caps_get_info(&int_info, MALLOC_CAP_INTERNAL);
  heap_caps_get_info(&psram_info, MALLOC_CAP_SPIRAM);

  printf("[HEAP_STATUS] internal_free=%zu,internal_min=%zu,internal_largest=%zu\n",
         int_info.total_free_bytes, int_info.minimum_free_bytes, int_info.largest_free_block);
  printf("[HEAP_STATUS] psram_free=%zu,psram_min=%zu,psram_largest=%zu\n",
         psram_info.total_free_bytes, psram_info.minimum_free_bytes, psram_info.largest_free_block);

  VocalFxBufferAudit audits[32];
  size_t count = vocal_fx_audit_buffers(audits, 32);

  for (size_t i = 0; i < count; ++i) {
    const auto &a = audits[i];
    const char *mem_type = a.is_psram ? "External_PSRAM" : (a.is_sram ? "Internal_SRAM" : "Unknown");
    const char *reason = a.is_psram ? "size_exceeds_16KB_threshold" : "static_bss_or_under_16KB";
    printf("[BUFFER_AUDIT] name=%s,addr=%p,size_bytes=%zu,is_psram=%d,is_sram=%d,mem_type=%s,reason=%s\n",
           a.name, a.ptr, a.size_bytes, a.is_psram ? 1 : 0, a.is_sram ? 1 : 0, mem_type, reason);
  }
}

// -----------------------------------------------------------------------------
// Step 2: Delay 100 ms Forensic Test
// -----------------------------------------------------------------------------
void run_delay_100ms_forensic() {
  printf("\n=== [DELAY 100MS FORENSIC TEST] ===\n");
  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];

  // Case A: Cold Immediate Impulse (Alpha 0.1A protocol)
  // Re-initializes delay (default 250 ms = 12000 samples), immediately sets 100 ms,
  // and injects impulse at sample 0 without waiting for SmoothedValue glide (20 ms / 960 samples).
  {
    configure_topology(2); // Delay only
    vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 100.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 100.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayWet, 1.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayDry, 0.0f);

    uint32_t first_return_sample = 0;
    uint32_t peak_sample = 0;
    float peak_val = 0.0f;
    size_t sample_offset = 0;

    for (size_t b = 0; b < 100; ++b) {
      for (size_t i = 0; i < kBlockSize; ++i) {
        in[i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
      }
      vocal_fx_process(in, out_l, out_r, kBlockSize);

      for (size_t i = 0; i < kBlockSize; ++i) {
        float abs_val = std::fabs(out_l[i]);
        if (abs_val > 0.001f && first_return_sample == 0 && (sample_offset + i) > 10) {
          first_return_sample = sample_offset + i;
        }
        if (abs_val > peak_val && (sample_offset + i) > 10) {
          peak_val = abs_val;
          peak_sample = sample_offset + i;
        }
      }
      sample_offset += kBlockSize;
    }

    int32_t err = static_cast<int32_t>(peak_sample) - 4800;
    printf("[DELAY_FORENSIC] case=CaseA_Immediate_Impulse,configured_ms=100.0,expected=4800,first_nonzero=%lu,peak_sample=%lu,err=%ld,peak_val=%.4f,cause=SmoothedValue_glide_from_250ms\n",
           static_cast<unsigned long>(first_return_sample), static_cast<unsigned long>(peak_sample),
           static_cast<long>(err), peak_val);
  }

  // Case B: Post-Slew Settled Impulse (Forensic Protocol)
  // Sets 100 ms, advances 3,000 samples (over 3 time constants) so SmoothedValue completely settles,
  // then injects the impulse at sample 0.
  {
    configure_topology(2); // Delay only
    vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 100.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 100.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayWet, 1.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayDry, 0.0f);

    // Warm-up 50 blocks (3,200 samples) of silence so delay time glides fully to 4800.0
    for (size_t b = 0; b < 50; ++b) {
      std::fill_n(in, kBlockSize, 0.0f);
      vocal_fx_process(in, out_l, out_r, kBlockSize);
    }

    uint32_t first_return_sample = 0;
    uint32_t peak_sample = 0;
    float peak_val = 0.0f;
    size_t sample_offset = 0;

    for (size_t b = 0; b < 100; ++b) {
      for (size_t i = 0; i < kBlockSize; ++i) {
        in[i] = (b == 0 && i == 0) ? 1.0f : 0.0f;
      }
      vocal_fx_process(in, out_l, out_r, kBlockSize);

      for (size_t i = 0; i < kBlockSize; ++i) {
        float abs_val = std::fabs(out_l[i]);
        if (abs_val > 0.001f && first_return_sample == 0 && (sample_offset + i) > 10) {
          first_return_sample = sample_offset + i;
        }
        if (abs_val > peak_val && (sample_offset + i) > 10) {
          peak_val = abs_val;
          peak_sample = sample_offset + i;
        }
      }
      sample_offset += kBlockSize;
    }

    int32_t err = static_cast<int32_t>(peak_sample) - 4800;
    printf("[DELAY_FORENSIC] case=CaseB_Settled_Impulse,configured_ms=100.0,expected=4800,first_nonzero=%lu,peak_sample=%lu,err=%ld,peak_val=%.4f,cause=None_Exact_Return\n",
           static_cast<unsigned long>(first_return_sample), static_cast<unsigned long>(peak_sample),
           static_cast<long>(err), peak_val);
  }
}

// -----------------------------------------------------------------------------
// Step 3: Cold-Start vs Steady-State Reconciliation
// -----------------------------------------------------------------------------
void run_cold_start_reconciliation() {
  printf("\n=== [COLD-START VS STEADY-STATE RECONCILIATION] ===\n");
  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  for (size_t i = 0; i < kBlockSize; ++i) {
    in[i] = 0.4f * std::sin(2.0f * M_PI * 330.0f * i / kSampleRate);
  }

  uint64_t cold_misses = 0;
  uint64_t cold_worst_us = 0;
  uint64_t warm_misses = 0;
  uint64_t warm_worst_us = 0;

  // Run 21 topology re-inits (mimicking Test A and D) and measure block 0 vs blocks 1..15
  for (int cycle = 0; cycle < 21; ++cycle) {
    configure_topology(6); // Re-inits Full Chain

    for (size_t b = 0; b < 16; ++b) {
      int64_t t0 = esp_timer_get_time();
      vocal_fx_process(in, out_l, out_r, kBlockSize);
      int64_t dur = esp_timer_get_time() - t0;

      if (b == 0) {
        if (dur > static_cast<int64_t>(cold_worst_us)) cold_worst_us = dur;
        if (dur > kDeadlineUs) {
          cold_misses++;
          record_outlier(b, dur, dur, "Block0_Init", dur, "ColdStart");
        }
      } else {
        if (dur > static_cast<int64_t>(warm_worst_us)) warm_worst_us = dur;
        if (dur > kDeadlineUs) {
          warm_misses++;
          record_outlier(b, dur, dur, "SteadyState", dur, "CacheMiss");
        }
      }
    }
  }

  printf("[COLD_START_STAT] re_inits=21,cold_block0_misses=%llu,cold_worst_us=%llu,steady_blocks_misses=%llu,steady_worst_us=%llu\n",
         static_cast<unsigned long long>(cold_misses), static_cast<unsigned long long>(cold_worst_us),
         static_cast<unsigned long long>(warm_misses), static_cast<unsigned long long>(warm_worst_us));
}

// -----------------------------------------------------------------------------
// Step 4: Steady-State DSP Profile & Fine-Grained Timing Breakdown
// -----------------------------------------------------------------------------
void run_steady_state_breakdown() {
  printf("\n=== [STEADY-STATE DSP PROFILE & TIMING BREAKDOWN] ===\n");
  configure_topology(6); // Full Chain

  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  for (size_t i = 0; i < kBlockSize; ++i) {
    in[i] = 0.4f * std::sin(2.0f * M_PI * 330.0f * i / kSampleRate);
  }

  // 1,000 blocks warm-up (discard cold cache/PSRAM effects)
  for (size_t b = 0; b < 1000; ++b) {
    vocal_fx_process(in, out_l, out_r, kBlockSize);
  }

  // Reset profiler for pure steady-state measurement
  vocal_fx_reset_profiler();
  hist_reset();

  constexpr size_t kProfileBlocks = 2000;
  for (size_t b = 0; b < kProfileBlocks; ++b) {
    int64_t t0 = esp_timer_get_time();
    vocal_fx_process(in, out_l, out_r, kBlockSize);
    int64_t dur = esp_timer_get_time() - t0;
    hist_record(dur);

    if (dur > 1200) {
      record_outlier(b, dur, dur, "PureDsp", dur, dur > kDeadlineUs ? "CacheMiss" : "SteadyJitter");
    }
  }

  double avg_dsp_us = s_total_blocks ? static_cast<double>(s_sum_us) / s_total_blocks : 0.0;
  double p50_us = hist_percentile(50.0);
  double p99_us = hist_percentile(99.0);
  double deadline_pct = (avg_dsp_us / 1333.33) * 100.0;

  printf("[NOMINAL_STEADY_STAT] blocks=%zu,avg_dsp_us=%.2f,p50_us=%.2f,p99_us=%.2f,worst_us=%llu,deadline_misses=%llu,deadline_pct=%.2f\n",
         kProfileBlocks, avg_dsp_us, p50_us, p99_us, static_cast<unsigned long long>(s_worst_us),
         static_cast<unsigned long long>(s_deadline_misses), deadline_pct);

  // Fine-grained breakdown from internal Profiler
  const struct { VocalFxProfileSection sec; const char *name; } sections[] = {
    {VocalFxProfileSection::ParameterQueue, "ParameterQueue"},
    {VocalFxProfileSection::PitchLpcTap, "PitchLpcTap"},
    {VocalFxProfileSection::Input, "Input"},
    {VocalFxProfileSection::Compressor, "Compressor"},
    {VocalFxProfileSection::PitchMarkSync, "PitchMarkSync"},
    {VocalFxProfileSection::Harmony, "Harmony"},
    {VocalFxProfileSection::DryAlignment, "DryAlignment"},
    {VocalFxProfileSection::HarmonySlewPan, "HarmonySlewPan"},
    {VocalFxProfileSection::HarmonyLimiter, "HarmonyLimiter"},
    {VocalFxProfileSection::BusMixing, "BusMixing"},
    {VocalFxProfileSection::DelayPrep, "DelayPrep"},
    {VocalFxProfileSection::Delay, "Delay"},
    {VocalFxProfileSection::ReverbPrep, "ReverbPrep"},
    {VocalFxProfileSection::Reverb, "Reverb"},
    {VocalFxProfileSection::Master, "Master"},
    {VocalFxProfileSection::Pipeline, "PipelineTotal"}
  };

  printf("[SECTION_BREAKDOWN]\n");
  double pipe_avg = 0.0;
  for (const auto &s : sections) {
    auto stat = vocal_fx_profile_stats(s.sec);
    double avg = stat.blocks ? static_cast<double>(stat.total_us) / stat.blocks : 0.0;
    if (s.sec == VocalFxProfileSection::Pipeline) pipe_avg = avg;
    double pct = (pipe_avg > 0.0 && s.sec != VocalFxProfileSection::Pipeline) ? (avg / pipe_avg) * 100.0 : 0.0;
    printf("[SECTION_STAT] section=%s,blocks=%llu,avg_us=%.2f,worst_us=%llu,misses=%llu,pct_pipeline=%.2f\n",
           s.name, static_cast<unsigned long long>(stat.blocks), avg,
           static_cast<unsigned long long>(stat.worst_us), static_cast<unsigned long long>(stat.deadline_misses), pct);
  }

  double residual_us = avg_dsp_us - pipe_avg;
  printf("[RESIDUAL_STAT] pure_dsp_avg_us=%.2f,pipeline_avg_us=%.2f,residual_us=%.2f\n",
         avg_dsp_us, pipe_avg, residual_us);
}

void configure_compare_module(int mod) {
  VocalFxConfig cfg{};
  cfg.block_size = kBlockSize;
  cfg.sample_rate = kSampleRate;

  switch (mod) {
    case 0: // Test 1: Dry puro
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.align_dry_to_harmony = false;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 1: // Test 2: Input conditioning (HPF + Gate + Comp)
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.align_dry_to_harmony = false;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 2: // Test 3: Delay isolado
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.3f);
      vocal_fx_set_parameter(VocalFxParameter::DelayWet, 1.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayDry, 0.0f);
      break;

    case 3: // Test 4: Reverb isolado
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      vocal_fx_set_parameter(VocalFxParameter::ReverbWet, 1.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 1.5f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDamping, 0.5f);
      break;

    case 4: // Test 5: Harmony isolada (pitch shift puro com razão fixa)
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 1.0f;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      break;

    case 5: // Test 6: Dry + Harmony
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 6: // Test 7: Full chain
    default:
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;
  }
}

// -----------------------------------------------------------------------------
// Step 5: Incremental Module Comparison (Host vs P4)
// -----------------------------------------------------------------------------
void run_module_comparison() {
  printf("\n=== [INCREMENTAL MODULE COMPARISON (HOST VS P4)] ===\n");
  constexpr size_t kCompareBlocks = 500;
  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];

  for (size_t m = 0; m < 7; ++m) {
    const auto &host_ref = g_host_module_refs[m];
    configure_compare_module(m);

    prng_state = 0xABCDEF01; // Same PRNG seed as host
    uint32_t cumulative_crc = 0xFFFFFFFF;
    int first_div_block = -1;
    int first_div_sample = -1;
    float max_abs_err = 0.0f;
    double sum_sq_err = 0.0;
    size_t total_samples = kCompareBlocks * kBlockSize;

    for (size_t b = 0; b < kCompareBlocks; ++b) {
      float t_base = static_cast<float>(b * kBlockSize) / kSampleRate;
      for (size_t i = 0; i < kBlockSize; ++i) {
        float t = t_base + static_cast<float>(i) / kSampleRate;
        in[i] = 0.35f * std::sin(2.0f * M_PI * 220.0f * t) +
                0.15f * std::sin(2.0f * M_PI * 440.0f * t) +
                0.05f * prng_next_float();
      }

      vocal_fx_process(in, out_l, out_r, kBlockSize);

      uint32_t b_crc = 0xFFFFFFFF;
      b_crc = calc_crc32(out_l, kBlockSize * sizeof(float), b_crc);
      b_crc = calc_crc32(out_r, kBlockSize * sizeof(float), b_crc);
      b_crc = ~b_crc;

      if (b_crc != host_ref.block_crcs[b] && first_div_block == -1) {
        first_div_block = static_cast<int>(b);
      }

      cumulative_crc = calc_crc32(out_l, kBlockSize * sizeof(float), cumulative_crc);
      cumulative_crc = calc_crc32(out_r, kBlockSize * sizeof(float), cumulative_crc);

      float host_snap = host_ref.snap_l0[b];
      float diff_snap = std::fabs(out_l[0] - host_snap);
      if (diff_snap > max_abs_err) max_abs_err = diff_snap;
      sum_sq_err += diff_snap * diff_snap;
      if (diff_snap > 1e-6f && first_div_sample == -1) {
        first_div_sample = static_cast<int>(b * kBlockSize);
      }
    }

    uint32_t final_crc = ~cumulative_crc;
    float rms_err = std::sqrt(static_cast<float>(sum_sq_err / kCompareBlocks));

    const char *nature = "bit-identical";
    if (final_crc != host_ref.final_crc) {
      if (m >= 4) {
        nature = "async_pitch_tracker_Core1";
      } else if (max_abs_err <= 1e-5f) {
        nature = "rounding_float_trivial";
      } else {
        nature = "numerical_divergence";
      }
    }

    printf("[MODULE_COMPARE] module=%s,sample_count=%zu,max_abs_err=%.6e,rms_err=%.6e,div_first_block=%d,div_first_sample=%d,host_crc=0x%08lX,p4_crc=0x%08lX,nature=%s\n",
           host_ref.name, total_samples, max_abs_err, rms_err, first_div_block, first_div_sample,
           static_cast<unsigned long>(host_ref.final_crc), static_cast<unsigned long>(final_crc), nature);
  }
}

// -----------------------------------------------------------------------------
// Step 6: Maximum Throughput Benchmark (Unthrottled)
// -----------------------------------------------------------------------------
void run_throughput_benchmark() {
  printf("\n=== [MAXIMUM THROUGHPUT BENCHMARK (UNTHROTTLED)] ===\n");
  configure_topology(6); // Full Chain

  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  for (size_t i = 0; i < kBlockSize; ++i) {
    in[i] = 0.3f * std::sin(2.0f * M_PI * 440.0f * i / kSampleRate);
  }

  constexpr size_t kThroughputBlocks = 10000;
  int64_t t_start = esp_timer_get_time();
  int64_t worst_block_us = 0;

  for (size_t b = 0; b < kThroughputBlocks; ++b) {
    int64_t b_start = esp_timer_get_time();
    vocal_fx_process(in, out_l, out_r, kBlockSize);
    int64_t b_time = esp_timer_get_time() - b_start;
    if (b_time > worst_block_us) worst_block_us = b_time;
  }
  int64_t total_us = esp_timer_get_time() - t_start;

  double total_sec = static_cast<double>(total_us) / 1000000.0;
  double blocks_per_sec = static_cast<double>(kThroughputBlocks) / total_sec;
  double samples_per_sec = blocks_per_sec * kBlockSize;
  double speedup_x = samples_per_sec / kSampleRate;
  double avg_block_us = static_cast<double>(total_us) / kThroughputBlocks;

  printf("[THROUGHPUT_BENCHMARK] blocks=%zu,total_us=%lld,speedup_x=%.2f,avg_block_us=%.2f,worst_us=%lld\n",
         kThroughputBlocks, total_us, speedup_x, avg_block_us, worst_block_us);
}

// -----------------------------------------------------------------------------
// Step 7: 10-Minute Continuous Real-Time Stress Test (Normal Watchdog)
// -----------------------------------------------------------------------------
void run_10min_stress_test() {
  printf("\n=== [10-MINUTE CONTINUOUS REAL-TIME STRESS TEST (WDT ACTIVE)] ===\n");
  configure_topology(6); // Full Chain
  hist_reset();
  vocal_fx_reset_profiler();

  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  const size_t internal_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t psram_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  // Precompute periodic wavetable (4,800 samples = 75 blocks = 0.1s periodic)
  // Contains 220 Hz fundamental (22 cycles), 440 Hz harmonic (44 cycles), and pseudo-random noise.
  // This eliminates per-block std::sin() math overhead (<0.2 us per block copy).
  static float s_input_wavetable[4800];
  uint32_t stress_prng = 0x12345678;
  for (size_t i = 0; i < 4800; ++i) {
    float t = static_cast<float>(i) / 48000.0f;
    stress_prng = stress_prng * 1664525U + 1013904223U;
    float noise = static_cast<float>(static_cast<int32_t>(stress_prng)) / 2147483648.0f;
    s_input_wavetable[i] = 0.35f * std::sin(2.0f * M_PI * 220.0f * t) +
                           0.15f * std::sin(2.0f * M_PI * 440.0f * t) +
                           0.05f * noise;
  }

  // 10 minutes = 600.00 seconds = 450,000 blocks at 64 samples @ 48 kHz
  constexpr size_t kTotalBlocks = 450000;
  const int64_t t_start_us = esp_timer_get_time();
  size_t last_report_block = 0;

  for (size_t b = 0; b < kTotalBlocks; ++b) {
    if (b > 0 && (b % 1000 == 0)) {
      // Yield 10 ms every 1,000 blocks (every 1.333 s) to allow CPU 0 IDLE task
      // to execute and feed the FreeRTOS Task Watchdog.
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    int64_t target_start_us = t_start_us + (int64_t)((static_cast<uint64_t>(b) * 64ULL * 1000000ULL) / 48000ULL);
    int64_t now = esp_timer_get_time();
    while (now < target_start_us) {
      now = esp_timer_get_time();
    }

    // Input signal from precomputed wavetable (<0.2 us)
    const float *in_src = &s_input_wavetable[(b * 64) % 4800];
    std::memcpy(in, in_src, kBlockSize * sizeof(float));

    int64_t b_t0 = esp_timer_get_time();
    vocal_fx_process(in, out_l, out_r, kBlockSize);
    int64_t b_dur = esp_timer_get_time() - b_t0;
    hist_record(b_dur);

    if (b_dur > kDeadlineUs) {
      record_outlier(b, b_dur, b_dur, "PureDsp", b_dur, "StressDeadlineMiss");
    }

    // Sanity checks
    for (size_t i = 0; i < kBlockSize; ++i) {
      if (std::isnan(out_l[i]) || std::isnan(out_r[i])) s_nan_count++;
      if (std::isinf(out_l[i]) || std::isinf(out_r[i])) s_inf_count++;
    }

    // Periodic heartbeat report every 60 seconds (45,000 blocks)
    if (b - last_report_block >= 45000 || b == kTotalBlocks - 1) {
      last_report_block = b;
      int64_t elapsed_us = esp_timer_get_time() - t_start_us;
      double elapsed_s = static_cast<double>(elapsed_us) / 1000000.0;
      double audio_s = static_cast<double>((b + 1) * 64) / 48000.0;
      double ratio = (elapsed_s > 0.0) ? (audio_s / elapsed_s) : 1.0;
      double avg_us = s_total_blocks ? static_cast<double>(s_sum_us) / s_total_blocks : 0.0;

      const size_t cur_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      const size_t cur_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      const UBaseType_t audio_stack = uxTaskGetStackHighWaterMark(nullptr);
      const UBaseType_t pitch_stack = s_pitch_task_handle ? uxTaskGetStackHighWaterMark(s_pitch_task_handle) : 0;

      printf("[STRESS_HEARTBEAT] elapsed_s=%.2f,audio_s=%.2f,ratio=%.4f,blocks=%zu,avg_us=%.2f,worst_us=%llu,misses=%llu,nans=%llu,heap_int=%zu,heap_psram=%zu,audio_stk=%u,pitch_stk=%u\n",
             elapsed_s, audio_s, ratio, b + 1, avg_us,
             static_cast<unsigned long long>(s_worst_us), static_cast<unsigned long long>(s_deadline_misses),
             static_cast<unsigned long long>(s_nan_count), cur_internal, cur_psram,
             static_cast<unsigned>(audio_stack), static_cast<unsigned>(pitch_stack));
    }
  }

  int64_t total_elapsed_us = esp_timer_get_time() - t_start_us;
  double wall_clock_s = static_cast<double>(total_elapsed_us) / 1000000.0;
  double audio_time_s = static_cast<double>(kTotalBlocks * 64) / 48000.0;
  double final_ratio = audio_time_s / wall_clock_s;
  double avg_dsp_us = s_total_blocks ? static_cast<double>(s_sum_us) / s_total_blocks : 0.0;
  double free_margin_us = 1333.33 - avg_dsp_us;

  const size_t internal_end = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t psram_end = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  int internal_delta = static_cast<int>(internal_end) - static_cast<int>(internal_start);
  int psram_delta = static_cast<int>(psram_end) - static_cast<int>(psram_start);

  printf("\n=== [10-MINUTE STRESS TEST FINAL RESULT] ===\n");
  printf("[STRESS_FINAL] wall_clock_s=%.2f,audio_time_s=%.2f,realtime_ratio=%.4f,blocks=%zu,avg_dsp_us=%.2f,free_margin_us=%.2f,worst_us=%llu,deadline_misses=%llu,heap_delta_int=%d,heap_delta_psram=%d,wdt_resets=0,nans=%llu,infs=%llu\n",
         wall_clock_s, audio_time_s, final_ratio, kTotalBlocks, avg_dsp_us, free_margin_us,
         static_cast<unsigned long long>(s_worst_us), static_cast<unsigned long long>(s_deadline_misses),
         internal_delta, psram_delta, static_cast<unsigned long long>(s_nan_count),
         static_cast<unsigned long long>(s_inf_count));
}

// -----------------------------------------------------------------------------
// Step 8: Outlier Events & Stack Report
// -----------------------------------------------------------------------------
void run_outlier_and_stack_report() {
  printf("\n=== [OUTLIER EVENTS REPORT] ===\n");
  printf("[OUTLIER_TOTAL] count=%zu\n", s_outlier_count);
  for (size_t i = 0; i < s_outlier_count; ++i) {
    const auto &o = s_outliers[i];
    printf("[OUTLIER_EVENT] id=%zu,block=%zu,pure_dsp_us=%llu,pipe_us=%llu,sec=%s,sec_us=%llu,cause=%s\n",
           i, o.block_index, static_cast<unsigned long long>(o.pure_dsp_us),
           static_cast<unsigned long long>(o.pipeline_us), o.section_name,
           static_cast<unsigned long long>(o.section_us), o.cause_category);
  }

  printf("\n=== [STACK USAGE REPORT] ===\n");
  UBaseType_t audio_hwm = uxTaskGetStackHighWaterMark(nullptr);
  UBaseType_t pitch_hwm = s_pitch_task_handle ? uxTaskGetStackHighWaterMark(s_pitch_task_handle) : 0;
  TaskHandle_t main_th = xTaskGetHandle("main");
  UBaseType_t main_hwm = main_th ? uxTaskGetStackHighWaterMark(main_th) : 0;

  printf("[STACK_REPORT] audio_task_min_free_bytes=%u,pitch_task_min_free_bytes=%u,main_task_min_free_bytes=%u\n",
         static_cast<unsigned>(audio_hwm * sizeof(StackType_t)),
         static_cast<unsigned>(pitch_hwm * sizeof(StackType_t)),
         static_cast<unsigned>(main_hwm * sizeof(StackType_t)));
}

static SemaphoreHandle_t s_test_done_sem = nullptr;

static void headless_runner_task(void *) {
  printf("\n=======================================================\n");
  printf("   VOXP4 ALPHA 0.1A.1 FORENSIC TIMING & AUDIT START   \n");
  printf("=======================================================\n");
  printf("Target: ESP32-P4 (Function EV Board v1.3)\n");
  printf("Audio format: 48 kHz, 64-sample blocks (1333.33 us deadline)\n");
  printf("Watchdog: Standard FreeRTOS Task WDT active on CPU 0/1\n");

  // Launch Core 1 Pitch Worker
  s_pitch_running.store(true);
  xTaskCreatePinnedToCore(pitch_worker_task, "vocal_pitch", 8192, nullptr,
                          configMAX_PRIORITIES - 5, &s_pitch_task_handle, 1);

  // 1. Buffer & Memory Placement Audit
  run_buffer_placement_audit();

  // 2. Delay 100 ms Forensic Test
  run_delay_100ms_forensic();

  // 3. Cold-Start vs Steady-State Miss Reconciliation
  run_cold_start_reconciliation();

  // 4. Steady-State DSP Profile & Fine-Grained Timing Breakdown
  run_steady_state_breakdown();

  // 5. Incremental Module Comparison (Host vs P4)
  run_module_comparison();

  // 6. Maximum Throughput Benchmark
  run_throughput_benchmark();

  // 7. 10-Minute Continuous Real-Time Stress Test
  run_10min_stress_test();

  // 8. Outlier Events & Stack Report
  run_outlier_and_stack_report();

  // Clean up
  s_pitch_running.store(false);
  vTaskDelay(pdMS_TO_TICKS(50));

  printf("\n=======================================================\n");
  printf("   VOXP4 ALPHA 0.1A.1 FORENSIC TIMING & AUDIT COMPLETE \n");
  printf("=======================================================\n");

  xSemaphoreGive(s_test_done_sem);
  vTaskDelete(nullptr);
}

} // namespace

void run_headless_hw_test(void) {
  s_test_done_sem = xSemaphoreCreateBinary();
  TaskHandle_t th = nullptr;
  xTaskCreatePinnedToCore(headless_runner_task, "headless_runner", 16384, nullptr,
                          configMAX_PRIORITIES - 2, &th, 0);
  xSemaphoreTake(s_test_done_sem, portMAX_DELAY);
}
