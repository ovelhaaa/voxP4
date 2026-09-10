#pragma once

#include "biquad.h"
#include "vocal_fx_types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

class HarmonyUnvoicedArticulation {
public:
  void init(float sample_rate, uint32_t voice_index,
            const HarmonyUnvoicedArticulationConfig &config);
  void reset();

  // Process a single sample
  // source_sample: source sample (e.g. aligned history or current input)
  // pitch: pitch analysis snapshot
  // track: tracker state
  // block_input_rms: input block RMS level
  // out_art_component: optional output pointer for isolated articulation
  float process_sample(float source_sample, const PitchResult &pitch,
                       PitchTrackState track, float block_input_rms,
                       float *out_art_component = nullptr);

  // Telemetry and inspector accessors
  bool is_active() const { return transition_gain_ > 1e-4f; }
  float transition_gain() const { return transition_gain_; }
  float hf_envelope() const { return hf_envelope_; }
  UnvoicedAcousticClass acoustic_class() const { return current_class_; }
  bool is_eligible() const { return eligible_; }

  void set_mode(UnvoicedArticulationMode mode) { config_.mode = mode; }
  void set_gain(float gain) { feed_gain_ = std::clamp(gain, 0.0f, 2.0f); }
  void set_hpf_cutoff(float hz);

private:
  float sample_rate_ = 48000.0f;
  uint32_t voice_index_ = 0;
  HarmonyUnvoicedArticulationConfig config_{};

  // Filters
  Biquad analysis_hpf_;
  Biquad output_hpf_;
  Biquad presence_filter_;

  // PRNG state (deterministic 32-bit xorshift)
  uint32_t initial_prng_state_ = 0x12345678;
  uint32_t prng_state_ = 0x12345678;

  // Running acoustic feature tracking
  float running_source_energy_ = 0.0f;
  float running_hf_energy_ = 0.0f;
  float zcr_smooth_ = 0.0f;
  float prev_source_sample_ = 0.0f;
  UnvoicedAcousticClass current_class_ = UnvoicedAcousticClass::Silence;
  bool eligible_ = false;

  // High-frequency envelope follower
  float hf_envelope_ = 0.0f;
  float attack_alpha_ = 0.0f;
  float release_alpha_ = 0.0f;

  // Transition smoothing gain
  float transition_gain_ = 0.0f;
  float transition_step_ = 0.0f;

  float feed_gain_ = 0.25f;

  float next_prng_float();
  UnvoicedAcousticClass classify_acoustic(float source_sample, float hf_sample,
                                          const PitchResult &pitch,
                                          PitchTrackState track,
                                          float block_input_rms);
};
