#pragma once
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
  HarmonyVoice1Smoothing, HarmonyVoice2Smoothing
  ,FormantVoice1Mode, FormantVoice2Mode,
  FormantVoice1Amount, FormantVoice2Amount,
  FormantVoice1ShiftSemitones, FormantVoice2ShiftSemitones
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
  SourceLookup,
  GrainPreparation,
  WindowOla,
  Normalization,
  Unvoiced,
  Crossfade,
  Total,
  Count
};

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
  bool onset = false;
  bool pitch_changed = false;
  uint8_t coherent_marks = 0;
  // Centre of the analysis window in original input sample positions.
  uint64_t analysis_timestamp_samples = 0;
  // Compatibility alias retained for Milestone-1 callers.
  uint64_t timestamp_samples = 0;
};

struct PitchMark {
  uint64_t sample_position = 0;
  float confidence = 0.0f;
  bool predicted = false;
};

enum class PitchTrackState : uint8_t { Unlocked, Acquiring, Locked, Coasting };

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
};
#pragma pack(pop)


enum class PitchAnalysisProfileSection : uint8_t {
  Decimator,
  YinDifference,
  YinCmnd,
  YinSearch,
  YinInterpolation,
  VoicedClassifier,
  PitchSmoother,
  PitchMarkSearch,
  Total,
  Count
};

enum class VocalFxProfileSection : uint8_t {
  Input,
  Compressor,
  Delay,
  Reverb,
  Pipeline
};
struct VocalFxProfileStats {
  uint64_t blocks = 0;
  uint64_t total_us = 0;
  uint64_t worst_us = 0;
  uint64_t deadline_misses = 0;
};
