#pragma once
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include "harmony_engine.h"
#include "lpc.h"
#include <cstddef>

struct VocalFxEffectiveDspConfig {
  YinDifferenceVariant yin_difference = YinDifferenceVariant::Fma8Acc;
  uint8_t yin_incremental_rebase_hops = 8;
  YinEnergyVariant yin_energy = YinEnergyVariant::ReferenceDouble;
  YinCmndVariant yin_cmnd = YinCmndVariant::ReferenceDouble;
  PitchMarkNccVariant pitch_mark_ncc = PitchMarkNccVariant::Reference;
  LpcWindowVariant lpc_windowing = LpcWindowVariant::Reference;
  LpcAutocorrelationVariant lpc_autocorrelation =
      LpcAutocorrelationVariant::AutocorrReferenceDouble;
};

// Host-callable factory used by the ESP_PLATFORM production path. Keeping the
// selection in one function lets regression tests instantiate the exact P4
// defaults without defining embedded-only macros.
PitchAnalysisConfig
vocal_fx_p4_pitch_analysis_defaults(float input_sample_rate = 48000.0f);
VocalFxEffectiveDspConfig vocal_fx_effective_dsp_config();

bool vocal_fx_init(const VocalFxConfig &config);
bool vocal_fx_init_pitch_analysis(const PitchAnalysisConfig &config);
void vocal_fx_reset();
void vocal_fx_process(const float *input, float *output_l, float *output_r,
                      size_t frames);
void vocal_fx_set_parameter(VocalFxParameter parameter, float value);
// Control-plane variant: reports whether the bounded SPSC queue accepted the
// update. Retained DSP behavior is identical to vocal_fx_set_parameter.
bool vocal_fx_try_set_parameter(VocalFxParameter parameter, float value);
// Control-plane observability: true once vocal_fx_init() succeeded and the
// audio task may drain the parameter queue.
bool vocal_fx_is_ready();
// Returns the exact float last handed to the engine for `parameter` at a
// block-boundary drain. Diagnostic only; no DSP effect. False if never applied
// since the last init.
bool vocal_fx_last_applied_parameter(VocalFxParameter parameter, float *value);
// Number of parameter applications recorded since the last init.
uint64_t vocal_fx_applied_parameter_count();
void vocal_fx_set_pitch_shift_enabled(bool enabled);
void vocal_fx_set_pitch_shift_semitones(float semitones);
void vocal_fx_set_pitch_shift_mix(float wet);
void vocal_fx_set_harmony_mode(HarmonyMode mode);
void vocal_fx_set_key(uint8_t chromatic_root);
void vocal_fx_set_scale(ScaleType scale);
void vocal_fx_set_harmony_enabled(size_t voice, bool enabled);
void vocal_fx_set_harmony_interval(size_t voice, float semitones);
void vocal_fx_set_harmony_degree(size_t voice, int degrees);
void vocal_fx_set_harmony_gain(size_t voice, float gain);
void vocal_fx_set_harmony_pan(size_t voice, float pan);
void vocal_fx_set_harmony_smoothing(size_t voice, float milliseconds);
void vocal_fx_set_formant_mode(size_t voice, FormantMode mode);
void vocal_fx_set_formant_amount(size_t voice, float amount);
void vocal_fx_set_formant_shift(size_t voice, float shift_semitones);
void vocal_fx_set_dry_alignment(bool enabled, float delay_ms = 32.0f);
void vocal_fx_set_harmony_attack_ms(float milliseconds);
void vocal_fx_set_harmony_release_ms(float milliseconds);
void vocal_fx_set_harmony_limiter(bool enabled, float threshold_db = -3.0f);
void vocal_fx_set_psola_kernels(PsolaLpcKernel lpc, PsolaOlaKernel ola,
                                PsolaGrainKernel grain, PsolaSynthesisKernel synth);
