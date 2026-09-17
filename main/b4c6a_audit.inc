// B4C.6A — End-to-end audio-loop timing closure and lost-block forensics.
// Included by main/i2s_bringup.cpp. DSP kernels are FROZEN here: this file
// only selects the qualified production configuration (DeferredSlice,
// MultiFma, optimized GainNorm basis, FormantWarpCache, FX OFF) and records
// wall-clock RX/conversion/DSP/TX/bookkeeping/scheduler timing. No DSP,
// grain, OLA, LPC, pitch, articulation, FX, or Core 1 logic is modified.

#if defined(CONFIG_VOXP4_MODE_I2S_B4C_6A_TIMING_FORENSICS)

#include "esp_timer.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include <cmath>

namespace {

using vocal_fx_platform::B4B6StimulusKind;

// Canonical 1V forensic case (§23): 1 voice, real vocal, formant ON,
// DeferredSlice + MultiFma + optimized GainNorm, FX OFF.
inline void b4c6a_configure_frozen_dsp(int voices, bool transport_only) {
  vocal_fx_set_psola_grain_render_mode(0, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_grain_render_mode(1, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_fir_kernel(0, PsolaFirKernel::MultiFma);
  vocal_fx_set_psola_fir_kernel(1, PsolaFirKernel::MultiFma);
  vocal_fx_configure_psola_residual_cache(0, 2, true, false);
  vocal_fx_configure_psola_residual_cache(1, 2, true, false);
  vocal_fx_set_psola_residual_cache_enabled(0, true);
  vocal_fx_set_psola_residual_cache_enabled(1, true);
  vocal_fx_set_psola_warp_cache(true, true, false);
  SharedLpcAnalysis::prepare_gainnorm_basis(48000.0f);
  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);
  s_audio.set_b4c1_transport_only(transport_only);
  // Production gate/compressor/delay/reverb selection per case.
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

inline void b4c6a_print_task_map() {
  const UBaseType_t audio_prio =
      s_audio_task_handle ? uxTaskPriorityGet(s_audio_task_handle) : 0;
  const UBaseType_t pitch_prio =
      s_pitch_task_handle ? uxTaskPriorityGet(s_pitch_task_handle) : 0;
  const UBaseType_t coord_prio = uxTaskPriorityGet(nullptr);
  // Cores are pinned at creation: audio→0, pitch→1, coordinator→1.
  printf("B4C6A_TASK audio_prio=%u audio_core=0 pitch_prio=%u pitch_core=1 coord_prio=%u coord_core=1\n",
         static_cast<unsigned>(audio_prio), static_cast<unsigned>(pitch_prio),
         static_cast<unsigned>(coord_prio));
}

inline void b4c6a_print_memory_placement() {
  printf("B4C6A_MEMORY internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u\n",
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  // Hot-path placement evidence (§32). Allocation capabilities are fixed at
  // the call sites referenced; no PSRAM clock is changed in this stage.
  printf("B4C6A_MEMORY_PLACEMENT object=b4c6a_timing_ring placement=SPIRAM evidence=heap_caps_calloc_SPIRAM_8BIT_audio_i2s_init\n");
  printf("B4C6A_MEMORY_PLACEMENT object=profiler_distribution placement=SPIRAM evidence=distribution_calloc_SPIRAM_profiling_cpp\n");
  printf("B4C6A_MEMORY_PLACEMENT object=audio_task_stack placement=INTERNAL evidence=xTaskCreateStaticPinnedToCore_bss_128KB_core0\n");
  printf("B4C6A_MEMORY_PLACEMENT object=pitch_task_stack placement=INTERNAL evidence=xTaskCreateStaticPinnedToCore_bss_64KB_core1\n");
  printf("B4C6A_MEMORY_PLACEMENT object=coordinator_stack placement=SPIRAM evidence=xTaskCreatePinnedToCoreWithCaps_SPIRAM_core1\n");
  printf("B4C6A_MEMORY_PLACEMENT object=gainnorm_trig_basis placement=INTERNAL_STATIC evidence=SharedLpcAnalysis_internal_static_4032B\n");
  printf("B4C6A_MEMORY_PLACEMENT object=audio_loop_scratch placement=STACK_INTERNAL evidence=in32_out32_mono_l_r_task_stack\n");
  VocalFxBufferAudit audits[64]{};
  const size_t n = vocal_fx_audit_buffers(audits, 64);
  for (size_t i = 0; i < n; ++i) {
    printf("B4C6A_MEMORY_PLACEMENT object=%s placement=%s evidence=vocal_fx_audit_buffers\n",
           audits[i].name,
           audits[i].is_psram ? "PSRAM" : (audits[i].is_sram ? "INTERNAL" : "UNKNOWN"));
  }
}

inline void b4c6a_print_counter_snapshot(const char *label, double wall_s) {
  const auto &c = s_audio.counters();
  const double expected = wall_s * 48000.0 / 64.0;
  const double rx = static_cast<double>(c.rx_blocks_read.load());
  const double tx = static_cast<double>(c.tx_blocks_written.load());
  const double dsp = static_cast<double>(c.audio_blocks_processed.load());
  const double rx_frames = static_cast<double>(c.rx_frames_read.load());
  const double tx_frames = static_cast<double>(c.tx_frames_written.load());
  const double eff_fs = wall_s > 0.0 ? rx_frames / wall_s : 0.0;
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
  // Qualification rule (§30): effective Fs ±0.5%, frame counts consistent,
  // DMA/I2S errors 0, DSP misses 0, no cumulative backlog. Sequence gaps
  // alone are NOT sufficient (§13).
  const double err_pct =
      expected > 0.0 ? (rx - expected) / expected * 100.0 : 0.0;
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
  s_audio.print_b4c6a_forensics();
}

// Wall-clock driven window (§20): termination is N seconds of physical time
// measured with esp_timer, never a processed-block count. The coordinator
// blocks on vTaskDelay during the window (§19: no coordinator work while
// audio is measured). No printf/ESP_LOG/CSV formatting runs in the audio
// loop (§17): data lands in fixed PSRAM buffers and prints only below.
inline void b4c6a_run_window(const char *name, uint32_t seconds,
                             int voices, bool transport_only,
                             uint8_t detail) {
  s_audio.reset_counters();
  s_audio.set_b4c6a_detail(detail);
  // Real-vocal replay uses the shared 8 kHz mu-law fixture (also feeds the
  // identical analysis/DSP tap as prior stages); transport-only uses a
  // deterministic tone since DSP is bypassed there.
  if (transport_only) {
    s_audio.configure_b4b6_stimulus(B4B6StimulusKind::HarmonicVoiced, 220.0f);
  } else {
    s_audio.configure_b4b6_stimulus(B4B6StimulusKind::VocalReplay, 220.0f,
                                    kB4b6VocalMulaw, kB4b6VocalMulawSize);
  }
  s_audio.set_b4b6_dsp_paused(false);
  b4c6a_configure_frozen_dsp(voices, transport_only);
  if (!start_pipeline_tasks(vocal_fx_platform::AudioI2sMode::B4C6ATimingForensics, true)) {
    printf("B4C6A_CASE name=%s status=START_FAILED\n", name);
    return;
  }
  // Settle past the audio task's 100-block warmup (which zeroes its own
  // counters), then zero all counters from here so the wall-clock window
  // below contains only measured steady-state blocks (§20).
  vTaskDelay(pdMS_TO_TICKS(500));
  s_audio.reset_counters();
  const int64_t begin = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(seconds * 1000U));
  const double wall_s = static_cast<double>(esp_timer_get_time() - begin) / 1000000.0;
  // Stop tasks first so the window is exact, but defer deinit: it frees the
  // PSRAM timing ring that the snapshot below prints.
  stop_pipeline_tasks(false);
  b4c6a_print_counter_snapshot(name, wall_s);
  s_audio.deinit();
}

void b4c6a_coordinator_task(void *) {
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
    printf("B4C6A_CASE name=init status=ENGINE_INIT_FAILED\nB4C6A_END\n");
    vTaskDelete(nullptr);
    return;
  }
  printf("\nB4C6A_START frozen_dsp=yes block_frames=64 nominal_fs=48000 deadline_us=1333.333 termination=WALL_CLOCK esp_timer run_rule=vTaskDelay_seconds\n");
  printf("B4C6A_BOUNDARY avg_us=inside vocal_fx_process only; begins immediately before vocal_fx_process, ends immediately after; excludes I2S read blocking, PCM conversion, fixture generation, TX preparation/write, bookkeeping, scheduler, and harness\n");
  printf("B4C6A_SECTIONS A=rx_wait_driver_read B=rx_copy_convert C=dsp_pipeline D=tx_prepare E=tx_wait_submit F=fixture G=bookkeeping H=other_residual I=loop_total inter_gap=between_loops period=begin_to_begin\n");
  b4c6a_print_task_map();
  b4c6a_print_memory_placement();
  // Controls (§24 dry, §25 transport-only): expected effective Fs ≈ 48000.
  b4c6a_run_window("transport_only_10s", 10, 0, true, 2);
  b4c6a_run_window("dry_10s", 10, 0, false, 2);
  // Harness-cost A/B (§18): identical DSP, qualification harness OFF (detail
  // 0: no per-block ring writes) vs minimal B4C harness ON (detail 2).
  b4c6a_run_window("harness_off_1v_10s", 10, 1, false, 0);
  b4c6a_run_window("harness_on_1v_10s", 10, 1, false, 2);
  // Primary forensic cases (§23): 1V 10 s + 60 s wall-clock.
  b4c6a_run_window("1v_vocal_10s", 10, 1, false, 2);
  b4c6a_run_window("1v_vocal_60s", 60, 1, false, 2);
  // 2V diagnostic (§26): timing equation only, no optimization.
  b4c6a_run_window("2v_vocal_10s", 10, 2, false, 2);
  printf("B4C6A_END\n");
  vTaskSuspend(nullptr);
}

} // namespace

#endif
