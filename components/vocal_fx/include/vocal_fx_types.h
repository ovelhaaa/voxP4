#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

enum class VocalFxParameter : uint16_t {
  GateThresholdDb,
  GateAttackMs,
  GateHoldMs,
  GateReleaseMs,
  GateRangeDb,
  CompressorThresholdDb,
  CompressorRatio,
  CompressorAttackMs,
  CompressorReleaseMs,
  CompressorMakeupDb,
  CompressorKneeDb,
  DelayLeftMs,
  DelayRightMs,
  DelayFeedback,
  DelayWet,
  DelayDry,
  DelayFeedbackLowpassHz,
  ReverbWet,
  ReverbDecaySeconds,
  ReverbDamping,
  LimiterCeiling,
  EnableGate,
  EnableCompressor,
  EnableDelay,
  EnableReverb,
  PitchShiftEnabled,
  PitchShiftSemitones,
  PitchShiftWet
  ,HarmonyMode, HarmonyKey, HarmonyScale,
  HarmonyVoice1Enabled, HarmonyVoice2Enabled,
  HarmonyVoice1Interval, HarmonyVoice2Interval,
  HarmonyVoice1Degree, HarmonyVoice2Degree,
  HarmonyVoice1Gain, HarmonyVoice2Gain,
  HarmonyVoice1Pan, HarmonyVoice2Pan,
  HarmonyVoice1Smoothing, HarmonyVoice2Smoothing,
  HarmonyVoice1NonScalePolicy, HarmonyVoice2NonScalePolicy,
  HarmonyVoice1VoiceLeadingEnabled, HarmonyVoice2VoiceLeadingEnabled,
  HarmonyVoice1MinMidi, HarmonyVoice2MinMidi,
  HarmonyVoice1MaxMidi, HarmonyVoice2MaxMidi
  ,FormantVoice1Mode, FormantVoice2Mode,
  FormantVoice1Amount, FormantVoice2Amount,
  FormantVoice1ShiftSemitones, FormantVoice2ShiftSemitones,
  DryAlignmentEnabled, DryAlignmentMs,
  HarmonyAttackMs, HarmonyReleaseMs,
  HarmonyLimiterEnabled, HarmonyLimiterThresholdDb,
  SpatialRouting, SpatialSource, MuteDry
};

enum class SpatialFxRouting : uint8_t {
  Parallel = 0,
  DelayIntoReverb = 1
};

enum class SpatialFxSource : uint8_t {
  MainMix = 0,    // Fold-down mono 0.5 * (Left + Right) [Current product behavior]
  DryOnly = 1,    // Dry voice pre-harmony [Reserved for future milestone]
  HarmonyOnly = 2 // Harmony bus only [Reserved for future milestone]
};

enum class PitchShiftMode : uint8_t { Bypass, FixedInterval };
enum class FormantMode : uint8_t { Off, Lpc };
enum class FormantNormalizationStrategy : uint8_t {
  StrategyA_DC = 0,
  StrategyB_ReferenceFrequency = 1,
  StrategyC_IntegratedSpectral = 2,
  StrategyD_ResidualEnergy = 3
};
enum class PitchShiftState : uint8_t {
  Bypass,
  WaitingForAnalysis,
  Acquiring,
  Active,
  Fallback
};

enum class PitchShiftFallbackReason : uint8_t {
  None,
  Unvoiced,
  LowConfidence,
  Acquiring,
  Onset,
  TargetInvalid,
  MarkInvalid,
  HistoryInvalid,
  Resync,
  RatioOutOfRange,
  Other
};

// OLA normalization variants used by the host energy investigation. Current
// and WindowSum are deliberately separate controls even though the present
// implementation of both is sum(x*w) / sum(w).
enum class OlaNormalizationMode : uint8_t {
  Current,
  WindowSum,
  WindowEnergy,
  ColaEnergyHybrid
};

enum class IsolatedPitchShiftStem : uint8_t {
  Final,
  Psola,
  Fallback,
  ReferenceDelayed,
  MeasuredPsola,
  CoastedPsola,
  CombinedPsola,
  Articulation,
  PlosiveBridge
};

enum class PlosiveBridgePolicy : uint8_t {
  B0_None = 0,
  B1_HpfSource = 1,
  B2_BandpassSource = 2,
  B3_Hybrid = 3,
  B4_BroadbandTransient = 4
};

struct HarmonyPlosiveBridgeConfig {
  bool enabled = true;
  PlosiveBridgePolicy policy = PlosiveBridgePolicy::B0_None;
  float transient_duration_ms = 3.0f; // 2.0 to 8.0 ms
  float transient_gain = 0.20f;        // 0.10 to 0.30
  float phrase_start_silence_ms = 80.0f; // silence threshold for phrase start
  float phrase_start_gain_multiplier = 1.25f; // 1.0 to 1.5
  float hybrid_alpha = 0.30f;          // weight for filtered source in hybrid (0.20 to 0.50)
  float bandpass_low_hz = 400.0f;
  float bandpass_high_hz = 6000.0f;
  float max_bridge_duration_ms = 20.0f; // hard clamp
  float timing_offset_ms = 0.0f;       // alignment relative to synthesis cursor
  float attack_ms = 0.5f;              // fast rise
  float decay_ms = 10.0f;              // smooth decay into articulation/PSOLA
};

enum class UnvoicedArticulationMode : uint8_t {
  U0_Silence = 0,
  U1_FullBand = 1,
  U2_Hpf = 2,
  U3_HpfEnvelope = 3,
  U4_NoiseExcitation = 4,
  U5_MultibandNoise = 5
};

enum class UnvoicedTimingReference : uint8_t {
  DelayedHistory = 0,
  CurrentInput = 1
};

enum class UnvoicedAcousticClass : uint8_t {
  Silence = 0,
  GenuinelyNonPeriodic = 1,
  VocalFryLowFreq = 2,
  Periodic = 3
};

struct HarmonyUnvoicedArticulationConfig {
  bool enabled = true;
  UnvoicedArticulationMode mode = UnvoicedArticulationMode::U0_Silence;
  float hpf_cutoff_hz = 2000.0f;
  float feed_gain = 0.25f;
  float attack_ms = 3.0f;
  float release_ms = 40.0f;
  float transition_ms = 5.0f;
  float silence_threshold = 0.005f;
  UnvoicedTimingReference timing_reference = UnvoicedTimingReference::DelayedHistory;
  uint32_t prng_seed = 0x12345678;
};

enum class PsolaContinuityPolicy : uint8_t {
  Baseline,
  OnsetContinuity,
  Coasting,
  OnsetContinuityCoasting
};

enum class HarmonyFallbackPolicy : uint8_t {
  CurrentDry,
  Muted,
  UnvoicedOnly,
  OnsetAndUnvoiced,
  HighpassUnvoiced
};

enum class PsolaRecoveryClass : uint8_t {
  None,
  SmallError,
  MediumError,
  NoteChangeCrossfade,
  HardReset
};