void vocal_fx_set_psola_warp_cache(bool enable_local, bool enable_shared, bool enable_neutral_fast_path);
PsolaWarpCacheStats vocal_fx_get_psola_warp_cache_stats(size_t voice);
void vocal_fx_reset_psola_warp_cache_stats();
size_t vocal_fx_get_harmonizer_trace(HarmonizerBlockTraceRecord *dst, size_t max_count);
void vocal_fx_reset_harmonizer_trace();
void vocal_fx_set_spatial_routing(SpatialFxRouting routing);
void vocal_fx_set_spatial_source(SpatialFxSource source);
void vocal_fx_set_mute_dry(bool mute);
void vocal_fx_midi_note_on(uint8_t note, uint8_t velocity);
void vocal_fx_midi_note_off(uint8_t note);
void vocal_fx_midi_all_notes_off();
PitchShiftTelemetry vocal_fx_harmony_telemetry(size_t voice);
GrainRejectionTelemetry vocal_fx_grain_rejection_telemetry(size_t voice);
uint32_t vocal_fx_pitch_shift_latency_samples();
PitchShiftTelemetry vocal_fx_pitch_shift_telemetry();
PitchShiftDebug vocal_fx_pitch_shift_debug();
PitchShiftDebug vocal_fx_harmony_debug(size_t voice);
#ifndef ESP_PLATFORM
bool vocal_fx_harmony_fallback_sample(size_t voice, size_t frame,
                                      PitchShiftFallbackReason *reason,
                                      bool *active);
bool vocal_fx_harmony_continuity_sample(size_t voice, size_t frame,
                                        bool *measured, bool *coasted);
bool vocal_fx_harmony_articulation_sample(size_t voice, size_t frame,
                                          bool *active, float *component,
                                          uint8_t *acoustic_class);
bool vocal_fx_harmony_plosive_bridge_sample(size_t voice, size_t frame,
                                            bool *active, float *component,
                                            float *gain, float *score,
                                            bool *phrase_start);
#endif
VocalFxProfileStats
vocal_fx_pitch_shift_profile_stats(PitchShiftProfileSection section);
VocalFxProfileStats
vocal_fx_harmony_voice_profile_stats(size_t voice, PitchShiftProfileSection section);
VocalFxLimiterDiagnostics vocal_fx_limiter_diagnostics();
void vocal_fx_reset_limiter_diagnostics();
void vocal_fx_publish_pitch(const PitchResult &result);
PitchResult vocal_fx_latest_pitch();
// Single-attempt snapshot for real-time callers. Returns false rather than
// waiting when a publisher is active or the snapshot changes during the read.
bool vocal_fx_try_latest_pitch(PitchResult *result);
// Called by the lower-priority analysis task; never by the audio callback.
size_t vocal_fx_run_pitch_analysis(size_t max_hops = 1);
bool vocal_fx_get_latest_pitch_mark(PitchMark *mark);
size_t vocal_fx_get_pitch_marks(uint64_t start_sample, uint64_t end_sample,
                                PitchMark *destination, size_t capacity);
bool vocal_fx_try_get_pitch_marks(uint64_t start_sample, uint64_t end_sample,
                                  PitchMark *destination, size_t capacity,
                                  size_t *written);
PitchTrackState vocal_fx_pitch_track_state();
PitchAnalysisDebug vocal_fx_pitch_analysis_debug();
PitchAnalysisAuditTelemetry vocal_fx_pitch_analysis_audit_telemetry();
// B4D.12: cheap current pitch-analysis backlog (ms) for per-block burn-in
// telemetry. Does not copy the full audit snapshot.
float vocal_fx_pitch_backlog_ms();
YinForensicTelemetry vocal_fx_yin_forensic_telemetry();
PitchMarkForensicTelemetry vocal_fx_pitch_mark_forensic_telemetry();
size_t vocal_fx_read_pitch_audit_events(PitchAuditEvent *events,
                                        size_t capacity);
