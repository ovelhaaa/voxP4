#include "harmony_plosive_bridge.h"

namespace {
constexpr float kPi = 3.14159265358979323846f;
} // namespace

void HarmonyPlosiveBridge::init(float sample_rate, uint32_t voice_index,
                                const HarmonyPlosiveBridgeConfig &config) {
  sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
  voice_index_ = voice_index;
  config_ = config;

  // Band-pass filter cascade: 400 Hz HPF + 6000 Hz LPF
  bp_hpf_.configure(BiquadType::HighPass, sample_rate_, config_.bandpass_low_hz,
                    0.7071f);
  bp_lpf_.configure(BiquadType::LowPass, sample_rate_, config_.bandpass_high_hz,
                    0.7071f);
  // Standard 2.0 kHz HPF
  hpf_2k_.configure(BiquadType::HighPass, sample_rate_, 2000.0f, 0.7071f);

  // Decorrelated deterministic PRNG per voice
  initial_prng_state_ = 0x5a5a5a5a ^ ((voice_index + 1) * 0x9e3779b9);
  prng_state_ = initial_prng_state_;

  set_transient_duration_ms(config_.transient_duration_ms);
  set_phrase_start_silence_ms(config_.phrase_start_silence_ms);

  attack_samples_ = std::max<uint32_t>(
      4, static_cast<uint32_t>(config_.attack_ms * 0.001f * sample_rate_));
  decay_samples_ = std::max<uint32_t>(
      48, static_cast<uint32_t>(config_.decay_ms * 0.001f * sample_rate_));
  max_bridge_samples_ = std::max<uint32_t>(
      transient_samples_ + attack_samples_,
      static_cast<uint32_t>(config_.max_bridge_duration_ms * 0.001f *
                            sample_rate_));
  cooldown_limit_samples_ = static_cast<uint32_t>(0.040f * sample_rate_);

  reset();
}

void HarmonyPlosiveBridge::set_transient_duration_ms(float ms) {
  config_.transient_duration_ms = std::clamp(ms, 1.0f, 15.0f);
  transient_samples_ = static_cast<uint32_t>(config_.transient_duration_ms *
                                             0.001f * sample_rate_);
  max_bridge_samples_ = std::max<uint32_t>(
      transient_samples_ + attack_samples_,
      static_cast<uint32_t>(config_.max_bridge_duration_ms * 0.001f *
                            sample_rate_));
}

void HarmonyPlosiveBridge::set_phrase_start_silence_ms(float ms) {
  config_.phrase_start_silence_ms = std::clamp(ms, 20.0f, 500.0f);
  phrase_start_thresh_samples_ = static_cast<uint32_t>(
      config_.phrase_start_silence_ms * 0.001f * sample_rate_);
}

void HarmonyPlosiveBridge::reset() {
  prng_state_ = initial_prng_state_;
  bp_hpf_.reset();
  bp_lpf_.reset();
  hpf_2k_.reset();

  prev_source_sample_ = 0.0f;
  short_term_energy_ = 0.0f;
  prev_short_term_energy_ = 0.0f;
  running_zcr_ = 0.0f;
  running_hf_energy_ = 0.0f;

  silence_samples_count_ = phrase_start_thresh_samples_;
  vocal_active_samples_ = 0;
  is_phrase_start_ = true;

  bridge_active_ = false;
  was_phrase_start_ = false;
  bridge_elapsed_samples_ = 0;
  cooldown_samples_ = 0;
  current_gain_ = 0.0f;
  plosive_score_ = 0.0f;
}

float HarmonyPlosiveBridge::next_prng_float() {
  uint32_t x = prng_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  prng_state_ = x;
  return static_cast<float>(static_cast<int32_t>(x)) * (1.0f / 2147483648.0f);
}