enum class PsolaRecoveryType : uint8_t {
  None,
  SameNote,
  Glide,
  NoteChange,
  OctaveSuspect
};

enum class PsolaRecoveryMode : uint8_t {
  Soft,
  OldHard
};

enum class WarmReacquireMode : uint8_t {
  W0_Current = 0,
  W1_Warm2Frame = 1,
  W2_Warm1FrameSameNote = 2,
  W3_Adaptive = 3
};

enum class WarmCandidateClass : uint8_t {
  None = 0,
  SameNote = 1,
  PlausibleMovement = 2,
  LikelyNoteChange = 3
};

enum class RejectionReason : uint8_t {
  None = 0,
  InsufficientConsecutiveFrames = 1,
  ConfidenceBelowThreshold = 2,
  PitchDeltaTooLarge = 3,
  MarkNotReady = 4,
  VoicedHysteresis = 5,
  OnsetHold = 6,
  StabilityCheck = 7,
  Other = 8
};

enum class PsolaLpcKernel : uint8_t {
  Reference = 0,
  ContiguousExact = 1,
  ContiguousMulti4 = 2,
  ContiguousMulti8 = 3,
};

enum class PsolaOlaKernel : uint8_t {
  Reference = 0,
  Contiguous = 1,
};

enum class PsolaGrainKernel : uint8_t {
  Reference = 0,
  ContiguousMulti4 = 1,
  ContiguousMulti8 = 2,
};

enum class PsolaSynthesisKernel : uint8_t {
  Reference = 0,
  UnrolledExact = 1,
};

struct PitchShiftConfig {
  bool enabled = false;
  float semitones = 0.0f;
  float wet = 1.0f;
  float smoothing_ms = 30.0f;
  uint32_t history_offset_samples = 1536;
  OlaNormalizationMode ola_normalization = OlaNormalizationMode::ColaEnergyHybrid;
  HarmonyFallbackPolicy fallback_policy = HarmonyFallbackPolicy::CurrentDry;
  float unvoiced_fallback_gain = 0.25f;
  float onset_fallback_gain = 0.15f;
  float unvoiced_hpf_hz = 2000.0f;
  PsolaContinuityPolicy continuity_policy = PsolaContinuityPolicy::Baseline;
  float coast_ms = 15.0f;
  PsolaRecoveryMode recovery_mode = PsolaRecoveryMode::Soft;
  float recovery_crossfade_ms = 5.0f;
  float recovery_small_error_cents = 50.0f;
  float recovery_note_change_cents = 150.0f;
  HarmonyUnvoicedArticulationConfig unvoiced_articulation{};
  HarmonyPlosiveBridgeConfig plosive_bridge{};
  FormantMode formant_mode = FormantMode::Off;
  float formant_amount = 1.0f;
  float formant_shift_semitones = 0.0f;
  float formant_bandwidth_expansion = 0.985f;
  FormantNormalizationStrategy formant_normalization_strategy = FormantNormalizationStrategy::StrategyC_IntegratedSpectral;
  bool stateful_voicing_enabled = true;
  float voiced_enter_confidence = 0.80f;
  float voiced_stay_confidence = 0.45f;
  float voiced_exit_confidence = 0.60f;
  uint8_t voiced_attack_frames = 2;
  uint8_t voiced_release_frames = 3;
  float f0_continuity_tolerance_cents = 150.0f;
  float max_unvoiced_zcr = 0.35f;
  float min_unvoiced_r1 = 0.30f;
  PsolaLpcKernel lpc_kernel = PsolaLpcKernel::ContiguousMulti8;
  PsolaOlaKernel ola_kernel = PsolaOlaKernel::Contiguous;
  PsolaGrainKernel grain_kernel = PsolaGrainKernel::ContiguousMulti8;
  PsolaSynthesisKernel synthesis_kernel = PsolaSynthesisKernel::UnrolledExact;
};

struct PitchShiftDebug {
  uint64_t input_absolute_sample = 0, pitch_timestamp = 0;
  uint64_t selected_source_mark = 0, source_grain_timestamp = 0;
  uint64_t output_synthesis_timestamp = 0, analysis_age = 0;
  uint32_t history_offset = 0;
  float requested_semitones = 0, target_ratio = 1, current_smoothed_ratio = 1;
  float source_f0 = 0, pitch_confidence = 0, target_f0 = 0;
  float actual_synthesis_period = 0;
  float psola_gain = 0, active_mix = 0, harmony_mix = 0, formant_mix = 0;
  float output_gain = 0, fallback_gain = 1;
  float ola_norm_min = 0, ola_norm_max = 0, ola_norm_mean = 0;
#ifndef ESP_PLATFORM
  float ola_norm_square_min = 0, ola_norm_square_max = 0;
  float ola_norm_square_mean = 0, ola_sum_mean = 0;
  float ola_overlap_mean = 0;
  uint64_t source_marks_1x = 0, source_marks_2x = 0;
  uint64_t source_marks_3x = 0, source_marks_4x_or_more = 0;
  uint64_t source_mark_reuses_total = 0;
  uint32_t ola_overlap_min = 0, ola_overlap_max = 0;
#endif
  uint64_t fallback_samples_total = 0, grains_total = 0, resyncs_total = 0;
  uint32_t onset_hold_samples = 0, ola_norm_samples_below_threshold = 0;
  uint8_t pitch_tracker_state = 0;
  PsolaRecoveryClass recovery_class = PsolaRecoveryClass::None;
  PsolaRecoveryType recovery_type = PsolaRecoveryType::None;
  float recovery_period_error_cents = 0.0f;
  float recovery_mark_error_fraction = 0.0f;
  float recovery_excess_discontinuity = 0.0f;
  float transition_energy_dip_db = 0.0f;
  uint32_t recovery_mark_error_samples = 0;
  bool crossfade_active = false;
  bool pitch_voiced = false, pitch_onset = false, pitch_changed = false;
  bool usable = false, onset_unvoiced_attenuation_enabled = true;
  bool target_valid = false, target_range_invalid = false;
  bool mark_valid = false, history_valid = false, fallback_active = true;
  PitchShiftFallbackReason fallback_reason = PitchShiftFallbackReason::Other;
  PitchShiftState voice_state = PitchShiftState::Bypass;
  float articulation_gain = 0.0f;
  uint8_t unvoiced_eligible = 0;
  uint8_t unvoiced_acoustic_class = 0;
  uint8_t articulation_active = 0;
  uint8_t plosive_bridge_active = 0;
  float plosive_bridge_gain = 0.0f;
  float plosive_score = 0.0f;
  uint8_t phrase_start_flag = 0;
  float formant_shift_semitones = 0.0f;
  uint32_t formant_resets = 0;
  float max_restored = 0.0f;
  float max_pre_tanh = 0.0f;
  float last_pre_tanh = 0.0f;
  float last_post_tanh = 0.0f;
  float last_gain_scale = 1.0f;
  float lpc_energy = 0.0f;
  float psola_ref_energy = 0.0f;
  float formant_gain_norm = 1.0f;
  uint32_t softclip_events = 0;
  uint32_t gain_rail_events = 0;
  uint64_t first_detected_mark = 0;
  uint64_t first_window_safe_mark = 0;
  uint64_t first_scheduled_mark = 0;
};

