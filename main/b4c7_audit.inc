// B4C.7 — 2-voice throughput, cross-voice reuse & PSRAM 200 MHz audit.
// Included by main/i2s_bringup.cpp. DSP semantics are FROZEN (§1): this file
// only stages input outside the timed loop, selects the qualified production
// configuration, and prints observability. No YIN/pitch/LPC/formant/grain/
// OLA/synthesis/articulation/recovery/FX/Core-1 logic is modified.

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT)

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

// 65 s prestage capacity: 65*48000 mono floats in PSRAM (12.48 MB).
constexpr size_t kB4C7StageSeconds = 65;
constexpr size_t kB4C7StageFrames = kB4C7StageSeconds * 48000;
float *s_b4c7_stage = nullptr;
double s_b4c7_audio_seconds = 0.0;
uint64_t s_b4c7_dma_errors = 0;

inline uint32_t b4c7_fnv(const float *buf, size_t frames) {
  uint32_t h = 2166136261U;
  for (size_t i = 0; i < frames; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &buf[i], sizeof(bits));
    h = (h ^ bits) * 16777619U;
  }
  return h;
}

inline double b4c7_rms(const float *buf, size_t frames) {
  double s = 0.0;
  for (size_t i = 0; i < frames; ++i) s += static_cast<double>(buf[i]) * buf[i];
  return frames ? std::sqrt(s / frames) : 0.0;
}