float HarmonyPlosiveBridge::calculate_plosive_score(
    float source_sample, float abs_deriv, float energy_rise, float hf_sample,
    const PitchResult &pitch, PitchTrackState track, float block_input_rms) {
  // 1. Silence and low-transient gate: plosive bursts have sharp sample derivatives
  const float min_deriv = is_phrase_start_ ? 0.008f : 0.012f;
  if (abs_deriv < min_deriv) {
    return 0.0f;
  }
  if (block_input_rms < 0.003f && short_term_energy_ < 1e-6f &&
      std::fabs(source_sample) < 0.01f) {
    return 0.0f;
  }

  // 2. Confident voiced gate: do not trigger inside periodic vowels
  if (track == PitchTrackState::Locked && pitch.voiced &&
      pitch.confidence >= 0.65f) {
    return 0.0f;
  }
  if (track == PitchTrackState::Coasting) {
    return 0.0f;
  }

  // 3. Vocal fry / low rumble suppression
  // Plosives have high-frequency edge content (passed by HPF > 2 kHz).
  // Low-frequency vocal fry has virtually zero energy above 2 kHz.
  const float instant_hf_ratio =
      std::fabs(hf_sample) / (std::fabs(source_sample) + 1e-5f);
  const float hf_ratio = std::max(
      instant_hf_ratio,
      std::sqrt(running_hf_energy_ / (short_term_energy_ + 1e-8f)));
  if (!is_phrase_start_ && running_zcr_ < 0.06f && hf_ratio < 0.06f) {
    return 0.0f;
  }
  if (is_phrase_start_ && hf_ratio < 0.04f) {
    return 0.0f;
  }

  // 4. Normalized acoustic components
  const float rel_rise = energy_rise / (prev_short_term_energy_ + 1e-4f);
  const float norm_energy_rise = std::clamp(rel_rise * 0.5f, 0.0f, 1.0f);
  const float norm_deriv =
      std::clamp((abs_deriv - min_deriv) * 35.0f, 0.0f, 1.0f);
  const float norm_zcr = std::clamp(running_zcr_ * 2.5f, 0.0f, 1.0f);

  float score = 0.45f * norm_energy_rise + 0.35f * norm_deriv + 0.20f * norm_zcr;

  // Phrase-start boost: plosive onsets after silence are high-priority
  if (is_phrase_start_) {
    score *= 1.30f;
  }

  return score;
}