enum class PitchShiftProfileSection : uint8_t {
  MarkSelection = 0,
  GrainScheduling,
  GrainHistoryLookup,
  PlainWindowOLA,
  LpcResidualFIR,
  LpcModelLookup,
  LpcModelWarpPolynomial,
  LpcModelWarpGainNorm,
  LpcWindowOLA,
  LpcSynthesisAllPole,
  LpcStateShift,
  FormantGainMatcher,
  FormantSoftClip,
  FormantBlend,
  Fallback,
  Articulation,
  PlosiveBridge,
  Telemetry,
  Other,
  Total,
  Count
};

// Backwards compatibility alias
constexpr PitchShiftProfileSection PitchShiftProfileSection_LpcModelLookupWarp = PitchShiftProfileSection::LpcModelLookup;

struct PitchShiftTelemetry {
  uint64_t blocks = 0;
  uint64_t grains = 0;
  uint32_t max_grains_per_block = 0;
  uint64_t pitch_mark_underflows = 0;
  uint64_t audio_history_underflows = 0;
  uint64_t psola_resyncs = 0;
  uint64_t fallback_frames = 0;
  uint64_t max_grains_exceeded = 0;
  uint64_t invalid_pitch = 0;
  uint64_t invalid_mark = 0;
  uint64_t formant_frames = 0;
  uint64_t formant_resets = 0;
  float max_restored = 0.0f;
  PitchShiftState state = PitchShiftState::Bypass;
};

struct SourceGrainKey {
  uint64_t mark_center = 0;
  uint32_t half_window = 0;
  uint64_t lpc_timestamp = 0;
  uint16_t lpc_order = 0;
  uint32_t lambda_bits = 0;
  uint32_t gamma_bits = 0;
  uint8_t norm_strategy = 0;

  bool operator==(const SourceGrainKey &o) const {
    return mark_center == o.mark_center &&
           half_window == o.half_window &&
           lpc_timestamp == o.lpc_timestamp &&
           lpc_order == o.lpc_order &&
           lambda_bits == o.lambda_bits &&
           gamma_bits == o.gamma_bits &&
           norm_strategy == o.norm_strategy;
  }
};

struct PsolaSourceResidualCacheStats {
  uint64_t total_lookups = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t fir_samples_computed = 0;
  uint64_t fir_samples_reused = 0;
  uint64_t fir_cycles_saved = 0;
  uint64_t total_lookup_cycles = 0;
  uint64_t total_fir_cycles = 0;
  std::array<uint64_t, 10> reuse_distance_hist{}; // 1, 2, 3, 4, 5, 6, 7, 8, 9..16, >16

  double hit_rate_pct() const {
    return total_lookups > 0 ? (100.0 * static_cast<double>(hits) / total_lookups) : 0.0;
  }
  double sample_hit_rate_pct() const {
    const uint64_t tot = fir_samples_computed + fir_samples_reused;
    return tot > 0 ? (100.0 * static_cast<double>(fir_samples_reused) / tot) : 0.0;
  }
  void reset() { *this = PsolaSourceResidualCacheStats{}; }
};

struct PsolaPrecomputeStats {
  uint64_t precomputes_attempted = 0;
  uint64_t precomputes_completed = 0;
  uint64_t precomputes_consumed = 0;
  uint64_t precomputes_expired = 0;
  uint64_t synchronous_fallbacks = 0;
  uint32_t queue_max_depth = 0;

  void reset() { *this = PsolaPrecomputeStats{}; }
};

enum class PsolaGrainRenderMode : uint8_t {
  Eager = 0,
  DeferredSlice = 1,
};

enum class PsolaFirKernel : uint8_t {
  ContiguousScalar = 0,
  Multi2 = 1,
  Multi4 = 2,
  Multi8 = 3,
  MultiFma = 4,
};

struct SingleGrainBenchmarkResult {
  size_t grain_length = 0;
  size_t order = 10;
  // Component cycles
  uint32_t history_setup_cycles = 0;
  uint32_t model_lookup_cycles = 0;
  uint32_t warp_cache_lookup_cycles = 0;
  uint32_t warp_poly_cycles = 0;
  uint32_t gain_norm_cycles = 0;
  uint32_t fir_cycles = 0;
  uint32_t window_cycles = 0;
  uint32_t ola_write_cycles = 0;
  uint32_t norm_write_cycles = 0;
  uint32_t other_cycles = 0;
  uint32_t total_add_grain_cycles = 0;
  // Microseconds @ 360 MHz
  double history_setup_us = 0.0;
  double model_lookup_us = 0.0;
  double warp_cache_lookup_us = 0.0;
  double warp_poly_us = 0.0;
  double gain_norm_us = 0.0;
  double fir_us = 0.0;
  double window_us = 0.0;
  double ola_write_us = 0.0;
  double norm_write_us = 0.0;
  double other_us = 0.0;
  double total_add_grain_us = 0.0;
  // Metrics
  double cycles_per_source_sample = 0.0;
  double cycles_per_fir_tap = 0.0;
  double reconciliation_pct = 100.0;
};

struct PsolaDeferredStats {
  uint64_t descriptors_allocated = 0;
  uint64_t descriptors_retired = 0;
  uint64_t slices_rendered = 0;
  uint64_t slice_samples_rendered = 0;
  uint64_t slice_fir_samples = 0;
  uint32_t max_active_descriptors = 0;
  void reset() { *this = PsolaDeferredStats{}; }
};

struct HarmonizerBlockTraceRecord {
  uint32_t block_index = 0;
  // Final per-block stage deltas, populated when the pipeline completes.
  uint32_t input_cycles = 0;
  uint32_t compressor_cycles = 0;
  uint32_t harmony_cycles = 0;
  uint32_t delay_cycles = 0;
  uint32_t reverb_cycles = 0;
  uint32_t master_cycles = 0;
  // Existing TD-PSOLA cycle accumulators, sampled once per audio block.
  uint32_t psola_sched_cycles = 0;
  uint32_t psola_addgrain_cycles = 0;
  uint32_t psola_deferred_cycles = 0;
  uint32_t psola_mark_cycles = 0;
  uint32_t psola_desc_cycles = 0;
  uint32_t psola_near_cycles = 0;
  uint32_t psola_poly_cycles = 0;
  uint32_t psola_gn_cycles = 0;
  float block_runtime_us = 0.0f;
  float pipeline_base_us = 0.0f;
  float voice0_runtime_us = 0.0f;
  float voice1_runtime_us = 0.0f;
  // Distinct names prescribed by B4C.3A
  uint8_t new_grains_scheduled_v0 = 0;
  uint8_t new_grains_scheduled_v1 = 0;
  uint8_t active_grains_rendered_v0 = 0;
  uint8_t active_grains_rendered_v1 = 0;
  uint8_t source_grains_built_v0 = 0;
  uint8_t source_grains_built_v1 = 0;
  uint16_t grain_samples_processed = 0;
  uint16_t unique_grain_samples = 0;
  uint8_t unique_source_grain_keys = 0;
  uint8_t duplicate_source_grain_keys = 0;
  uint8_t model_warp_lookups = 0;
  uint8_t model_warp_expensive_calls = 0;
  float model_warp_us = 0.0f;
  uint64_t lpc_model_timestamp = 0;
  uint8_t formant_model_changed = 0;
  uint8_t pitch_changed = 0;
  uint8_t track_state = 0;
  uint8_t fallback_active = 0;
  uint8_t articulation_active = 0;
  uint8_t plosive_active = 0;
  uint8_t recovery_active = 0;
  float render_slack_blocks_min = 0.0f;

