// B4C.8 — Voice-Other breakdown, instrumentation removal & grain-burst tail
// optimization.  Included by main/i2s_bringup.cpp.  DSP semantics are
// FROZEN (§1): this file only stages input, selects the qualified
// production configuration, measures instrumentation overhead, decomposes
// the voice-Other bucket, records slow blocks, and prints observability.

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION)

#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using vocal_fx_platform::AudioI2sMode;
using vocal_fx_platform::B4B6StimulusKind;
using vocal_fx_platform::B4C7InputKind;

constexpr size_t kB4C8StageSeconds = 65;
constexpr size_t kB4C8StageFrames = kB4C8StageSeconds * 48000;
float *s_b4c8_stage = nullptr;
double s_b4c8_audio_seconds = 0.0;
uint64_t s_b4c8_dma_errors = 0;

inline uint32_t b4c8_fnv(const float *buf, size_t frames) {
  uint32_t h = 2166136261U;
  for (size_t i = 0; i < frames; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &buf[i], sizeof(bits));
    h = (h ^ bits) * 16777619U;
  }
  return h;
}

inline double b4c8_rms(const float *buf, size_t frames) {
  double s = 0.0;
  for (size_t i = 0; i < frames; ++i) s += static_cast<double>(buf[i]) * buf[i];
  return frames ? std::sqrt(s / frames) : 0.0;
}

inline void b4c8_configure_frozen_dsp(int voices) {
  vocal_fx_set_psola_grain_render_mode(0, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_grain_render_mode(1, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_fir_kernel(0, PsolaFirKernel::MultiFma);
  vocal_fx_set_psola_fir_kernel(1, PsolaFirKernel::MultiFma);
  vocal_fx_configure_psola_residual_cache(0, 0, false, false);
  vocal_fx_configure_psola_residual_cache(1, 0, false, false);
  vocal_fx_set_psola_residual_cache_enabled(0, false);
  vocal_fx_set_psola_residual_cache_enabled(1, false);
  vocal_fx_set_psola_warp_cache(true, true, false);
  SharedLpcAnalysis::prepare_gainnorm_basis(48000.0f);
  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);
  s_audio.set_b4c1_transport_only(false);
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 0.0f);
  vocal_fx_set_harmony_enabled(0, voices >= 1);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_formant_mode(0, voices >= 1 ? FormantMode::Lpc : FormantMode::Off);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_harmony_enabled(1, voices >= 2);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_formant_mode(1, voices >= 2 ? FormantMode::Lpc : FormantMode::Off);
  vocal_fx_set_harmony_gain(1, 1.0f);
}

