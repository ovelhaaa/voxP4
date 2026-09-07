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
  FormantVoice1Amount, FormantVoice2Amount
};

enum class PitchShiftMode : uint8_t { Bypass, FixedInterval };
enum class FormantMode : uint8_t { Off, Lpc };
enum class PitchShiftState : uint8_t {
  Bypass,
  WaitingForAnalysis,
  Acquiring,
  Active,
  Fallback
};

struct PitchShiftConfig {
  bool enabled = false;
  float semitones = 0.0f;
  float wet = 1.0f;
  float smoothing_ms = 30.0f;
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
  PitchShiftState state = PitchShiftState::Bypass;
};

struct PitchResult {
  PitchResult() = default;
  // Milestone-1 source compatibility.
  PitchResult(float hz, float certainty, bool is_voiced, uint64_t timestamp)
      : frequency_hz(hz), confidence(certainty), voiced(is_voiced),
        analysis_timestamp_samples(timestamp), timestamp_samples(timestamp) {}
  float frequency_hz = 0.0f;
  // Period in the original input-rate domain (48 kHz by default).
  float period_samples = 0.0f;
  float confidence = 0.0f;
  bool voiced = false;
  bool onset = false;
  bool pitch_changed = false;
  // Centre of the analysis window in original input sample positions.
  uint64_t analysis_timestamp_samples = 0;
  // Compatibility alias retained for Milestone-1 callers.
  uint64_t timestamp_samples = 0;
};

struct PitchMark {
  uint64_t sample_position = 0;
  float confidence = 0.0f;
};

enum class PitchTrackState : uint8_t { Unlocked, Acquiring, Locked };

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