  // Prescribed B4C.3B trace metrics
  uint8_t residual_cache_hits_v0 = 0;
  uint8_t residual_cache_hits_v1 = 0;
  uint8_t residual_cache_misses_v0 = 0;
  uint8_t residual_cache_misses_v1 = 0;
  uint16_t fir_samples_computed = 0;
  uint16_t fir_samples_reused = 0;
  uint8_t active_overlapping_grains_v0 = 0;
  uint8_t active_overlapping_grains_v1 = 0;
  uint8_t precompute_queue_depth = 0;
  uint8_t precompute_expired_count = 0;

  // Prescribed B4C.4 deferred metrics
  uint8_t deferred_active_grains_v0 = 0;
  uint8_t deferred_active_grains_v1 = 0;
  uint8_t deferred_slices_rendered_v0 = 0;
  uint8_t deferred_slices_rendered_v1 = 0;
  uint16_t deferred_fir_samples_v0 = 0;
  uint16_t deferred_fir_samples_v1 = 0;

  // Backwards compatibility accessors
  uint8_t grains_scheduled_v0() const { return new_grains_scheduled_v0; }
  uint8_t grains_scheduled_v1() const { return new_grains_scheduled_v1; }
  uint8_t grains_rendered_v0() const { return active_grains_rendered_v0; }
  uint8_t grains_rendered_v1() const { return active_grains_rendered_v1; }
  uint8_t model_warp_calls() const { return model_warp_lookups; }
};

struct PsolaModelWarpAudit {
  uint64_t model_near_calls = 0;
  uint64_t model_near_cycles = 0;
  uint64_t lambda_calc_calls = 0;
  uint64_t lambda_calc_cycles = 0;
  uint64_t cache_lookup_calls = 0;
  uint64_t cache_lookup_cycles = 0;
  uint64_t cache_hit_calls = 0;
  uint64_t cache_hit_cycles = 0;
  uint64_t cache_miss_calls = 0;
  uint64_t cache_miss_cycles = 0;
  uint64_t warp_poly_calls = 0;
  uint64_t warp_poly_cycles = 0;
  uint64_t gain_norm_calls = 0;
  uint64_t gain_norm_cycles = 0;
  uint64_t coeff_copy_calls = 0;
  uint64_t coeff_copy_cycles = 0;
  uint64_t local_hits = 0;
  uint64_t shared_hits = 0;
  uint64_t neutral_hits = 0;
  uint64_t expensive_computations = 0;

  void reset() { *this = PsolaModelWarpAudit{}; }
};

struct PsolaSourceGrainAudit {
  uint64_t source_grains_requested = 0;
  uint64_t unique_source_grains = 0;
  uint64_t duplicate_source_grains = 0;
  uint64_t same_voice_reuses = 0;
  uint64_t cross_voice_reuses = 0;
  uint64_t total_grain_samples = 0;
  uint64_t reusable_grain_samples = 0;

  double reuse_ratio_pct() const {
    return source_grains_requested > 0
               ? (100.0 * static_cast<double>(duplicate_source_grains) /
                  static_cast<double>(source_grains_requested))
               : 0.0;
  }
  double cross_voice_reuse_pct() const {
    return source_grains_requested > 0
               ? (100.0 * static_cast<double>(cross_voice_reuses) /
                  static_cast<double>(source_grains_requested))
               : 0.0;
  }
  double same_voice_reuse_pct() const {
    return source_grains_requested > 0
               ? (100.0 * static_cast<double>(same_voice_reuses) /
                  static_cast<double>(source_grains_requested))
               : 0.0;
  }
  double weighted_reusable_samples_pct() const {
    return total_grain_samples > 0
               ? (100.0 * static_cast<double>(reusable_grain_samples) /
                  static_cast<double>(total_grain_samples))
               : 0.0;
  }
  void reset() { *this = PsolaSourceGrainAudit{}; }
};

// B4C.7 exact slice-level reuse snapshot (observability only).
struct B4C7SliceAuditSnapshot {
  uint64_t slices_requested = 0;
  uint64_t duplicates = 0;
  uint64_t cross_voice = 0;
  uint64_t same_voice = 0;
  uint64_t samples = 0;
  uint64_t reusable_samples = 0;
  uint64_t fir_taps = 0;
  uint64_t reusable_taps = 0;
  uint64_t entry_drops = 0;
};

struct PsolaSchedulingSlackAudit {
  static constexpr size_t kHistogramBins = 64; // 0..63 blocks
  uint64_t total_grains = 0;
  uint64_t slack_ge_1_block = 0;
  uint64_t slack_ge_2_blocks = 0;
  uint64_t slack_ge_4_blocks = 0;
  uint64_t slack_ge_8_blocks = 0;
  float min_slack_blocks = 9999.0f;
  float max_slack_blocks = -9999.0f;
  uint64_t histogram[kHistogramBins]{0};

  void record(float slack_blocks) {
    ++total_grains;
    min_slack_blocks = std::min(min_slack_blocks, slack_blocks);
    max_slack_blocks = std::max(max_slack_blocks, slack_blocks);
    if (slack_blocks >= 1.0f) ++slack_ge_1_block;
    if (slack_blocks >= 2.0f) ++slack_ge_2_blocks;
    if (slack_blocks >= 4.0f) ++slack_ge_4_blocks;
    if (slack_blocks >= 8.0f) ++slack_ge_8_blocks;
    const int bin = std::clamp(static_cast<int>(std::floor(slack_blocks)), 0, static_cast<int>(kHistogramBins - 1));
    histogram[bin]++;
  }

