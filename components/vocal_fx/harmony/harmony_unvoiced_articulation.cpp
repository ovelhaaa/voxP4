#include "harmony_unvoiced_articulation.h"

void HarmonyUnvoicedArticulation::init(
    float sample_rate, uint32_t voice_index,
    const HarmonyUnvoicedArticulationConfig &config) {
  sample_rate_ = std::max(1.0f, sample_rate);
  voice_index_ = voice_index;
  config_ = config;

  feed_gain_ = std::clamp(config.feed_gain, 0.0f, 2.0f);

  analysis_hpf_.configure(BiquadType::HighPass, sample_rate_, 2000.0f, 0.7071f);
  output_hpf_.configure(BiquadType::HighPass, sample_rate_,
                        std::clamp(config.hpf_cutoff_hz, 200.0f, 15000.0f),
                        0.7071f);
  presence_filter_.configure(BiquadType::Peaking, sample_rate_, 4500.0f, 1.5f,
                             4.0f);

  const float att_ms = std::clamp(config.attack_ms, 0.1f, 100.0f);
  const float rel_ms = std::clamp(config.release_ms, 1.0f, 500.0f);
  attack_alpha_ = 1.0f - std::exp(-1.0f / (sample_rate_ * att_ms * 0.001f));
  release_alpha_ = 1.0f - std::exp(-1.0f / (sample_rate_ * rel_ms * 0.001f));

  const float trans_ms = std::clamp(config.transition_ms, 0.5f, 100.0f);
  transition_step_ = 1.0f / (sample_rate_ * trans_ms * 0.001f);

  // Initialize PRNG state with distinct per-voice decorrelation
  uint32_t seed = config.prng_seed;
  if (seed == 0)
    seed = 0x12345678;
  seed ^= (voice_index_ * 0x9e3779b9u + 0x85ebca6bu);
  if (seed == 0)
    seed = 0x87654321;
  initial_prng_state_ = seed;
  prng_state_ = seed;

  reset();
}

void HarmonyUnvoicedArticulation::reset() {
  prng_state_ = initial_prng_state_;
  analysis_hpf_.reset();
  output_hpf_.reset();
  presence_filter_.reset();

  running_source_energy_ = 0.0f;
  running_hf_energy_ = 0.0f;
  zcr_smooth_ = 0.0f;
  prev_source_sample_ = 0.0f;
  current_class_ = UnvoicedAcousticClass::Silence;
  eligible_ = false;

  hf_envelope_ = 0.0f;
  transition_gain_ = 0.0f;
}

void HarmonyUnvoicedArticulation::set_hpf_cutoff(float hz) {
  config_.hpf_cutoff_hz = std::clamp(hz, 200.0f, 15000.0f);
  output_hpf_.configure(BiquadType::HighPass, sample_rate_,
                        config_.hpf_cutoff_hz, 0.7071f);
}

float HarmonyUnvoicedArticulation::next_prng_float() {
  uint32_t x = prng_state_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  prng_state_ = x;
  return static_cast<float>(static_cast<int32_t>(x)) * (1.0f / 2147483648.0f);
}

UnvoicedAcousticClass HarmonyUnvoicedArticulation::classify_acoustic(
    float source_sample, float hf_sample, const PitchResult &pitch,
    PitchTrackState track, float block_input_rms) {
  (void)source_sample;
  (void)hf_sample;

  // 1. Check for silence or inter-phrase background pauses
  const float silence_thr = config_.silence_threshold;
  if (block_input_rms < silence_thr &&
      running_source_energy_ < (silence_thr * silence_thr)) {
    return UnvoicedAcousticClass::Silence;
  }

  // 2. Inhibit during confident voiced synthesis or COASTING
  if (track == PitchTrackState::Locked && pitch.voiced &&
      pitch.confidence >= 0.60f) {
    return UnvoicedAcousticClass::Periodic;
  }
  if (track == PitchTrackState::Coasting) {
    return UnvoicedAcousticClass::Periodic;
  }
  if (track == PitchTrackState::Acquiring && pitch.voiced &&
      pitch.confidence >= 0.60f) {
    return UnvoicedAcousticClass::Periodic;
  }

  // 3. Evaluate acoustic evidence for non-periodic fricatives / plosives / breaths
  const float hf_ratio =
      std::sqrt(running_hf_energy_ / (running_source_energy_ + 1e-8f));

  // High zero-crossing rate and high spectral frequency balance
  if (zcr_smooth_ >= 0.08f && hf_ratio >= 0.12f) {
    return UnvoicedAcousticClass::GenuinelyNonPeriodic;
  }
  // Strong high frequency energy during unvoiced / low confidence
  if (hf_ratio >= 0.20f && pitch.confidence < 0.40f) {
    return UnvoicedAcousticClass::GenuinelyNonPeriodic;
  }
  // Unvoiced onset or consonant burst
  if (pitch.confidence < 0.25f && zcr_smooth_ >= 0.06f && hf_ratio >= 0.08f) {
    return UnvoicedAcousticClass::GenuinelyNonPeriodic;
  }

  // Material dominated by low frequencies without clean pitch is vocal fry / low rumble
  return UnvoicedAcousticClass::VocalFryLowFreq;
}

