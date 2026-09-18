// B4D.12 — Physical Pass Qualification / Product Robustness Burn-In.
//
// Included by main/i2s_bringup.cpp.  This stage is NOT an optimization: the
// S0 TD-PSOLA production architecture is frozen (§1, §6, §50).  The harness
// stages representative inputs, runs a layered burn-in campaign (reference
// soak, F0 range, pitch transitions, harmony intervals, voiced/unvoiced,
// plosive/transient, parameter-control, Full-FX and long-duration soaks) and
// reports lightweight telemetry:
//   * transport counters (Gate A),
//   * deadline-miss classes, max consecutive late blocks, lateness histogram,
//   * per-late-event recovery context (N+1..N+3, backlog before/after),
//   * analysis backlog / FIFO occupation,
//   * 10-minute window aggregates,
//   * memory and stack high-water marks.
// Aggregate counters and bounded rings only; no logging happens in the audio
// path.  DSP arithmetic is not touched.

#if defined(CONFIG_VOXP4_MODE_I2S_B4D_12_BURN_IN)

#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef CONFIG_VOXP4_B4D12_REFERENCE_MINUTES
#define CONFIG_VOXP4_B4D12_REFERENCE_MINUTES 30
#endif
#ifndef CONFIG_VOXP4_B4D12_STRESS_SECONDS
#define CONFIG_VOXP4_B4D12_STRESS_SECONDS 20
#endif
#ifndef CONFIG_VOXP4_B4D12_CONTROL_MINUTES
#define CONFIG_VOXP4_B4D12_CONTROL_MINUTES 10
#endif
#ifndef CONFIG_VOXP4_B4D12_SOAK_MINUTES
#define CONFIG_VOXP4_B4D12_SOAK_MINUTES 240
#endif

