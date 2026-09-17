// B4C.4A - Deferred Harmonizer Standalone Realtime Qualification
// Included directly inside main/i2s_bringup.cpp
//
// Production path FROZEN:
//   PsolaGrainRenderMode::DeferredSlice
//   PsolaFirKernel::MultiFma
//   Shared ModelWarp cache, LPC formant preservation, OLA, gain, articulation
//
// Downstream FX (Compressor, Delay, Reverb) are DISABLED for Steps 1-5.
// Only Gate + HPF kept. Step 6 adds effects incrementally.
// Step 0A verifies transport/timeline integrity before DSP measurement.

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE)

#include <cstring>
#include <cmath>
#include <array>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdio>
#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#ifdef ESP_PLATFORM
#include "esp_task_wdt.h"
#endif

namespace {

using vocal_fx_platform::B4B6StimulusKind;
using vocal_fx_platform::TimingPercentiles;

// ---------------------------------------------------------------------------
// AudioTimelineStats - Step 0A block-rate integrity
// ---------------------------------------------------------------------------
struct AudioTimelineStats {
  uint64_t actual_rx_blocks     = 0;
  uint64_t actual_tx_blocks     = 0;
  uint64_t actual_rx_frames     = 0;
  uint64_t rx_sequence_gaps     = 0;
  uint64_t tx_sequence_gaps     = 0;
  uint64_t rx_bytes             = 0;
  uint64_t tx_bytes             = 0;
  double   wall_seconds         = 0.0;
  double   expected_blocks      = 0.0;   // wall_s * 48000 / 64
  double   block_rate_error_pct = 0.0;  // (actual-expected)/expected * 100
  double   effective_fs         = 0.0;  // actual_rx_frames / wall_s
  bool     timeline_pass        = false; // |block_rate_error_pct|<=0.5 && gaps==0
};

inline AudioTimelineStats compute_timeline(double wall_s) {
  AudioTimelineStats tl;
  tl.wall_seconds = wall_s;
  const auto &c   = s_audio.counters();
  tl.actual_rx_blocks   = c.rx_blocks_read.load(std::memory_order_relaxed);
  tl.actual_tx_blocks   = c.tx_blocks_written.load(std::memory_order_relaxed);
  tl.actual_rx_frames   = c.rx_frames_read.load(std::memory_order_relaxed);
  tl.rx_sequence_gaps   = c.rx_sequence_gaps.load(std::memory_order_relaxed);
  tl.tx_sequence_gaps   = c.tx_sequence_gaps.load(std::memory_order_relaxed);
  tl.rx_bytes           = c.rx_bytes_read.load(std::memory_order_relaxed);
  tl.tx_bytes           = c.tx_bytes_written.load(std::memory_order_relaxed);
  tl.expected_blocks    = wall_s * 48000.0 / 64.0;
  tl.effective_fs       = (wall_s > 0.0)
      ? (static_cast<double>(tl.actual_rx_frames) / wall_s) : 0.0;
  tl.block_rate_error_pct = (tl.expected_blocks > 0.0)
      ? ((static_cast<double>(tl.actual_rx_blocks) - tl.expected_blocks)
         / tl.expected_blocks) * 100.0
      : 0.0;
  tl.timeline_pass = std::fabs(tl.block_rate_error_pct) <= 0.5
                  && (tl.rx_sequence_gaps == 0)
                  && (tl.tx_sequence_gaps == 0);
  return tl;
}

// ---------------------------------------------------------------------------
// Case definitions
// ---------------------------------------------------------------------------
struct B4C4ACaseDefinition {
  const char      *name;
  const char      *suite;
  B4B6StimulusKind stimulus;
  float            nominal_f0_hz;
  bool             is_vocal;
  bool             transport_only;
  bool             enable_gate;
  bool             enable_compressor;
  bool             comp_active_reduction;
  size_t           harmony_voice_count;
  float            harmony_voice0_interval;
  float            harmony_voice1_interval;
  FormantMode      formant_mode;
  bool             enable_harmony_limiter;
  bool             harm_limiter_active;
  bool             enable_delay;
  bool             delay_stereo;
  float            delay_feedback;
  bool             enable_reverb;
  float            reverb_rt60;
  bool             master_limiter_active;
  uint32_t         measure_seconds;
};

struct SectionTiming {
  double   avg_us_block    = 0.0;
  double   avg_us_call     = 0.0;
  double   calls_per_block = 0.0;
  uint64_t total_cycles    = 0;
  uint64_t total_calls     = 0;
};

struct B4C4ACaseResult {
  const B4C4ACaseDefinition *definition       = nullptr;
  uint32_t  measured_seconds                  = 0;
  double    wall_seconds                      = 0.0;
  uint64_t  block_count                       = 0;
  double    dsp_avg_us                        = 0.0;
  uint64_t  dsp_p50_us                        = 0;
  uint64_t  dsp_p90_us                        = 0;
  uint64_t  dsp_p95_us                        = 0;
  uint64_t  dsp_p99_us                        = 0;
  uint64_t  dsp_p999_us                       = 0;
  uint64_t  dsp_max_us                        = 0;
  double    cycle_avg_us                      = 0.0;
  uint64_t  cycle_max_us                      = 0;
  double    cpu_pct                           = 0.0;
  double    margin_us                         = 0.0;
  double    margin_pct                        = 0.0;
  uint64_t  deadline_misses                   = 0;
  double    late_block_pct                    = 0.0;
  uint64_t  max_single_block_lateness_us      = 0;
  uint64_t  cumulative_lateness_us            = 0;
  uint64_t  max_consecutive_late_blocks       = 0;
  uint64_t  nan_inf                           = 0;
  AudioTimelineStats timeline;
  bool      realtime_pass                     = false;
  // Pipeline sections (us/block) - DIAGNOSTIC pass only
  double    pitch_tap_us = 0.0, param_q_us = 0.0, input_hpf_us = 0.0;
  double    input_gate_us = 0.0, compressor_us = 0.0, pitch_sync_us = 0.0;
  double    harmony_voice0_us = 0.0, harmony_voice1_us = 0.0;
  double    dry_align_us = 0.0, slew_pan_us = 0.0, harm_limiter_us = 0.0;
  double    bus_mix_us = 0.0, delay_prep_us = 0.0, delay_us = 0.0;
  double    reverb_prep_us = 0.0, reverb_us = 0.0;
  double    master_mix_us = 0.0, master_limiter_us = 0.0;
  double    pipeline_us = 0.0, sections_sum_us = 0.0;
  double    reconciliation_pct = 0.0, unaccounted_us = 0.0;
  // PSOLA 19 sections - DIAGNOSTIC only
  static constexpr size_t kPsolaSections = 19;
  SectionTiming v0_sections[kPsolaSections]{};
  SectionTiming v1_sections[kPsolaSections]{};
  double    v0_total_us = 0.0, v1_total_us = 0.0;
  double    v0_reconciliation_err_pct = 0.0, v1_reconciliation_err_pct = 0.0;
  // Grain metrics
  uint64_t  grain_attempted = 0, grain_scheduled = 0, grain_rendered = 0;
  double    avg_grains_block = 0.0;
  PsolaSourceResidualCacheStats cache_stats_v0{}, cache_stats_v1{};
  PsolaDeferredStats deferred_stats_v0{}, deferred_stats_v1{};
};

inline bool b4c4a_f0_is_worse(const B4C4ACaseResult &a,
                              const B4C4ACaseResult &b) {
  if (std::fabs(a.late_block_pct - b.late_block_pct) > 1e-9)
    return a.late_block_pct > b.late_block_pct;
  if (a.deadline_misses != b.deadline_misses)
    return a.deadline_misses > b.deadline_misses;
  if (a.dsp_p99_us != b.dsp_p99_us)
    return a.dsp_p99_us > b.dsp_p99_us;
  if (a.dsp_p999_us != b.dsp_p999_us)
    return a.dsp_p999_us > b.dsp_p999_us;
  if (std::fabs(a.dsp_avg_us - b.dsp_avg_us) > 1e-9)
    return a.dsp_avg_us > b.dsp_avg_us;
  if (a.dsp_max_us != b.dsp_max_us)
    return a.dsp_max_us > b.dsp_max_us;
  return std::fabs(a.timeline.block_rate_error_pct) >
         std::fabs(b.timeline.block_rate_error_pct);
}

// ---------------------------------------------------------------------------
// DSP helpers
// ---------------------------------------------------------------------------
inline void set_grain_render_mode_both(PsolaGrainRenderMode mode) {
  vocal_fx_set_psola_grain_render_mode(0, mode);
  vocal_fx_set_psola_grain_render_mode(1, mode);
}
inline void set_fir_kernel_both(PsolaFirKernel kernel) {
  vocal_fx_set_psola_fir_kernel(0, kernel);
  vocal_fx_set_psola_fir_kernel(1, kernel);
}
inline void set_residual_cache_both(size_t entries) {
  const bool enable = (entries > 0);
  vocal_fx_configure_psola_residual_cache(0, entries, enable, false);
  vocal_fx_configure_psola_residual_cache(1, entries, enable, false);
  vocal_fx_set_psola_residual_cache_enabled(0, enable);
  vocal_fx_set_psola_residual_cache_enabled(1, enable);
}

[[maybe_unused]] inline const char *psola_section_name(size_t s) {
  switch (s) {
    case 0:  return "Total";          case 1:  return "BufferShift";
    case 2:  return "Crossfade";      case 3:  return "PitchAnalysis";
    case 4:  return "GrainSchedule";  case 5:  return "LpcLookup";
    case 6:  return "LpcWarpPoly";    case 7:  return "LpcResidualFIR";
    case 8:  return "LpcSynthesizeIIR"; case 9: return "HannWindow";
    case 10: return "OlaAccumulate";  case 11: return "OlaNormalize";
    case 12: return "TransientDetect"; case 13: return "FormantShift";
    case 14: return "RingBufferWrap"; case 15: return "SafetyFallback";
    case 16: return "VoicedLogic";    case 17: return "UnvoicedLogic";
    case 18: return "other";
    default: return "unknown";
  }
}

// ---------------------------------------------------------------------------
// b4c4a_configure_dsp - production path frozen: DeferredSlice + MultiFma
// ---------------------------------------------------------------------------
inline void b4c4a_configure_dsp(const B4C4ACaseDefinition &def) {
  set_grain_render_mode_both(PsolaGrainRenderMode::DeferredSlice);
  set_fir_kernel_both(PsolaFirKernel::MultiFma);
  set_residual_cache_both(2);
  s_audio.configure_b4b6_stimulus(def.stimulus, def.nominal_f0_hz,
      def.is_vocal ? kB4b6VocalMulaw : nullptr,
      def.is_vocal ? kB4b6VocalMulawSize : 0);
  s_audio.set_b4c1_transport_only(def.transport_only);
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, def.enable_gate ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, def.enable_compressor ? 1.0f : 0.0f);
  if (def.comp_active_reduction) {
    vocal_fx_set_parameter(VocalFxParameter::CompressorThresholdDb, -24.0f);
    vocal_fx_set_parameter(VocalFxParameter::CompressorRatio, 4.0f);
    vocal_fx_set_parameter(VocalFxParameter::CompressorMakeupDb, 3.0f);
  } else {
    vocal_fx_set_parameter(VocalFxParameter::CompressorThresholdDb, 0.0f);
    vocal_fx_set_parameter(VocalFxParameter::CompressorRatio, 1.0f);
    vocal_fx_set_parameter(VocalFxParameter::CompressorMakeupDb, 0.0f);
  }
  vocal_fx_set_harmony_enabled(0, def.harmony_voice_count >= 1);
  vocal_fx_set_harmony_interval(0, def.harmony_voice0_interval);
  vocal_fx_set_formant_mode(0, def.formant_mode);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_harmony_enabled(1, def.harmony_voice_count >= 2);
  vocal_fx_set_harmony_interval(1, def.harmony_voice1_interval);
  vocal_fx_set_formant_mode(1, def.formant_mode);
  vocal_fx_set_harmony_gain(1, 1.0f);
  vocal_fx_set_harmony_limiter(def.enable_harmony_limiter,
                               def.harm_limiter_active ? -12.0f : -3.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, def.enable_delay ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, def.delay_stereo ? 375.0f : 250.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, def.delay_feedback);
  vocal_fx_set_parameter(VocalFxParameter::DelayWet, 0.5f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, def.enable_reverb ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, def.reverb_rt60);
  vocal_fx_set_parameter(VocalFxParameter::ReverbWet, 0.5f);
  vocal_fx_set_parameter(VocalFxParameter::LimiterCeiling,
                         def.master_limiter_active ? 0.50f : 0.95f);
}

