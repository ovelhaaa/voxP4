#include "harmony_articulation_envelope.h"

#include <algorithm>
#include <cmath>

uint32_t HarmonyArticulationEnvelope::samples(float milliseconds) const {
  return static_cast<uint32_t>(
      std::lround(sample_rate_ * std::max(0.0f, milliseconds) * .001f));
}

void HarmonyArticulationEnvelope::init(
    float sample_rate, const HarmonyArticulationConfig &config) {
  sample_rate_ = std::max(1.0f, sample_rate);
  config_ = config;
  config_.pitch_loss_grace_ms =
      std::clamp(config_.pitch_loss_grace_ms, 0.0f, 200.0f);
  config_.unvoiced_hold_ms =
      std::clamp(config_.unvoiced_hold_ms, 0.0f, 200.0f);
  config_.attack_ms = std::clamp(config_.attack_ms, 1.0f, 200.0f);
  config_.release_ms = std::clamp(config_.release_ms, 1.0f, 500.0f);
  config_.onset_min_gain = std::clamp(config_.onset_min_gain, 0.0f, 1.0f);
  config_.unvoiced_min_gain =
      std::clamp(config_.unvoiced_min_gain, 0.0f, 1.0f);
  pitch_loss_grace_samples_ = samples(config_.pitch_loss_grace_ms);
  unvoiced_hold_samples_ = samples(config_.unvoiced_hold_ms);
  reset();
}

void HarmonyArticulationEnvelope::reset() {
  pitch_loss_remaining_ = 0;
  unvoiced_remaining_ = 0;
  gain_ = 0.0f;
}

void HarmonyArticulationEnvelope::begin_block(bool target_valid, bool voiced) {
  if (target_valid)
    pitch_loss_remaining_ = pitch_loss_grace_samples_;
  if (voiced)
    unvoiced_remaining_ = unvoiced_hold_samples_;
}

bool HarmonyArticulationEnvelope::keep_target_active() const {
  return config_.enabled &&
         (pitch_loss_remaining_ != 0 || unvoiced_remaining_ != 0);
}

float HarmonyArticulationEnvelope::process_sample(bool target_usable,
                                                   bool voiced, bool onset) {
  float desired = target_usable ? 1.0f : 0.0f;
  if (onset)
    desired = std::max(desired, config_.onset_min_gain);
  if (!voiced) {
    if (pitch_loss_remaining_ || unvoiced_remaining_)
      desired = std::max(gain_, config_.unvoiced_min_gain);
    else
      desired = config_.unvoiced_min_gain;
  }
  const float time_ms = desired > gain_ ? config_.attack_ms : config_.release_ms;
  const float step = 1.0f / std::max(1.0f, sample_rate_ * time_ms * .001f);
  gain_ += std::clamp(desired - gain_, -step, step);
  if (pitch_loss_remaining_)
    --pitch_loss_remaining_;
  if (unvoiced_remaining_)
    --unvoiced_remaining_;
  return gain_;
}