// Frozen production DSP selection (identical to B4C.6A canonical).
inline void b4c7_configure_frozen_dsp(int voices) {
  vocal_fx_set_psola_grain_render_mode(0, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_grain_render_mode(1, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_fir_kernel(0, PsolaFirKernel::MultiFma);
  vocal_fx_set_psola_fir_kernel(1, PsolaFirKernel::MultiFma);
  // B4C.7 FIR-cache A/B: disabled here (direct clip FIR). The enabled run
  // showed net loss on vocal (2-entry thrash + full-grain fills); this run
  // isolates the cache delta with slew hoist + audits held constant.
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

// §45 PSRAM bandwidth microbenchmark (outside realtime audio).
inline void b4c7_psram_bandwidth() {
  for (int cfg = 0; cfg < 2; ++cfg) {
    const size_t floats = cfg == 0 ? 65536 : 262144;  // 256 KB, 1 MB
    float *a = static_cast<float *>(heap_caps_malloc(
        floats * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    float *b = static_cast<float *>(heap_caps_malloc(
        floats * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!a || !b) {
      printf("B4C7_PSRAM_BW size_kb=%u status=ALLOC_FAILED\n",
             static_cast<unsigned>(floats * sizeof(float) / 1024));
      if (a) heap_caps_free(a);
      if (b) heap_caps_free(b);
      continue;
    }
    const double bytes = static_cast<double>(floats * sizeof(float));
    volatile double sink = 0.0;
    int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < floats; ++i) a[i] = static_cast<float>(i) * 0.5f;
    vTaskDelay(1);  // feed watchdog during long PSRAM sweeps
    int64_t t1 = esp_timer_get_time();
    for (size_t i = 0; i < floats; ++i) sink += a[i];
    int64_t t2 = esp_timer_get_time();
    std::memcpy(b, a, floats * sizeof(float));
    int64_t t3 = esp_timer_get_time();
    // Spot-verify copy integrity (full memcmp would dominate the timing).
    bool ok = (b[0] == a[0]) && (b[floats - 1] == a[floats - 1]) &&
              (b[floats / 2] == a[floats / 2]);
    printf("B4C7_PSRAM_BW size_kb=%u write_MBps=%.2f read_MBps=%.2f copy_MBps=%.2f verify=%s sink=%.1f\n",
           static_cast<unsigned>(floats * sizeof(float) / 1024),
           bytes / (t1 - t0 > 0 ? (t1 - t0) : 1),
           bytes / (t2 - t1 > 0 ? (t2 - t1) : 1),
           bytes / (t3 - t2 > 0 ? (t3 - t2) : 1), ok ? "OK" : "FAIL", sink);
    heap_caps_free(a);
    heap_caps_free(b);
  }
}

// §47 internal heap audit (§32 carry-over + B4C.7 named buffers).
inline void b4c7_print_memory_placement() {
  printf("B4C7_MEMORY internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  VocalFxBufferAudit audits[64]{};
  const size_t n = vocal_fx_audit_buffers(audits, 64);
  for (size_t i = 0; i < n; ++i) {
    printf("B4C7_MEMORY_PLACEMENT object=%s bytes=%zu placement=%s\n",
           audits[i].name, audits[i].size_bytes,
           audits[i].is_psram ? "PSRAM" : (audits[i].is_sram ? "INTERNAL" : "UNKNOWN"));
  }
  printf("B4C7_MEMORY_PLACEMENT object=GainNormTrigBasis bytes=%zu placement=INTERNAL_STATIC\n",
         SharedLpcAnalysis::gainnorm_basis_bytes());
  printf("B4C7_MEMORY_PLACEMENT object=FormantWarpCacheV0V1 bytes=member placement=INTERNAL_ENGINE_BSS\n");
  printf("B4C7_MEMORY_PLACEMENT object=PrestageBuffer bytes=%zu placement=SPIRAM\n",
         kB4C7StageFrames * sizeof(float));
}

// Pre-stage stimulus into PSRAM outside the timed window (§12). Uses the
// identical generator the loop would have called; identity is verified by
// regenerating the head after a reset and comparing checksums.
inline bool b4c7_prestage(const char *name, B4B6StimulusKind kind, float f0,
                          const uint8_t *mulaw, size_t mulaw_n,
                          size_t frames) {
  if (!s_b4c7_stage || frames > kB4C7StageFrames) return false;
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  for (size_t pos = 0; pos < frames; pos += 4096) {
    const size_t chunk = (frames - pos > 4096) ? 4096 : (frames - pos);
    s_audio.generate_b4b6_mono(s_b4c7_stage + pos, chunk);
    // Staging millions of frames exceeds the 5 s task watchdog; yield
    // per chunk (staging is outside every timed window).
    vTaskDelay(1);
  }
  const uint32_t fnv = b4c7_fnv(s_b4c7_stage, frames);
  const double rms = b4c7_rms(s_b4c7_stage, frames);
  // Identity proof: reset + regenerate head, compare. Head length is an
  // exact multiple of the chunk (6x1024) so every checked sample is written.
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  float head[6144];
  for (size_t pos = 0; pos < 6144; pos += 1024)
    s_audio.generate_b4b6_mono(head + pos, 1024);
  const uint32_t fnv_head = b4c7_fnv(head, 6144);
  const uint32_t fnv_staged = b4c7_fnv(s_b4c7_stage, 6144);
  const bool ok = (fnv_head == fnv_staged);
  printf("B4C7_PRESTAGE name=%s frames=%u fnv=%08lx rms=%.6f identity=%s\n",
         name, static_cast<unsigned>(frames), static_cast<unsigned long>(fnv),
         rms, ok ? "OK" : "FAIL");
  // Re-reset generator so the staged sequence starts at block 0 semantics.
  s_audio.configure_b4b6_stimulus(kind, f0, mulaw, mulaw_n);
  return ok;
}

inline void b4c7_print_snapshot(const char *label, double wall_s) {
  const auto &c = s_audio.counters();
  const double expected = wall_s * 48000.0 / 64.0;
  const double rx = static_cast<double>(c.rx_blocks_read.load());
  const double tx = static_cast<double>(c.tx_blocks_written.load());
  const double dsp = static_cast<double>(c.audio_blocks_processed.load());
  const double rx_frames = static_cast<double>(c.rx_frames_read.load());
  const double eff_fs = wall_s > 0.0 ? rx_frames / wall_s : 0.0;
  // B4C6A-compatible records so the whole-loop timing stays parseable.
  printf("B4C6A_CASE name=%s wall_s=%.6f expected_blocks=%.3f actual_rx_blocks=%.0f actual_tx_blocks=%.0f actual_dsp_blocks=%.0f missing_rx_blocks=%.3f missing_tx_blocks=%.3f missing_dsp_blocks=%.3f effective_fs=%.3f\n",
         label, wall_s, expected, rx, tx, dsp, expected - rx, expected - tx,
         expected - dsp, eff_fs);
  printf("B4C6A_TRANSPORT rx_dma_events=%llu tx_dma_events=%llu rx_overruns=%llu tx_underruns=%llu dma_errors=%llu actual_rx_dma_errors=%llu actual_tx_dma_errors=%llu read_failures=%llu write_failures=%llu rx_seq_gaps=%llu tx_seq_gaps=%llu rx_frames=%llu tx_frames=%llu\n",
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
  printf("B4C6A_QUALIFY name=%s eff_fs=%.2f err_pct=%.4f fs_ok=%d dma_ok=%d miss_ok=%d backlog_ok=%d result=%s\n",
         label, eff_fs, err_pct, fs_ok ? 1 : 0, dma_ok ? 1 : 0,
         miss_ok ? 1 : 0, backlog_ok ? 1 : 0,
         (fs_ok && dma_ok && miss_ok && backlog_ok) ? "PASS" : "FAIL");
  // Loop-timing aggregates only (per-block rows would double serial volume;
  // B4C7_BLOCK carries the B4C.7 sample).
  s_audio.print_b4c6a_forensics(false);
  s_audio.print_b4c7_forensics(true);
  s_b4c7_audio_seconds += wall_s;
  s_b4c7_dma_errors += c.dma_errors.load();
}

// Wall-clock window with prestaged or live input. Staging/identity work
// happens before this call and is never inside the timed region.
inline void b4c7_run_window(const char *name, uint32_t seconds, int voices,
                            B4C7InputKind input, uint8_t detail) {
  vocal_fx_reset_profiling_epoch();
  printf("B4C7_TRACE name=%s step=epoch_reset\n", name);
  vocal_fx_reset_slice_audit();
  s_audio.reset_counters();
  printf("B4C7_TRACE name=%s step=counters_reset\n", name);
  s_audio.set_b4c6a_detail(detail);
  s_audio.set_b4c7_input(input);
  s_audio.set_b4b6_dsp_paused(false);
  vocal_fx_set_slice_audit_enabled(detail > 0);
  printf("B4C7_TRACE name=%s step=slice_enabled\n", name);
  b4c7_configure_frozen_dsp(voices);
  printf("B4C7_TRACE name=%s step=dsp_configured\n", name);
  if (!start_pipeline_tasks(AudioI2sMode::B4C7HarmonizerAudit, true)) {
    printf("B4C6A_CASE name=%s status=START_FAILED\n", name);
    return;
  }
  printf("B4C7_TRACE name=%s step=pipeline_started\n", name);
  vTaskDelay(pdMS_TO_TICKS(500));
  // Re-zero after settle so the wall window holds steady-state blocks only.
  s_audio.reset_counters();
  vocal_fx_reset_profiling_epoch();
  vocal_fx_reset_slice_audit();
  const int64_t begin = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000U));
  const double wall_s = static_cast<double>(esp_timer_get_time() - begin) / 1000000.0;
  stop_pipeline_tasks(false);
  b4c7_print_snapshot(name, wall_s);
  s_audio.deinit();
  vocal_fx_set_slice_audit_enabled(false);
}

void b4c7_coordinator_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(250));
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
    printf("B4C7_CASE name=init status=ENGINE_INIT_FAILED\nB4C7_END\n");
    vTaskDelete(nullptr);
    return;
  }
  printf("\nB4C7_START frozen_dsp=yes block_frames=64 nominal_fs=48000 deadline_us=1333.333 termination=WALL_CLOCK\n");
  printf("B4C7_BOUNDARY avg_us=inside vocal_fx_process only; BOTH-ACTIVE class=both voices rendered >=1 deferred slice this block (render state, not enable flags)\n");
  printf("B4C7_TASK audio_prio=MAX-2 audio_core=0 pitch_prio=MAX-5 pitch_core=1 coord_prio=1 coord_core=1\n");
  b4c7_psram_bandwidth();
  // LPC-order forensic gate (§33): compile default, analyzer runtime.
  // Per-voice synthesis orders print live per case in B4C7_LPC lines.
  printf("B4C7_LPC_GATE compile_default_order=%u analyzer_order=%u\n",
         static_cast<unsigned>(LpcConfig{}.order),
         static_cast<unsigned>(vocal_fx_lpc_config_order()));
  b4c7_configure_frozen_dsp(2);
  b4c7_print_memory_placement();
  s_b4c7_stage = static_cast<float *>(heap_caps_calloc(
      kB4C7StageFrames, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!s_b4c7_stage) {
    printf("B4C7_CASE name=prestage status=ALLOC_FAILED\nB4C7_END\n");
    vTaskDelete(nullptr);
    return;
  }
  s_audio.set_b4c7_prestaged(s_b4c7_stage, kB4C7StageFrames);

  // 1V production-equivalent 60 s (prestaged real vocal, §11-15).
  if (b4c7_prestage("vocal_65s", B4B6StimulusKind::VocalReplay, 220.0f,
                    kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c7_stage, 65 * 48000);
    b4c7_run_window("1v_prod_60s", 60, 1, B4C7InputKind::Prestaged, 2);
  }
  // 2V diagnostic prestaged vocal 10 s (reuse measurement, §21-23).
  if (b4c7_prestage("vocal_12s", B4B6StimulusKind::VocalReplay, 220.0f,
                    kB4b6VocalMulaw, kB4b6VocalMulawSize, 12 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c7_stage, 12 * 48000);
    b4c7_run_window("2v_diag_10s", 10, 2, B4C7InputKind::Prestaged, 2);
  }
  // 2V production 60 s baseline (before optimization).
  if (b4c7_prestage("vocal_65s_b", B4B6StimulusKind::VocalReplay, 220.0f,
                    kB4b6VocalMulaw, kB4b6VocalMulawSize, 65 * 48000)) {
    s_audio.set_b4c7_prestaged(s_b4c7_stage, 65 * 48000);
    b4c7_run_window("2v_prod_60s", 60, 2, B4C7InputKind::Prestaged, 2);
  }
  // High-F0 matrix (§42): prestaged harmonic tones, 10 s each.
  constexpr float kF0[] = {65.0f, 80.0f, 110.0f, 147.0f, 220.0f,
                           330.0f, 440.0f, 700.0f, 900.0f, 1000.0f};
  char fname[32];
  for (float f0 : kF0) {
    std::snprintf(fname, sizeof(fname), "f0_%.0f", static_cast<double>(f0));
    if (!b4c7_prestage(fname, B4B6StimulusKind::HarmonicVoiced, f0,
                       nullptr, 0, 12 * 48000))
      continue;
    s_audio.set_b4c7_prestaged(s_b4c7_stage, 12 * 48000);
    char wname[40];
    std::snprintf(wname, sizeof(wname), "f0_%.0f_10s", static_cast<double>(f0));
    b4c7_run_window(wname, 10, 2, B4C7InputKind::Prestaged, 1);
  }
  // Live-ADC sanity (true product path, §13): no prestage involved.
  b4c7_run_window("liveadc_1v_10s", 10, 1, B4C7InputKind::LiveAdc, 2);

  printf("B4C7_STABILITY total_audio_s=%.1f dma_errors=%llu panics=0 note=suspension_or_reset_would_have_aborted_END\n",
         s_b4c7_audio_seconds,
         static_cast<unsigned long long>(s_b4c7_dma_errors));
  heap_caps_free(s_b4c7_stage);
  s_b4c7_stage = nullptr;
  printf("B4C7_END\n");
  vTaskSuspend(nullptr);
}

} // namespace

#endif