  double pct_ge_1_block() const {
    return total_grains > 0 ? (100.0 * static_cast<double>(slack_ge_1_block) / total_grains) : 0.0;
  }
  double pct_ge_2_blocks() const {
    return total_grains > 0 ? (100.0 * static_cast<double>(slack_ge_2_blocks) / total_grains) : 0.0;
  }
  double pct_ge_4_blocks() const {
    return total_grains > 0 ? (100.0 * static_cast<double>(slack_ge_4_blocks) / total_grains) : 0.0;
  }
  double pct_ge_8_blocks() const {
    return total_grains > 0 ? (100.0 * static_cast<double>(slack_ge_8_blocks) / total_grains) : 0.0;
  }

  float calculate_percentile_blocks(double p) const {
    if (total_grains == 0) return 0.0f;
    const uint64_t target = static_cast<uint64_t>(std::ceil(p * 0.01 * total_grains));
    uint64_t cum = 0;
    for (size_t i = 0; i < kHistogramBins; ++i) {
      cum += histogram[i];
      if (cum >= target) {
        return static_cast<float>(i) + 0.5f;
      }
    }
    return max_slack_blocks;
  }

  void reset() {
    *this = PsolaSchedulingSlackAudit{};
  }
};

struct PsolaWarpCacheStats {
  uint64_t total_calls = 0;
  uint64_t local_hits = 0;
  uint64_t shared_hits = 0;
  uint64_t neutral_hits = 0;
  uint64_t misses = 0;
  double hit_rate_pct() const {
    return total_calls > 0 ? 100.0 * static_cast<double>(local_hits + shared_hits + neutral_hits) / static_cast<double>(total_calls) : 0.0;
  }
};

enum class GrainFailureReason : uint8_t {
  None,
  AttemptSourceNegative,
  SelectMarkNoMarks,
  SelectMarkLowConfidence,
  SelectMarkInvalidPeriod,
  SelectMarkDistanceTooLarge,
  HistoryCenterBeforeHalf,
  HistoryTooOld,
  HistoryFutureEnd,
};

struct GrainFailureDiagnostic {
  uint64_t input_end = 0;
  uint64_t history_oldest_sample = 0;
  uint64_t pitch_analysis_timestamp = 0;
  uint64_t pitch_age_samples = 0;
  double requested_source = 0.0;
  double destination = 0.0;
  uint64_t selected_mark_center = 0;
  uint64_t mark_age_samples = 0;
  uint32_t half_window = 0;
  uint64_t required_first_sample = 0;
  uint64_t required_last_sample = 0;
  float source_to_nearest_mark = 0.0f;
  float signed_delta_samples = 0.0f;
  float delta_in_periods = 0.0f;
  float pitch_period = 0.0f;
  float allowed_distance = 0.0f;
  GrainFailureReason reason = GrainFailureReason::None;
};

struct GrainRejectionTelemetry {
  static constexpr size_t kDiagnosticCapacity = 1;
  uint64_t attempt_source_negative = 0;
  uint64_t select_mark_failure_total = 0;
  uint64_t select_mark_no_marks = 0;
  uint64_t select_mark_low_confidence = 0;
  uint64_t select_mark_invalid_period = 0;
  uint64_t select_mark_distance_too_large = 0;
  uint64_t history_failure_total = 0;
  uint64_t history_center_before_half = 0;
  uint64_t history_too_old = 0;
  uint64_t history_future_end = 0;
  uint32_t history_size_samples = 0;
  float history_size_ms = 0.0f;
  uint64_t input_end = 0;
  uint64_t oldest_available = 0;
  uint64_t alignment_observations = 0;
  // <-3, [-3,-2), [-2,-1), [-1,0), [0,1), [1,2), [2,3], >3.
  std::array<uint64_t, 8> signed_delta_period_histogram{};
  // [0,20), [20,40), [40,60), [60,100), [100,150), [150,250), >=250 ms.
  std::array<uint64_t, 7> pitch_age_attempt_histogram{};
  std::array<uint64_t, 7> pitch_age_distance_failure_histogram{};
  uint32_t diagnostic_count = 0;
  std::array<GrainFailureDiagnostic, kDiagnosticCapacity> diagnostics{};
};

struct PitchResult {
  PitchResult() = default;
  // Milestone-1 source compatibility.
  PitchResult(float hz, float certainty, bool is_voiced, uint64_t timestamp)
      : frequency_hz(hz), confidence(certainty), voiced(is_voiced),
        analysis_timestamp_samples(timestamp), timestamp_samples(timestamp) {}
  float frequency_hz = 0.0f;
  // Unsmoothed detector result for diagnostics; it does not drive synthesis.
  float raw_frequency_hz = 0.0f;
  // Period in the original input-rate domain (48 kHz by default).
  float period_samples = 0.0f;
  float confidence = 0.0f;
  bool voiced = false;
  bool voiced_raw = false;
  bool voiced_stateful = false;
  bool onset = false;
  bool pitch_changed = false;
  uint8_t coherent_marks = 0;
  float yin_min = 1.0f;
  float yin_tau = 0.0f;
  float input_rms = 0.0f;
  float input_peak = 0.0f;
  float spectral_centroid = 0.0f;
  float high_frequency_ratio = 0.0f;
  float zero_crossing_rate = 0.0f;
#ifndef ESP_PLATFORM
  // Host-only B4B.4K observability. These mirror existing intermediates and do
  // not alter the embedded PitchResult layout or the production algorithm.
  float detector_rms_db = -160.0f;
  float detector_linear_energy = 0.0f;
  float previous_energy_before = 0.0f;
  float previous_energy_after = 0.0f;
  bool level_ok = false;
#endif
  uint8_t pitch_track_state = 0;
  uint32_t coast_remaining = 0;
  // Centre of the analysis window in original input sample positions.
  uint64_t analysis_timestamp_samples = 0;
  // Compatibility alias retained for Milestone-1 callers.
  uint64_t timestamp_samples = 0;
};

// One successful scheduling call. Cache path: 0=no LPC, 1=local,
// 2=shared, 3=neutral, 4=polynomial and gain normalization executed.
// Cache difference bits: 1=empty, 2=model timestamp, 4=order,
// 8=source coefficients, 16=lambda, 32=gamma, 64=sample rate,
// 128=normalization strategy. A full cache hit has no difference bits.
struct PsolaGrainAuditRecord {
#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
  uint64_t scheduling_block_start = 0;
  uint64_t destination_bits = 0;
  uint64_t requested_source_bits = 0;
  uint32_t source_period_bits = 0;
  uint32_t formant_shift_bits = 0;
  uint32_t formant_amount_bits = 0;
  uint8_t formant_mode = 0;
#endif
  uint64_t source_center = 0;
  uint64_t model_timestamp = 0;
#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
  uint32_t model_publication_serial = 0;
  uint32_t model_publication_block = 0;
  uint32_t model_consumption_block = 0;
  uint64_t model_publication_time_us = 0;
  uint64_t model_consumption_time_us = 0;
#endif
  int64_t destination_center = 0;
  uint32_t addgrain_cycles = 0;
  uint32_t select_cycles = 0;
  uint32_t model_near_cycles = 0;
  uint32_t cache_lookup_cycles = 0;
  uint32_t polynomial_cycles = 0;
  uint32_t gain_cycles = 0;
  uint32_t lambda_bits = 0;
  uint32_t gamma_bits = 0;
  uint32_t sample_rate_bits = 0;
  uint16_t source_period = 0;
  uint16_t model_order = 0;
  uint8_t cache_path = 0;
  uint8_t local_difference = 0;
  uint8_t shared_difference = 0;
  uint8_t normalization_strategy = 0;
};