// ---------------------------------------------------------------------------
// b4c4a_collect_results
// ---------------------------------------------------------------------------
inline void b4c4a_collect_results(B4C4ACaseResult &res, double wall_s, bool is_diagnostic) {
  res.wall_seconds = wall_s;
  res.timeline     = compute_timeline(wall_s);
  const TimingPercentiles dsp_p   = s_audio.calculate_dsp_percentiles();
  const TimingPercentiles cycle_p = s_audio.calculate_cycle_percentiles();
  res.block_count   = dsp_p.samples;
  res.dsp_avg_us    = dsp_p.avg_us;
  res.dsp_p50_us    = dsp_p.p50_us;
  res.dsp_p90_us    = dsp_p.p90_us;
  res.dsp_p95_us    = dsp_p.p95_us;
  res.dsp_p99_us    = dsp_p.p99_us;
  res.dsp_p999_us   = dsp_p.p99_9_us;
  res.dsp_max_us    = dsp_p.max_us;
  res.cycle_avg_us  = cycle_p.avg_us;
  res.cycle_max_us  = cycle_p.max_us;
  constexpr double kDeadlineUs = 1333.3333;
  res.cpu_pct   = (res.dsp_avg_us / kDeadlineUs) * 100.0;
  res.margin_us = kDeadlineUs > res.dsp_max_us
                      ? (kDeadlineUs - res.dsp_max_us) : (kDeadlineUs - res.dsp_avg_us);
  res.margin_pct = (res.margin_us / kDeadlineUs) * 100.0;
  const auto &c = s_audio.counters();
  res.deadline_misses              = c.dsp_deadline_misses.load(std::memory_order_relaxed);
  res.max_single_block_lateness_us = c.max_single_block_lateness_us.load(std::memory_order_relaxed);
  res.cumulative_lateness_us       = c.cumulative_lateness_us.load(std::memory_order_relaxed);
  res.max_consecutive_late_blocks  = c.max_consecutive_late_blocks.load(std::memory_order_relaxed);
  res.late_block_pct = res.block_count > 0
      ? (static_cast<double>(res.deadline_misses) / res.block_count) * 100.0 : 0.0;
  res.nan_inf = c.nan_inf_count.load(std::memory_order_relaxed);
  res.realtime_pass = (res.dsp_p99_us < 1333) && (res.deadline_misses == 0)
                   && (res.nan_inf == 0);
  const double c2us   = 1.0 / Profiler::cycles_per_us();
  const double blocks = res.block_count > 0 ? static_cast<double>(res.block_count) : 1.0;
  if (is_diagnostic) {
    auto gs = [&](VocalFxProfileSection s) {
      return vocal_fx_profile_stats(s).total_cycles * c2us / blocks;
    };
    res.pitch_tap_us      = gs(VocalFxProfileSection::PitchLpcTap);
    res.param_q_us        = gs(VocalFxProfileSection::ParameterQueue);
    res.input_hpf_us      = gs(VocalFxProfileSection::InputHpf);
    res.input_gate_us     = gs(VocalFxProfileSection::InputGate);
    res.compressor_us     = gs(VocalFxProfileSection::Compressor);
    res.pitch_sync_us     = gs(VocalFxProfileSection::PitchMarkSync);
    res.harmony_voice0_us = gs(VocalFxProfileSection::HarmonyVoice0);
    res.harmony_voice1_us = gs(VocalFxProfileSection::HarmonyVoice1);
    res.dry_align_us      = gs(VocalFxProfileSection::DryAlignment);
    res.slew_pan_us       = gs(VocalFxProfileSection::HarmonySlewPan);
    res.harm_limiter_us   = gs(VocalFxProfileSection::HarmonyLimiter);
    res.bus_mix_us        = gs(VocalFxProfileSection::BusMixing);
    res.delay_prep_us     = gs(VocalFxProfileSection::DelayPrep);
    res.delay_us          = gs(VocalFxProfileSection::Delay);
    res.reverb_prep_us    = gs(VocalFxProfileSection::ReverbPrep);
    res.reverb_us         = gs(VocalFxProfileSection::Reverb);
    res.master_mix_us     = gs(VocalFxProfileSection::MasterMix);
    res.master_limiter_us = gs(VocalFxProfileSection::MasterLimiter);
    res.pipeline_us       = gs(VocalFxProfileSection::Pipeline);
    res.sections_sum_us = res.pitch_tap_us + res.param_q_us + res.input_hpf_us +
        res.input_gate_us + res.compressor_us + res.pitch_sync_us +
        res.harmony_voice0_us + res.harmony_voice1_us + res.dry_align_us +
        res.slew_pan_us + res.harm_limiter_us + res.bus_mix_us +
        res.delay_prep_us + res.delay_us + res.reverb_prep_us + res.reverb_us +
        res.master_mix_us + res.master_limiter_us;
    res.reconciliation_pct = res.pipeline_us > 0.0
        ? (res.sections_sum_us / res.pipeline_us) * 100.0 : 100.0;
    res.unaccounted_us = std::max(0.0, res.pipeline_us - res.sections_sum_us);
    double v0s = 0.0, v1s = 0.0;
    for (size_t s = 0; s < B4C4ACaseResult::kPsolaSections; ++s) {
      const auto sec = static_cast<PitchShiftProfileSection>(s);
      const auto st0 = vocal_fx_harmony_voice_profile_stats(0, sec);
      const auto st1 = vocal_fx_harmony_voice_profile_stats(1, sec);
      res.v0_sections[s] = {st0.total_cycles * c2us / blocks,
          st0.blocks > 0 ? st0.total_cycles * c2us / st0.blocks : 0.0,
          st0.blocks / blocks, st0.total_cycles, st0.blocks};
      res.v1_sections[s] = {st1.total_cycles * c2us / blocks,
          st1.blocks > 0 ? st1.total_cycles * c2us / st1.blocks : 0.0,
          st1.blocks / blocks, st1.total_cycles, st1.blocks};
      v0s += res.v0_sections[s].avg_us_block;
      v1s += res.v1_sections[s].avg_us_block;
    }
    const auto v0t = vocal_fx_harmony_voice_profile_stats(0, PitchShiftProfileSection::Total);
    const auto v1t = vocal_fx_harmony_voice_profile_stats(1, PitchShiftProfileSection::Total);
    res.v0_total_us = v0t.total_cycles * c2us / blocks;
    res.v1_total_us = v1t.total_cycles * c2us / blocks;
    res.v0_reconciliation_err_pct = res.v0_total_us > 0.0
        ? (std::fabs(v0s - res.v0_total_us) / res.v0_total_us) * 100.0 : 0.0;
    res.v1_reconciliation_err_pct = res.v1_total_us > 0.0
        ? (std::fabs(v1s - res.v1_total_us) / res.v1_total_us) * 100.0 : 0.0;
  }
  res.cache_stats_v0    = vocal_fx_get_psola_residual_cache_stats(0);
  res.cache_stats_v1    = vocal_fx_get_psola_residual_cache_stats(1);
  res.deferred_stats_v0 = vocal_fx_get_psola_deferred_stats(0);
  res.deferred_stats_v1 = vocal_fx_get_psola_deferred_stats(1);
}

