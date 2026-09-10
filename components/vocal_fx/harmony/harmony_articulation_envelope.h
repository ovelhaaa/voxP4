#pragma once

#include <cstdint>

struct HarmonyArticulationConfig {
  bool enabled = false;
  float pitch_loss_grace_ms = 0.0f;
  float unvoiced_hold_ms = 0.0f;
  float attack_ms = 20.0f;
  float release_ms = 20.0f;
  float onset_min_gain = 0.0f;
  float unvoiced_min_gain = 0.0f;
};

class HarmonyArticulationEnvelope {
public:
  void init(float sample_rate, const HarmonyArticulationConfig &config);
  void reset();
  void begin_block(bool target_valid, bool voiced);
  float process_sample(bool target_usable, bool voiced, bool onset);
  bool enabled() const { return config_.enabled; }
  bool keep_target_active() const;
  float gain() const { return gain_; }

private:
  uint32_t samples(float milliseconds) const;
  float sample_rate_ = 48000.0f;
  HarmonyArticulationConfig config_{};
  uint32_t pitch_loss_grace_samples_ = 0, unvoiced_hold_samples_ = 0;
  uint32_t pitch_loss_remaining_ = 0, unvoiced_remaining_ = 0;
  float gain_ = 0.0f;
};