struct PsolaPreparedWarpStats {
  uint64_t polls = 0;
  uint64_t poll_cycles = 0;
  uint64_t preparations = 0;
  uint64_t preparation_cycles = 0;
  uint64_t preparation_blocks = 0;
  uint64_t preparation_block_cycles = 0;
  uint32_t max_preparation_block_cycles = 0;
  uint64_t polynomial_cycles = 0;
  uint64_t gain_cycles = 0;
  uint64_t useful_preparations = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t snapshot_failures = 0;
};

// Read-only end-of-block scheduler state used by the prediction experiment.
// This is a value snapshot; observing it has no effect on the renderer.
struct PsolaPredictionCursor {
  uint64_t next_block_start = 0;
  uint64_t history_offset = 0;
  uint64_t input_end = 0;
  double next_synthesis_mark = 0.0;
  float source_period = 0.0f;
  float current_synthesis_period = 0.0f;
  float current_semitones = 0.0f;
  float target_semitones = 0.0f;
  float slew_period_step = 0.0f;
  float smoothing_ms = 0.0f;
  float sample_rate = 0.0f;
  uint32_t frames = 0;
  uint32_t model_max_distance = 0;
  uint32_t formant_amount_bits = 0;
  uint32_t formant_shift_bits = 0;
  uint32_t gamma_bits = 0;
  uint32_t slew_grains_remaining = 0;
  uint8_t formant_mode = 0;
  uint8_t normalization_strategy = 0;
  uint8_t have_cursor = 0;
  uint8_t target_enabled = 0;
  uint8_t pitch_voiced = 0;
  uint8_t pitch_onset = 0;
  uint8_t pitch_changed = 0;
  uint8_t track_state = 0;
};

struct LpcPublishedRef {
  uint64_t timestamp = 0;
  uint32_t serial = 0;
  uint32_t confidence_bits = 0;
  uint16_t order = 0;
  uint8_t valid = 0;
};

struct PitchMark {
  uint64_t sample_position = 0;
  float confidence = 0.0f;
  bool predicted = false;
};

enum class PitchTrackState : uint8_t { Unlocked, Acquiring, Locked, Coasting };

enum class PitchAuditEventType : uint8_t {
  TrackTransition,
  CoherentMarkIncrement,
  CoherentMarkReset,
  CorrelationReject,
};

enum class PitchMarkResetReason : uint8_t {
  None,
  CorrelationBelowThreshold,
  PeriodInvalid,
  VoicedLost,
  AnalysisReset,
  MarkTimeout,
  Other,
};

struct PitchAuditEvent {
  uint64_t input_position = 0;
  uint64_t analysis_position = 0;
  uint64_t published_timestamp = 0;
  uint64_t predicted_position = 0;
  uint32_t backlog_samples = 0;
  uint32_t pitch_age_samples = 0;
  float detected_f0_hz = 0.0f;
  float confidence = 0.0f;
  float period_samples = 0.0f;
  float best_correlation = 0.0f;
  int16_t best_offset = 0;
  uint8_t coherent_marks = 0;
  uint8_t mark_failures = 0;
  PitchTrackState old_state = PitchTrackState::Unlocked;
  PitchTrackState new_state = PitchTrackState::Unlocked;
  PitchAuditEventType type = PitchAuditEventType::TrackTransition;
  PitchMarkResetReason reason = PitchMarkResetReason::None;
};

struct PitchAnalysisAuditTelemetry {
  uint64_t audio_input_position = 0;
  uint64_t latest_analysis_position = 0;
  uint64_t published_analysis_timestamp = 0;
  uint64_t algorithmic_latency_samples = 0;
  uint64_t analysis_backlog_samples = 0;
  uint64_t analysis_backlog_max_samples = 0;
  float analysis_backlog_ms = 0.0f;
  float analysis_backlog_max_ms = 0.0f;
  float analysis_backlog_average_ms = 0.0f;
  float analysis_backlog_p95_ms = 0.0f;
  float analysis_backlog_p99_ms = 0.0f;
  float latest_pitch_age_ms = 0.0f;
  float pitch_age_average_ms = 0.0f;
  float pitch_age_p50_ms = 0.0f;
  float pitch_age_p95_ms = 0.0f;
  float pitch_age_p99_ms = 0.0f;
  float pitch_age_max_ms = 0.0f;
  uint64_t fifo_pushes = 0;
  uint64_t fifo_pops = 0;
  uint64_t fifo_drops = 0;
  uint64_t fifo_overflow_attempts = 0;
  uint32_t fifo_current_occupancy = 0;
  uint32_t fifo_maximum_occupancy = 0;
  float fifo_average_occupancy = 0.0f;
  uint8_t coherent_marks = 0;
  uint8_t coherent_marks_maximum = 0;
  uint8_t mark_failures = 0;
  uint8_t mark_failures_maximum = 0;
  uint64_t coherent_mark_increments = 0;
  uint64_t coherent_mark_resets = 0;
  uint64_t reset_correlation_below_threshold = 0;
  uint64_t reset_period_invalid = 0;
  uint64_t reset_voiced_lost = 0;
  uint64_t reset_analysis = 0;
  uint64_t reset_mark_timeout = 0;
  uint64_t reset_other = 0;
  float best_correlation_average = 0.0f;
  float best_correlation_minimum = 0.0f;
  float best_correlation_maximum = 0.0f;
  uint64_t accepted_marks = 0;
  uint64_t rejected_marks = 0;
  uint64_t audit_event_drops = 0;
  uint64_t first_locked_input_position = 0;
};

struct YinForensicTelemetry {
  uint32_t tau_min = 0;
  uint32_t tau_max = 0;
  uint32_t window_size = 0;
  uint32_t tau_values_per_execution = 0;
  uint64_t executions = 0;
  uint64_t actual_inner_iterations = 0;
  uint64_t expected_inner_iterations = 0;
  uint64_t incremental_rebases = 0;
  uint64_t incremental_gap_rebases = 0;
  uint64_t periodic_rebases = 0;
  uint64_t incremental_update_terms = 0;
  uint64_t full_rebase_products = 0;
  float yin_threshold = 0.0f;
  float selected_cmnd_minimum = 1.0f;
  float selected_cmnd_average = 0.0f;
  float closest_threshold_distance = 1.0f;
  uint32_t selected_tau_minimum = 0;
  uint32_t selected_tau_maximum = 0;
};