// ---------------------------------------------------------------------------
// b4c4a_print_case
// ---------------------------------------------------------------------------
inline void b4c4a_print_case(const B4C4ACaseDefinition &def,
                              const B4C4ACaseResult &res, bool is_diag) {
  printf("B4C4A_CASE name=%s suite=%s pass=%s diag=%s stimulus=%d f0=%.1f"
         " vocal=%s voices=%zu blocks=%llu wall_s=%.3f"
         " p99_us=%llu misses=%llu timeline=%s\n",
         def.name, def.suite, res.realtime_pass ? "PASS" : "FAIL",
         is_diag ? "true" : "false", static_cast<int>(def.stimulus), def.nominal_f0_hz,
         def.is_vocal ? "true" : "false", def.harmony_voice_count,
         static_cast<unsigned long long>(res.block_count), res.wall_seconds,
         static_cast<unsigned long long>(res.dsp_p99_us),
         static_cast<unsigned long long>(res.deadline_misses),
         res.timeline.timeline_pass ? "PASS" : "FAIL");
  printf("B4C4A_BLOCK_METRICS name=%s avg_us=%.2f p50_us=%llu p90_us=%llu"
         " p95_us=%llu p99_us=%llu p999_us=%llu max_us=%llu cpu_pct=%.2f"
         " margin_us=%.2f misses=%llu late_pct=%.3f max_late_us=%llu"
         " cum_late_us=%llu max_consec_late=%llu nan_inf=%llu\n",
         def.name, res.dsp_avg_us,
         static_cast<unsigned long long>(res.dsp_p50_us),
         static_cast<unsigned long long>(res.dsp_p90_us),
         static_cast<unsigned long long>(res.dsp_p95_us),
         static_cast<unsigned long long>(res.dsp_p99_us),
         static_cast<unsigned long long>(res.dsp_p999_us),
         static_cast<unsigned long long>(res.dsp_max_us),
         res.cpu_pct, res.margin_us,
         static_cast<unsigned long long>(res.deadline_misses),
         res.late_block_pct,
         static_cast<unsigned long long>(res.max_single_block_lateness_us),
         static_cast<unsigned long long>(res.cumulative_lateness_us),
         static_cast<unsigned long long>(res.max_consecutive_late_blocks),
         static_cast<unsigned long long>(res.nan_inf));
  const AudioTimelineStats &tl = res.timeline;
  printf("B4C4A_TIMELINE name=%s wall_s=%.3f rx_blocks=%llu tx_blocks=%llu"
         " rx_frames=%llu rx_bytes=%llu tx_bytes=%llu"
         " expected_blocks=%.1f block_rate_err_pct=%.4f effective_fs=%.2f"
         " rx_gaps=%llu tx_gaps=%llu timeline_pass=%s\n",
         def.name, tl.wall_seconds,
         static_cast<unsigned long long>(tl.actual_rx_blocks),
         static_cast<unsigned long long>(tl.actual_tx_blocks),
         static_cast<unsigned long long>(tl.actual_rx_frames),
         static_cast<unsigned long long>(tl.rx_bytes),
         static_cast<unsigned long long>(tl.tx_bytes),
         tl.expected_blocks, tl.block_rate_error_pct, tl.effective_fs,
         static_cast<unsigned long long>(tl.rx_sequence_gaps),
         static_cast<unsigned long long>(tl.tx_sequence_gaps),
         tl.timeline_pass ? "PASS" : "FAIL");
  if (is_diag) {
    printf("B4C4A_PIPELINE_SECTIONS name=%s"
           " tap_us=%.2f param_us=%.2f hpf_us=%.2f gate_us=%.2f comp_us=%.2f"
           " sync_us=%.2f v0_us=%.2f v1_us=%.2f dry_us=%.2f slew_us=%.2f"
           " hlim_us=%.2f bmix_us=%.2f dprep_us=%.2f delay_us=%.2f"
           " rprep_us=%.2f rev_us=%.2f mmix_us=%.2f mlim_us=%.2f"
           " pipe_us=%.2f sum_us=%.2f unacc_us=%.2f rec_pct=%.2f\n",
           def.name,
           res.pitch_tap_us, res.param_q_us, res.input_hpf_us, res.input_gate_us,
           res.compressor_us, res.pitch_sync_us, res.harmony_voice0_us, res.harmony_voice1_us,
           res.dry_align_us, res.slew_pan_us, res.harm_limiter_us, res.bus_mix_us,
           res.delay_prep_us, res.delay_us, res.reverb_prep_us, res.reverb_us,
           res.master_mix_us, res.master_limiter_us,
           res.pipeline_us, res.sections_sum_us, res.unaccounted_us, res.reconciliation_pct);
    const double v0f = res.v0_sections[7].avg_us_block;
    const double v0o = res.v0_sections[10].avg_us_block;
    const double v1f = res.v1_sections[7].avg_us_block;
    const double v1o = res.v1_sections[10].avg_us_block;
    printf("B4C4A_PSOLA_SECTIONS name=%s"
           " v0_fir_us=%.2f v0_ola_us=%.2f v1_fir_us=%.2f v1_ola_us=%.2f"
           " total_fir_us=%.2f total_ola_us=%.2f"
           " v0_total_us=%.2f v1_total_us=%.2f"
           " v0_rec_err_pct=%.2f v1_rec_err_pct=%.2f\n",
           def.name, v0f, v0o, v1f, v1o, v0f + v1f, v0o + v1o,
           res.v0_total_us, res.v1_total_us,
           res.v0_reconciliation_err_pct, res.v1_reconciliation_err_pct);
    for (size_t s = 1; s < B4C4ACaseResult::kPsolaSections; ++s) {
      printf("B4C4A_PSOLA_SECTION_ROW name=%s section=%s"
             " v0_avg_us=%.3f v0_call_us=%.3f v0_cpb=%.3f"
             " v1_avg_us=%.3f v1_call_us=%.3f v1_cpb=%.3f\n",
             def.name, psola_section_name(s),
             res.v0_sections[s].avg_us_block, res.v0_sections[s].avg_us_call,
             res.v0_sections[s].calls_per_block,
             res.v1_sections[s].avg_us_block, res.v1_sections[s].avg_us_call,
             res.v1_sections[s].calls_per_block);
    }
  }
  // Cache stats (always)
  const uint64_t tl2 = res.cache_stats_v0.total_lookups + res.cache_stats_v1.total_lookups;
  const uint64_t th  = res.cache_stats_v0.hits + res.cache_stats_v1.hits;
  const uint64_t tm  = res.cache_stats_v0.misses + res.cache_stats_v1.misses;
  const uint64_t te  = res.cache_stats_v0.evictions + res.cache_stats_v1.evictions;
  const uint64_t fc  = res.cache_stats_v0.fir_samples_computed + res.cache_stats_v1.fir_samples_computed;
  const uint64_t fr  = res.cache_stats_v0.fir_samples_reused + res.cache_stats_v1.fir_samples_reused;
  const double hp    = tl2 > 0 ? (100.0 * th / tl2) : 0.0;
  const double sp    = (fc + fr) > 0 ? (100.0 * fr / (fc + fr)) : 0.0;
  const double bd    = res.block_count > 0 ? static_cast<double>(res.block_count) : 1.0;
  const double c2us2 = 1.0 / Profiler::cycles_per_us();
  printf("B4C4A_RESIDUAL_CACHE_STATS name=%s lookups=%llu hits=%llu misses=%llu"
         " evictions=%llu hit_pct=%.2f sample_hit_pct=%.2f"
         " fir_computed=%llu fir_reused=%llu lookup_us=%.2f fir_saved_us=%.2f\n",
         def.name,
         static_cast<unsigned long long>(tl2), static_cast<unsigned long long>(th),
         static_cast<unsigned long long>(tm), static_cast<unsigned long long>(te),
         hp, sp, static_cast<unsigned long long>(fc), static_cast<unsigned long long>(fr),
         (res.cache_stats_v0.total_lookup_cycles + res.cache_stats_v1.total_lookup_cycles) * c2us2 / bd,
         (res.cache_stats_v0.fir_cycles_saved + res.cache_stats_v1.fir_cycles_saved) * c2us2 / bd);
  // Deferred stats (always)
  const uint64_t ds  = res.deferred_stats_v0.slices_rendered + res.deferred_stats_v1.slices_rendered;
  const uint64_t dss = res.deferred_stats_v0.slice_samples_rendered + res.deferred_stats_v1.slice_samples_rendered;
  const uint64_t dfs = res.deferred_stats_v0.slice_fir_samples + res.deferred_stats_v1.slice_fir_samples;
  printf("B4C4A_DEFERRED_STATS name=%s slices=%llu slice_samples=%llu"
         " slice_fir_samples=%llu max_active_v0=%lu max_active_v1=%lu\n",
         def.name,
         static_cast<unsigned long long>(ds), static_cast<unsigned long long>(dss),
         static_cast<unsigned long long>(dfs),
         static_cast<unsigned long>(res.deferred_stats_v0.max_active_descriptors),
         static_cast<unsigned long>(res.deferred_stats_v1.max_active_descriptors));

  // B4C.5 formant-warp attribution.  This is emitted for every case so the
  // cache campaign can compare setup, exact-key lookup, polynomial warp, and
  // GainNorm without changing the frozen DSP path.
  for (size_t voice = 0; voice < 2; ++voice) {
    const auto a = vocal_fx_get_psola_model_warp_audit(voice);
    printf("B4C5_FORMANT_AUDIT name=%s voice=%zu model_near_calls=%llu"
           " cache_lookup_calls=%llu cache_hit_calls=%llu cache_miss_calls=%llu"
           " local_hits=%llu shared_hits=%llu neutral_hits=%llu"
           " warp_poly_calls=%llu gain_norm_calls=%llu coeff_copy_calls=%llu"
           " expensive_computations=%llu cache_lookup_cycles=%llu"
           " cache_hit_cycles=%llu warp_poly_cycles=%llu gain_norm_cycles=%llu"
           " coeff_copy_cycles=%llu\n",
           def.name, voice,
           static_cast<unsigned long long>(a.model_near_calls),
           static_cast<unsigned long long>(a.cache_lookup_calls),
           static_cast<unsigned long long>(a.cache_hit_calls),
           static_cast<unsigned long long>(a.cache_miss_calls),
           static_cast<unsigned long long>(a.local_hits),
           static_cast<unsigned long long>(a.shared_hits),
           static_cast<unsigned long long>(a.neutral_hits),
           static_cast<unsigned long long>(a.warp_poly_calls),
           static_cast<unsigned long long>(a.gain_norm_calls),
           static_cast<unsigned long long>(a.coeff_copy_calls),
           static_cast<unsigned long long>(a.expensive_computations),
           static_cast<unsigned long long>(a.cache_lookup_cycles),
           static_cast<unsigned long long>(a.cache_hit_cycles),
           static_cast<unsigned long long>(a.warp_poly_cycles),
           static_cast<unsigned long long>(a.gain_norm_cycles),
           static_cast<unsigned long long>(a.coeff_copy_cycles));
  }
}