inline void b4c8_print_memory() {
  printf("B4C8_MEMORY internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

inline bool b4c8_prestage(const char *name, B4B6StimulusKind kind, float f0,
                           const uint8_t *mulaw, size_t mulaw_n,
                           size_t frames) {
  if (!s_b4c8_stage || frames > kB4C8StageFrames) return false;
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  for (size_t pos = 0; pos < frames; pos += 4096) {
    const size_t chunk = (frames - pos > 4096) ? 4096 : (frames - pos);
    s_audio.generate_b4b6_mono(s_b4c8_stage + pos, chunk);
    vTaskDelay(1);
  }
  const uint32_t fnv = b4c8_fnv(s_b4c8_stage, frames);
  const double rms = b4c8_rms(s_b4c8_stage, frames);
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  float head[6144];
  for (size_t pos = 0; pos < 6144; pos += 1024)
    s_audio.generate_b4b6_mono(head + pos, 1024);
  const uint32_t fnv_head = b4c8_fnv(head, 6144);
  const uint32_t fnv_staged = b4c8_fnv(s_b4c8_stage, 6144);
  const bool ok = (fnv_head == fnv_staged);
  printf("B4C8_PRESTAGE name=%s frames=%u fnv=%08lx rms=%.6f identity=%s\n",
         name, static_cast<unsigned>(frames), static_cast<unsigned long>(fnv),
         rms, ok ? "OK" : "FAIL");
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  return ok;
}

// Print per-voice Other breakdown from the TdPsola cycle counters.
inline void b4c8_print_other_breakdown(int voice) {
  // Index mapping: b4c7_section_cycles_[0..4] + b4c8_other_cycles_[0..17]
  // plus the profile-section totals from the profiler.
  printf("B4C8_OTHER voice=%d\n", voice);

  // Legacy B4C.7 section totals (still valid subcomponents).
  for (size_t i = 0; i < 5; ++i) {
    printf("B4C8_OTHER_SECTION voice=%d legacy_section=%zu cycles=%llu\n",
           voice, i,
           static_cast<unsigned long long>(
               vocal_fx_voice_b4c7_cycles(voice, i)));
  }

  // Extended 18-category Other decomposition (B4C.8).
  // Printed as accumulated cycles over the entire window; caller divides
  // by block count to get per-block averages.
  for (size_t i = 0; i < static_cast<size_t>(B4c8OtherCategory::Count); ++i) {
    printf("B4C8_OTHER_CAT voice=%d cat=%zu cycles=%llu\n",
           voice, i,
           static_cast<unsigned long long>(
               vocal_fx_voice_other_cycles(voice, i)));
  }
}

// Print B4C.8A deferred traversal zero-vs-active split.
inline void b4c8_print_deferred_split(int voice) {
  const uint64_t zc = vocal_fx_deferred_zero_cycles(voice);
  const uint64_t zl = vocal_fx_deferred_zero_calls(voice);
  const uint64_t ac = vocal_fx_deferred_active_cycles(voice);
  const uint64_t al = vocal_fx_deferred_active_calls(voice);
  printf("B4C8A_DEFERRED voice=%d zero_calls=%llu zero_cycles=%llu "
         "active_calls=%llu active_cycles=%llu\n",
         voice,
         static_cast<unsigned long long>(zl),
         static_cast<unsigned long long>(zc),
         static_cast<unsigned long long>(al),
         static_cast<unsigned long long>(ac));
}

// Print B4C.8A PerSampleLoopControl decomposition.
inline void b4c8_print_loop_decomp(int voice) {
  const uint64_t hist = vocal_fx_loop_history_fetch_cycles(voice);
  const uint64_t onr = vocal_fx_loop_ola_norm_reset_cycles(voice);
  const uint64_t idx = vocal_fx_loop_indexing_cycles(voice);
  printf("B4C8A_LOOP_DECOMP voice=%d history_fetch=%llu ola_norm_reset=%llu "
         "loop_indexing_control=%llu\n",
         voice,
         static_cast<unsigned long long>(hist),
         static_cast<unsigned long long>(onr),
         static_cast<unsigned long long>(idx));
}

// Print B4C.8A model-change paired comparison.
inline void b4c8_print_model_change_comparison(int voice) {
  const uint64_t mc_blocks = vocal_fx_model_change_blocks(voice);
  const uint64_t mc_cycles = vocal_fx_model_change_cycles(voice);
  const uint64_t mc_max = vocal_fx_model_change_max_cycles(voice);
  const uint64_t nmc_blocks = vocal_fx_no_model_change_blocks(voice);
  const uint64_t nmc_cycles = vocal_fx_no_model_change_cycles(voice);
  const uint64_t nmc_max = vocal_fx_no_model_change_max_cycles(voice);
  const double mc_avg = mc_blocks ? static_cast<double>(mc_cycles) / mc_blocks : 0.0;
  const double nmc_avg = nmc_blocks ? static_cast<double>(nmc_cycles) / nmc_blocks : 0.0;
  printf("B4C8A_MODEL_CHANGE voice=%d "
         "mc_blocks=%llu mc_avg=%.2f mc_max=%llu "
         "nmc_blocks=%llu nmc_avg=%.2f nmc_max=%llu "
         "delta_avg=%.2f\n",
         voice,
         static_cast<unsigned long long>(mc_blocks),
         mc_avg,
         static_cast<unsigned long long>(mc_max),
         static_cast<unsigned long long>(nmc_blocks),
         nmc_avg,
         static_cast<unsigned long long>(nmc_max),
         mc_avg - nmc_avg);
}

// Print slow-block recorder contents.
inline void b4c8_print_slow_blocks(int voice) {
  const size_t count = vocal_fx_slow_block_count(voice);
  printf("B4C8_SLOW_BLOCKS voice=%d count=%zu\n", voice, count);
  for (size_t i = 0; i < count; ++i) {
    B4c8SlowBlockRecord rec;
    if (vocal_fx_get_slow_block(voice, i, &rec)) {
      printf("B4C8_SLOW_BLOCK voice=%d idx=%zu dur_us=%lu blk=%lu core=%u "
             "mask=%u grn0=%u grn1=%u desc0=%u desc1=%u "
             "slc0=%u slc1=%u src0=%u src1=%u "
             "fir0=%u fir1=%u ola=%u syn=%u "
             "f0=%.1f marks=%u model_chg=%u\n",
             voice, i,
             static_cast<unsigned long>(rec.block_duration_us),
             static_cast<unsigned long>(rec.block_index),
             rec.core, rec.voice_active_mask,
             rec.new_grains_v0, rec.new_grains_v1,
             rec.active_descriptors_v0, rec.active_descriptors_v1,
             rec.deferred_slices_v0, rec.deferred_slices_v1,
             rec.source_samples_v0, rec.source_samples_v1,
             rec.fir_samples_v0, rec.fir_samples_v1,
             rec.ola_samples, rec.synthesis_samples,
             static_cast<double>(rec.pitch_f0), rec.mark_count,
             rec.model_changes);
    }
  }
}

// Print stall events.
inline void b4c8_print_stall_events() {
  const size_t count = vocal_fx_stall_event_count();
  printf("B4C8_STALL_EVENTS count=%zu\n", count);
  for (size_t i = 0; i < count; ++i) {
    B4c8StallEvent ev;
    if (vocal_fx_get_stall_event(i, &ev)) {
      printf("B4C8_STALL idx=%zu dur_us=%lu blk=%lu core=%u mask=%u "
             "section=%u pitch_state=%u published=%u diag_ring=%u "
             "psram=%u i2s=%u\n",
             i,
             static_cast<unsigned long>(ev.block_duration_us),
             static_cast<unsigned long>(ev.block_index),
             ev.core, ev.voice_active_mask,
             ev.pre_stall_section, ev.pitch_sync_state,
             ev.analysis_published, ev.diag_ring_writes,
             ev.psram_activity, ev.i2s_state);
    }
  }
}

// Print transport counters.
inline void b4c8_print_transport(const char *label) {
  const auto &c = s_audio.counters();
  printf("B4C8_TRANSPORT label=%s rx_blocks=%llu tx_blocks=%llu "
         "dsp_blocks=%llu rx_frames=%llu tx_frames=%llu "
         "rx_overruns=%llu tx_underruns=%llu dma_errors=%llu "
         "dsp_misses=%llu transport_misses=%llu\n",
         label,
         static_cast<unsigned long long>(c.rx_blocks_read.load()),
         static_cast<unsigned long long>(c.tx_blocks_written.load()),
         static_cast<unsigned long long>(c.audio_blocks_processed.load()),
         static_cast<unsigned long long>(c.rx_frames_read.load()),
         static_cast<unsigned long long>(c.tx_frames_written.load()),
         static_cast<unsigned long long>(c.rx_overruns.load()),
         static_cast<unsigned long long>(c.tx_underruns.load()),
         static_cast<unsigned long long>(c.dma_errors.load()),
         static_cast<unsigned long long>(c.dsp_deadline_misses.load()),
         static_cast<unsigned long long>(c.transport_deadline_misses.load()));
}

// Print B4C6A-compatible qualification snapshot.
inline void b4c8_print_snapshot(const char *label, double wall_s,
                                int detail) {
  const auto &c = s_audio.counters();
  const double expected = wall_s * 48000.0 / 64.0;
  const double rx = static_cast<double>(c.rx_blocks_read.load());
  const double tx = static_cast<double>(c.tx_blocks_written.load());
  const double dsp = static_cast<double>(c.audio_blocks_processed.load());
  const double rx_frames = static_cast<double>(c.rx_frames_read.load());
  const double eff_fs = wall_s > 0.0 ? rx_frames / wall_s : 0.0;
  printf("B4C6A_CASE name=%s wall_s=%.6f expected_blocks=%.3f "
         "actual_rx_blocks=%.0f actual_tx_blocks=%.0f actual_dsp_blocks=%.0f "
         "missing_rx_blocks=%.3f missing_tx_blocks=%.3f missing_dsp_blocks=%.3f "
         "effective_fs=%.3f\n",
         label, wall_s, expected, rx, tx, dsp, expected - rx, expected - tx,
         expected - dsp, eff_fs);
  printf("B4C6A_TRANSPORT rx_dma_events=%llu tx_dma_events=%llu "
         "rx_overruns=%llu tx_underruns=%llu dma_errors=%llu "
         "actual_rx_dma_errors=%llu actual_tx_dma_errors=%llu "
         "read_failures=%llu write_failures=%llu "
         "rx_seq_gaps=%llu tx_seq_gaps=%llu rx_frames=%llu tx_frames=%llu\n",
         static_cast<unsigned long long>(c.rx_dma_events.load()),
         static_cast<unsigned long long>(c.tx_dma_events.load()),
         static_cast<unsigned long long>(c.rx_overruns.load()),
         static_cast<unsigned long long>(c.tx_underruns.load()),
         static_cast<unsigned long long>(c.dma_errors.load()),
         static_cast<unsigned long long>(c.actual_rx_dma_errors.load()),
         static_cast<unsigned long long>(c.actual_tx_dma_errors.load()),
         static_cast<unsigned long long>(c.i2s_read_failures.load()),
         static_cast<unsigned long long>(c.i2s_write_failures.load()),
         static_cast<unsigned long long>(c.rx_sequence_gaps.load()),
         static_cast<unsigned long long>(c.tx_sequence_gaps.load()),
         static_cast<unsigned long long>(c.rx_frames_read.load()),
         static_cast<unsigned long long>(c.tx_frames_written.load()));
  const double err_pct = expected > 0.0 ? (rx - expected) / expected * 100.0 : 0.0;
  const bool fs_ok = std::fabs(eff_fs - 48000.0) <= 240.0;
  const bool dma_ok = c.dma_errors.load() == 0 &&
                      c.actual_rx_dma_errors.load() == 0 &&
                      c.actual_tx_dma_errors.load() == 0 &&
                      c.i2s_read_failures.load() == 0 &&
                      c.i2s_write_failures.load() == 0;
  const bool miss_ok = c.dsp_deadline_misses.load() == 0;
  const bool backlog_ok = c.rx_overruns.load() == 0;
  printf("B4C6A_QUALIFY name=%s eff_fs=%.2f err_pct=%.4f fs_ok=%d "
         "dma_ok=%d miss_ok=%d backlog_ok=%d result=%s\n",
         label, eff_fs, err_pct, fs_ok ? 1 : 0, dma_ok ? 1 : 0,
         miss_ok ? 1 : 0, backlog_ok ? 1 : 0,
         (fs_ok && dma_ok && miss_ok && backlog_ok) ? "PASS" : "FAIL");
  s_audio.print_b4c6a_forensics(false);
  s_audio.print_b4c7_forensics(true);
  s_b4c8_audio_seconds += wall_s;
  s_b4c8_dma_errors += c.dma_errors.load();
}

// Print harmonizer trace summary for the window.
inline void b4c8_print_trace_summary() {
  HarmonizerBlockTraceRecord records[64];
  const size_t n = vocal_fx_get_harmonizer_trace(records, 64);
  if (n == 0) return;
  // Compute averages from the tail of the trace ring.
  double total_runtime = 0, total_v0 = 0, total_v1 = 0;
  double total_other_combined = 0;
  uint64_t total_new_grains = 0, total_active_desc = 0;
  uint64_t total_deferred_slices = 0;
  for (size_t i = 0; i < n; ++i) {
    total_runtime += records[i].block_runtime_us;
    total_v0 += records[i].voice0_runtime_us;
    total_v1 += records[i].voice1_runtime_us;
    total_new_grains += records[i].new_grains_scheduled_v0 +
                        records[i].new_grains_scheduled_v1;
    total_active_desc += records[i].deferred_active_grains_v0 +
                         records[i].deferred_active_grains_v1;
    total_deferred_slices += records[i].deferred_slices_rendered_v0 +
                             records[i].deferred_slices_rendered_v1;
  }
  if (n > 0) {
    printf("B4C8_TRACE_AVG blocks=%zu avg_runtime=%.1f avg_v0=%.1f "
           "avg_v1=%.1f avg_grains=%.1f avg_desc=%.1f avg_slices=%.1f\n",
           n, total_runtime / n, total_v0 / n, total_v1 / n,
           static_cast<double>(total_new_grains) / n,
           static_cast<double>(total_active_desc) / n,
           static_cast<double>(total_deferred_slices) / n);
  }
}

// Print B4C.8 Other decomposition CSV header and rows.
inline void b4c8_print_other_csv(int voice, const char *label) {
  printf("B4C8_OTHER_CSV voice=%d label=%s\n", voice, label);
  // Header
  printf("category,cycles_total,cycles_per_block_us,calls_per_block\n");
  // The actual rows are printed by b4c8_print_other_breakdown as
  // B4C8_OTHER_CAT lines; the analysis script converts to CSV.
}

// Print slow-block correlation data (top-100 blocks with work breakdown).
inline void b4c8_print_burst_correlations(int voice) {
  const size_t count = vocal_fx_slow_block_count(voice);
  printf("B4C8_BURST_CORRELATIONS voice=%d count=%zu\n", voice, count);
  for (size_t i = 0; i < count; ++i) {
    B4c8SlowBlockRecord rec;
    if (vocal_fx_get_slow_block(voice, i, &rec)) {
      printf("B4C8_BURST_ROW voice=%d dur_us=%lu grn0=%u grn1=%u "
             "desc0=%u desc1=%u slc0=%u slc1=%u "
             "src0=%u src1=%u fir0=%u fir1=%u "
             "ola=%u syn=%u model=%u f0=%.1f\n",
             voice,
             static_cast<unsigned long>(rec.block_duration_us),
             rec.new_grains_v0, rec.new_grains_v1,
             rec.active_descriptors_v0, rec.active_descriptors_v1,
             rec.deferred_slices_v0, rec.deferred_slices_v1,
             rec.source_samples_v0, rec.source_samples_v1,
             rec.fir_samples_v0, rec.fir_samples_v1,
             rec.ola_samples, rec.synthesis_samples,
             rec.model_changes,
             static_cast<double>(rec.pitch_f0));
    }
  }
}

// Print cache layout audit.
inline void b4c8_print_cache_layout() {
  printf("B4C8_CACHE_LAYOUT\n");
  VocalFxBufferAudit audits[64]{};
  const size_t n = vocal_fx_audit_buffers(audits, 64);
  for (size_t i = 0; i < n; ++i) {
    printf("B4C8_CACHE_OBJECT name=%s bytes=%zu placement=%s\n",
           audits[i].name, audits[i].size_bytes,
           audits[i].is_psram ? "PSRAM" : (audits[i].is_sram ? "INTERNAL" : "UNKNOWN"));
  }
  printf("B4C8_CACHE_OBJECT name=GainNormTrigBasis bytes=%zu placement=INTERNAL_STATIC\n",
         SharedLpcAnalysis::gainnorm_basis_bytes());
  // TdPsola per-voice sizes.
  for (int v = 0; v < 2; ++v) {
    printf("B4C8_CACHE_OBJECT name=TdPsola%d size=%llu\n", v,
           vocal_fx_harmony_voice_profile_stats(v, PitchShiftProfileSection::Total).blocks);
  }
}

// Run a single measurement window.
inline void b4c8_run_window(const char *name, uint32_t seconds, int voices,
                             B4C7InputKind input, uint8_t detail,
                             bool enable_slow_blocks, bool enable_stalls) {
  vocal_fx_reset_profiling_epoch();
  vocal_fx_reset_slice_audit();
  s_audio.reset_counters();
  s_audio.set_b4c6a_detail(detail);
  s_audio.set_b4c7_input(input);
  s_audio.set_b4b6_dsp_paused(false);
  vocal_fx_set_slice_audit_enabled(detail > 0);
  // Initialize slow-block and stall recorders on the TdPsola instances.
  vocal_fx_init_b4c8_recorders(enable_slow_blocks, enable_stalls);
  vocal_fx_init_b4c8b_recorder();
  b4c8_configure_frozen_dsp(voices);
  if (!start_pipeline_tasks(AudioI2sMode::B4C7HarmonizerAudit, true)) {
    printf("B4C8_CASE name=%s status=START_FAILED\n", name);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(500));
  s_audio.reset_counters();
  vocal_fx_reset_profiling_epoch();
  vocal_fx_reset_slice_audit();
  vocal_fx_reset_harmonizer_trace();
  const int64_t begin = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000U));
  const double wall_s = static_cast<double>(esp_timer_get_time() - begin) / 1000000.0;
  stop_pipeline_tasks(false);
  b4c8_print_snapshot(name, wall_s, detail);
  b4c8_print_transport(name);
  // Per-voice decomposition.
  for (int v = 0; v < voices; ++v) {
    b4c8_print_other_breakdown(v);
    b4c8_print_other_csv(v, name);
    b4c8_print_slow_blocks(v);
    b4c8_print_burst_correlations(v);
    b4c8_print_deferred_split(v);
    b4c8_print_loop_decomp(v);
    b4c8_print_model_change_comparison(v);
  }
  b4c8_print_stall_events();
  b4c8_print_trace_summary();
  // B4C.8B: per-block recording and MC delta breakdown.
  vocal_fx_print_b4c8b_block_records();
  vocal_fx_print_b4c8b_mc_delta_breakdown();
  printf("B4C8B_GLOBAL_BLOCK_ID count=%lu\n",
         static_cast<unsigned long>(vocal_fx_b4c8b_block_count()));
  // LPC order forensic gate (§46).
  printf("B4C8_LPC_GATE compile_default_order=%u analyzer_order=%u "
         "v0_synth=%u v1_synth=%u\n",
         static_cast<unsigned>(LpcConfig{}.order),
         static_cast<unsigned>(vocal_fx_lpc_config_order()),
         static_cast<unsigned>(vocal_fx_synthesis_order(0)),
         static_cast<unsigned>(vocal_fx_synthesis_order(1)));
  s_audio.deinit();
  vocal_fx_set_slice_audit_enabled(false);
  vocal_fx_free_b4c8b_recorder();
  vocal_fx_free_b4c8_recorders();
}