struct PitchMarkForensicTelemetry {
  uint64_t pitch_hops = 0;
  uint64_t correlation_searches = 0;
  uint64_t candidate_offsets_evaluated = 0;
  uint64_t sample_pairs_correlated = 0;
  uint64_t mac_like_operations = 0;
  uint32_t geometry_observations = 0;
  uint32_t period_p50 = 0, period_p95 = 0, period_p99 = 0, period_max = 0;
  uint32_t radius_p50 = 0, radius_p95 = 0, radius_p99 = 0, radius_max = 0;
  uint32_t window_p50 = 0, window_p95 = 0, window_p99 = 0, window_max = 0;
  uint32_t offsets_p50 = 0, offsets_p95 = 0, offsets_p99 = 0, offsets_max = 0;
  uint32_t pairs_p50 = 0, pairs_p95 = 0, pairs_p99 = 0, pairs_max = 0;
};

struct VocalFxInputIdentity {
  uint64_t sequence = 0;
  float dsp_input_rms = 0.0f;
  float pitch_tap_rms = 0.0f;
  uint32_t dsp_input_checksum = 0;
  uint32_t pitch_tap_checksum = 0;
};

struct PitchAnalysisDebug {
  uint64_t previous_mark = 0;
  uint64_t predicted_mark = 0;
  uint64_t coast_started_sample = 0;
  uint32_t coast_elapsed_samples = 0;
  uint32_t coast_limit_samples = 0;
  uint32_t recovery_mark_error_samples = 0;
  float recovery_period_error_cents = 0.0f;
  float last_reliable_period = 0.0f;
  float last_mark_confidence = 0.0f;
  uint16_t coherent_marks = 0;
  uint16_t mark_failure_count = 0;
  uint32_t reset_events = 0;
  uint32_t soft_resync_events = 0;
  uint32_t hard_resync_events = 0;
  uint32_t coast_timeout_events = 0;
  uint32_t predicted_marks = 0;
  bool reliable_measurement = false;
  bool coasting = false;
  bool recovery_event = false;
  bool reset_event = false;
  bool soft_resync = false;
  bool hard_resync = false;
  bool warm_reacquire_eligible = false;
  WarmCandidateClass warm_candidate_class = WarmCandidateClass::None;
  RejectionReason rejection_reason = RejectionReason::None;
  float warm_prior_pitch_hz = 0.0f;
  float warm_prior_age_ms = 0.0f;
  float warm_delta_cents = 0.0f;
};

enum class PsolaUsableReason : uint32_t {
  Usable = 0,
  TargetDisabled = (1 << 0),
  TrackUnlocked = (1 << 1),
  NotVoiced = (1 << 2),
  LowConfidence = (1 << 3),
  InvalidPeriod = (1 << 4),
  NoMarks = (1 << 5),
  AcquiringNotReady = (1 << 6),
  ReleaseActive = (1 << 7),
};

#pragma pack(push, 1)
struct SampleTelemetryRecord {
  uint64_t sample_index;
  float input_rms;
  float input_peak;
  float input_envelope;
  uint8_t pitch_voiced;
  float pitch_confidence;
  float pitch_period_samples;
  float pitch_f0_hz;
  uint8_t pitch_onset;
  uint8_t pitch_track_state;
  uint16_t coherent_marks;
  uint16_t mark_count;
  uint8_t target_enabled;
  uint8_t psola_usable;
  uint32_t psola_usable_reason;
  uint8_t continuity_coasting;
  uint8_t new_grain_scheduled;
  uint16_t active_grain_count;
  uint32_t last_grain_age;
  double next_synthesis_mark;
  float ola_weight_sum;
  float ola_output_rms;
  uint8_t release_active;
  uint32_t release_remaining;
  uint8_t unvoiced_path_active;
  float unvoiced_gain;
  uint8_t plosive_path_active;
  float plosive_gain;
  float psola_gain;
  float active_mix;
  float final_harmony_rms;
  float effective_total_gain;
  uint8_t pitch_voiced_raw;
  float yin_min;
  float spectral_centroid;
  float high_frequency_ratio;
  float zero_crossing_rate;
  float limiter_gain;
  float limiter_reduction_db;
  float limiter_peak;
  uint16_t dry_delay_samples;
  uint8_t dry_alignment_active;
  float wanted_mix;
};
#pragma pack(pop)

enum class PitchAnalysisProfileSection : uint8_t {
  Decimator,
  FifoDrain,
  RollingWindow,
  LinearWindowCopy,
  YinEnergy,
  YinDifference,
  YinCmnd,
  YinSearch,
  YinInterpolation,
  YinTotal,
  VoicedFeatures,
  VoicedClassifier,
  PitchSmoother,
  PitchMarkSearch,
  PitchMarkCorrelation,
  PitchPublication,
  Total,
  RunTotal,
  Count
};

enum class VocalFxProfileSection : uint8_t {
  Input,
  Compressor,
  Delay,
  Reverb,
  Pipeline,
  Harmony,
  Master,
  ParameterQueue,
  PitchLpcTap,
  PitchMarkSync,
  DryAlignment,
  HarmonySlewPan,
  HarmonyLimiter,
  BusMixing,
  DelayPrep,
  ReverbPrep,
  HarmonyVoice0,
  HarmonyVoice1,
  InputHpf,
  InputGate,
  MasterMix,
  MasterLimiter,
  Count
};

struct VocalFxLimiterDiagnostics {
  float harmony_pre_peak = 0.0f;
  float harmony_post_peak = 0.0f;
  float master_peak = 0.0f;
  float max_reduction_db = 0.0f;
};
struct VocalFxProfileStats {
  uint64_t blocks = 0;
  uint64_t total_us = 0;
  uint64_t worst_us = 0;
  uint64_t deadline_misses = 0;
  uint64_t total_cycles = 0;
  uint64_t worst_cycles = 0;
};
struct VocalFxProfileDistribution {
  uint32_t p50_us = 0;
  uint32_t p95_us = 0;
  uint32_t p99_us = 0;
  uint32_t samples = 0;
};

struct VocalFxBufferAudit {
  char name[32] = {0};
  const void *ptr = nullptr;
  size_t size_bytes = 0;
  bool is_psram = false;
  bool is_sram = false;
};

// Stage funnel metrics for forensic analysis (Req 7)
struct VocalFxFunnelStats {
  uint64_t synthetic_tone_blocks = 0;
  uint64_t pitch_analysis_blocks = 0;
  uint64_t pitch_results_produced = 0;
  uint64_t voiced_pitch_results = 0;
  uint64_t pitch_marks_generated = 0;
  uint64_t pitch_marks_transferred = 0;
  uint64_t pitch_marks_consumed = 0;
  uint64_t harmony_target_activations = 0;
  uint64_t psola_process_calls = 0;
  uint64_t grain_schedule_attempts = 0;
  uint64_t grains_scheduled = 0;
  uint64_t grains_rendered = 0;
};