float HarmonyUnvoicedArticulation::process_sample(
    float source_sample, const PitchResult &pitch, PitchTrackState track,
    float block_input_rms, float *out_art_component) {
  if (!config_.enabled || config_.mode == UnvoicedArticulationMode::U0_Silence) {
    transition_gain_ = 0.0f;
    if (out_art_component)
      *out_art_component = 0.0f;
    return 0.0f;
  }

  // High-pass filter analysis for envelope follower and spectral classification
  const float x_hf = analysis_hpf_.process(source_sample);

  // Update running acoustic features
  const bool zc = (source_sample >= 0.0f) != (prev_source_sample_ >= 0.0f);
  prev_source_sample_ = source_sample;
  const float feature_alpha = 0.005f; // ~4 ms integration
  zcr_smooth_ += feature_alpha * ((zc ? 1.0f : 0.0f) - zcr_smooth_);
  running_source_energy_ +=
      feature_alpha * (source_sample * source_sample - running_source_energy_);
  running_hf_energy_ += feature_alpha * (x_hf * x_hf - running_hf_energy_);

  // High-frequency envelope follower
  const float abs_hf = std::fabs(x_hf);
  if (abs_hf > hf_envelope_) {
    hf_envelope_ += attack_alpha_ * (abs_hf - hf_envelope_);
  } else {
    hf_envelope_ += release_alpha_ * (abs_hf - hf_envelope_);
  }

  // Explicit classification
  current_class_ = classify_acoustic(source_sample, x_hf, pitch, track,
                                     block_input_rms);
  eligible_ = (current_class_ == UnvoicedAcousticClass::GenuinelyNonPeriodic);

  // Slew transition gain
  const float target_gain = eligible_ ? 1.0f : 0.0f;
  if (target_gain > transition_gain_) {
    transition_gain_ +=
        std::min(target_gain - transition_gain_, transition_step_);
  } else if (target_gain < transition_gain_) {
    transition_gain_ +=
        std::max(target_gain - transition_gain_, -transition_step_);
  }

  // Synthesize articulation candidate
  float raw_art = 0.0f;
  switch (config_.mode) {
  case UnvoicedArticulationMode::U0_Silence:
    raw_art = 0.0f;
    break;

  case UnvoicedArticulationMode::U1_FullBand:
    raw_art = source_sample;
    break;

  case UnvoicedArticulationMode::U2_Hpf:
    raw_art = output_hpf_.process(source_sample);
    break;

  case UnvoicedArticulationMode::U3_HpfEnvelope: {
    const float hf_source = output_hpf_.process(source_sample);
    const float scale = std::clamp(hf_envelope_ * 4.0f, 0.0f, 1.0f);
    raw_art = hf_source * scale;
    break;
  }

  case UnvoicedArticulationMode::U4_NoiseExcitation: {
    const float white = next_prng_float();
    const float shaped_noise = output_hpf_.process(white);
    raw_art = shaped_noise * hf_envelope_;
    break;
  }

  case UnvoicedArticulationMode::U5_MultibandNoise: {
    const float white = next_prng_float();
    const float shaped_noise = output_hpf_.process(white);
    const float presence_noise = presence_filter_.process(white);
    raw_art = (0.7f * shaped_noise + 0.3f * presence_noise) * hf_envelope_;
    break;
  }
  }

  const float art_sample = raw_art * feed_gain_ * transition_gain_;
  if (out_art_component) {
    *out_art_component = art_sample;
  }
  return art_sample;
}