// ---------------------------------------------------------------------------
// b4c4a_run_case - dual-pass: DIAGNOSTIC (full sections) + QUALIFICATION (sleep-only)
// ---------------------------------------------------------------------------
void b4c4a_run_case(const B4C4ACaseDefinition &def,
                    B4C4ACaseResult &res, bool is_diagnostic) {
  res = B4C4ACaseResult{};
  res.definition       = &def;
  res.measured_seconds = def.measure_seconds;
  b4c4a_configure_dsp(def);
  vTaskDelay(pdMS_TO_TICKS(500));
  s_b4b6_measurement_pause.store(true, std::memory_order_release);
  s_audio.set_b4b6_dsp_paused(true);
  while (s_pitch_worker_in_call.load(std::memory_order_acquire) || s_audio.b4b6_dsp_active())
    vTaskDelay(1);
  vocal_fx_reset_profiling_epoch();
  s_audio.reset_counters();
  vocal_fx_reset_psola_residual_cache_stats(0);
  vocal_fx_reset_psola_residual_cache_stats(1);
  vocal_fx_reset_psola_deferred_stats(0);
  vocal_fx_reset_psola_deferred_stats(1);
  vocal_fx_reset_harmonizer_trace();
  const uint64_t window_start_us = esp_timer_get_time();
  const VocalFxFunnelStats funnel_start = vocal_fx_funnel_stats();
  s_audio.set_b4b6_dsp_paused(false);
  s_b4b6_measurement_pause.store(false, std::memory_order_release);
  for (uint32_t sec = 0; sec < def.measure_seconds; ++sec)
    vTaskDelay(pdMS_TO_TICKS(1000));
  s_b4b6_measurement_pause.store(true, std::memory_order_release);
  s_audio.set_b4b6_dsp_paused(true);
  while (s_pitch_worker_in_call.load(std::memory_order_acquire) || s_audio.b4b6_dsp_active())
    vTaskDelay(1);
  const uint64_t window_end_us = esp_timer_get_time();
  const double   wall_s        = (window_end_us - window_start_us) / 1000000.0;
  const VocalFxFunnelStats funnel_end = vocal_fx_funnel_stats();
  res.grain_attempted = funnel_end.grain_schedule_attempts - funnel_start.grain_schedule_attempts;
  res.grain_scheduled = funnel_end.grains_scheduled - funnel_start.grains_scheduled;
  res.grain_rendered  = funnel_end.grains_rendered  - funnel_start.grains_rendered;
  b4c4a_collect_results(res, wall_s, is_diagnostic);
  res.avg_grains_block = res.block_count > 0
      ? static_cast<double>(res.grain_rendered) / static_cast<double>(res.block_count) : 0.0;
  b4c4a_print_case(def, res, is_diagnostic);
}