VocalFxInputIdentity vocal_fx_input_identity();
uint64_t vocal_fx_analysis_latency_samples();
VocalFxProfileStats
vocal_fx_pitch_profile_stats(PitchAnalysisProfileSection section);
VocalFxProfileDistribution
vocal_fx_pitch_profile_distribution(PitchAnalysisProfileSection section);
size_t vocal_fx_dsp_memory_bytes();
size_t vocal_fx_delay_memory_bytes();
size_t vocal_fx_reverb_memory_bytes();
VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section);
void vocal_fx_reset_profiler();
void vocal_fx_reset_voice_profilers();
void vocal_fx_reset_profiling_epoch();
PsolaModelWarpAudit vocal_fx_get_psola_model_warp_audit(size_t voice);
PsolaSourceGrainAudit vocal_fx_get_psola_source_grain_audit();
PsolaSchedulingSlackAudit vocal_fx_get_psola_scheduling_slack_audit();
// B4C.7 observability additions (no DSP behavior change).
bool vocal_fx_latest_harmonizer_trace(HarmonizerBlockTraceRecord *out);
VocalFxProfileStats vocal_fx_voice_profile_stats(size_t voice,
                                                 PitchShiftProfileSection section);
uint16_t vocal_fx_lpc_config_order();
uint16_t vocal_fx_synthesis_order(size_t voice);
uint64_t vocal_fx_voice_b4c7_cycles(size_t voice, size_t idx);
void vocal_fx_set_slice_audit_enabled(bool enabled);
void vocal_fx_reset_slice_audit();
void vocal_fx_next_slice_audit_block();
B4C7SliceAuditSnapshot vocal_fx_slice_audit_snapshot();
void vocal_fx_configure_psola_residual_cache(size_t voice, size_t entries, bool enable_residual, bool enable_windowed, bool force_psram = false);
void vocal_fx_set_psola_residual_cache_enabled(size_t voice, bool enabled);
void vocal_fx_set_psola_windowed_cache_enabled(size_t voice, bool enabled);
PsolaSourceResidualCacheStats vocal_fx_get_psola_residual_cache_stats(size_t voice);
void vocal_fx_reset_psola_residual_cache_stats(size_t voice);
void vocal_fx_set_psola_precompute_enabled(size_t voice, bool enabled, float horizon = 1.0f);
PsolaPrecomputeStats vocal_fx_get_psola_precompute_stats(size_t voice);
void vocal_fx_reset_psola_precompute_stats(size_t voice);
void vocal_fx_set_psola_grain_render_mode(size_t voice, PsolaGrainRenderMode mode);
void vocal_fx_set_psola_fir_kernel(size_t voice, PsolaFirKernel kernel);
PsolaDeferredStats vocal_fx_get_psola_deferred_stats(size_t voice);
void vocal_fx_reset_psola_deferred_stats(size_t voice);
SingleGrainBenchmarkResult vocal_fx_benchmark_single_grain(
    size_t grain_length, size_t order, PsolaFirKernel kernel = PsolaFirKernel::Multi8);
// Audit-only counter reset. DSP/tracker/filter state remains untouched.
void vocal_fx_reset_measurement_telemetry();
size_t vocal_fx_audit_buffers(VocalFxBufferAudit *out, size_t max_count);
LpcTelemetry vocal_fx_lpc_telemetry();
VocalFxProfileStats vocal_fx_lpc_profile_stats(LpcProfileSection section);
LpcFrameCostSummary vocal_fx_lpc_frame_cost_summary();

// Stage funnel & diagnostic telemetry (Req 5, 7, 8)
VocalFxFunnelStats vocal_fx_funnel_stats();
void vocal_fx_reset_funnel_stats();
void vocal_fx_funnel_inc_synthetic_tone_blocks(uint64_t count = 1);
float vocal_fx_latest_pitch_age_ms();
float vocal_fx_effective_harmony_mix(size_t voice);
PitchSyncDiagnostics vocal_fx_pitch_sync_diagnostics();

