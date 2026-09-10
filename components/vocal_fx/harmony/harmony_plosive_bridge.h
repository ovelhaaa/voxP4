#pragma once

#include "biquad.h"
#include "vocal_fx_types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

class HarmonyPlosiveBridge {
public:
  void init(float sample_rate, uint32_t voice_index,
            const HarmonyPlosiveBridgeConfig &config);
  void reset();

  // Process a single sample
  // source_sample: time-aligned source sample (from history buffer or input)
  // pitch: pitch analysis snapshot
  // track: tracker state
  // block_input_rms: input block RMS level
  // out_bridge_component: optional output pointer for isolated bridge stem
  float process_sample(float source_sample, const PitchResult &pitch,
                       PitchTrackState track, float block_input_rms,
                       float *out_bridge_component = nullptr);

  // Inspector and telemetry accessors
  bool is_active() const { return bridge_active_; }
  float current_gain() const { return current_gain_; }
  float plosive_score() const { return plosive_score_; }
  bool phrase_start_flag() const { return was_phrase_start_; }
  bool is_phrase_start() const { return was_phrase_start_; }

  void set_policy(PlosiveBridgePolicy policy) { config_.policy = policy; }
  void set_transient_duration_ms(float ms);
  void set_transient_gain(float gain) {
    config_.transient_gain = std::clamp(gain, 0.0f, 1.0f);
  }
  void set_phrase_start_silence_ms(float ms);
  void set_phrase_start_gain_multiplier(float mult) {
    config_.phrase_start_gain_multiplier = std::clamp(mult, 0.5f, 2.0f);
  }
  void set_hybrid_alpha(float alpha) {
    config_.hybrid_alpha = std::clamp(alpha, 0.0f, 1.0f);
  }
  void set_timing_offset_ms(float ms) { config_.timing_offset_ms = ms; }
  float timing_offset_ms() const { return config_.timing_offset_ms; }

private:
  float sample_rate_ = 48000.0f;
  uint32_t voice_index_ = 0;
  HarmonyPlosiveBridgeConfig config_{};

  // Band-pass cascade: HighPass (low cutoff) + LowPass (high cutoff)
  Biquad bp_hpf_;
  Biquad bp_lpf_;
  // High-pass filter for B1 and hybrid tail
  Biquad hpf_2k_;

  // 32-bit xorshift PRNG for hybrid noise component
  uint32_t prng_state_ = 0x5a5a5a5a;
  uint32_t initial_prng_state_ = 0x5a5a5a5a;

  // Running acoustic metrics for plosive detection
  float prev_source_sample_ = 0.0f;
  float short_term_energy_ = 0.0f;
  float prev_short_term_energy_ = 0.0f;
  float running_zcr_ = 0.0f;
  float running_hf_energy_ = 0.0f;

  // Phrase-start tracking
  uint32_t silence_samples_count_ = 0;
  uint32_t vocal_active_samples_ = 0;
  uint32_t phrase_start_thresh_samples_ = 3840; // 80 ms @ 48kHz
  bool is_phrase_start_ = true;

  // Bridge execution state
  bool bridge_active_ = false;
  bool was_phrase_start_ = false;
  uint32_t bridge_elapsed_samples_ = 0;
  uint32_t transient_samples_ = 144;      // 3.0 ms
  uint32_t attack_samples_ = 24;          // 0.5 ms
  uint32_t decay_samples_ = 480;          // 10.0 ms
  uint32_t max_bridge_samples_ = 960;     // 20.0 ms hard clamp
  uint32_t cooldown_samples_ = 0;
  uint32_t cooldown_limit_samples_ = 1920; // 40 ms refractory cooldown

  float current_gain_ = 0.0f;
  float plosive_score_ = 0.0f;

  float next_prng_float();
  float calculate_plosive_score(float source_sample, float abs_deriv,
                                float energy_rise, float hf_sample,
                                const PitchResult &pitch, PitchTrackState track,
                                float block_input_rms);
};
