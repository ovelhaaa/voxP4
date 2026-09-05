#pragma once
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
};
