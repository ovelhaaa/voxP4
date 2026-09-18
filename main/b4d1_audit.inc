// B4D.1 — 1V production realtime closure & Full-FX CPU budget.
// Included by main/i2s_bringup.cpp.  DSP semantics are FROZEN (§3): this file
// only stages input, selects the qualified production configuration, runs a
// cleaned steady-state qualification window (explicit warmup, full reset, then
// >=60 s of steady-state audio), and reports DSP/loop percentiles, late-block
// forensics, the model-change x deadline-miss contingency, TX-prep breakdown,
// incremental FX costs and the F0 matrix.  No DSP arithmetic is changed here.

#if defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_12_BURN_IN)

#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using vocal_fx_platform::AudioI2sMode;
using vocal_fx_platform::B4B6StimulusKind;
using vocal_fx_platform::B4C7InputKind;
using vocal_fx_platform::B4D1BlockRecord;

// §7 Warmup policy: 3 s (2250 blocks) is documented and excluded from every
// qualification statistic.  §9 hard-realtime gate uses the block deadline.
constexpr uint32_t kB4D1WarmupMs = 3000;
constexpr uint32_t kB4D1LateThresholdUs = 1200;
constexpr size_t kB4D1StageSeconds = 65;

float *s_b4d1_stage = nullptr;
double s_b4d1_audio_seconds = 0.0;
uint64_t s_b4d1_dma_errors = 0;
bool s_b4d1_fullfx_pass = false;
// B4D.3S: runtime sample rate / deadline / stage size.
uint32_t s_b4d1_rate = 48000;
double s_b4d1_deadline_us = 1333.3333333333;
size_t s_b4d1_stage_frames = kB4D1StageSeconds * 48000;
LpcTelemetry s_b4d1_lpc_start{};
// B4D.5 model-lookup sub-decomposition baseline (voice 0).
PsolaModelWarpAudit s_b4d5_warp_start{};
struct B4D1Pct {
  double avg = 0.0;
  unsigned p50 = 0, p90 = 0, p95 = 0, p99 = 0, p999 = 0, max = 0;
  unsigned samples = 0;
};

// Exact percentiles from the per-block recorder (1 us bins in PSRAM).  Runs
// only after the measured window, so its cost never touches the hot path.
B4D1Pct b4d1_percentiles(const uint16_t *values, size_t count) {
  B4D1Pct p{};
  p.samples = static_cast<uint32_t>(count);
  if (!count) return p;
  constexpr size_t kBins = 65536;
  uint32_t *hist = static_cast<uint32_t *>(
      heap_caps_calloc(kBins, sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!hist) return p;
  uint64_t sum = 0;
  uint32_t mx = 0;
  for (size_t i = 0; i < count; ++i) {
    hist[values[i]]++;
    sum += values[i];
    if (values[i] > mx) mx = values[i];
  }
  p.avg = static_cast<double>(sum) / static_cast<double>(count);
  p.max = mx;
  auto pct = [&](double q) -> unsigned {
    uint64_t target = static_cast<uint64_t>(std::ceil(q * static_cast<double>(count)));
    if (target < 1) target = 1;
    uint64_t acc = 0;
    for (size_t b = 0; b < kBins; ++b) {
      acc += hist[b];
      if (acc >= target) return static_cast<unsigned>(b);
    }
    return mx;
  };
  p.p50 = pct(0.50);
  p.p90 = pct(0.90);
  p.p95 = pct(0.95);
  p.p99 = pct(0.99);
  p.p999 = pct(0.999);
  heap_caps_free(hist);
  return p;
}

inline uint32_t b4d1_fnv(const float *buf, size_t frames) {
  uint32_t h = 2166136261U;
  for (size_t i = 0; i < frames; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &buf[i], sizeof(bits));
    h = (h ^ bits) * 16777619U;
  }
  return h;
}

inline double b4d1_rms(const float *buf, size_t frames) {
  double s = 0.0;
  for (size_t i = 0; i < frames; ++i) s += static_cast<double>(buf[i]) * buf[i];
  return frames ? std::sqrt(s / frames) : 0.0;
}

// §3 canonical production harmonizer + optional FX selection.  Identical DSP
// configuration to the B4C.8 qualified production path.
inline void b4d1_configure_dsp(int voices, bool comp, bool delay, bool reverb) {
  vocal_fx_set_psola_grain_render_mode(0, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_grain_render_mode(1, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_fir_kernel(0, PsolaFirKernel::MultiFma);
  vocal_fx_set_psola_fir_kernel(1, PsolaFirKernel::MultiFma);
  vocal_fx_configure_psola_residual_cache(0, 0, false, false);
  vocal_fx_configure_psola_residual_cache(1, 0, false, false);
  vocal_fx_set_psola_residual_cache_enabled(0, false);
  vocal_fx_set_psola_residual_cache_enabled(1, false);
  vocal_fx_set_psola_warp_cache(true, true, false);
  SharedLpcAnalysis::prepare_gainnorm_basis(static_cast<float>(s_b4d1_rate));
  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);
  s_audio.set_b4c1_transport_only(false);
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, comp ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, delay ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, reverb ? 1.0f : 0.0f);
  vocal_fx_set_harmony_enabled(0, voices >= 1);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_formant_mode(0, voices >= 1 ? FormantMode::Lpc : FormantMode::Off);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_harmony_enabled(1, voices >= 2);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_formant_mode(1, voices >= 2 ? FormantMode::Lpc : FormantMode::Off);
  vocal_fx_set_harmony_gain(1, 1.0f);
}