// B4C.8 observability (no DSP behavior change).
uint64_t vocal_fx_voice_other_cycles(size_t voice, size_t cat_index);
void vocal_fx_init_b4c8_recorders(bool slow_blocks, bool stalls);
void vocal_fx_free_b4c8_recorders();
size_t vocal_fx_slow_block_count(size_t voice);
bool vocal_fx_get_slow_block(size_t voice, size_t index, B4c8SlowBlockRecord *out);
size_t vocal_fx_stall_event_count();
bool vocal_fx_get_stall_event(size_t index, B4c8StallEvent *out);

// B4C.8A observability: deferred traversal, loop decomposition, model-change.
uint64_t vocal_fx_deferred_zero_cycles(size_t voice);
uint64_t vocal_fx_deferred_zero_calls(size_t voice);
uint64_t vocal_fx_deferred_active_cycles(size_t voice);
uint64_t vocal_fx_deferred_active_calls(size_t voice);
uint64_t vocal_fx_loop_history_fetch_cycles(size_t voice);
uint64_t vocal_fx_loop_ola_norm_reset_cycles(size_t voice);
uint64_t vocal_fx_loop_indexing_cycles(size_t voice);
uint64_t vocal_fx_model_change_blocks(size_t voice);
uint64_t vocal_fx_model_change_cycles(size_t voice);
uint64_t vocal_fx_no_model_change_blocks(size_t voice);
uint64_t vocal_fx_no_model_change_cycles(size_t voice);
uint64_t vocal_fx_model_change_max_cycles(size_t voice);
uint64_t vocal_fx_no_model_change_max_cycles(size_t voice);

// B4C.8B observability: per-block recording and MC delta breakdown.
void vocal_fx_init_b4c8b_recorder();
void vocal_fx_free_b4c8b_recorder();
void vocal_fx_print_b4c8b_block_records();
void vocal_fx_print_b4c8b_mc_delta_breakdown();
uint32_t vocal_fx_b4c8b_block_count();
bool vocal_fx_get_b4c8b_block_record(size_t voice, size_t index,
                                     B4c8bBlockRecord *out);

// B4D.1: 1 when either harmony voice observed a new LPC formant model in the
// most recently processed DSP block. Sampled by the audio transport in the
// same block as the whole-DSP timing for the MC x deadline-miss contingency.
uint8_t vocal_fx_last_block_model_changed();

// B4D.3: per-block voice-0 renderer counters sampled by the audio transport in
// the same block, so MC/grain/miss contingencies are exact (no index join).
struct VocalFxLastBlockStats {
  uint8_t new_grains = 0;
  uint8_t exp_warp = 0;
  uint8_t active_desc = 0;
  uint8_t slices = 0;
  // B4D.5 paired-block matching inputs (voice 0).
  float f0 = 0.0f;
  uint8_t source_grains = 0;
  // B4D.7: scheduler iterations in the block.
  uint16_t schedule_attempts = 0;
  uint32_t sched_cycles = 0;
  uint32_t addgrain_cycles = 0;
  uint32_t deferred_cycles = 0;
  uint8_t model_new_count = 0;
  uint8_t warp_hit_count = 0;
  uint8_t warp_miss_count = 0;
  uint32_t mark_cycles = 0;
  uint32_t warp_phase_cycles = 0;
  uint32_t desc_cycles = 0;
  uint32_t near_cycles = 0;
  uint32_t poly_cycles = 0;
  uint32_t gn_cycles = 0;
  uint32_t cl_cycles = 0;
  uint32_t ch_cycles = 0;
  // B4D.8 per-grain ordinals (0,1,2).
  uint32_t ord_mark[3]{};
  uint32_t ord_warp[3]{};
  uint32_t ord_desc[3]{};
  uint32_t ord_near[3]{};
  uint32_t ord_sel[3]{};
  uint32_t ord_align[3]{};
  uint16_t mark_count = 0;
  uint32_t prewarm_cycles = 0;
  int32_t debt_samples = 0;
  uint32_t output_period_q8 = 0;
  int32_t g_dest[4]{};
  uint16_t g_half[4]{};
};
VocalFxLastBlockStats vocal_fx_last_block_stats();

