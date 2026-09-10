#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

class LightHarmonyLimiter {
public:
  void init(float sample_rate, float threshold_db = -3.0f, float attack_ms = 0.5f,
            float release_ms = 40.0f, float max_reduction_db = 6.0f) {
    sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
    set_threshold_db(threshold_db);
    set_attack_ms(attack_ms);
    set_release_ms(release_ms);
    set_max_reduction_db(max_reduction_db);
    reset();
  }

  void reset() {
    current_gain_ = 1.0f;
    last_peak_ = 0.0f;
    last_out_peak_ = 0.0f;
  }

  void set_threshold_db(float threshold_db) {
    threshold_db_ = std::clamp(threshold_db, -24.0f, 0.0f);
    threshold_linear_ = std::pow(10.0f, threshold_db_ / 20.0f);
  }

  void set_attack_ms(float attack_ms) {
    attack_ms_ = std::clamp(attack_ms, 0.05f, 20.0f);
    attack_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * attack_ms_ * 0.001f));
  }

  void set_release_ms(float release_ms) {
    release_ms_ = std::clamp(release_ms, 1.0f, 500.0f);
    release_coeff_ = 1.0f - std::exp(-1.0f / (sample_rate_ * release_ms_ * 0.001f));
  }

  void set_max_reduction_db(float max_reduction_db) {
    max_reduction_db_ = std::clamp(max_reduction_db, 0.0f, 24.0f);
    min_gain_ = std::pow(10.0f, -max_reduction_db_ / 20.0f);
  }

  inline void process(float &l, float &r) {
    const float peak = std::max(std::fabs(l), std::fabs(r));
    last_peak_ = peak;

    float target_gain = 1.0f;
    if (peak > threshold_linear_) {
      target_gain = threshold_linear_ / peak;
      if (target_gain < min_gain_) {
        target_gain = min_gain_;
      }
    }

    if (target_gain < current_gain_) {
      current_gain_ += (target_gain - current_gain_) * attack_coeff_;
    } else {
      current_gain_ += (target_gain - current_gain_) * release_coeff_;
    }

    l *= current_gain_;
    r *= current_gain_;

    // Safety rail: light soft clamp
    l = std::clamp(l, -1.0f, 1.0f);
    r = std::clamp(r, -1.0f, 1.0f);
    last_out_peak_ = std::max(std::fabs(l), std::fabs(r));
  }

  float current_gain() const { return current_gain_; }
  float reduction_db() const {
    return (current_gain_ > 1e-4f && current_gain_ < 0.999f)
               ? -20.0f * std::log10(current_gain_)
               : 0.0f;
  }
  float last_peak() const { return last_peak_; }
  float last_out_peak() const { return last_out_peak_; }

private:
  float sample_rate_ = 48000.0f;
  float threshold_db_ = -3.0f;
  float threshold_linear_ = 0.70794578f;
  float attack_ms_ = 0.5f;
  float release_ms_ = 40.0f;
  float max_reduction_db_ = 6.0f;
  float min_gain_ = 0.50118723f;
  float attack_coeff_ = 0.04074124f;
  float release_coeff_ = 0.00052070f;
  float current_gain_ = 1.0f;
  float last_peak_ = 0.0f;
  float last_out_peak_ = 0.0f;
};