inline void b4d1_print_memory(const char *tag) {
  printf("B4D1_MEMORY tag=%s internal_free=%u internal_largest=%u "
         "spiram_free=%u spiram_largest=%u\n",
         tag,
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

inline void b4d1_print_stack() {
  const UBaseType_t a = s_audio_task_handle
                            ? uxTaskGetStackHighWaterMark(s_audio_task_handle)
                            : 0;
  const UBaseType_t p = s_pitch_task_handle
                            ? uxTaskGetStackHighWaterMark(s_pitch_task_handle)
                            : 0;
  printf("B4D1_STACK audio_hwm_bytes=%u pitch_hwm_bytes=%u\n",
         static_cast<unsigned>(a * sizeof(StackType_t)),
         static_cast<unsigned>(p * sizeof(StackType_t)));
}

// B4D.2 global DSP section attribution (non-overlapping profiler sections).
inline void b4d1_print_profiler_sections(const char *name) {
  struct Sec {
    VocalFxProfileSection s;
    const char *n;
  };
  const Sec secs[] = {
      {VocalFxProfileSection::PitchLpcTap, "pitch_lpc_tap"},
      {VocalFxProfileSection::ParameterQueue, "param_queue"},
      {VocalFxProfileSection::InputHpf, "input_hpf"},
      {VocalFxProfileSection::InputGate, "input_gate"},
      {VocalFxProfileSection::Compressor, "compressor"},
      {VocalFxProfileSection::PitchMarkSync, "pitch_mark_sync"},
      {VocalFxProfileSection::Harmony, "harmony_parent"},
      {VocalFxProfileSection::HarmonyVoice0, "harmony_voice0"},
      {VocalFxProfileSection::DryAlignment, "dry_alignment"},
      {VocalFxProfileSection::HarmonySlewPan, "harmony_slew_pan"},
      {VocalFxProfileSection::HarmonyLimiter, "harmony_limiter"},
      {VocalFxProfileSection::BusMixing, "bus_mixing"},
      {VocalFxProfileSection::DelayPrep, "delay_prep"},
      {VocalFxProfileSection::Delay, "delay"},
      {VocalFxProfileSection::ReverbPrep, "reverb_prep"},
      {VocalFxProfileSection::Reverb, "reverb"},
      {VocalFxProfileSection::MasterMix, "master_mix"},
      {VocalFxProfileSection::MasterLimiter, "master_limiter"},
  };
  for (const Sec &x : secs) {
    const VocalFxProfileStats st = vocal_fx_profile_stats(x.s);
    printf("B4D2_SECTION name=%s sec=%s calls=%llu avg_us=%.2f max_us=%llu\n",
           name, x.n, static_cast<unsigned long long>(st.blocks),
           st.blocks ? static_cast<double>(st.total_us) / st.blocks : 0.0,
           static_cast<unsigned long long>(st.worst_us));
  }
}

// B4D.2 per-voice TD-PSOLA section attribution (cumulative over the window).
inline void b4d1_print_voice_sections(const char *name) {
  struct Sec {
    PitchShiftProfileSection s;
    const char *n;
  };
  const Sec secs[] = {
      {PitchShiftProfileSection::MarkSelection, "mark_selection"},
      {PitchShiftProfileSection::GrainScheduling, "grain_scheduling"},
      {PitchShiftProfileSection::GrainHistoryLookup, "grain_history_lookup"},
      {PitchShiftProfileSection::PlainWindowOLA, "plain_window_ola"},
      {PitchShiftProfileSection::LpcResidualFIR, "lpc_residual_fir"},
      {PitchShiftProfileSection::LpcModelLookup, "lpc_model_lookup"},
      {PitchShiftProfileSection::LpcModelWarpPolynomial, "lpc_warp_poly"},
      {PitchShiftProfileSection::LpcModelWarpGainNorm, "lpc_warp_gainnorm"},
      {PitchShiftProfileSection::LpcWindowOLA, "lpc_window_ola"},
      {PitchShiftProfileSection::LpcSynthesisAllPole, "lpc_synthesis_iir"},
      {PitchShiftProfileSection::LpcStateShift, "lpc_state_shift"},
      {PitchShiftProfileSection::FormantGainMatcher, "formant_gain_match"},
      {PitchShiftProfileSection::FormantSoftClip, "formant_soft_clip"},
      {PitchShiftProfileSection::FormantBlend, "formant_blend"},
      {PitchShiftProfileSection::Fallback, "fallback"},
      {PitchShiftProfileSection::Articulation, "articulation"},
      {PitchShiftProfileSection::PlosiveBridge, "plosive_bridge"},
      {PitchShiftProfileSection::Telemetry, "telemetry"},
      {PitchShiftProfileSection::Other, "other"},
      {PitchShiftProfileSection::Total, "total"},
  };
  for (size_t v = 0; v < 2; ++v) {
    for (const Sec &x : secs) {
      const VocalFxProfileStats st = vocal_fx_voice_profile_stats(v, x.s);
      printf("B4D2_VOICE_SECTION name=%s voice=%zu sec=%s calls=%llu "
             "avg_us=%.2f max_us=%llu\n",
             name, v, x.n, static_cast<unsigned long long>(st.blocks),
             st.blocks ? static_cast<double>(st.total_us) / st.blocks : 0.0,
             static_cast<unsigned long long>(st.worst_us));
    }
  }
}

// B4D.4 per-grain-count section attribution (voice 0).
inline void b4d1_print_class_sections(const char *name) {
  static const char *sec_names[] = {
      "mark_selection",       "grain_scheduling",   "grain_history_lookup",
      "plain_window_ola",     "lpc_residual_fir",   "lpc_model_lookup",
      "lpc_warp_poly",        "lpc_warp_gainnorm",  "lpc_window_ola",
      "lpc_synthesis_iir",    "lpc_state_shift",    "formant_gain_match",
      "formant_soft_clip",    "formant_blend",      "fallback",
      "articulation",         "plosive_bridge",     "telemetry",
      "other",                "total"};
  for (size_t b = 0; b < 8; ++b) {
    const uint64_t blocks = vocal_fx_b4d4_class_blocks(b);
    if (!blocks) continue;
    for (size_t s = 0; s < 20; ++s) {
      const uint64_t cyc = vocal_fx_b4d4_class_cycles(b, s);
      printf("B4D4_CLASS_SECTION name=%s bucket=%zu mc=%zu grains=%zu "
             "blocks=%llu sec=%s avg_us=%.2f\n",
             name, b, b / 4, b % 4,
             static_cast<unsigned long long>(blocks), sec_names[s],
             static_cast<double>(cyc) / blocks /
                 static_cast<double>(Profiler::cycles_per_us()));
    }
  }
}

// B4D.2 reverb subsection breakdown (§19/§20). Cycles -> us at the profiled
// cycle rate; blocks/samples give the per-block and per-sample denominators.
inline void b4d1_print_reverb_profile(const char *name) {
  const VocalFxReverbProfile p = vocal_fx_reverb_profile();
  const double blocks = p.blocks ? static_cast<double>(p.blocks) : 1.0;
  const double samples = p.samples ? static_cast<double>(p.samples) : 1.0;
  const double cpus = static_cast<double>(Profiler::cycles_per_us());
  auto us_per_block = [&](uint64_t c) {
    return static_cast<double>(c) / cpus / blocks;
  };
  auto us_per_sample = [&](uint64_t c) {
    return static_cast<double>(c) / cpus / samples;
  };
  printf("B4D2_REVERB_PROFILE name=%s blocks=%llu samples=%llu "
         "read_damping_us=%.2f mix_us=%.2f hadamard_us=%.2f diffuser_us=%.2f "
         "write_index_us=%.2f wet_mix_us=%.2f total_us=%.2f\n",
         name, static_cast<unsigned long long>(p.blocks),
         static_cast<unsigned long long>(p.samples),
         us_per_block(p.read_damping_cycles), us_per_block(p.mix_cycles),
         us_per_block(p.hadamard_cycles), us_per_block(p.diffuser_cycles),
         us_per_block(p.write_index_cycles), us_per_block(p.wet_mix_cycles),
         us_per_block(p.total_cycles));
  printf("B4D2_REVERB_PROFILE_SAMPLE name=%s read_damping=%.4f mix=%.4f "
         "hadamard=%.4f diffuser=%.4f write_index=%.4f wet_mix=%.4f "
         "total=%.4f cycles_per_sample=%.2f\n",
         name, us_per_sample(p.read_damping_cycles), us_per_sample(p.mix_cycles),
         us_per_sample(p.hadamard_cycles), us_per_sample(p.diffuser_cycles),
         us_per_sample(p.write_index_cycles), us_per_sample(p.wet_mix_cycles),
         us_per_sample(p.total_cycles),
         samples ? static_cast<double>(p.total_cycles) / samples : 0.0);
}

// B4D.2 reverb buffer placement / memory audit (§21).
inline void b4d1_print_reverb_memory() {
  size_t total = 0;
  for (size_t i = 0; i < 8; ++i) {
    const size_t b = vocal_fx_reverb_line_bytes(i);
    total += b;
    printf("B4D2_REVERB_LINE i=%zu bytes=%zu psram=%d\n", i, b,
           vocal_fx_reverb_line_in_psram(i) ? 1 : 0);
  }
  for (size_t i = 0; i < 3; ++i) {
    const size_t b = vocal_fx_reverb_diffuser_bytes(i);
    total += b;
    printf("B4D2_REVERB_DIFFUSER i=%zu bytes=%zu psram=%d\n", i, b,
           vocal_fx_reverb_diffuser_in_psram(i) ? 1 : 0);
  }
  printf("B4D2_REVERB_MEMORY total_bytes=%zu reported=%zu\n", total,
         vocal_fx_reverb_total_bytes());
}

// Stage a deterministic production-equivalent input outside the timed path.
inline bool b4d1_prestage(const char *name, B4B6StimulusKind kind, float f0,
                          const uint8_t *mulaw, size_t mulaw_n, size_t frames) {
  if (!s_b4d1_stage || frames > s_b4d1_stage_frames) return false;
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  for (size_t pos = 0; pos < frames; pos += 4096) {
    const size_t chunk = (frames - pos > 4096) ? 4096 : (frames - pos);
    s_audio.generate_b4b6_mono(s_b4d1_stage + pos, chunk);
    vTaskDelay(1);
  }
  const uint32_t fnv = b4d1_fnv(s_b4d1_stage, frames);
  const double rms = b4d1_rms(s_b4d1_stage, frames);
  printf("B4D1_PRESTAGE name=%s kind=%d f0=%.1f frames=%u fnv=%08lx rms=%.6f\n",
         name, static_cast<int>(kind), static_cast<double>(f0),
         static_cast<unsigned>(frames), static_cast<unsigned long>(fnv), rms);
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  return true;
}

// Run one cleaned qualification window.
void b4d1_run_case(const char *name, uint32_t seconds, int voices, bool comp,
                   bool delay, bool reverb, bool forensic,
                   bool reverb_profile = false, bool class_profile = false) {
  vocal_fx_reset_profiling_epoch();
  vocal_fx_reset_slice_audit();
  s_audio.reset_counters();
  s_audio.set_b4c6a_detail(forensic ? 2 : 1);
  s_audio.set_b4c7_input(B4C7InputKind::Prestaged);
  s_audio.set_b4b6_dsp_paused(false);
  vocal_fx_set_slice_audit_enabled(false);
  vocal_fx_init_b4c8_recorders(true, false);
  vocal_fx_init_b4c8b_recorder();
  b4d1_configure_dsp(voices, comp, delay, reverb);
  if (class_profile) vocal_fx_b4d4_class_init();
  if (!s_audio.b4d1_recorder_init()) {
    printf("B4D1_CASE name=%s status=RECORDER_ALLOC_FAILED\n", name);
    return;
  }
  s_audio.b4d1_recorder_enable(false);
  if (!start_pipeline_tasks(AudioI2sMode::B4D1Qualification, true)) {
    printf("B4D1_CASE name=%s status=START_FAILED\n", name);
    return;
  }

  // §7 explicit warmup, excluded from every statistic.
  vTaskDelay(pdMS_TO_TICKS(kB4D1WarmupMs));

  // §7 reset everything at the qualification origin.
  s_audio.reset_counters();
  vocal_fx_reset_profiling_epoch();
  vocal_fx_reset_slice_audit();
  vocal_fx_reset_harmonizer_trace();
  vocal_fx_init_b4c8_recorders(true, false);
  vocal_fx_init_b4c8b_recorder();
  vocal_fx_set_reverb_profile_enabled(reverb_profile);
  vocal_fx_reset_reverb_profile();
  vocal_fx_reset_warp_cache_audit();
  vocal_fx_reset_measurement_telemetry();
  vocal_fx_reset_lpc_invalid_reasons();
  s_b4d1_lpc_start = vocal_fx_lpc_telemetry();
  // B4D.5 exact mark/model decomposition + engine global-class attribution.
  vocal_fx_b4d5_audit_reset();
  vocal_fx_b4d5_audit_enable(true);
  vocal_fx_b4d5_global_class_init();
  s_b4d5_warp_start = vocal_fx_get_psola_model_warp_audit(0);
  s_audio.b4d1_recorder_reset(0);
  s_audio.b4d1_recorder_enable(true);

  const int64_t begin = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000U));
  const double wall_s =
      static_cast<double>(esp_timer_get_time() - begin) / 1000000.0;
  s_audio.b4d1_recorder_enable(false);

  // Stack high-water must be read while the tasks are still alive.
  b4d1_print_stack();
  stop_pipeline_tasks(false);

  const size_t count = s_audio.b4d1_recorder_count();
  const auto &c = s_audio.counters();
  const double expected = wall_s * static_cast<double>(s_b4d1_rate) / 64.0;
  const double rx_frames = static_cast<double>(c.rx_frames_read.load());
  const double eff_fs = wall_s > 0.0 ? rx_frames / wall_s : 0.0;

  printf("B4D1_CASE name=%s voices=%d comp=%d delay=%d reverb=%d forensic=%d "
         "wall_s=%.6f blocks=%zu expected_blocks=%.3f\n",
         name, voices, comp ? 1 : 0, delay ? 1 : 0, reverb ? 1 : 0,
         forensic ? 1 : 0, wall_s, count, expected);
  printf("B4D1_PHYSICAL name=%s rx_blocks=%llu tx_blocks=%llu dsp_blocks=%llu "
         "rx_frames=%llu tx_frames=%llu effective_fs=%.3f err_pct=%.4f\n",         name,
         static_cast<unsigned long long>(c.rx_blocks_read.load()),
         static_cast<unsigned long long>(c.tx_blocks_written.load()),
         static_cast<unsigned long long>(c.audio_blocks_processed.load()),
         static_cast<unsigned long long>(c.rx_frames_read.load()),
         static_cast<unsigned long long>(c.tx_frames_written.load()), eff_fs,
         expected > 0.0 ? (static_cast<double>(c.rx_blocks_read.load()) - expected) /
                              expected * 100.0
                        : 0.0);
  printf("B4D1_TXHASH name=%s tx_frames=%llu tx_pcm_fnv=%08lx\n", name,
         static_cast<unsigned long long>(c.tx_frames_written.load()),
         static_cast<unsigned long>(c.tx_pcm_fnv.load()));
  printf("B4D1_TRANSPORT name=%s rx_overruns=%llu tx_underruns=%llu "
         "dma_errors=%llu actual_rx_dma_errors=%llu actual_tx_dma_errors=%llu "
         "read_failures=%llu write_failures=%llu rx_dropped_frames=%llu "
         "tx_dropped_frames=%llu rx_seq_gaps=%llu tx_seq_gaps=%llu nan_inf=%llu\n",
         name,
         static_cast<unsigned long long>(c.rx_overruns.load()),
         static_cast<unsigned long long>(c.tx_underruns.load()),
         static_cast<unsigned long long>(c.dma_errors.load()),
         static_cast<unsigned long long>(c.actual_rx_dma_errors.load()),
         static_cast<unsigned long long>(c.actual_tx_dma_errors.load()),
         static_cast<unsigned long long>(c.i2s_read_failures.load()),
         static_cast<unsigned long long>(c.i2s_write_failures.load()),
         static_cast<unsigned long long>(c.rx_dropped_frames.load()),
         static_cast<unsigned long long>(c.tx_dropped_frames.load()),
         static_cast<unsigned long long>(c.rx_sequence_gaps.load()),
         static_cast<unsigned long long>(c.tx_sequence_gaps.load()),
         static_cast<unsigned long long>(c.nan_inf_count.load()));

  // Per-block percentiles and MC x miss contingency from observed blocks.
  B4D1Pct dsp{}, loop{};
  uint64_t mc_miss = 0, mc_ok = 0, nmc_miss = 0, nmc_ok = 0;
  uint64_t dsp_miss = 0, loop_miss = 0, late = 0;
  uint64_t mc_dsp_sum = 0, nmc_dsp_sum = 0;
  // B4D.3 exact block-level population: [mc][grain bucket 0..3] and
  // [mc][exp_warp] block/miss counts.
  uint64_t grain_pop[2][4] = {}, grain_miss[2][4] = {};
  uint64_t exp_pop[2][2] = {}, exp_miss[2][2] = {};
  uint64_t grain_new_sum = 0, active_desc_sum = 0, slices_sum = 0;
  // B4D.4 block classes A..E: NMC0, NMC1, MC1, MC2, MC3+.
  constexpr int kClasses = 5;
  uint16_t *cls_dsp[kClasses] = {};
  size_t cls_count[kClasses] = {};
  if (count) {
    for (int c = 0; c < kClasses; ++c) {
      cls_dsp[c] = static_cast<uint16_t *>(heap_caps_malloc(
          count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
  }
  if (count) {
    uint16_t *dv = static_cast<uint16_t *>(
        heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint16_t *lv = static_cast<uint16_t *>(
        heap_caps_malloc(count * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (dv && lv) {
      for (size_t i = 0; i < count; ++i) {
        B4D1BlockRecord rec;
        if (!s_audio.b4d1_recorder_get(i, &rec)) break;
        dv[i] = rec.dsp_us;
        lv[i] = rec.loop_us;
        if (rec.dsp_miss) dsp_miss++;
        if (rec.loop_miss) loop_miss++;
        if (rec.dsp_us > kB4D1LateThresholdUs) late++;
        if (rec.model_changed) {
          if (rec.dsp_miss) mc_miss++; else mc_ok++;
          mc_dsp_sum += rec.dsp_us;
        } else {
          if (rec.dsp_miss) nmc_miss++; else nmc_ok++;
          nmc_dsp_sum += rec.dsp_us;
        }
        uint8_t ng = rec.new_grains;
        // exp_warp is a call count; bucket it to a binary flag so the
        // [2][2] accumulator stays in bounds.
        uint8_t expw = rec.exp_warp ? 1 : 0;
        const uint8_t gb = ng >= 3 ? 3 : ng;
        const uint8_t mci = rec.model_changed ? 1 : 0;
        grain_pop[mci][gb]++;
        if (rec.dsp_miss) grain_miss[mci][gb]++;
        exp_pop[mci][expw]++;
        if (rec.dsp_miss) exp_miss[mci][expw]++;
        grain_new_sum += rec.new_grains;
        active_desc_sum += rec.active_desc;
        slices_sum += rec.slices;
        int cls = -1;
        if (mci == 0 && ng == 0) cls = 0;
        else if (mci == 0 && ng == 1) cls = 1;
        else if (mci == 1 && ng == 1) cls = 2;
        else if (mci == 1 && ng == 2) cls = 3;
        else if (mci == 1 && ng >= 3) cls = 4;
        if (cls >= 0 && cls_dsp[cls] && cls_count[cls] < count)
          cls_dsp[cls][cls_count[cls]++] = rec.dsp_us;
      }
      dsp = b4d1_percentiles(dv, count);
      loop = b4d1_percentiles(lv, count);
    }
    if (dv) heap_caps_free(dv);
    if (lv) heap_caps_free(lv);
  }

  // B4D.4 per-class DSP timing distribution.
  {
    static const char *kClsName[kClasses] = {"A_nmc0g", "B_nmc1g", "C_mc1g",
                                             "D_mc2g", "E_mc3pg"};
    for (int c = 0; c < kClasses; ++c) {
      if (!cls_dsp[c]) continue;
      const B4D1Pct p = b4d1_percentiles(cls_dsp[c], cls_count[c]);
      const size_t n = cls_count[c];
      uint64_t miss = 0;
      for (size_t i = 0; i < n; ++i)
        if (cls_dsp[c][i] > s_b4d1_deadline_us) ++miss;
      printf("B4D4_CLASS name=%s cls=%s count=%zu avg=%.2f p50=%u p90=%u "
             "p95=%u p99=%u p999=%u max=%u misses=%llu miss_pct=%.3f\n",
             name, kClsName[c], n, p.avg, p.p50, p.p90, p.p95, p.p99, p.p999,
             p.max, static_cast<unsigned long long>(miss),
             n ? 100.0 * static_cast<double>(miss) / n : 0.0);
      heap_caps_free(cls_dsp[c]);
      cls_dsp[c] = nullptr;
    }
  }

  printf("B4D1_DSP name=%s avg=%.2f p50=%u p90=%u p95=%u p99=%u p999=%u max=%u "
         "samples=%u misses=%llu\n",
         name, dsp.avg, dsp.p50, dsp.p90, dsp.p95, dsp.p99, dsp.p999, dsp.max,
         dsp.samples, static_cast<unsigned long long>(dsp_miss));
  printf("B4D1_LOOP name=%s avg=%.2f p50=%u p90=%u p95=%u p99=%u p999=%u max=%u "
         "samples=%u misses=%llu\n",
         name, loop.avg, loop.p50, loop.p90, loop.p95, loop.p99, loop.p999,
         loop.max, loop.samples, static_cast<unsigned long long>(loop_miss));
  {
    const auto pp = s_audio.b4c6a_period_percentiles();
    printf("B4D1_LOOPPERIOD name=%s avg=%.2f p50=%u p90=%u p95=%u p99=%u "
           "p999=%u max=%u samples=%u\n",
           name, pp.avg_us, static_cast<unsigned>(pp.p50_us),
           static_cast<unsigned>(pp.p90_us), static_cast<unsigned>(pp.p95_us),
           static_cast<unsigned>(pp.p99_us),
           static_cast<unsigned>(pp.p99_9_us),
           static_cast<unsigned>(pp.max_us),
           static_cast<unsigned>(pp.samples));
    printf("B4D1_DEADLINES name=%s dsp_misses=%llu audio_misses=%llu "
           "transport_misses=%llu max_consecutive_late=%llu "
           "max_lateness_us=%llu cumulative_lateness_us=%llu\n",
           name,
           static_cast<unsigned long long>(c.dsp_deadline_misses.load()),
           static_cast<unsigned long long>(c.audio_deadline_misses.load()),
           static_cast<unsigned long long>(c.transport_deadline_misses.load()),
           static_cast<unsigned long long>(c.max_consecutive_late_blocks.load()),
           static_cast<unsigned long long>(c.max_single_block_lateness_us.load()),
           static_cast<unsigned long long>(c.cumulative_lateness_us.load()));
  }
  printf("B4D1_LATE name=%s threshold_us=%u count=%llu\n", name,
         static_cast<unsigned>(kB4D1LateThresholdUs),
         static_cast<unsigned long long>(late));

  const uint64_t mc_blocks = mc_miss + mc_ok;
  const uint64_t nmc_blocks = nmc_miss + nmc_ok;
  const uint64_t total_miss = mc_miss + nmc_miss;
  printf("B4D1_MC name=%s mc_blocks=%llu nmc_blocks=%llu mc_miss=%llu "
         "mc_nomiss=%llu nmc_miss=%llu nmc_nomiss=%llu "
         "p_miss_given_mc=%.6f p_miss_given_nmc=%.6f p_mc_given_miss=%.6f "
         "mc_dsp_avg=%.2f nmc_dsp_avg=%.2f\n",
         name, static_cast<unsigned long long>(mc_blocks),
         static_cast<unsigned long long>(nmc_blocks),
         static_cast<unsigned long long>(mc_miss),
         static_cast<unsigned long long>(mc_ok),
         static_cast<unsigned long long>(nmc_miss),
         static_cast<unsigned long long>(nmc_ok),
         mc_blocks ? static_cast<double>(mc_miss) / mc_blocks : 0.0,
         nmc_blocks ? static_cast<double>(nmc_miss) / nmc_blocks : 0.0,
         total_miss ? static_cast<double>(mc_miss) / total_miss : 0.0,
         mc_blocks ? static_cast<double>(mc_dsp_sum) / mc_blocks : 0.0,
         nmc_blocks ? static_cast<double>(nmc_dsp_sum) / nmc_blocks : 0.0);

  // B4D.3 exact grain-count population and exp_warp statistics.
  for (int mci = 0; mci < 2; ++mci) {
    for (int gb = 0; gb < 4; ++gb) {
      const uint64_t tot = grain_pop[mci][gb];
      const uint64_t ms = grain_miss[mci][gb];
      printf("B4D3_MC_GRAIN name=%s mc=%d grains=%d blocks=%llu misses=%llu "
             "p_miss=%.6f\n",
             name, mci, gb, static_cast<unsigned long long>(tot),
             static_cast<unsigned long long>(ms),
             tot ? static_cast<double>(ms) / tot : 0.0);
    }
  }
  for (int mci = 0; mci < 2; ++mci) {
    for (int ew = 0; ew < 2; ++ew) {
      const uint64_t tot = exp_pop[mci][ew];
      const uint64_t ms = exp_miss[mci][ew];
      printf("B4D3_EXP_WARP name=%s mc=%d exp_warp=%d blocks=%llu misses=%llu "
             "p_miss=%.6f\n",
             name, mci, ew, static_cast<unsigned long long>(tot),
             static_cast<unsigned long long>(ms),
             tot ? static_cast<double>(ms) / tot : 0.0);
    }
  }
  {
    const uint64_t mc_ew = exp_pop[1][1];
    const uint64_t mc_ew_miss = exp_miss[1][1];
    const uint64_t any_miss = total_miss;
    printf("B4D3_MC_EXPWARP_SUMMARY name=%s p_expwarp_given_mc=%.6f "
           "p_miss_given_mc_expwarp=%.6f p_mc_expwarp_given_miss=%.6f\n",
           name,
           mc_blocks ? static_cast<double>(mc_ew) / mc_blocks : 0.0,
           mc_ew ? static_cast<double>(mc_ew_miss) / mc_ew : 0.0,
           any_miss ? static_cast<double>(mc_ew_miss) / any_miss : 0.0);
  }

  // B4D.3S harmonizer geometry and analysis-core load per rate.
  {
    // Authoritative reconciliation: every row comes from the same window.
    uint64_t gm = 0, em = 0, gb_total = 0, eb_total = 0;
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 4; ++b) {
        gm += grain_miss[a][b];
        gb_total += grain_pop[a][b];
      }
    for (int a = 0; a < 2; ++a)
      for (int b = 0; b < 2; ++b) {
        em += exp_miss[a][b];
        eb_total += exp_pop[a][b];
      }
    const bool ok = (gm == dsp_miss) && (em == dsp_miss) &&
                    (gb_total == count) && (eb_total == count);
    printf("B4D3S_RECONCILE name=%s dsp_miss=%llu grain_miss_sum=%llu "
           "exp_miss_sum=%llu grain_blocks_sum=%llu exp_blocks_sum=%llu "
           "count=%zu status=%s\n",
           name, static_cast<unsigned long long>(dsp_miss),
           static_cast<unsigned long long>(gm),
           static_cast<unsigned long long>(em),
           static_cast<unsigned long long>(gb_total),
           static_cast<unsigned long long>(eb_total), count, ok ? "OK" : "MISMATCH");
  }
  {
    const double secs = wall_s > 0.0 ? wall_s : 1.0;
    const VocalFxProfileStats gn =
        vocal_fx_voice_profile_stats(0, PitchShiftProfileSection::LpcModelWarpGainNorm);
    const LpcTelemetry lt = vocal_fx_lpc_telemetry();
    const PitchAnalysisAuditTelemetry pa =
        vocal_fx_pitch_analysis_audit_telemetry();
    const uint64_t lpc_frames = lt.lpc_frames - s_b4d1_lpc_start.lpc_frames;
    const uint64_t lpc_invalid =
        lt.lpc_invalid_frames - s_b4d1_lpc_start.lpc_invalid_frames;
    const uint64_t lpc_fallback =
        lt.lpc_fallback_frames - s_b4d1_lpc_start.lpc_fallback_frames;
    uint64_t lpc_unvoiced = 0, lpc_solve_fail = 0;
    vocal_fx_lpc_invalid_reasons(&lpc_unvoiced, &lpc_solve_fail);
    printf("B4D3S_GEOMETRY name=%s rate=%u avg_new_grains=%.4f "
           "avg_active_desc=%.4f avg_slices=%.4f mc_per_s=%.2f "
           "gainnorm_per_s=%.2f gainnorm_us_per_call=%.2f\n",
           name, static_cast<unsigned>(s_b4d1_rate),
           count ? static_cast<double>(grain_new_sum) / count : 0.0,
           count ? static_cast<double>(active_desc_sum) / count : 0.0,
           count ? static_cast<double>(slices_sum) / count : 0.0,
           static_cast<double>(mc_blocks) / secs,
           static_cast<double>(gn.blocks) / secs,
           gn.blocks ? static_cast<double>(gn.total_us) / gn.blocks : 0.0);
    // B4D.4 analysis-core gate: sample-domain pitch age (not wall-clock).
    printf("B4D4_PITCH_AGE name=%s rate=%u avg_ms=%.3f p50_ms=%.3f p95_ms=%.3f "
           "p99_ms=%.3f max_ms=%.3f latest_ms=%.3f backlog_ms=%.3f "
           "backlog_max_ms=%.3f backlog_avg_ms=%.3f backlog_p95_ms=%.3f "
           "backlog_p99_ms=%.3f latency_samples=%llu\n",
           name, static_cast<unsigned>(s_b4d1_rate), pa.pitch_age_average_ms,
           pa.pitch_age_p50_ms, pa.pitch_age_p95_ms, pa.pitch_age_p99_ms,
           pa.pitch_age_max_ms, pa.latest_pitch_age_ms, pa.analysis_backlog_ms,
           pa.analysis_backlog_max_ms, pa.analysis_backlog_average_ms,
           pa.analysis_backlog_p95_ms, pa.analysis_backlog_p99_ms,
           static_cast<unsigned long long>(pa.algorithmic_latency_samples));
    {
      // B4D.5 §3: the age average/percentiles/max must come from the SAME
      // sample set and be mutually consistent (avg <= max, P50 <= P95 <= P99
      // <= max).  The old general-audit histogram used 54.4 ms bins and
      // reported the bin upper edge, so P99 (54.42) could exceed the true max
      // (30.57).  Fail the harness analysis if the invariant is violated.
      const bool age_order =
          pa.pitch_age_average_ms <= pa.pitch_age_max_ms + 1e-3f &&
          pa.pitch_age_p50_ms <= pa.pitch_age_p95_ms + 1e-3f &&
          pa.pitch_age_p95_ms <= pa.pitch_age_p99_ms + 1e-3f &&
          pa.pitch_age_p99_ms <= pa.pitch_age_max_ms + 1e-3f;
      const bool backlog_order =
          pa.analysis_backlog_average_ms <= pa.analysis_backlog_max_ms + 1e-3f &&
          pa.analysis_backlog_p95_ms <= pa.analysis_backlog_p99_ms + 1e-3f &&
          pa.analysis_backlog_p99_ms <= pa.analysis_backlog_max_ms + 1e-3f;
      printf("B4D5_AGE_GATE name=%s avg=%.3f p50=%.3f p95=%.3f p99=%.3f "
             "max=%.3f age_valid=%d backlog_avg=%.3f backlog_p95=%.3f "
             "backlog_p99=%.3f backlog_max=%.3f backlog_valid=%d result=%s\n",
             name, pa.pitch_age_average_ms, pa.pitch_age_p50_ms,
             pa.pitch_age_p95_ms, pa.pitch_age_p99_ms, pa.pitch_age_max_ms,
             age_order ? 1 : 0, pa.analysis_backlog_average_ms,
             pa.analysis_backlog_p95_ms, pa.analysis_backlog_p99_ms,
             pa.analysis_backlog_max_ms, backlog_order ? 1 : 0,
             (age_order && backlog_order) ? "PASS" : "FAIL");
    }
    printf("B4D4_ANALYSIS name=%s rate=%u lpc_frames=%llu lpc_invalid=%llu "
           "lpc_fallback=%llu lpc_frames_per_s=%.2f invalid_pct=%.3f "
           "unvoiced=%llu solve_fail=%llu\n",
           name, static_cast<unsigned>(s_b4d1_rate),
           static_cast<unsigned long long>(lpc_frames),
           static_cast<unsigned long long>(lpc_invalid),
           static_cast<unsigned long long>(lpc_fallback),
           static_cast<double>(lpc_frames) / secs,
           lpc_frames ? 100.0 * static_cast<double>(lpc_invalid) / lpc_frames
                      : 0.0,
           static_cast<unsigned long long>(lpc_unvoiced),
           static_cast<unsigned long long>(lpc_solve_fail));
    printf("B4D4_FIFO name=%s rate=%u pushes=%llu pops=%llu drops=%llu "
           "overflow_attempts=%llu max_occ=%u avg_occ=%.2f current=%u\n",
           name, static_cast<unsigned>(s_b4d1_rate),
           static_cast<unsigned long long>(pa.fifo_pushes),
           static_cast<unsigned long long>(pa.fifo_pops),
           static_cast<unsigned long long>(pa.fifo_drops),
           static_cast<unsigned long long>(pa.fifo_overflow_attempts),
           static_cast<unsigned>(pa.fifo_maximum_occupancy),
           static_cast<double>(pa.fifo_average_occupancy),
           static_cast<unsigned>(pa.fifo_current_occupancy));
  }

  // Bounded late-block list with voice0 work counters joined by sequence.
  {
    size_t printed = 0;
    for (size_t i = 0; i < count && printed < 200; ++i) {
      B4D1BlockRecord rec;
      if (!s_audio.b4d1_recorder_get(i, &rec)) break;
      if (rec.dsp_us <= kB4D1LateThresholdUs) continue;
      uint8_t ng = rec.new_grains;
      uint8_t ad = rec.active_desc;
      uint8_t ds = rec.slices;
      uint8_t exp = rec.exp_warp;
      uint16_t fir = 0;
      float f0 = 0.0f;
      printf("B4D1_LATE_BLOCK name=%s blk=%u dsp_us=%u loop_us=%u mc=%u "
             "new_grains=%u active_desc=%u slices=%u fir=%u exp_warp=%u "
             "att=%u sch_us=%.1f ag_us=%.1f df_us=%.1f mk_us=%.1f "
             "wp_us=%.1f dc_us=%.1f nr_us=%.1f po_us=%.1f gn_us=%.1f "
             "cl_us=%.1f ch_us=%.1f mn=%u wh=%u wm=%u f0=%.1f\n",
             name, static_cast<unsigned>(rec.block_id),
             static_cast<unsigned>(rec.dsp_us),
             static_cast<unsigned>(rec.loop_us),
             static_cast<unsigned>(rec.model_changed),
             static_cast<unsigned>(ng), static_cast<unsigned>(ad),
             static_cast<unsigned>(ds), static_cast<unsigned>(fir),
             static_cast<unsigned>(exp),
             static_cast<unsigned>(rec.schedule_attempts),
             static_cast<double>(rec.sched_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.addgrain_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.deferred_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.mark_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.warp_phase_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.desc_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.near_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.poly_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.gn_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.cl_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<double>(rec.ch_cycles) /
                 static_cast<double>(Profiler::cycles_per_us()),
             static_cast<unsigned>(rec.model_new_count),
             static_cast<unsigned>(rec.warp_hit_count),
             static_cast<unsigned>(rec.warp_miss_count),
             static_cast<double>(f0));
      if (rec.new_grains >= 2) {
        const double cpu = static_cast<double>(Profiler::cycles_per_us());
        printf("B4D8_ORD name=%s blk=%u "
               "g0_mk=%.1f g0_wp=%.1f g0_nr=%.1f g0_dc=%.1f "
               "g1_mk=%.1f g1_wp=%.1f g1_nr=%.1f g1_dc=%.1f "
               "g2_mk=%.1f g2_wp=%.1f g2_nr=%.1f g2_dc=%.1f mc=%u "
               "g0_sel=%.1f g0_al=%.1f g1_sel=%.1f g1_al=%.1f pw=%.1f "
               "debt=%d op=%.1f g0=%d,%u g1=%d,%u g2=%d,%u g3=%d,%u\n",
               name, static_cast<unsigned>(rec.block_id),
               rec.ord_mark[0] / cpu, rec.ord_warp[0] / cpu,
               rec.ord_near[0] / cpu, rec.ord_desc[0] / cpu,
               rec.ord_mark[1] / cpu, rec.ord_warp[1] / cpu,
               rec.ord_near[1] / cpu, rec.ord_desc[1] / cpu,
               rec.ord_mark[2] / cpu, rec.ord_warp[2] / cpu,
               rec.ord_near[2] / cpu, rec.ord_desc[2] / cpu,
               static_cast<unsigned>(rec.mark_count),
               rec.ord_sel[0] / cpu, rec.ord_align[0] / cpu,
               rec.ord_sel[1] / cpu, rec.ord_align[1] / cpu,
               rec.prewarm_cycles / cpu,
               static_cast<int>(rec.debt_samples),
               static_cast<double>(rec.output_period_q8) / 256.0,
               static_cast<int>(rec.g_dest[0]),
               static_cast<unsigned>(rec.g_half[0]),
               static_cast<int>(rec.g_dest[1]),
               static_cast<unsigned>(rec.g_half[1]),
               static_cast<int>(rec.g_dest[2]),
               static_cast<unsigned>(rec.g_half[2]),
               static_cast<int>(rec.g_dest[3]),
               static_cast<unsigned>(rec.g_half[3]));
      }
      printed++;
    }
  }

  // §13/§16 per-category voice attribution and MC vs NMC cycle comparison so
  // the largest measured contributor to the miss blocks is explicit.
  {
    printf("B4D1_MC_CYCLES name=%s mc_blocks=%llu mc_cycles=%llu "
           "mc_max=%llu nmc_blocks=%llu nmc_cycles=%llu nmc_max=%llu\n",
           name,
           static_cast<unsigned long long>(vocal_fx_model_change_blocks(0)),
           static_cast<unsigned long long>(vocal_fx_model_change_cycles(0)),
           static_cast<unsigned long long>(vocal_fx_model_change_max_cycles(0)),
           static_cast<unsigned long long>(vocal_fx_no_model_change_blocks(0)),
           static_cast<unsigned long long>(vocal_fx_no_model_change_cycles(0)),
           static_cast<unsigned long long>(vocal_fx_no_model_change_max_cycles(0)));
    for (size_t cat = 0; cat < static_cast<size_t>(B4c8OtherCategory::Count);
         ++cat) {
      printf("B4D1_VOICE_CAT name=%s cat=%zu cycles=%llu\n", name, cat,
             static_cast<unsigned long long>(vocal_fx_voice_other_cycles(0, cat)));
    }
    printf("B4D1_DEFERRED name=%s zero_calls=%llu zero_cycles=%llu "
           "active_calls=%llu active_cycles=%llu\n",
           name,
           static_cast<unsigned long long>(vocal_fx_deferred_zero_calls(0)),
           static_cast<unsigned long long>(vocal_fx_deferred_zero_cycles(0)),
           static_cast<unsigned long long>(vocal_fx_deferred_active_calls(0)),
           static_cast<unsigned long long>(vocal_fx_deferred_active_cycles(0)));
  }

  // TX-prep breakdown (§23) from the B4C.7 totals.
  if (const auto *tt = s_audio.b4c7_totals(); tt && count) {
    const double n = static_cast<double>(count);
    printf("B4D1_TXPREP name=%s ramp_avg=%.2f sanity_encode_avg=%.2f rms_avg=%.2f "
           "total_avg=%.2f ramp_max=%llu sanity_encode_max=%llu rms_max=%llu\n",
           name, tt->txp_ramp / n, tt->txp_sanity_encode / n, tt->txp_rms / n,
           (tt->txp_ramp + tt->txp_sanity_encode + tt->txp_rms) / n,
           static_cast<unsigned long long>(tt->txp_ramp_max),
           static_cast<unsigned long long>(tt->txp_sanity_encode_max),
           static_cast<unsigned long long>(tt->txp_rms_max));
  }

  // §11/§13 loop reconciliation and section averages from the B4C.6A engine.
  {
    const auto &t = s_audio.b4c6a_totals();
    if (t.samples) {
      const double n = static_cast<double>(t.samples);
      const double accounted =
          static_cast<double>(t.rx_wait_us + t.rx_copy_us + t.fixture_us +
                              t.dsp_us + t.tx_prep_us + t.tx_wait_us +
                              t.bookkeeping_us);
      printf("B4D1_SECTIONS name=%s samples=%llu rx_wait=%.2f rx_copy=%.2f "
             "fixture=%.2f dsp=%.2f tx_prep=%.2f tx_wait=%.2f book=%.2f "
             "other=%.2f loop_total=%.2f recon=%.4f\n",
             name, static_cast<unsigned long long>(t.samples), t.rx_wait_us / n,
             t.rx_copy_us / n, t.fixture_us / n, t.dsp_us / n,
             t.tx_prep_us / n, t.tx_wait_us / n, t.bookkeeping_us / n,
             t.other_us / n, t.loop_total_us / n,
             accounted / static_cast<double>(t.loop_total_us));
    }
  }

  // Bounded slow-block (voice >1200 us) forensic records.
  {
    const size_t sc = vocal_fx_slow_block_count(0);
    printf("B4D1_SLOW_BLOCKS name=%s voice=0 count=%zu\n", name, sc);
    for (size_t i = 0; i < sc && i < 200; ++i) {
      B4c8SlowBlockRecord r;
      if (!vocal_fx_get_slow_block(0, i, &r)) break;
      printf("B4D1_SLOW_BLOCK name=%s idx=%zu dur_us=%lu blk=%lu grn=%u desc=%u "
             "slc=%u src=%u fir=%u model=%u f0=%.1f\n",
             name, i, static_cast<unsigned long>(r.block_duration_us),
             static_cast<unsigned long>(r.block_index), r.new_grains_v0,
             r.active_descriptors_v0, r.deferred_slices_v0, r.source_samples_v0,
             r.fir_samples_v0, r.model_changes, static_cast<double>(r.pitch_f0));
    }
  }

  const bool b4d3_fs_ok =
      std::fabs(eff_fs - static_cast<double>(s_b4d1_rate)) <=
      static_cast<double>(s_b4d1_rate) * 0.005;
  const bool b4d3_dma_ok =
      c.dma_errors.load() == 0 && c.actual_rx_dma_errors.load() == 0 &&
      c.actual_tx_dma_errors.load() == 0 && c.i2s_read_failures.load() == 0 &&
      c.i2s_write_failures.load() == 0;
  const bool b4d3_partial_ok =
      c.rx_dropped_frames.load() == 0 && c.tx_dropped_frames.load() == 0;
  const bool b4d3_pass = b4d3_fs_ok && b4d3_dma_ok && c.rx_overruns.load() == 0 &&
                         dsp_miss == 0 && loop_miss == 0 && b4d3_partial_ok;
  if (std::strncmp(name, "1v_fullfx", 9) == 0) s_b4d1_fullfx_pass = b4d3_pass;
  printf("B4D1_QUALIFY name=%s fs_ok=%d dma_ok=%d dsp_miss_ok=%d loop_miss_ok=%d "
         "backlog_ok=%d partial_ok=%d result=%s\n",
         name, b4d3_fs_ok ? 1 : 0, b4d3_dma_ok ? 1 : 0, dsp_miss == 0 ? 1 : 0,
         loop_miss == 0 ? 1 : 0, c.rx_overruns.load() == 0 ? 1 : 0,
         b4d3_partial_ok ? 1 : 0, b4d3_pass ? "PASS" : "FAIL");

  // B4D.2 global DSP section attribution and reverb subsection profile.
  b4d1_print_profiler_sections(name);
  b4d1_print_voice_sections(name);
  if (class_profile) {
    b4d1_print_class_sections(name);
    vocal_fx_b4d4_class_free();
  }
  {
    uint64_t wm = 0, wo = 0;
    vocal_fx_warp_cache_audit(&wm, &wo);
    printf("B4D2_WARP_AUDIT name=%s miss_total=%llu math_only_miss=%llu "
           "math_only_pct=%.4f\n",
           name, static_cast<unsigned long long>(wm),
           static_cast<unsigned long long>(wo),
           wm ? 100.0 * static_cast<double>(wo) / static_cast<double>(wm) : 0.0);
  }
  if (reverb_profile) {
    b4d1_print_reverb_profile(name);
    b4d1_print_reverb_memory();
  }

  // B4D.5 exact mark/model decomposition and engine global-class attribution.
  vocal_fx_b4d5_audit_enable(false);
  vocal_fx_b4d5_global_class_free();
  {
    uint64_t mc = 0, cand = 0, scan = 0, tot = 0;
    vocal_fx_b4d5_mark_audit(&mc, &cand, &scan, &tot);
    const double cpu = static_cast<double>(Profiler::cycles_per_us());
    const double calls = mc ? static_cast<double>(mc) : 1.0;
    printf("B4D5_MARK_BREAKDOWN name=%s calls=%llu candidates=%llu "
           "candidates_per_call=%.2f scan_us_per_call=%.3f "
           "total_us_per_call=%.3f validity_us_per_call=%.3f "
           "us_per_new_grain=%.3f\n",
           name, static_cast<unsigned long long>(mc),
           static_cast<unsigned long long>(cand),
           static_cast<double>(cand) / calls,
           static_cast<double>(scan) / calls / cpu,
           static_cast<double>(tot) / calls / cpu,
           static_cast<double>(tot > scan ? tot - scan : 0) / calls / cpu,
           grain_new_sum ? static_cast<double>(tot) / cpu /
                               static_cast<double>(grain_new_sum)
                         : 0.0);
  }
  {
    uint64_t mc = 0, cand = 0, scan = 0, copies = 0, rep = 0;
    vocal_fx_b4d5_model_audit(&mc, &cand, &scan, &copies, &rep);
    const double cpu = static_cast<double>(Profiler::cycles_per_us());
    const double calls = mc ? static_cast<double>(mc) : 1.0;
    printf("B4D5_MODEL_BREAKDOWN name=%s calls=%llu candidates=%llu "
           "candidates_per_call=%.2f scan_us_per_call=%.3f copies=%llu "
           "copies_per_call=%.3f repeat_copies=%llu bytes_per_lookup=%.1f\n",
           name, static_cast<unsigned long long>(mc),
           static_cast<unsigned long long>(cand),
           static_cast<double>(cand) / calls,
           static_cast<double>(scan) / calls / cpu,
           static_cast<unsigned long long>(copies),
           static_cast<double>(copies) / calls,
           static_cast<unsigned long long>(rep),
           static_cast<double>(copies) / calls * 68.0);
  }
  {
    const auto wa = vocal_fx_get_psola_model_warp_audit(0);
    const double cpu = static_cast<double>(Profiler::cycles_per_us());
    auto d = [](uint64_t a, uint64_t b) { return a > b ? a - b : 0; };
    printf("B4D5_MODEL_WARP name=%s near_calls=%llu near_us=%.2f "
           "cache_lookup_us=%.2f cache_hit_us=%.2f coeff_copy_us=%.2f "
           "warp_poly_us=%.2f gain_norm_us=%.2f miss_calls=%llu\n",
           name, static_cast<unsigned long long>(d(
                      wa.model_near_calls, s_b4d5_warp_start.model_near_calls)),
           d(wa.model_near_cycles, s_b4d5_warp_start.model_near_cycles) / cpu,
           d(wa.cache_lookup_cycles, s_b4d5_warp_start.cache_lookup_cycles) /
               cpu,
           d(wa.cache_hit_cycles, s_b4d5_warp_start.cache_hit_cycles) / cpu,
           d(wa.coeff_copy_cycles, s_b4d5_warp_start.coeff_copy_cycles) / cpu,
           d(wa.warp_poly_cycles, s_b4d5_warp_start.warp_poly_cycles) / cpu,
           d(wa.gain_norm_cycles, s_b4d5_warp_start.gain_norm_cycles) / cpu,
           static_cast<unsigned long long>(d(
               wa.cache_miss_calls, s_b4d5_warp_start.cache_miss_calls)));
  }
  for (size_t b = 0; b < 4; ++b) {
    const uint64_t blocks = vocal_fx_b4d5_global_class_blocks(b);
    if (!blocks) continue;
    for (size_t g = 0; g < 7; ++g) {
      printf("B4D5_GLOBAL_CLASS name=%s bucket=%zu blocks=%llu group=%s "
             "avg_us=%.2f\n",
             name, b, static_cast<unsigned long long>(blocks),
             vocal_fx_b4d5_global_class_name(g),
             static_cast<double>(
                 vocal_fx_b4d5_global_class_cycles(b, g)) /
                 static_cast<double>(blocks) /
                 static_cast<double>(Profiler::cycles_per_us()));
    }
  }
  // B4D.5 contiguous paired-block window: enough for offline nearest-neighbour
  // matching of NMC+1 against NMC+0 on F0 / active descriptors / slices /
  // source grains / model state.
  {
    const size_t window = count < 2048 ? count : 2048;
    const size_t start = count > window ? (count - window) / 2 : 0;
    for (size_t i = start; i < start + window; ++i) {
      B4D1BlockRecord rec;
      if (!s_audio.b4d1_recorder_get(i, &rec)) break;
      printf("B4D5_PAIRED name=%s blk=%u dsp_us=%u mc=%u ng=%u ad=%u sl=%u "
             "sg=%u att=%u mkc=%u f0=%.2f\n",
             name, static_cast<unsigned>(rec.block_id),
             static_cast<unsigned>(rec.dsp_us),
             static_cast<unsigned>(rec.model_changed),
             static_cast<unsigned>(rec.new_grains),
             static_cast<unsigned>(rec.active_desc),
             static_cast<unsigned>(rec.slices),
             static_cast<unsigned>(rec.source_grains),
             static_cast<unsigned>(rec.schedule_attempts),
             static_cast<unsigned>(rec.mark_count),
             static_cast<double>(rec.f0));
    }
  }

  s_b4d1_audio_seconds += wall_s;
  s_b4d1_dma_errors += c.dma_errors.load();
  s_audio.deinit();
  vocal_fx_free_b4c8b_recorder();
  vocal_fx_free_b4c8_recorders();
  s_audio.b4d1_recorder_free();
}

void b4d1_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));

  esp_task_wdt_config_t wdt = {
      .timeout_ms = 300000,
      .idle_core_mask = (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&wdt);

  s_b4d1_stage = static_cast<float *>(heap_caps_calloc(
      kB4D1StageSeconds * 48000, sizeof(float),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_b4d1_stage) {
    printf("B4D1_CASE name=prestage status=ALLOC_FAILED\nB4D1_END\n");
    vTaskDelete(nullptr);
    return;
  }
  s_audio.set_output_rms_enabled(false);

  // B4D.3S canonical sweep: identical source/time region, only Fs changes.
  struct RateCase {
    uint32_t rate;
    const char *tag;
  };
  const RateCase rates[] = {{44100, "44100"}};
  struct Stage {
    const char *name;
    bool comp;
    bool delay;
    bool reverb;
  };
  const Stage stages[] = {
      {"1v_core", false, false, false},
      {"1v_comp", true, false, false},
      {"1v_delay", false, true, false},
      {"1v_reverb", false, false, true},
      {"1v_fullfx", true, true, true},
  };

  for (const RateCase &rc : rates) {
    s_b4d1_rate = rc.rate;
    s_b4d1_deadline_us =
        1000000.0 * 64.0 / static_cast<double>(rc.rate);
    s_b4d1_stage_frames = kB4D1StageSeconds * rc.rate;
    g_pipeline_sample_rate = rc.rate;

    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(rc.rate);
    cfg.enable_gate = true;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = true;
    cfg.pitch_shift.enabled = true;
    cfg.pitch_shift.semitones = 4.0f;
    cfg.pitch_shift.wet = 1.0f;
    cfg.align_dry_to_harmony = true;
    cfg.enable_harmony_limiter = true;
    vocal_fx_set_reverb_profile_enabled(false);
    if (!vocal_fx_init(cfg)) {
      printf("B4D1_CASE name=init status=ENGINE_INIT_FAILED\nB4D1_END\n");
      vTaskDelete(nullptr);
      return;
    }
    printf("\nB4D3S_RATE rate=%u deadline_us=%.3f stage_frames=%u\n",
           static_cast<unsigned>(rc.rate), s_b4d1_deadline_us,
           static_cast<unsigned>(s_b4d1_stage_frames));
    printf("B4D1_START frozen_dsp=yes block_frames=64 nominal_fs=%u "
           "deadline_us=%.3f warmup_ms=%u\n",
           static_cast<unsigned>(rc.rate), s_b4d1_deadline_us,
           static_cast<unsigned>(kB4D1WarmupMs));
    printf("B4D1_LPC_GATE compile_default_order=%u analyzer_order=%u\n",
           static_cast<unsigned>(LpcConfig{}.order),
           static_cast<unsigned>(vocal_fx_lpc_config_order()));
    printf("B4D4_ANALYSIS_CONFIG rate=%u pitch_window=512 pitch_hop=60 "
           "analysis_rate=%.1f pitch_hops_per_s=%.2f lpc_window=%u "
           "lpc_hop=%u lpc_target_fps=%.2f lpc_order=%u fifo_capacity=2049\n",
           static_cast<unsigned>(rc.rate),
           static_cast<double>(rc.rate) * 0.25, static_cast<double>(rc.rate) * 0.25 / 60.0,
           static_cast<unsigned>(LpcConfig{}.window_size),
           static_cast<unsigned>(LpcConfig{}.hop_size),
           static_cast<double>(rc.rate) / static_cast<double>(LpcConfig{}.hop_size),
           static_cast<unsigned>(LpcConfig{}.order));
    b4d1_print_memory("init");
    s_b4d1_fullfx_pass = false;

    for (const Stage &st : stages) {
      char label[64];
      std::snprintf(label, sizeof(label), "%s_%s_60s", st.name, rc.tag);
      if (b4d1_prestage(label, B4B6StimulusKind::VocalReplay, 220.0f,
                        kB4b6VocalMulaw, kB4b6VocalMulawSize,
                        s_b4d1_stage_frames)) {
        s_audio.set_b4c7_prestaged(s_b4d1_stage, s_b4d1_stage_frames);
        const bool cp = false;
        b4d1_run_case(label, 60, 1, st.comp, st.delay, st.reverb, false,
                      st.reverb, cp);
        b4d1_print_memory(label);
      }
    }

    // B4D.11: two identical S0 production runs for TX-PCM-hash reproducibility
    // and phase-debt capture.
    {
      for (int rep = 0; rep < 2; ++rep) {
        vocal_fx_b4d10_set_sched_variant(0);
        char label[64];
        std::snprintf(label, sizeof(label), "b4d11_s0_fullfx_%s_r%d", rc.tag,
                      rep);
        if (b4d1_prestage(label, B4B6StimulusKind::VocalReplay, 220.0f,
                          kB4b6VocalMulaw, kB4b6VocalMulawSize,
                          s_b4d1_stage_frames)) {
          s_audio.set_b4c7_prestaged(s_b4d1_stage, s_b4d1_stage_frames);
          b4d1_run_case(label, 60, 1, true, true, true, false, false, false);
        }
      }
    }
  }

  printf("B4D1_STABILITY total_audio_s=%.1f dma_errors=%llu\n",
         s_b4d1_audio_seconds,
         static_cast<unsigned long long>(s_b4d1_dma_errors));
  heap_caps_free(s_b4d1_stage);
  s_b4d1_stage = nullptr;
  printf("B4D1_END\n");
  esp_task_wdt_config_t restore = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true,
  };
  (void)esp_task_wdt_reconfigure(&restore);
  vTaskSuspend(nullptr);
}

} // namespace

#endif // CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION || B4D_12_BURN_IN
