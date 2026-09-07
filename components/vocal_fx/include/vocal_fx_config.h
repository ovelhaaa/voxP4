#pragma once
#include "vocal_fx_types.h"
#include <cstdint>

constexpr float VOCAL_FX_DEFAULT_SAMPLE_RATE = 48000.0f;
constexpr uint32_t VOCAL_FX_DEFAULT_BLOCK_SIZE = 64;
constexpr uint32_t VOCAL_FX_MAX_BLOCK_SIZE = 256;
constexpr float VOCAL_FX_MAX_DELAY_SECONDS = 2.0f;
constexpr float VOCAL_FX_MAX_SAMPLE_RATE = 48000.0f;

struct VocalFxConfig {
  float sample_rate = VOCAL_FX_DEFAULT_SAMPLE_RATE;
  uint32_t block_size = VOCAL_FX_DEFAULT_BLOCK_SIZE;
  bool enable_gate = true;
  bool enable_compressor = true;
  bool enable_delay = true;
  bool enable_reverb = true;
  bool enable_pitch_analysis = true;
  PitchShiftConfig pitch_shift{};
};

enum class PitchDetectorAlgorithm : uint8_t { Yin, Mpm };

struct PitchAnalysisConfig {
  float input_sample_rate = 48000.0f;
  float analysis_sample_rate = 12000.0f;
  float min_frequency = 65.0f;
  float max_frequency = 1000.0f;
  uint32_t window_size = 512;
  uint32_t hop_size = 60;
  float yin_threshold = 0.15f;
  float voiced_enter_confidence = 0.80f;
  float voiced_exit_confidence = 0.60f;
  float min_input_db = -55.0f;
  float smoothing_ms = 30.0f;
  uint8_t voiced_attack_frames = 2;
  uint8_t voiced_release_frames = 3;
  float pitch_change_cents = 75.0f;
  float onset_ratio = 2.5f;
  PitchDetectorAlgorithm algorithm = PitchDetectorAlgorithm::Yin;
};