// ---------------------------------------------------------------------------
// Case constants - ALL standalone: Compressor/Delay/Reverb=false (Steps 1-5)
// Field order: name, suite, stimulus, f0_hz, is_vocal, transport_only,
//   enable_gate, enable_compressor, comp_active_reduction,
//   voices, v0_interval, v1_interval, formant_mode,
//   enable_harm_limiter, harm_limiter_active,
//   enable_delay, delay_stereo, delay_feedback,
//   enable_reverb, reverb_rt60, master_limiter_active, measure_seconds
// ---------------------------------------------------------------------------

constexpr B4C4ACaseDefinition kCaseTransportOnly{
    "transport_only","baseline",B4B6StimulusKind::HarmonicVoiced,220.0f,false,true,
    false,false,false,0,0.0f,0.0f,FormantMode::Off,
    false,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseDryPath{
    "dry_path","baseline",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,false,false,0,0.0f,0.0f,FormantMode::Off,
    false,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseHarm1V{
    "harm_1v","baseline",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,false,false,1,4.0f,0.0f,FormantMode::Lpc,
    true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseHarm2V{
    "harm_2v","baseline",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,
    true,false,false,false,0.0f,false,1.2f,false,10};

constexpr B4C4ACaseDefinition kCaseF0_65{
    "f0_65hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,65.4f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_80{
    "f0_80hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,82.4f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_110{
    "f0_110hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,110.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_147{
    "f0_147hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,146.8f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_220{
    "f0_220hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,220.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_330{
    "f0_330hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,330.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_440{
    "f0_440hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,440.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_700{
    "f0_700hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,700.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_900{
    "f0_900hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,900.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseF0_1000{
    "f0_1000hz","f0_matrix",B4B6StimulusKind::HarmonicVoiced,1000.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};

constexpr B4C4ACaseDefinition kCaseTrans65To130{
    "trans_65to130","transition",B4B6StimulusKind::Step65To130,65.4f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseTrans80To160{
    "trans_80to160","transition",B4B6StimulusKind::Step80To160,82.4f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseTrans110To220{
    "trans_110to220","transition",B4B6StimulusKind::Step110To220,110.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseTrans220To110{
    "trans_220to110","transition",B4B6StimulusKind::Step220To110,220.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseTrans220To440{
    "trans_220to440","transition",B4B6StimulusKind::Step220To440,220.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseTrans440To220{
    "trans_440to110","transition",B4B6StimulusKind::Step440To110,440.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseGliss{
    "gliss_80to440","transition",B4B6StimulusKind::Gliss80To440,80.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseVibrato{
    "vibrato_2st","transition",B4B6StimulusKind::VibratoTwoSemitones,220.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseStaccato{
    "staccato","transition",B4B6StimulusKind::Staccato,220.0f,false,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};

constexpr B4C4ACaseDefinition kCaseStability1VVocal60s{
    "stability_1v_vocal_60s","stability",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,false,false,1,4.0f,0.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,60};
constexpr B4C4ACaseDefinition kCaseStability2VVocal60s{
    "stability_2v_vocal_60s","stability",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,false,false,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,60};

constexpr B4C4ACaseDefinition kCaseStep6CompOnly{
    "fx_comp_only","fx_incremental",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,true,true,2,4.0f,7.0f,FormantMode::Lpc,true,false,false,false,0.0f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseStep6CompDelay{
    "fx_comp_delay","fx_incremental",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,true,true,2,4.0f,7.0f,FormantMode::Lpc,true,false,true,true,0.25f,false,1.2f,false,10};
constexpr B4C4ACaseDefinition kCaseStep6CompDelayReverb{
    "fx_comp_delay_reverb","fx_incremental",B4B6StimulusKind::VocalReplay,220.0f,true,false,
    true,true,true,2,4.0f,7.0f,FormantMode::Lpc,true,false,true,true,0.25f,true,1.2f,false,10};

// ---------------------------------------------------------------------------
// Heap context
// ---------------------------------------------------------------------------
struct B4C4AHeapContext {
  B4C4ACaseResult res_transport{}, res_dry{}, res_1v{}, res_2v{};
  static constexpr size_t kF0Count = 10, kTransCount = 9;
  std::array<B4C4ACaseResult, kF0Count>    f0_matrix{};
  std::array<B4C4ACaseResult, kTransCount> trans_matrix{};
  B4C4ACaseResult res_1v_vocal_60s{}, res_2v_vocal_60s{}, res_2v_worst_60s{};
  B4C4ACaseResult res_fx_comp{}, res_fx_comp_delay{}, res_fx_all{};
  VocalFxBufferAudit buf_audits[64]{};
};

} // namespace

// ---------------------------------------------------------------------------
// b4c4a_coordinator_task - pinned to Core 1 at tskIDLE_PRIORITY+1
// ---------------------------------------------------------------------------
void b4c4a_coordinator_task(void *pvParameters) {
  (void)pvParameters;
  vTaskDelay(pdMS_TO_TICKS(250));

  auto *ctx = static_cast<B4C4AHeapContext *>(
      heap_caps_calloc(1, sizeof(B4C4AHeapContext), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!ctx) {
    printf("B4C.4A: SPIRAM alloc failed\n");
    vTaskSuspend(nullptr);
    return;
  }

  printf("\n================================================================================\n");
  printf(" STAGE B4C.4A - DEFERRED HARMONIZER STANDALONE REALTIME QUALIFICATION          \n");
  printf("================================================================================\n");
  printf("Target : WT9932P4-TINY (ESP32-P4 @ 360 MHz)\n");
  printf("Deadline: 1333.33 us/block (48 kHz, 64 frames/block)\n");
  printf("Path   : DeferredSlice + MultiFma + LPC formant preservation\n");
  printf("Gate   : P99<1333us, misses=0, |block_rate_err|<=0.5%%, gaps=0\n");
  printf("FX     : DISABLED for Steps 1-5; Step 6 incremental\n");
  printf("================================================================================\n\n");

  // Engine init (no harmonizer yet)
  VocalFxConfig cfg0{};
  cfg0.enable_gate=false; cfg0.enable_compressor=false;
  cfg0.enable_delay=false; cfg0.enable_reverb=false;
  cfg0.enable_pitch_analysis=true; cfg0.pitch_shift.enabled=false;
  cfg0.pitch_shift.semitones=4.0f; cfg0.pitch_shift.wet=1.0f;
  cfg0.pitch_shift.continuity_policy=PsolaContinuityPolicy::Baseline;
  cfg0.psola_continuity_policy=PsolaContinuityPolicy::Baseline;
  cfg0.align_dry_to_harmony=false; cfg0.enable_harmony_limiter=false;
  if (!vocal_fx_init(cfg0)) {
    printf("B4C.4A RESULT:\nFAIL\nEngine init failed\n");
    heap_caps_free(ctx); vTaskDelete(nullptr); return;
  }
  vocal_fx_set_psola_warp_cache(true, true, false);
#ifdef ESP_PLATFORM
  esp_task_wdt_config_t wdt = {.timeout_ms=120000,.idle_core_mask=(1<<1),.trigger_panic=false};
  (void)esp_task_wdt_reconfigure(&wdt);
#endif
  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::B4C4ADeferredStandalone, true)) {
    printf("B4C.4A RESULT:\nFAIL\nstart_pipeline_tasks failed\n");
    heap_caps_free(ctx); vTaskDelete(nullptr); return;
  }

  // =========================================================================
  // STEP 0A: Audio Timeline / Block-Rate Integrity
  // =========================================================================
  printf("=================================================================\n");
  printf(" STEP 0A: AUDIO TIMELINE / BLOCK-RATE INTEGRITY\n");
  printf("=================================================================\n");
  printf(">>> 10s transport-only timeline check (QUALIFICATION)...\n");
  b4c4a_run_case(kCaseTransportOnly, ctx->res_transport, false);
  const AudioTimelineStats &tl0a = ctx->res_transport.timeline;
  printf("B4C4A_STEP0A_RESULT rx_blocks=%llu expected=%.0f err_pct=%.4f"
         " eff_fs=%.2f rx_gaps=%llu tx_gaps=%llu pass=%s\n",
         static_cast<unsigned long long>(tl0a.actual_rx_blocks),
         tl0a.expected_blocks, tl0a.block_rate_error_pct, tl0a.effective_fs,
         static_cast<unsigned long long>(tl0a.rx_sequence_gaps),
         static_cast<unsigned long long>(tl0a.tx_sequence_gaps),
         tl0a.timeline_pass ? "PASS" : "FAIL");
  if (!tl0a.timeline_pass) {
    printf("B4C4A_ABORT reason=TRANSPORT_INTEGRITY_FAIL err_pct=%.4f rx_gaps=%llu tx_gaps=%llu\n",
           tl0a.block_rate_error_pct,
           static_cast<unsigned long long>(tl0a.rx_sequence_gaps),
           static_cast<unsigned long long>(tl0a.tx_sequence_gaps));
    printf("B4C4A_AUDIT_COMPLETE\n");
    heap_caps_free(ctx);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  printf(">>> STEP 0A PASS\n\n");

  // =========================================================================
  // STEP 0: Descriptor Placement & Single-Grain Attribution
  // =========================================================================
  printf("=================================================================\n");
  printf(" STEP 0: DESCRIPTOR PLACEMENT & SINGLE-GRAIN ATTRIBUTION\n");
  printf("=================================================================\n");
  VocalFxConfig cfg_h{};
  cfg_h.enable_gate=true; cfg_h.enable_compressor=false;
  cfg_h.enable_delay=false; cfg_h.enable_reverb=false;
  cfg_h.enable_pitch_analysis=true; cfg_h.pitch_shift.enabled=true;
  cfg_h.pitch_shift.semitones=4.0f; cfg_h.pitch_shift.wet=1.0f;
  cfg_h.pitch_shift.continuity_policy=PsolaContinuityPolicy::Baseline;
  cfg_h.psola_continuity_policy=PsolaContinuityPolicy::Baseline;
  cfg_h.align_dry_to_harmony=true; cfg_h.enable_harmony_limiter=true;
  if (!vocal_fx_init(cfg_h)) {
    printf("B4C.4A: harm re-init failed\n");
    printf("B4C4A_AUDIT_COMPLETE\n"); heap_caps_free(ctx);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  vocal_fx_set_psola_warp_cache(true, true, false);
  set_grain_render_mode_both(PsolaGrainRenderMode::DeferredSlice);
  set_fir_kernel_both(PsolaFirKernel::MultiFma);
  set_residual_cache_both(2);

  const size_t bcount = vocal_fx_audit_buffers(ctx->buf_audits, 64);
  printf("B4C4A_BUFFER_AUDIT_START count=%zu\n", bcount);
  for (size_t i = 0; i < bcount; ++i)
    printf("B4C4A_BUFFER name=%s bytes=%zu is_sram=%d is_psram=%d\n",
           ctx->buf_audits[i].name, ctx->buf_audits[i].size_bytes,
           ctx->buf_audits[i].is_sram?1:0, ctx->buf_audits[i].is_psram?1:0);
  printf("B4C4A_BUFFER_AUDIT_END\n");
  printf("B4C6_GAINNORM_BASIS bytes=%zu bit_mismatches=%zu placement=internal_static\n",
         SharedLpcAnalysis::gainnorm_basis_bytes(),
         SharedLpcAnalysis::gainnorm_basis_bit_mismatches(48000.0f));

  constexpr std::array<size_t, 9> kGL = {109,163,218,327,436,600,738,1201,1601};
  printf("\n--- Isolated Single-Grain Attribution (MultiFma, Order 10) ---\n");
  for (size_t len : kGL) {
    auto b = vocal_fx_benchmark_single_grain(len, 10, PsolaFirKernel::MultiFma);
    printf("B4C4A_SINGLE_GRAIN length=%zu order=%zu"
           " setup_us=%.2f model_us=%.2f warp_look_us=%.2f warp_poly_us=%.2f"
           " gain_us=%.2f fir_us=%.2f win_us=%.2f ola_us=%.2f norm_us=%.2f"
           " total_us=%.2f fir_per_sample_ns=%.1f\n",
           b.grain_length, b.order, b.history_setup_us, b.model_lookup_us,
           b.warp_cache_lookup_us, b.warp_poly_us, b.gain_norm_us,
           b.fir_us, b.window_us, b.ola_write_us, b.norm_write_us,
           b.total_add_grain_us, len > 0 ? b.fir_us * 1000.0 / len : 0.0);
  }

  // =========================================================================
  // STEP 1: 4 Canonical Baselines - FX disabled
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 1: CANONICAL BASELINES - STANDALONE (FX DISABLED)\n");
  printf("=================================================================\n");
  struct BC { const B4C4ACaseDefinition *def; B4C4ACaseResult *res; const char *lbl; };
  const std::array<BC, 4> kBL = {{
      {&kCaseTransportOnly,&ctx->res_transport,"TransportOnly"},
      {&kCaseDryPath,      &ctx->res_dry,      "Dry"},
      {&kCaseHarm1V,       &ctx->res_1v,       "1V"},
      {&kCaseHarm2V,       &ctx->res_2v,       "2V"},
  }};
  for (const auto &bc : kBL) {
    printf(">>> DIAGNOSTIC: %s\n", bc.lbl);
    b4c4a_run_case(*bc.def, *bc.res, true);
    printf(">>> QUALIFICATION: %s\n", bc.lbl);
    B4C4ACaseResult qr{};
    b4c4a_run_case(*bc.def, qr, false);
    printf("B4C4A_PASS_DELTA name=%s diag_p99=%llu qual_p99=%llu"
           " diag_avg=%.2f qual_avg=%.2f diag_tl=%s qual_tl=%s\n",
           bc.def->name,
           static_cast<unsigned long long>(bc.res->dsp_p99_us),
           static_cast<unsigned long long>(qr.dsp_p99_us),
           bc.res->dsp_avg_us, qr.dsp_avg_us,
           bc.res->timeline.timeline_pass?"PASS":"FAIL",
           qr.timeline.timeline_pass?"PASS":"FAIL");
  }

  // =========================================================================
  // STEP 2: Eager vs Deferred Top-100
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 2: EAGER vs DEFERRED TOP-100 (2V)\n");
  printf("=================================================================\n");
  constexpr size_t kMT = 512;
  auto *tbuf = static_cast<HarmonizerBlockTraceRecord *>(
      heap_caps_calloc(kMT, sizeof(HarmonizerBlockTraceRecord), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  size_t tcnt = 0;
  set_grain_render_mode_both(PsolaGrainRenderMode::Eager);
  set_residual_cache_both(2);
  printf(">>> Eager 2V DIAGNOSTIC...\n");
  B4C4ACaseResult eager_2v{};
  b4c4a_run_case(kCaseHarm2V, eager_2v, true);
  std::vector<HarmonizerBlockTraceRecord> etr;
  if (tbuf) {
    tcnt = vocal_fx_get_harmonizer_trace(tbuf, kMT);
    if (tcnt > 0) {
      etr.assign(tbuf, tbuf+tcnt);
      std::sort(etr.begin(), etr.end(),
          [](const HarmonizerBlockTraceRecord &a, const HarmonizerBlockTraceRecord &b)
          { return a.block_runtime_us > b.block_runtime_us; });
    }
  }
  printf("B4C4A_EAGER_TOP100_HEADER rank,block_index,runtime_us,v0_us,v1_us,"
         "sched_v0,sched_v1,rend_v0,rend_v1,fir_computed,fir_reused,is_collision,is_miss\n");
  for (size_t i = 0; i < std::min(etr.size(), static_cast<size_t>(100)); ++i) {
    const auto &r = etr[i];
    bool ic = (r.new_grains_scheduled_v0>0&&r.new_grains_scheduled_v1>0)||
              (r.active_grains_rendered_v0+r.active_grains_rendered_v1>1);
    bool im = (r.residual_cache_misses_v0>0||r.residual_cache_misses_v1>0);
    printf("B4C4A_EAGER_TOP100 %zu,%lu,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u,%u,%d,%d\n",
           i+1,static_cast<unsigned long>(r.block_index),r.block_runtime_us,
           r.voice0_runtime_us,r.voice1_runtime_us,
           r.new_grains_scheduled_v0,r.new_grains_scheduled_v1,
           r.active_grains_rendered_v0,r.active_grains_rendered_v1,
           r.fir_samples_computed,r.fir_samples_reused,ic?1:0,im?1:0);
  }
  set_grain_render_mode_both(PsolaGrainRenderMode::DeferredSlice);
  set_residual_cache_both(2);
  printf(">>> Deferred 2V DIAGNOSTIC...\n");
  B4C4ACaseResult def_2v{};
  b4c4a_run_case(kCaseHarm2V, def_2v, true);
  std::vector<HarmonizerBlockTraceRecord> dtr;
  if (tbuf) {
    tcnt = vocal_fx_get_harmonizer_trace(tbuf, kMT);
    if (tcnt > 0) {
      dtr.assign(tbuf, tbuf+tcnt);
      std::sort(dtr.begin(), dtr.end(),
          [](const HarmonizerBlockTraceRecord &a, const HarmonizerBlockTraceRecord &b)
          { return a.block_runtime_us > b.block_runtime_us; });
    }
  }
  printf("B4C4A_DEFERRED_TOP100_HEADER rank,block_index,runtime_us,v0_us,v1_us,"
         "sched_v0,sched_v1,rend_v0,rend_v1,"
         "def_act_v0,def_act_v1,def_rend_v0,def_rend_v1,def_fir_v0,def_fir_v1\n");
  for (size_t i = 0; i < std::min(dtr.size(), static_cast<size_t>(100)); ++i) {
    const auto &r = dtr[i];
    printf("B4C4A_DEFERRED_TOP100 %zu,%lu,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
           i+1,static_cast<unsigned long>(r.block_index),r.block_runtime_us,
           r.voice0_runtime_us,r.voice1_runtime_us,
           r.new_grains_scheduled_v0,r.new_grains_scheduled_v1,
           r.active_grains_rendered_v0,r.active_grains_rendered_v1,
           r.deferred_active_grains_v0,r.deferred_active_grains_v1,
           r.deferred_slices_rendered_v0,r.deferred_slices_rendered_v1,
           r.deferred_fir_samples_v0,r.deferred_fir_samples_v1);
  }
  printf("B4C4A_EAGER_VS_DEFERRED eager_p99=%llu deferred_p99=%llu"
         " eager_max=%llu deferred_max=%llu eager_avg=%.2f deferred_avg=%.2f"
         " eager_misses=%llu deferred_misses=%llu\n",
         static_cast<unsigned long long>(eager_2v.dsp_p99_us),
         static_cast<unsigned long long>(def_2v.dsp_p99_us),
         static_cast<unsigned long long>(eager_2v.dsp_max_us),
         static_cast<unsigned long long>(def_2v.dsp_max_us),
         eager_2v.dsp_avg_us, def_2v.dsp_avg_us,
         static_cast<unsigned long long>(eager_2v.deadline_misses),
         static_cast<unsigned long long>(def_2v.deadline_misses));
  if (tbuf) { heap_caps_free(tbuf); tbuf = nullptr; }

  // =========================================================================
  // STEP 3: Descriptor Occupancy & Slice Workload
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 3: DESCRIPTOR OCCUPANCY & SLICE WORKLOAD\n");
  printf("=================================================================\n");
  for (int run = 0; run < 3; ++run) {
    printf(">>> Descriptor occupancy run %d/3...\n", run+1);
    B4C4ACaseResult dr{};
    b4c4a_run_case(kCaseHarm2V, dr, run==0);
    const uint64_t ts  = dr.deferred_stats_v0.slices_rendered+dr.deferred_stats_v1.slices_rendered;
    const uint64_t tss = dr.deferred_stats_v0.slice_samples_rendered+dr.deferred_stats_v1.slice_samples_rendered;
    printf("B4C4A_DESCRIPTOR_OCCUPANCY run=%d avg_us=%.2f p99_us=%llu"
           " slices=%llu slice_samples=%llu slices_per_block=%.2f"
           " max_active_v0=%lu max_active_v1=%lu\n",
           run+1, dr.dsp_avg_us, static_cast<unsigned long long>(dr.dsp_p99_us),
           static_cast<unsigned long long>(ts), static_cast<unsigned long long>(tss),
           dr.block_count>0 ? static_cast<double>(ts)/dr.block_count : 0.0,
           static_cast<unsigned long>(dr.deferred_stats_v0.max_active_descriptors),
           static_cast<unsigned long>(dr.deferred_stats_v1.max_active_descriptors));
  }

  // =========================================================================
  // STEP 4: Pitch Matrix + Transition Matrix (QUALIFICATION)
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 4: PITCH MATRIX + TRANSITION MATRIX\n");
  printf("=================================================================\n");
  constexpr std::array<const B4C4ACaseDefinition *, B4C4AHeapContext::kF0Count> kF0C = {{
      &kCaseF0_65,&kCaseF0_80,&kCaseF0_110,&kCaseF0_147,&kCaseF0_220,
      &kCaseF0_330,&kCaseF0_440,&kCaseF0_700,&kCaseF0_900,&kCaseF0_1000,
  }};
  bool have_worst_f0 = false; size_t worst_f0_index = 0; float worst_f0_hz = 65.4f;
  for (size_t i = 0; i < B4C4AHeapContext::kF0Count; ++i) {
    printf(">>> F0: %s\n", kF0C[i]->name);
    b4c4a_run_case(*kF0C[i], ctx->f0_matrix[i], false);
    if (!have_worst_f0 || b4c4a_f0_is_worse(ctx->f0_matrix[i],
                                             ctx->f0_matrix[worst_f0_index])) {
      have_worst_f0 = true;
      worst_f0_index = i;
      worst_f0_hz = kF0C[i]->nominal_f0_hz;
    }
  }
  printf("B4C4A_WORST_F0_AFTER_F0 f0_hz=%.1f p99_us=%llu\n",
         worst_f0_hz, static_cast<unsigned long long>(
             have_worst_f0 ? ctx->f0_matrix[worst_f0_index].dsp_p99_us : 0));
  constexpr std::array<const B4C4ACaseDefinition *, B4C4AHeapContext::kTransCount> kTC = {{
      &kCaseTrans65To130,&kCaseTrans80To160,&kCaseTrans110To220,
      &kCaseTrans220To110,&kCaseTrans220To440,&kCaseTrans440To220,
      &kCaseGliss,&kCaseVibrato,&kCaseStaccato,
  }};
  for (size_t i = 0; i < B4C4AHeapContext::kTransCount; ++i) {
    printf(">>> Trans: %s\n", kTC[i]->name);
    b4c4a_run_case(*kTC[i], ctx->trans_matrix[i], false);
  }
  printf("B4C4A_WORST_F0_FINAL f0_hz=%.1f p99_us=%llu\n",
         worst_f0_hz, static_cast<unsigned long long>(
             have_worst_f0 ? ctx->f0_matrix[worst_f0_index].dsp_p99_us : 0));

  // =========================================================================
  // STEP 5: 60-Second Stability
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 5: 60-SECOND STABILITY\n");
  printf("=================================================================\n");
  printf(">>> 1V vocal 60s...\n");
  b4c4a_run_case(kCaseStability1VVocal60s, ctx->res_1v_vocal_60s, false);
  printf(">>> 2V vocal 60s...\n");
  b4c4a_run_case(kCaseStability2VVocal60s, ctx->res_2v_vocal_60s, false);
  B4C4ACaseDefinition wscase = {
      "stability_2v_worst_synth_60s","stability",
      B4B6StimulusKind::HarmonicVoiced,worst_f0_hz,false,false,
      true,false,false,2,4.0f,7.0f,FormantMode::Lpc,
      true,false,false,false,0.0f,false,1.2f,false,60};
  printf(">>> 2V worst-F0 synth (%.1f Hz) 60s...\n", worst_f0_hz);
  b4c4a_run_case(wscase, ctx->res_2v_worst_60s, false);
  const bool step5_2v_pass = ctx->res_2v_vocal_60s.realtime_pass
                          && ctx->res_2v_worst_60s.realtime_pass;
  printf("B4C4A_STEP5_SUMMARY 1v=%s 2v_vocal=%s 2v_worst=%s gate=%s\n",
         ctx->res_1v_vocal_60s.realtime_pass?"PASS":"FAIL",
         ctx->res_2v_vocal_60s.realtime_pass?"PASS":"FAIL",
         ctx->res_2v_worst_60s.realtime_pass?"PASS":"FAIL",
         step5_2v_pass?"PASS":"FAIL");

  // =========================================================================
  // STEP 6: Incremental Effect Addition (gated)
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 6: INCREMENTAL EFFECT ADDITION (GATED ON STEP 5 2V)\n");
  printf("=================================================================\n");
  if (step5_2v_pass) {
    printf(">>> Step 5 PASSED - full Step 6 qualification.\n");
    b4c4a_run_case(kCaseStep6CompOnly,        ctx->res_fx_comp,       true);
    b4c4a_run_case(kCaseStep6CompDelay,       ctx->res_fx_comp_delay, true);
    b4c4a_run_case(kCaseStep6CompDelayReverb, ctx->res_fx_all,        true);
    printf("B4C4A_STEP6_SUMMARY mode=full"
           " comp_p99=%llu comp_pass=%s comp_delay_p99=%llu comp_delay_pass=%s"
           " full_fx_p99=%llu full_fx_pass=%s\n",
           static_cast<unsigned long long>(ctx->res_fx_comp.dsp_p99_us),
           ctx->res_fx_comp.realtime_pass?"PASS":"FAIL",
           static_cast<unsigned long long>(ctx->res_fx_comp_delay.dsp_p99_us),
           ctx->res_fx_comp_delay.realtime_pass?"PASS":"FAIL",
           static_cast<unsigned long long>(ctx->res_fx_all.dsp_p99_us),
           ctx->res_fx_all.realtime_pass?"PASS":"FAIL");
  } else {
    printf(">>> Step 5 FAILED - diagnostic-only.\n");
    b4c4a_run_case(kCaseStep6CompOnly,        ctx->res_fx_comp, true);
    b4c4a_run_case(kCaseStep6CompDelayReverb, ctx->res_fx_all,  true);
    printf("B4C4A_STEP6_SUMMARY mode=diagnostic_only"
           " comp_p99=%llu full_fx_p99=%llu\n",
           static_cast<unsigned long long>(ctx->res_fx_comp.dsp_p99_us),
           static_cast<unsigned long long>(ctx->res_fx_all.dsp_p99_us));
  }

  // =========================================================================
  // STEP 7: Memory & Resource Audit
  // =========================================================================
  printf("\n=================================================================\n");
  printf(" STEP 7: MEMORY & RESOURCE AUDIT\n");
  printf("=================================================================\n");
  const size_t bc2 = vocal_fx_audit_buffers(ctx->buf_audits, 64);
  printf("B4C4A_BUFFER_AUDIT_START count=%zu\n", bc2);
  for (size_t i = 0; i < bc2; ++i)
    printf("B4C4A_BUFFER name=%s bytes=%zu is_sram=%d is_psram=%d\n",
           ctx->buf_audits[i].name, ctx->buf_audits[i].size_bytes,
           ctx->buf_audits[i].is_sram?1:0, ctx->buf_audits[i].is_psram?1:0);
  printf("B4C4A_BUFFER_AUDIT_END\n");
  printf("B4C4A_HEAP internal_free=%lu internal_largest=%lu"
         " spiram_free=%lu spiram_largest=%lu\n",
         static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));

  const bool overall = tl0a.timeline_pass && ctx->res_2v.realtime_pass && step5_2v_pass;
  printf("\n================================================================================\n");
  printf(" B4C.4A QUALIFICATION SUMMARY\n");
  printf("================================================================================\n");
  printf("B4C4A_SUMMARY transport=%s baseline_2v=%s baseline_2v_p99=%llu"
         " step5=%s 1v_60s=%s 2v_vocal_60s=%s 2v_worst_60s=%s worst_f0=%.1f overall=%s\n",
         tl0a.timeline_pass?"PASS":"FAIL",
         ctx->res_2v.realtime_pass?"PASS":"FAIL",
         static_cast<unsigned long long>(ctx->res_2v.dsp_p99_us),
         step5_2v_pass?"PASS":"FAIL",
         ctx->res_1v_vocal_60s.realtime_pass?"PASS":"FAIL",
         ctx->res_2v_vocal_60s.realtime_pass?"PASS":"FAIL",
         ctx->res_2v_worst_60s.realtime_pass?"PASS":"FAIL",
         worst_f0_hz, overall?"PASS":"FAIL");
  printf("================================================================================\n");
  printf(" B4C.4A AUDIT COMPLETE - ALL STEPS EXECUTED SUCCESSFULLY                       \n");
  printf("================================================================================\n");
  printf("B4C4A_AUDIT_COMPLETE\n");
  heap_caps_free(ctx);
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif // CONFIG_VOXP4_MODE_I2S_B4C_4A_DEFERRED_STANDALONE
