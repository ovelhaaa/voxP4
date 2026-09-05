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
  EnableReverb
};

struct PitchResult {
  float frequency_hz = 0.0f;
  float confidence = 0.0f;
  bool voiced = false;
  uint64_t timestamp_samples = 0;
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