namespace {

using vocal_fx_platform::AudioI2sMode;
using vocal_fx_platform::B4B6StimulusKind;
using vocal_fx_platform::B4D12LateEvent;
using vocal_fx_platform::B4D12Telemetry;
using vocal_fx_platform::B4D12WindowStats;

constexpr uint32_t kB4D12WarmupMs = 3000;

// Parameter-control timelines (§15, §16, §29).
enum class B4D12Control : uint8_t {
  None = 0,
  HarmonyIntervals,
  HarmonyLevelMix,
  DelayReverb,
  EffectsBypass,
  PresetLike,
};

struct B4D12CaseDef {
  const char *name;
  const char *category;
  uint32_t seconds;
  B4B6StimulusKind kind;
  float f0;
  bool vocal_fixture;
  int voices;
  bool comp;
  bool delay;
  bool reverb;
  float harmony_interval;
  B4D12Control control;
};

struct B4D12Pct {
  double avg = 0.0;
  double p50 = 0.0, p90 = 0.0, p95 = 0.0, p99 = 0.0, p999 = 0.0, p9999 = 0.0;
  double max = 0.0;
  uint64_t n = 0;
};

B4D12Pct b4d12_pct(const B4D12Telemetry *t) {
  B4D12Pct p;
  if (!t || !t->blocks) return p;
  p.n = t->blocks;
  p.avg = static_cast<double>(t->dsp_sum_us) / static_cast<double>(t->blocks);
  p.max = static_cast<double>(t->dsp_max_us);
  auto q = [&](double frac) -> double {
    uint64_t target =
        static_cast<uint64_t>(std::ceil(frac * static_cast<double>(t->blocks)));
    if (target < 1) target = 1;
    uint64_t acc = 0;
    for (size_t b = 0; b < B4D12Telemetry::kDspHistBins; ++b) {
      acc += t->dsp_hist[b];
      if (acc >= target) return static_cast<double>(b);
    }
    return p.max;
  };
  p.p50 = q(0.50);
  p.p90 = q(0.90);
  p.p95 = q(0.95);
  p.p99 = q(0.99);
  p.p999 = q(0.999);
  p.p9999 = q(0.9999);
  return p;
}

const char *b4d12_control_name(B4D12Control c) {
  switch (c) {
    case B4D12Control::HarmonyIntervals: return "harmony_intervals";
    case B4D12Control::HarmonyLevelMix: return "harmony_level_mix";
    case B4D12Control::DelayReverb: return "delay_reverb";
    case B4D12Control::EffectsBypass: return "effects_bypass";
    case B4D12Control::PresetLike: return "preset_like";
    default: return "none";
  }
}

// Apply one control tick.  Called from the coordinator (~10 Hz), never from
// the audio task.  Values cycle rapidly to stress parameter smoothing and the
// parameter queue.  Parameter semantics are unchanged.
void b4d12_apply_control(B4D12Control control, uint32_t tick) {
  switch (control) {
    case B4D12Control::HarmonyIntervals: {
      static const float semis[] = {0.0f, -12.0f, -7.0f, -5.0f, -3.0f,
                                    3.0f, 4.0f,    5.0f,  7.0f,  12.0f};
      vocal_fx_set_harmony_interval(0, semis[tick % 10]);
      if ((tick % 40) == 0) vocal_fx_set_harmony_enabled(0, (tick / 40) % 2 == 0);
      break;
    }
    case B4D12Control::HarmonyLevelMix: {
      const float g = 0.25f + 0.75f * static_cast<float>(tick % 8) / 7.0f;
      vocal_fx_set_harmony_gain(0, g);
      vocal_fx_set_parameter(VocalFxParameter::PitchShiftWet,
                             static_cast<float>(tick % 11) / 10.0f);
      vocal_fx_set_parameter(VocalFxParameter::HarmonyVoice1Gain, g);
      break;
    }
    case B4D12Control::DelayReverb: {
      vocal_fx_set_parameter(VocalFxParameter::EnableDelay,
                             ((tick / 20) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_parameter(VocalFxParameter::EnableReverb,
                             ((tick / 13) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayWet,
                             static_cast<float>(tick % 9) / 8.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayFeedback,
                             0.1f + 0.5f * static_cast<float>(tick % 5) / 4.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbWet,
                             static_cast<float>(tick % 7) / 6.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds,
                             0.6f + 3.0f * static_cast<float>(tick % 6) / 5.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDamping,
                             static_cast<float>(tick % 6) / 5.0f);
      break;
    }
    case B4D12Control::EffectsBypass: {
      vocal_fx_set_parameter(VocalFxParameter::EnableCompressor,
                             ((tick / 17) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_parameter(VocalFxParameter::EnableDelay,
                             ((tick / 11) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_parameter(VocalFxParameter::EnableReverb,
                             ((tick / 7) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_harmony_enabled(0, ((tick / 23) % 2) == 0);
      // B4D.11 lifetime regression: reverb-profile enable/disable cycles.
      vocal_fx_set_reverb_profile_enabled((tick / 37) % 2 == 0);
      break;
    }
    case B4D12Control::PresetLike: {
      static const float semis[] = {0.0f, 7.0f, -5.0f, 12.0f, 3.0f, -12.0f};
      vocal_fx_set_harmony_interval(0, semis[tick % 6]);
      vocal_fx_set_parameter(VocalFxParameter::CompressorThresholdDb,
                             -30.0f + static_cast<float>(tick % 25));
      vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs,
                             120.0f + 400.0f * static_cast<float>(tick % 10) / 9.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayRightMs,
                             150.0f + 450.0f * static_cast<float>(tick % 8) / 7.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbWet,
                             static_cast<float>(tick % 7) / 6.0f);
      vocal_fx_set_parameter(VocalFxParameter::LimiterCeiling,
                             -6.0f + static_cast<float>(tick % 9));
      vocal_fx_set_parameter(VocalFxParameter::EnableDelay,
                             ((tick / 9) % 2) ? 1.0f : 0.0f);
      vocal_fx_set_parameter(VocalFxParameter::EnableReverb,
                             ((tick / 5) % 2) ? 1.0f : 0.0f);
      break;
    }
    default:
      break;
  }
}

// One burn-in case.  Runs the frozen production chain for `def.seconds` with
// the audio task on Core 0 and the pitch worker on Core 1 (unchanged
// topology), resets every counter after warmup, and prints the telemetry only
// after the tasks stop.
void b4d12_run_case(const B4D12CaseDef &def) {
  char label[80];
  std::snprintf(label, sizeof(label), "b4d12_%s", def.name);

  vocal_fx_set_reverb_profile_enabled(false);
  vocal_fx_reset_reverb_profile();
  b4d1_configure_dsp(def.voices, def.comp, def.delay, def.reverb);
  vocal_fx_set_harmony_interval(0, def.harmony_interval);

  if (!b4d1_prestage(label, def.kind, def.f0,
                     def.vocal_fixture ? kB4b6VocalMulaw : nullptr,
                     def.vocal_fixture ? kB4b6VocalMulawSize : 0,
                     s_b4d1_stage_frames)) {
    printf("B4D12_CASE name=%s status=PRESTAGE_FAILED\n", def.name);
    return;
  }
  s_audio.set_b4c7_prestaged(s_b4d1_stage, s_b4d1_stage_frames);
  s_audio.set_b4c7_input(B4C7InputKind::Prestaged);
  s_audio.set_b4b6_dsp_paused(false);
  s_audio.set_b4c6a_detail(1);
  s_audio.set_output_rms_enabled(false);
  s_audio.set_b4c1_transport_only(false);

  if (!s_audio.b4d12_telemetry_init(s_b4d1_rate)) {
    printf("B4D12_CASE name=%s status=TELEMETRY_ALLOC_FAILED\n", def.name);
    return;
  }
  // Telemetry is calloc-zeroed and stays disabled during warmup, so no PSRAM
  // memset ever competes with the audio task (a 374 KB memset at the measured
  // origin perturbed the first blocks and produced false consecutive misses).
  s_audio.b4d12_telemetry_enable(false);

  if (!start_pipeline_tasks(AudioI2sMode::B4D1Qualification, true)) {
    printf("B4D12_CASE name=%s status=START_FAILED\n", def.name);
    s_audio.b4d12_telemetry_free();
    return;
  }

  // §7 warmup excluded from every statistic.
  vTaskDelay(pdMS_TO_TICKS(kB4D12WarmupMs));

  s_audio.reset_counters();
  vocal_fx_reset_measurement_telemetry();
  vocal_fx_reset_lpc_invalid_reasons();
  s_audio.b4d12_telemetry_enable(true);

  const int64_t begin = esp_timer_get_time();
  const int64_t end = begin + static_cast<int64_t>(def.seconds) * 1000000LL;
  uint32_t tick = 0;
  while (esp_timer_get_time() < end) {
    if (def.control != B4D12Control::None) b4d12_apply_control(def.control, tick);
    tick++;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  const double wall_s =
      static_cast<double>(esp_timer_get_time() - begin) / 1000000.0;

  // Stack high-water must be read while the tasks are still alive.
  b4d1_print_stack();

  const auto &c = s_audio.counters();
  const B4D12Telemetry *t = s_audio.b4d12_telemetry();
  const auto p = b4d12_pct(t);
  const PitchAnalysisAuditTelemetry pa = vocal_fx_pitch_analysis_audit_telemetry();

  s_audio.b4d12_telemetry_enable(false);
  stop_pipeline_tasks(false);

  const double rx_frames = static_cast<double>(c.rx_frames_read.load());
  const double eff_fs = wall_s > 0.0 ? rx_frames / wall_s : 0.0;
  const double misses = t ? static_cast<double>(t->misses) : 0.0;
  const double miss_per_s = wall_s > 0.0 ? misses / wall_s : 0.0;

  printf("B4D12_CASE name=%s category=%s seconds=%u voices=%d comp=%d delay=%d "
         "reverb=%d interval=%.1f control=%s kind=%d f0=%.1f vocal=%d "
         "wall_s=%.6f blocks=%llu\n",
         def.name, def.category, static_cast<unsigned>(def.seconds), def.voices,
         def.comp ? 1 : 0, def.delay ? 1 : 0, def.reverb ? 1 : 0,
         static_cast<double>(def.harmony_interval),
         b4d12_control_name(def.control), static_cast<int>(def.kind),
         static_cast<double>(def.f0), def.vocal_fixture ? 1 : 0, wall_s,
         static_cast<unsigned long long>(t ? t->blocks : 0));
  printf("B4D12_PHYSICAL name=%s rx_blocks=%llu tx_blocks=%llu dsp_blocks=%llu "
         "rx_frames=%llu tx_frames=%llu effective_fs=%.3f err_pct=%.4f\n",
         def.name, static_cast<unsigned long long>(c.rx_blocks_read.load()),
         static_cast<unsigned long long>(c.tx_blocks_written.load()),
         static_cast<unsigned long long>(c.audio_blocks_processed.load()),
         static_cast<unsigned long long>(c.rx_frames_read.load()),
         static_cast<unsigned long long>(c.tx_frames_written.load()), eff_fs,
         std::fabs(eff_fs - static_cast<double>(s_b4d1_rate)) /
             static_cast<double>(s_b4d1_rate) * 100.0);
  printf("B4D12_TRANSPORT name=%s rx_overruns=%llu tx_underruns=%llu "
         "dma_errors=%llu actual_rx_dma_errors=%llu actual_tx_dma_errors=%llu "
         "read_failures=%llu write_failures=%llu rx_dropped_frames=%llu "
         "tx_dropped_frames=%llu rx_seq_gaps=%llu tx_seq_gaps=%llu nan_inf=%llu "
         "rx_diag_ovf=%llu tx_diag_ovf=%llu\n",
         def.name, static_cast<unsigned long long>(c.rx_overruns.load()),
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
         static_cast<unsigned long long>(c.nan_inf_count.load()),
         static_cast<unsigned long long>(c.rx_diagnostic_queue_overflows.load()),
         static_cast<unsigned long long>(c.tx_diagnostic_queue_overflows.load()));

  const uint64_t missed = t ? t->misses : 0;
  printf("B4D12_DSP name=%s blocks=%llu avg_us=%.3f p50=%.0f p90=%.0f p95=%.0f "
         "p99=%.0f p999=%.0f p9999=%.0f max=%.0f misses=%llu "
         "miss_per_block=%.8f miss_per_s=%.4f miss_per_min=%.3f "
         "miss_per_hour=%.2f\n",
         def.name, static_cast<unsigned long long>(p.n), p.avg, p.p50, p.p90,
         p.p95, p.p99, p.p999, p.p9999, p.max,
         static_cast<unsigned long long>(missed),
         p.n ? misses / static_cast<double>(p.n) : 0.0, miss_per_s,
         miss_per_s * 60.0, miss_per_s * 3600.0);
  printf("B4D12_LATE name=%s threshold_1200_blocks=%llu "
         "max_consecutive_late=%llu max_lateness_us=%llu "
         "cumulative_lateness_us=%llu late_events=%llu late_events_dropped=%llu "
         "recovery_pending=%llu\n",
         def.name,
         static_cast<unsigned long long>(t ? t->late_1200_blocks : 0),
         static_cast<unsigned long long>(c.max_consecutive_late_blocks.load()),
         static_cast<unsigned long long>(c.max_single_block_lateness_us.load()),
         static_cast<unsigned long long>(c.cumulative_lateness_us.load()),
         static_cast<unsigned long long>(t ? t->late_events : 0),
         static_cast<unsigned long long>(t ? t->late_events_dropped : 0),
         static_cast<unsigned long long>(t ? t->recovery_pending : 0));
  if (t) {
    printf("B4D12_LATENESS_HIST name=%s le100=%llu b100_250=%llu b250_500=%llu "
           "b500_1000=%llu b1000_1500=%llu gt1500=%llu\n",
           def.name,
           static_cast<unsigned long long>(t->lateness_hist[0]),
           static_cast<unsigned long long>(t->lateness_hist[1]),
           static_cast<unsigned long long>(t->lateness_hist[2]),
           static_cast<unsigned long long>(t->lateness_hist[3]),
           static_cast<unsigned long long>(t->lateness_hist[4]),
           static_cast<unsigned long long>(t->lateness_hist[5]));
    printf("B4D12_CLASS name=%s miss_nmc=%llu miss_mc0=%llu miss_mc1=%llu "
           "miss_mc2=%llu miss_mc3=%llu blocks_nmc=%llu blocks_mc0=%llu "
           "blocks_mc1=%llu blocks_mc2=%llu blocks_mc3=%llu\n",
           def.name, static_cast<unsigned long long>(t->miss_nmc),
           static_cast<unsigned long long>(t->miss_mc0),
           static_cast<unsigned long long>(t->miss_mc1),
           static_cast<unsigned long long>(t->miss_mc2),
           static_cast<unsigned long long>(t->miss_mc3),
           static_cast<unsigned long long>(t->blocks_nmc),
           static_cast<unsigned long long>(t->blocks_mc0),
           static_cast<unsigned long long>(t->blocks_mc1),
           static_cast<unsigned long long>(t->blocks_mc2),
           static_cast<unsigned long long>(t->blocks_mc3));
    printf("B4D12_RECOVERY name=%s pending=%llu r0=%llu r1=%llu r2=%llu r3=%llu "
           "r4=%llu r5=%llu r6=%llu r7=%llu\n",
           def.name, static_cast<unsigned long long>(t->recovery_pending),
           static_cast<unsigned long long>(t->recovery_hist[0]),
           static_cast<unsigned long long>(t->recovery_hist[1]),
           static_cast<unsigned long long>(t->recovery_hist[2]),
           static_cast<unsigned long long>(t->recovery_hist[3]),
           static_cast<unsigned long long>(t->recovery_hist[4]),
           static_cast<unsigned long long>(t->recovery_hist[5]),
           static_cast<unsigned long long>(t->recovery_hist[6]),
           static_cast<unsigned long long>(t->recovery_hist[7]));
  }
  printf("B4D12_BACKLOG name=%s current_ms=%.3f avg_ms=%.3f p95_ms=%.3f "
         "p99_ms=%.3f max_ms=%.3f\n",
         def.name, static_cast<double>(pa.analysis_backlog_ms),
         static_cast<double>(pa.analysis_backlog_average_ms),
         static_cast<double>(pa.analysis_backlog_p95_ms),
         static_cast<double>(pa.analysis_backlog_p99_ms),
         static_cast<double>(pa.analysis_backlog_max_ms));
  printf("B4D12_FIFO name=%s pushes=%llu pops=%llu drops=%llu "
         "overflow_attempts=%llu max_occ=%llu avg_occ=%.3f current_occ=%llu\n",
         def.name, static_cast<unsigned long long>(pa.fifo_pushes),
         static_cast<unsigned long long>(pa.fifo_pops),
         static_cast<unsigned long long>(pa.fifo_drops),
         static_cast<unsigned long long>(pa.fifo_overflow_attempts),
         static_cast<unsigned long long>(pa.fifo_maximum_occupancy),
         static_cast<double>(pa.fifo_average_occupancy),
         static_cast<unsigned long long>(pa.fifo_current_occupancy));

  // Per-late-event recovery context.
  if (t && t->late_events > 0) {
    const size_t cap = vocal_fx_platform::kB4D12LateEventCapacity;
    const size_t n = t->late_events < cap ? t->late_events : cap;
    const size_t start = t->late_events <= cap ? 0 : (t->late_write % cap);
    for (size_t i = 0; i < n; ++i) {
      const B4D12LateEvent &e = t->late[(start + i) % cap];
      printf("B4D12_LATE_EVENT name=%s blk=%lu dsp_us=%u lateness_us=%u "
             "klass=%u mc=%u ng=%u ad=%u sl=%u prev_miss=%u next1=%u next2=%u "
             "next3=%u recov=%u bl_before_ds=%u bl_after_ds=%u\n",
             def.name, static_cast<unsigned long>(e.block_id),
             static_cast<unsigned>(e.dsp_us),
             static_cast<unsigned>(e.lateness_us),
             static_cast<unsigned>(e.klass),
             static_cast<unsigned>(e.model_changed),
             static_cast<unsigned>(e.new_grains),
             static_cast<unsigned>(e.active_desc),
             static_cast<unsigned>(e.slices),
             static_cast<unsigned>(e.prev_miss),
             static_cast<unsigned>(e.next1_dsp_us),
             static_cast<unsigned>(e.next2_dsp_us),
             static_cast<unsigned>(e.next3_dsp_us),
             static_cast<unsigned>(e.recovery_blocks),
             static_cast<unsigned>(e.backlog_before_ds),
             static_cast<unsigned>(e.backlog_after_ds));
    }
  }

  // 10-minute windows plus the trailing partial window.
  auto window_pct = [](const B4D12WindowStats &w, double frac) -> double {
    if (!w.blocks) return 0.0;
    uint64_t target =
        static_cast<uint64_t>(std::ceil(frac * static_cast<double>(w.blocks)));
    if (target < 1) target = 1;
    uint64_t acc = 0;
    for (size_t b = 0; b < vocal_fx_platform::kB4D12WindowHistBins; ++b) {
      acc += w.hist[b];
      if (acc >= target)
        return static_cast<double>(b * 100 + 100);  // bin upper edge
    }
    return static_cast<double>(w.dsp_max_us);
  };
  auto print_window = [&](const B4D12WindowStats &w, size_t idx, int partial) {
    printf("B4D12_WINDOW name=%s idx=%zu partial=%d blocks=%llu avg_us=%.3f "
           "p99_us=%.0f p999_us=%.0f max_us=%u misses=%u mc2=%u mc3=%u "
           "max_consecutive_late=%u backlog_max_ms=%.1f transport_errors=%llu "
           "cumulative_lateness_us=%llu\n",
           def.name, idx, partial, static_cast<unsigned long long>(w.blocks),
           w.blocks ? static_cast<double>(w.dsp_sum_us) /
                          static_cast<double>(w.blocks)
                    : 0.0,
           window_pct(w, 0.99), window_pct(w, 0.999),
           static_cast<unsigned>(w.dsp_max_us), static_cast<unsigned>(w.misses),
           static_cast<unsigned>(w.mc2), static_cast<unsigned>(w.mc3),
           static_cast<unsigned>(w.max_consecutive_late),
           static_cast<double>(w.backlog_max_ds) / 10.0,
           static_cast<unsigned long long>(w.transport_errors),
           static_cast<unsigned long long>(w.cumulative_lateness_us));
  };
  if (t) {
    for (size_t i = 0; i < t->window_count; ++i)
      print_window(t->windows[i], i, 0);
    if (t->window.blocks) {
      // The trailing partial window has no finalize point; fill its transport
      // and lateness deltas from the counters at print time.
      const uint64_t total_tr =
          c.rx_overruns.load() + c.tx_underruns.load() +
          c.actual_rx_dma_errors.load() + c.actual_tx_dma_errors.load() +
          c.i2s_read_failures.load() + c.i2s_write_failures.load() +
          c.rx_dropped_frames.load() + c.tx_dropped_frames.load() +
          c.rx_sequence_gaps.load() + c.tx_sequence_gaps.load() +
          c.dma_errors.load() + c.nan_inf_count.load();
      B4D12WindowStats pw = t->window;
      pw.transport_errors = total_tr >= t->window_transport_base
                                ? total_tr - t->window_transport_base
                                : 0;
      const uint64_t cum_late = c.cumulative_lateness_us.load();
      pw.cumulative_lateness_us = cum_late >= t->window_lateness_base
                                      ? cum_late - t->window_lateness_base
                                      : 0;
      print_window(pw, t->window_count, 1);
    }
  }

  // Gate evaluation (§5, §31).
  const bool gate_a =
      c.rx_overruns.load() == 0 && c.tx_underruns.load() == 0 &&
      c.dma_errors.load() == 0 && c.actual_rx_dma_errors.load() == 0 &&
      c.actual_tx_dma_errors.load() == 0 && c.i2s_read_failures.load() == 0 &&
      c.i2s_write_failures.load() == 0 && c.rx_dropped_frames.load() == 0 &&
      c.tx_dropped_frames.load() == 0 && c.rx_sequence_gaps.load() == 0 &&
      c.tx_sequence_gaps.load() == 0 && c.nan_inf_count.load() == 0 &&
      pa.fifo_drops == 0 && pa.fifo_overflow_attempts == 0;
  const bool gate_b = c.max_consecutive_late_blocks.load() <= 1;
  const bool fs_ok =
      std::fabs(eff_fs - static_cast<double>(s_b4d1_rate)) <=
      static_cast<double>(s_b4d1_rate) * 0.005;
  printf("B4D12_QUALIFY name=%s gate_a_transport=%s gate_b_physical=%s "
         "fs_ok=%d max_consecutive_late=%llu backlog_max_ms=%.3f "
         "dsp_miss_independent=%d result=%s\n",
         def.name, gate_a ? "PASS" : "FAIL", gate_b ? "PASS" : "FAIL",
         fs_ok ? 1 : 0,
         static_cast<unsigned long long>(c.max_consecutive_late_blocks.load()),
         static_cast<double>(pa.analysis_backlog_max_ms), missed > 0 ? 1 : 0,
         (gate_a && gate_b && fs_ok) ? "PASS" : "FAIL");

  b4d1_print_memory(label);
  s_audio.deinit();
}

void b4d12_run_campaign() {
  const uint32_t soak_s = static_cast<uint32_t>(CONFIG_VOXP4_B4D12_SOAK_MINUTES) * 60u;

#if !defined(CONFIG_VOXP4_B4D12_SOAK_ONLY)
  const uint32_t ref_s = static_cast<uint32_t>(CONFIG_VOXP4_B4D12_REFERENCE_MINUTES) * 60u;
  const uint32_t stress_s = static_cast<uint32_t>(CONFIG_VOXP4_B4D12_STRESS_SECONDS);
  const uint32_t ctrl_s = static_cast<uint32_t>(CONFIG_VOXP4_B4D12_CONTROL_MINUTES) * 60u;

  // Category A — deterministic reference soak (§8).
  const B4D12CaseDef base = {"a_ref_vocal_fullfx", "A_reference", ref_s,
                              B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                              true, true, true, 7.0f, B4D12Control::None};
  b4d12_run_case(base);

  // Category C — F0 range stress (§10): steady pitch across the supported
  // range.  Levels follow the fixture (-18 dBFS).
  static const float kF0[] = {80.0f, 100.0f, 130.0f, 160.0f, 200.0f,
                              250.0f, 300.0f, 350.0f, 400.0f};
  for (size_t i = 0; i < sizeof(kF0) / sizeof(kF0[0]); ++i) {
    char nm[48];
    std::snprintf(nm, sizeof(nm), "c_f0_%03d", static_cast<int>(kF0[i]));
    const B4D12CaseDef d = {nm, "C_f0_range", stress_s,
                            B4B6StimulusKind::HarmonicVoiced, kF0[i], false, 1,
                            true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(d);
  }
  {
    const B4D12CaseDef gliss_up = {"c_gliss_80_440", "C_f0_range", stress_s,
                                   B4B6StimulusKind::Gliss80To440, 0.0f, false,
                                   1, true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(gliss_up);
    const B4D12CaseDef gliss_dn = {"c_gliss_440_80", "C_f0_range", stress_s,
                                   B4B6StimulusKind::Gliss440To80, 0.0f, false,
                                   1, true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(gliss_dn);
  }

  // Category D — pitch-transition stress (§11): repeated period changes chosen
  // to maximise MC+2/MC+3 bursts.  Available deterministic step stimuli.
  static const struct {
    const char *n;
    B4B6StimulusKind k;
  } kSteps[] = {
      {"d_step_80_160", B4B6StimulusKind::Step80To160},
      {"d_step_220_110", B4B6StimulusKind::Step220To110},
      {"d_step_110_440", B4B6StimulusKind::Step110To440},
      {"d_step_440_110", B4B6StimulusKind::Step440To110},
      {"d_step_220_440", B4B6StimulusKind::Step220To440},
      {"d_step_65_130", B4B6StimulusKind::Step65To130},
      {"d_step_147_220_330", B4B6StimulusKind::Step147To220To330},
  };
  for (const auto &s : kSteps) {
    const B4D12CaseDef d = {s.n, "D_transition", stress_s, s.k, 0.0f, false, 1,
                            true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(d);
  }

  // Category E — harmony interval stress (§12).
  static const float kIntervals[] = {-12.0f, -7.0f, -5.0f, -3.0f, 0.0f,
                                     3.0f,   4.0f,  5.0f,  7.0f,  12.0f};
  for (size_t i = 0; i < sizeof(kIntervals) / sizeof(kIntervals[0]); ++i) {
    char nm[48];
    std::snprintf(nm, sizeof(nm), "e_interval_%s%.0f",
                  kIntervals[i] < 0 ? "m" : "p",
                  std::fabs(kIntervals[i]));
    const B4D12CaseDef d = {nm, "E_interval", stress_s,
                            B4B6StimulusKind::VocalReplay, 220.0f, true, 1, true,
                            true, true, kIntervals[i], B4D12Control::None};
    b4d12_run_case(d);
  }
  {
    const B4D12CaseDef rapid = {"e_interval_rapid", "E_interval", ctrl_s,
                                B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                                true, true, true, 7.0f,
                                B4D12Control::HarmonyIntervals};
    b4d12_run_case(rapid);
  }

  // Category F — voiced/unvoiced transitions (§13).
  static const struct {
    const char *n;
    B4B6StimulusKind k;
  } kVu[] = {
      {"f_silence", B4B6StimulusKind::Silence},
      {"f_near_silence", B4B6StimulusKind::NearSilence},
      {"f_noise", B4B6StimulusKind::BroadbandNoise},
      {"f_breathy", B4B6StimulusKind::BreathyVoiced},
      {"f_staccato", B4B6StimulusKind::Staccato},
      {"f_vibrato", B4B6StimulusKind::VibratoTwoSemitones},
  };
  for (const auto &s : kVu) {
    const B4D12CaseDef d = {s.n, "F_voiced_unvoiced", stress_s, s.k, 0.0f,
                            false, 1, true, true, true, 7.0f,
                            B4D12Control::None};
    b4d12_run_case(d);
  }

  // Category G — plosive / transient stress (§14): consonant-rich real
  // material and broadband bursts.
  {
    const B4D12CaseDef d = {"g_plosive_vocal", "G_transient", stress_s,
                            B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                            true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(d);
  }
  {
    const B4D12CaseDef d = {"g_plosive_noise", "G_transient", stress_s,
                            B4B6StimulusKind::BroadbandNoise, 0.0f, false, 1,
                            true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(d);
  }

  // Category H — Full-FX worst-case soak (§7H): real vocal replay with the
  // complete chain and the rapid interval control.
  {
    const B4D12CaseDef d = {"h_fullfx_worst_soak", "H_fullfx_soak", ref_s,
                            B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                            true, true, true, 7.0f,
                            B4D12Control::HarmonyIntervals};
    b4d12_run_case(d);
  }

  // Category I — parameter-control stress (§15, §16, §29).
  static const B4D12Control kControls[] = {
      B4D12Control::HarmonyLevelMix, B4D12Control::DelayReverb,
      B4D12Control::EffectsBypass, B4D12Control::PresetLike};
  for (size_t i = 0; i < sizeof(kControls) / sizeof(kControls[0]); ++i) {
    char nm[48];
    std::snprintf(nm, sizeof(nm), "i_control_%s",
                  b4d12_control_name(kControls[i]));
    const B4D12CaseDef d = {nm, "I_control", ctrl_s / 4u,
                            B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                            true, true, true, 7.0f, kControls[i]};
    b4d12_run_case(d);
  }
#endif // !CONFIG_VOXP4_B4D12_SOAK_ONLY

  // Category J — long-duration production soak (§30).  This is the main
  // Physical Pass qualification.  It runs the production-representative
  // steady Full-FX chain (no rapid control changes; parameter/control stress
  // is covered by H, I and the rapid interval case in E).
  {
    const B4D12CaseDef d = {"j_long_soak", "J_long_soak", soak_s,
                            B4B6StimulusKind::VocalReplay, 220.0f, true, 1,
                            true, true, true, 7.0f, B4D12Control::None};
    b4d12_run_case(d);
  }
}

void b4d12_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));

  esp_task_wdt_config_t wdt = {
      .timeout_ms = 600000,
      .idle_core_mask = (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&wdt);

  s_b4d1_stage = static_cast<float *>(heap_caps_calloc(
      kB4D1StageSeconds * 44100, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_b4d1_stage) {
    printf("B4D12_CASE name=prestage status=ALLOC_FAILED\nB4D12_END\n");
    vTaskDelete(nullptr);
    return;
  }
  s_audio.set_output_rms_enabled(false);

  s_b4d1_rate = 44100;
  s_b4d1_deadline_us = 1000000.0 * 64.0 / 44100.0;
  s_b4d1_stage_frames = kB4D1StageSeconds * 44100;
  g_pipeline_sample_rate = 44100;
  // Ensure stimulus generation before the first init() uses 44.1 kHz.
  s_audio.set_prestage_sample_rate(44100);

  VocalFxConfig cfg{};
  cfg.sample_rate = 44100.0f;
  cfg.enable_gate = true;
  cfg.enable_compressor = false;
  cfg.enable_delay = false;
  cfg.enable_reverb = false;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 7.0f;
  cfg.pitch_shift.wet = 1.0f;
  cfg.align_dry_to_harmony = true;
  cfg.enable_harmony_limiter = true;
  vocal_fx_set_reverb_profile_enabled(false);
  if (!vocal_fx_init(cfg)) {
    printf("B4D12_CASE name=init status=ENGINE_INIT_FAILED\nB4D12_END\n");
    vTaskDelete(nullptr);
    return;
  }

  printf("\nB4D12_START frozen_dsp=yes block_frames=64 nominal_fs=44100 "
         "deadline_us=%.3f warmup_ms=%u ref_min=%d stress_s=%d ctrl_min=%d "
         "soak_min=%d\n",
         s_b4d1_deadline_us, static_cast<unsigned>(kB4D12WarmupMs),
         (int)CONFIG_VOXP4_B4D12_REFERENCE_MINUTES,
         (int)CONFIG_VOXP4_B4D12_STRESS_SECONDS,
         (int)CONFIG_VOXP4_B4D12_CONTROL_MINUTES,
         (int)CONFIG_VOXP4_B4D12_SOAK_MINUTES);
  printf("B4D12_RATECFG rate=44100 deadline_us=%.3f stage_frames=%u "
         "fifo_capacity=2049 lpc_order=%u pitch_window=512 pitch_hop=60\n",
         s_b4d1_deadline_us, static_cast<unsigned>(s_b4d1_stage_frames),
         static_cast<unsigned>(LpcConfig{}.order));
  b4d1_print_memory("init");

  b4d12_run_campaign();

  printf("B4D12_END\n");
  heap_caps_free(s_b4d1_stage);
  s_b4d1_stage = nullptr;
  esp_task_wdt_config_t restore = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true,
  };
  (void)esp_task_wdt_reconfigure(&restore);
  vTaskSuspend(nullptr);
}

} // namespace

#endif // CONFIG_VOXP4_MODE_I2S_B4D_12_BURN_IN