float HarmonyPlosiveBridge::process_sample(float source_sample,
                                          const PitchResult &pitch,
                                          PitchTrackState track,
                                          float block_input_rms,
                                          float *out_bridge_component) {
  if (!config_.enabled || config_.policy == PlosiveBridgePolicy::B0_None) {
    bridge_active_ = false;
    current_gain_ = 0.0f;
    plosive_score_ = 0.0f;
    if (out_bridge_component) {
      *out_bridge_component = 0.0f;
    }
    return 0.0f;
  }

  if (cooldown_samples_ > 0) {
    --cooldown_samples_;
  }

  // Filter signals
  const float x_hpf = hpf_2k_.process(source_sample);
  const float x_bp = bp_lpf_.process(bp_hpf_.process(source_sample));

  // Running acoustic features
  const float abs_deriv = std::fabs(source_sample - prev_source_sample_);
  const bool zc = (source_sample >= 0.0f) != (prev_source_sample_ >= 0.0f);
  prev_source_sample_ = source_sample;

  prev_short_term_energy_ = short_term_energy_;
  const float e_curr = source_sample * source_sample;
  short_term_energy_ += 0.05f * (e_curr - short_term_energy_); // ~0.4 ms
  const float energy_rise =
      std::max(0.0f, short_term_energy_ - prev_short_term_energy_);

  running_zcr_ += 0.05f * ((zc ? 1.0f : 0.0f) - running_zcr_);
  running_hf_energy_ += 0.05f * (x_hpf * x_hpf - running_hf_energy_);

  // Phrase-start and silence tracking using time-aligned source energy
  const float local_rms = std::sqrt(short_term_energy_);
  if (local_rms < 0.005f && std::fabs(source_sample) < 0.008f) {
    ++silence_samples_count_;
    vocal_active_samples_ = 0;
    if (silence_samples_count_ >= phrase_start_thresh_samples_) {
      is_phrase_start_ = true;
    }
    if (silence_samples_count_ >= 480) { // 10 ms closure resets cooldown
      cooldown_samples_ = 0;
    }
  } else {
    silence_samples_count_ = 0;
    if (vocal_active_samples_ < 48000) {
      ++vocal_active_samples_;
    }
    if (vocal_active_samples_ >= 240) { // 5 ms vocal activity clears phrase-start
      is_phrase_start_ = false;
    }
  }

  // Calculate plosive onset score
  plosive_score_ =
      calculate_plosive_score(source_sample, abs_deriv, energy_rise, x_hpf,
                              pitch, track, block_input_rms);

  // Trigger plosive bridge event if eligible
  const float trigger_thresh = is_phrase_start_ ? 0.25f : 0.35f;
  if (!bridge_active_ && cooldown_samples_ == 0 &&
      plosive_score_ >= trigger_thresh) {
    bridge_active_ = true;
    bridge_elapsed_samples_ = 0;
    was_phrase_start_ = is_phrase_start_;
    is_phrase_start_ = false;
    silence_samples_count_ = 0;
  }

  float bridge_sample = 0.0f;

  if (bridge_active_) {
    ++bridge_elapsed_samples_;

    // Hard duration cap and termination upon confident voiced transition
    if (bridge_elapsed_samples_ >= max_bridge_samples_ ||
        (bridge_elapsed_samples_ > transient_samples_ &&
         track == PitchTrackState::Locked && pitch.voiced &&
         pitch.confidence >= 0.70f)) {
      bridge_active_ = false;
      cooldown_samples_ = cooldown_limit_samples_;
    }

    // Envelope calculation
    float env = 1.0f;
    if (bridge_elapsed_samples_ <= attack_samples_) {
      // 0.5 ms half-Hann rise
      const float frac = static_cast<float>(bridge_elapsed_samples_) /
                         static_cast<float>(attack_samples_);
      env = 0.5f * (1.0f - std::cos(kPi * frac));
    } else if (bridge_elapsed_samples_ > transient_samples_) {
      // Smooth half-cosine decay over remainder of bridge window
      const uint32_t decay_pos =
          bridge_elapsed_samples_ - transient_samples_;
      const uint32_t decay_total =
          max_bridge_samples_ - transient_samples_;
      const float frac =
          std::min(1.0f, static_cast<float>(decay_pos) /
                             static_cast<float>(decay_total));
      env = 0.5f * (1.0f + std::cos(kPi * frac));
    }

    const float base_gain =
        config_.transient_gain *
        (was_phrase_start_ ? config_.phrase_start_gain_multiplier : 1.0f);
    current_gain_ = env * base_gain;

    // Signal synthesis
    const float noise = next_prng_float();
    const float hf_noise = hpf_2k_.process(noise);
    const float noise_mod = hf_noise * std::sqrt(running_hf_energy_ + 1e-8f);

    float raw_burst = 0.0f;
    if (bridge_elapsed_samples_ <= transient_samples_) {
      // Phase 1: Transient Attack Burst (2-5 ms)
      switch (config_.policy) {
      case PlosiveBridgePolicy::B0_None:
        raw_burst = 0.0f;
        break;
      case PlosiveBridgePolicy::B1_HpfSource:
        raw_burst = x_hpf;
        break;
      case PlosiveBridgePolicy::B2_BandpassSource:
        raw_burst = x_bp;
        break;
      case PlosiveBridgePolicy::B3_Hybrid:
        raw_burst = config_.hybrid_alpha * x_bp +
                    (1.0f - config_.hybrid_alpha) * noise_mod;
        break;
      case PlosiveBridgePolicy::B4_BroadbandTransient:
        raw_burst = source_sample;
        break;
      }
    } else {
      // Phase 2: Tail Crossfade into articulation/PSOLA (strictly filtered)
      switch (config_.policy) {
      case PlosiveBridgePolicy::B0_None:
        raw_burst = 0.0f;
        break;
      case PlosiveBridgePolicy::B1_HpfSource:
        raw_burst = x_hpf;
        break;
      case PlosiveBridgePolicy::B2_BandpassSource:
        raw_burst = x_bp;
        break;
      case PlosiveBridgePolicy::B3_Hybrid:
      case PlosiveBridgePolicy::B4_BroadbandTransient:
        // Broadband transient transitions strictly to filtered hybrid in tail
        raw_burst = config_.hybrid_alpha * x_hpf +
                    (1.0f - config_.hybrid_alpha) * noise_mod;
        break;
      }
    }

    bridge_sample = raw_burst * current_gain_;
  } else {
    current_gain_ = 0.0f;
  }

  if (out_bridge_component) {
    *out_bridge_component = bridge_sample;
  }

  return bridge_sample;
}