// B4D.2 reverb observability (no DSP behaviour change). The profile is only
// accumulated while enabled so the production hot path pays nothing.
struct VocalFxReverbProfile {
  uint64_t read_damping_cycles = 0;
  uint64_t mix_cycles = 0;
  uint64_t hadamard_cycles = 0;
  uint64_t diffuser_cycles = 0;
  uint64_t write_index_cycles = 0;
  uint64_t wet_mix_cycles = 0;
  uint64_t total_cycles = 0;
  uint64_t samples = 0;
  uint64_t blocks = 0;
};
void vocal_fx_set_reverb_profile_enabled(bool enabled);
void vocal_fx_reset_reverb_profile();
VocalFxReverbProfile vocal_fx_reverb_profile();
size_t vocal_fx_reverb_line_bytes(size_t i);
bool vocal_fx_reverb_line_in_psram(size_t i);
size_t vocal_fx_reverb_diffuser_bytes(size_t i);
bool vocal_fx_reverb_diffuser_in_psram(size_t i);
size_t vocal_fx_reverb_total_bytes();

// B4D.4: invalid LPC frame reasons since the last reset.
void vocal_fx_lpc_invalid_reasons(uint64_t *unvoiced, uint64_t *solve_fail);
void vocal_fx_reset_lpc_invalid_reasons();

// B4D.4 per-grain-count section attribution (voice 0). Buckets 0,1,2,3+ new
// grains; section index is PitchShiftProfileSection.
bool vocal_fx_b4d4_class_init();
void vocal_fx_b4d4_class_free();
uint64_t vocal_fx_b4d4_class_cycles(size_t bucket, size_t section);
uint64_t vocal_fx_b4d4_class_blocks(size_t bucket);

// B4D.5 exact mark-selection / model-lookup decomposition (diagnostic).
void vocal_fx_b4d5_audit_reset();
void vocal_fx_b4d5_audit_enable(bool on);
void vocal_fx_b4d5_mark_audit(uint64_t *calls, uint64_t *candidates,
                              uint64_t *scan_cycles, uint64_t *total_cycles);
void vocal_fx_b4d5_model_audit(uint64_t *calls, uint64_t *candidates,
                               uint64_t *scan_cycles, uint64_t *copies,
                               uint64_t *repeat);
void vocal_fx_b4d5_global_class_init();
void vocal_fx_b4d5_global_class_free();
uint64_t vocal_fx_b4d5_global_class_cycles(size_t bucket, size_t group);
uint64_t vocal_fx_b4d5_global_class_blocks(size_t bucket);
const char *vocal_fx_b4d5_global_class_name(size_t group);

// B4D.9 diagnostic prewarm variant (0=off; bit0 marks, bit1 model ring,
// bit2 GainNorm tables). Read-only; no DSP effect.
void vocal_fx_b4d9_set_prewarm_variant(int v);
// B4D.10 scheduler geometry variant (0=S0, 1=S1). Diagnostic only.
void vocal_fx_b4d10_set_sched_variant(int v);

// B4D.2 warp-cache audit (§7): total model-change warp-cache misses and how
// many differ only by model_timestamp (all mathematical inputs identical).
void vocal_fx_warp_cache_audit(uint64_t *miss_total, uint64_t *math_only_miss);
void vocal_fx_reset_warp_cache_audit();

#ifndef ESP_PLATFORM
typedef void (*VocalFxSampleTelemetryCallback)(const SampleTelemetryRecord *records, size_t count, void *user_data);
void vocal_fx_set_sample_telemetry_callback(VocalFxSampleTelemetryCallback cb, void *user_data);
#endif