void b4c8_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));

  // Reconfigure WDT: extend to 120 s, monitor only Core 1 idle,
  // do not panic (measurement windows block the idle task).
  esp_task_wdt_config_t b4c8_wdt = {
      .timeout_ms = 120000,
      .idle_core_mask = (1 << 1),
      .trigger_panic = false,
  };
  (void)esp_task_wdt_reconfigure(&b4c8_wdt);

  VocalFxConfig cfg{};
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
  if (!vocal_fx_init(cfg)) {
    printf("B4C8_CASE name=init status=ENGINE_INIT_FAILED\nB4C8_END\n");
    vTaskDelete(nullptr);
    return;
  }
  printf("\nB4C8_START frozen_dsp=yes block_frames=64 nominal_fs=48000 "
         "deadline_us=1333.333\n");
  printf("B4C8_TASK audio_prio=MAX-2 audio_core=0 pitch_prio=MAX-5 "
         "pitch_core=1 coord_prio=1 coord_core=1\n");

  // §48 Memory audit.
  b4c8_print_memory();
  b4c8_print_cache_layout();

  // LPC order gate (§46): confirm once.
  printf("B4C8_LPC_GATE compile_default_order=%u analyzer_order=%u\n",
         static_cast<unsigned>(LpcConfig{}.order),
         static_cast<unsigned>(vocal_fx_lpc_config_order()));

  s_b4c8_stage = static_cast<float *>(heap_caps_calloc(
      kB4C8StageFrames, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_b4c8_stage) {
    printf("B4C8_CASE name=prestage status=ALLOC_FAILED\nB4C8_END\n");
    vTaskDelete(nullptr);
    return;
  }
  s_audio.set_b4c7_prestaged(s_b4c8_stage, kB4C8StageFrames);

  // ═══════════════════════════════════════════════════════════════════
  // §3 Diagnostic-overhead A/B: run identical input with DIAGNOSTIC
  // instrumentation and with QUALIFICATION minimal instrumentation.
  // ═══════════════════════════════════════════════════════════════════

#ifdef CONFIG_VOXP4_MODE_I2S_B4C_8_DIAGNOSTIC
  // ──── DIAGNOSTIC build: full instrumentation ────
  printf("B4C8_BUILD_MODE=DIAGNOSTIC\n");

  // §15-16: 1V production-equivalent 60 s.
  if (b4c8_prestage("vocal_65s", B4B6StimulusKind::VocalReplay, 220.0f,
                     kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("1v_diag_60s", 60, 1, B4C7InputKind::Prestaged, 2,
                     true, true);
  }

  // §15-16: 2V production-equivalent 60 s.
  if (b4c8_prestage("vocal_65s_b", B4B6StimulusKind::VocalReplay, 220.0f,
                     kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("2v_diag_60s", 60, 2, B4C7InputKind::Prestaged, 2,
                     true, true);
  }

  // §14-17: Worst-case synthetic >=60 s.
  if (b4c8_prestage("f0_1000", B4B6StimulusKind::HarmonicVoiced, 1000.0f,
                     nullptr, 0, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("worst_diag_60s", 60, 2, B4C7InputKind::Prestaged, 1,
                     true, true);
  }

  printf("B4C8_INSTRUMENTATION_RESULT mode=DIAGNOSTIC\n");
  printf("B4C8_INSTRUMENTATION_NOTE=full profiling active; overhead "
         "measured by comparing to QUALIFICATION build\n");

#endif // DIAGNOSTIC

#ifdef CONFIG_VOXP4_MODE_I2S_B4C_8_QUALIFICATION
  // ──── QUALIFICATION build: minimal instrumentation ────
  printf("B4C8_BUILD_MODE=QUALIFICATION\n");

  // §37: Hardware acceptance with low-overhead binary.
  // 1V >=60 s.
  if (b4c8_prestage("vocal_65s", B4B6StimulusKind::VocalReplay, 220.0f,
                     kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("1v_qual_60s", 60, 1, B4C7InputKind::Prestaged, 0,
                     true, true);
  }

  // 2V >=60 s.
  if (b4c8_prestage("vocal_65s_b", B4B6StimulusKind::VocalReplay, 220.0f,
                     kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("2v_qual_60s", 60, 2, B4C7InputKind::Prestaged, 0,
                     true, true);
  }

  // Worst-case synthetic >=60 s.
  if (b4c8_prestage("f0_1000", B4B6StimulusKind::HarmonicVoiced, 1000.0f,
                     nullptr, 0, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c8_stage, 65 * 48000);
    b4c8_run_window("worst_qual_60s", 60, 2, B4C7InputKind::Prestaged, 0,
                     true, true);
  }

  printf("B4C8_INSTRUMENTATION_RESULT mode=QUALIFICATION\n");

#endif // QUALIFICATION

  printf("B4C8_STABILITY total_audio_s=%.1f dma_errors=%llu\n",
         s_b4c8_audio_seconds,
         static_cast<unsigned long long>(s_b4c8_dma_errors));
  heap_caps_free(s_b4c8_stage);
  s_b4c8_stage = nullptr;
  printf("B4C8_END\n");
  // Restore normal WDT before exiting.
  esp_task_wdt_config_t restore_wdt = {
      .timeout_ms = 5000,
      .idle_core_mask = (1 << 0) | (1 << 1),
      .trigger_panic = true,
  };
  (void)esp_task_wdt_reconfigure(&restore_wdt);
  vTaskSuspend(nullptr);
}

} // namespace

#endif // B4C_8_DIAGNOSTIC || B4C_8_QUALIFICATION