// Diagnostic sync tracking between Core 0 and Core 1
struct PitchSyncDiagnostics {
  uint64_t try_pitch_attempts = 0;
  uint64_t try_pitch_successes = 0;
  uint64_t try_marks_attempts = 0;
  uint64_t try_marks_successes = 0;
  uint64_t try_marks_start_gt_end = 0;
  uint64_t last_mark_count = 0;
  uint64_t last_mark_end = 0;
};

// B4C.8: Voice-Other subcategory breakdown (18 non-overlapping categories).
// Cycle counters only; no DSP behavior change.
enum class B4c8OtherCategory : uint8_t {
  DeferredDescriptorTraversal = 0,  // A: scan active deferred descriptors
  ActiveDescriptorTests,             // B: deferred_grains_[i].active checks
  SliceGeometryClipping,             // C: n_min, n_max computation
  SourceRingIndex,                   // D: history_available / ring-index per sample
  HistorySourceFetchScaffolding,     // E: history_at overhead (excluding measured fetch)
  PerSampleLoopControl,              // F: loop overhead, branch, register spills
  NormalizationDenomGuards,          // G: norm_[oi] > 1e-5 checks
  OlaBufferIndexWrap,                // H: oi = (block_start + i) & mask
  SynthesisStateScaffolding,         // I: synthesis_state_ load/store (excl IIR MAC)
  FormantStatePreparation,           // J: formant checks (excl GainNorm/warp)
  WetDryGainSmoothing,               // K: psola_gain_, active_mix_, current_wet_
  RecoveryFallbackBranchTests,       // L: condition checks
  CrossfadeBlendMix,                 // M: crossfade theta, w_old, w_new
  TelemetryStateUpdate,              // N: debug_ assignments
  DiagnosticTraceFill,               // O: HarmonizerBlockTraceRecord assembly
  ProfilerOverhead,                  // P: VF_PROFILE_BEGIN/END cycle reads
  BoundsSafetyChecks,                // Q: isfinite, range checks
  OtherUnattributed,                 // R: everything else
  Count
};

// B4C.8: Per-voice Other breakdown snapshot (per block).
struct B4c8OtherBreakdown {
  uint64_t cycles[static_cast<size_t>(B4c8OtherCategory::Count)] = {};
  uint32_t total_accounted = 0;
  uint32_t total_other = 0;
  uint32_t total_block = 0;
};

// B4C.8: Slow-block event (DSP block exceeding threshold).
// Fixed-size, low-overhead, no allocation during timed window.
struct B4c8SlowBlockRecord {
  uint64_t timestamp_us = 0;       // esp_timer_get_time at capture
  uint32_t block_duration_us = 0;  // measured block wall time
  uint32_t block_index = 0;        // sequential block number
  uint8_t core = 0;                // executing core
  uint8_t voice_active_mask = 0;   // bit 0=V0 enabled, bit 1=V1 enabled
  uint8_t new_grains_v0 = 0;
  uint8_t new_grains_v1 = 0;
  uint8_t active_descriptors_v0 = 0;
  uint8_t active_descriptors_v1 = 0;
  uint8_t completed_grains_v0 = 0;
  uint8_t completed_grains_v1 = 0;
  uint8_t deferred_slices_v0 = 0;
  uint8_t deferred_slices_v1 = 0;
  uint16_t source_samples_v0 = 0;
  uint16_t source_samples_v1 = 0;
  uint16_t fir_samples_v0 = 0;
  uint16_t fir_samples_v1 = 0;
  uint16_t ola_samples = 0;
  uint16_t synthesis_samples = 0;
  uint8_t gainnorm_calls = 0;
  uint8_t model_changes = 0;
  uint8_t mark_count = 0;
  float pitch_f0 = 0.0f;
  uint32_t descriptor_create_calls = 0;
  // Major section totals (us, accumulated across both voices)
  uint32_t section_lpc_window_ola_us = 0;
  uint32_t section_synthesis_iir_us = 0;
  uint32_t section_residual_fir_us = 0;
  uint32_t section_state_shift_us = 0;
  uint32_t section_global_tap_us = 0;
  uint32_t section_gain_match_us = 0;
  uint32_t section_pitch_sync_us = 0;
  uint32_t section_model_lookup_us = 0;
  uint32_t section_mark_select_us = 0;
  uint32_t section_other_combined_us = 0;
};

// B4C.8B: Per-block record for contingency table and MC delta breakdown.
struct B4c8bBlockRecord {
  uint32_t block_id = 0;        // global audio-block ID
  uint32_t dsp_cycles = 0;      // this voice's total process_shared cycles
  uint8_t voice_index = 0;      // 0 or 1
  uint8_t model_change = 0;     // 0=no MC, 1=MC this block
  uint8_t deadline_miss = 0;    // 0=met, 1=missed (>1333 us)
  uint8_t voice_class = 0;      // 0=NEITHER, 1=V0_ONLY, 2=V1_ONLY, 3=BOTH
  uint8_t new_grains = 0;
  uint8_t active_descriptors = 0;
  uint8_t deferred_slices = 0;
  uint8_t model_warp_expensive = 0;
  uint16_t fir_samples = 0;
  float pitch_f0 = 0.0f;
  // Section breakdown (cycles) for MC delta attribution.
  uint32_t sec_mark_sel = 0;
  uint32_t sec_grain_sched = 0;
  uint32_t sec_hist_lookup = 0;
  uint32_t sec_model_lookup = 0;
  uint32_t sec_warp_poly = 0;
  uint32_t sec_warp_gainnorm = 0;
  uint32_t sec_residual_fir = 0;
  uint32_t sec_window_ola = 0;
  uint32_t sec_synth = 0;
  uint32_t sec_state_shift = 0;
  uint32_t sec_gain_match = 0;
  uint32_t sec_soft_clip = 0;
  uint32_t sec_blend = 0;
  uint32_t sec_fallback = 0;
  uint32_t sec_articulation = 0;
  uint32_t sec_plosive = 0;
  uint32_t sec_telemetry = 0;
  uint32_t sec_other = 0;
  uint32_t sec_deferred_render = 0;
  uint32_t sec_usability = 0;
};

// B4C.8B: Voice activity class for audio block.
enum class B4c8bVoiceClass : uint8_t {
  NeitherActive = 0,
  V0Only = 1,
  V1Only = 2,
  BothActive = 3,
};

// B4C.8: Stall event (single DSP block >10 ms).
struct B4c8StallEvent {
  uint64_t timestamp_us = 0;
  uint32_t block_duration_us = 0;
  uint32_t block_index = 0;
  uint8_t core = 0;
  uint8_t voice_active_mask = 0;
  uint8_t pre_stall_section = 0;   // which section was active
  uint8_t pitch_sync_state = 0;    // pitch tracker state
  uint8_t analysis_published = 0;  // whether analysis was just published
  uint8_t diag_ring_writes = 0;    // diagnostic ring write state
  uint8_t psram_activity = 0;      // PSRAM diagnostic activity
  uint8_t i2s_state = 0;           // I2S DMA state
};
