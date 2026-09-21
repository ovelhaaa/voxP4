#include "vocal_drive.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;

inline float flush_denormal(float x) {
  return (std::fabs(x) < 1e-15f) ? 0.0f : x;
}
} // namespace

bool VocalDrive::init(float sample_rate) {
  if (sample_rate < 8000.0f || sample_rate > 192000.0f) {
    return false;
  }
  sample_rate_ = sample_rate;
  update_coefficients();
  reset();
  return true;
}

void VocalDrive::reset() {
  pre_hpf_x_l_ = pre_hpf_y_l_ = 0.0f;
  pre_hpf_x_r_ = pre_hpf_y_r_ = 0.0f;

  mp_hp1_x_l_ = mp_hp1_y_l_ = 0.0f;
  mp_hp1_x_r_ = mp_hp1_y_r_ = 0.0f;
  mp_hp2_x_l_ = mp_hp2_y_l_ = 0.0f;
  mp_hp2_x_r_ = mp_hp2_y_r_ = 0.0f;

  mp_lp1_y_l_ = mp_lp1_y_r_ = 0.0f;
  mp_lp2_y_l_ = mp_lp2_y_r_ = 0.0f;

  dc_x_l_ = dc_y_l_ = 0.0f;
  dc_x_r_ = dc_y_r_ = 0.0f;

  tone_lp_l_ = tone_lp_r_ = 0.0f;
}

void VocalDrive::set_mode(DriveMode mode) {
  mode_ = mode;
}

void VocalDrive::set_drive(float drive) {
  drive_ = std::clamp(drive, 0.0f, 1.0f);
}

void VocalDrive::set_tone(float tone) {
  tone_ = std::clamp(tone, 0.0f, 1.0f);
  update_coefficients();
}

void VocalDrive::set_mix(float mix) {
  mix_ = std::clamp(mix, 0.0f, 1.0f);
}

void VocalDrive::set_output_level(float level) {
  output_level_ = std::clamp(level, 0.0f, 2.0f);
}

void VocalDrive::update_coefficients() {
  // Pre-HPF at 100 Hz
  const float w_pre = 2.0f * kPi * 100.0f / sample_rate_;
  pre_hpf_alpha_ = 1.0f / (1.0f + w_pre);

  // Megaphone HPF at 500 Hz
  const float w_mp_hp = 2.0f * kPi * 500.0f / sample_rate_;
  mp_hp_alpha_ = 1.0f / (1.0f + w_mp_hp);

  // Megaphone LPF at 3200 Hz
  const float w_mp_lp = 2.0f * kPi * 3200.0f / sample_rate_;
  mp_lp_alpha_ = 1.0f - std::exp(-w_mp_lp);

  // DC Blocker at 15 Hz
  dc_r_ = 1.0f - (2.0f * kPi * 15.0f / sample_rate_);

  // Tone low-pass filter (1500 Hz to 18000 Hz)
  const float fc_tone = 1500.0f * std::pow(12.0f, tone_);
  const float w_tone = 2.0f * kPi * std::min(fc_tone, sample_rate_ * 0.45f) / sample_rate_;
  tone_alpha_ = 1.0f - std::exp(-w_tone);
}

float VocalDrive::saturate_sample(float x, DriveMode mode, float drive_val) {
  switch (mode) {
  case DriveMode::Warm: {
    // Gentle input gain: 1.0 to 4.0 (0 to +12 dB)
    const float g = 1.0f + 3.0f * drive_val;
    const float u = x * g;
    // Mild second-harmonic warmth: u_asym = u + 0.08 * u * |u|
    const float u_asym = u + 0.08f * u * std::fabs(u);
    // Smooth rational saturator: y = u / (1 + 0.35 * |u|)
    const float y = u_asym / (1.0f + 0.35f * std::fabs(u_asym));
    return y;
  }
  case DriveMode::Overdrive: {
    // Punchy input gain: 1.0 to 7.0 (0 to +17 dB)
    const float g = 1.0f + 6.0f * drive_val;
    const float u = x * g;
    // Classic algebraic smooth saturation: y = u / sqrt(1 + u^2)
    // Infinitely differentiable (C-infinity), highly resistant to aliasing
    const float y = u / std::sqrt(1.0f + u * u);
    return y;
  }
  case DriveMode::Megaphone: {
    // Driven speaker saturation: 2.0 to 8.0 gain
    const float g = 2.0f + 6.0f * drive_val;
    const float u = x * g;
    // Asymmetric transistor-like bias: u_skew = u + 0.2
    const float u_skew = u + 0.2f;
    const float y = (u_skew / std::sqrt(1.0f + u_skew * u_skew)) - 0.1961f; // offset-neutralized
    return y * 1.3f;
  }
  default:
    return x;
  }
}

void VocalDrive::process(const float *in_l, const float *in_r,
                         float *out_l, float *out_r,
                         size_t frames) {
  if (!in_l || !in_r || !out_l || !out_r || frames == 0) {
    return;
  }

  const float drive_val = drive_;
  const float tone_a = tone_alpha_;
  const float mix_val = mix_;
  const float out_lvl = output_level_;
  const DriveMode mode = mode_;

  for (size_t i = 0; i < frames; ++i) {
    float xl = in_l[i];
    float xr = in_r[i];

    float wet_l = xl;
    float wet_r = xr;

    if (mode == DriveMode::Megaphone) {
      // Megaphone bandpass: 2-stage HPF 500 Hz + 2-stage LPF 3200 Hz
      // Left HPF
      float hp1_l = mp_hp_alpha_ * (mp_hp1_y_l_ + wet_l - mp_hp1_x_l_);
      mp_hp1_x_l_ = wet_l;
      mp_hp1_y_l_ = flush_denormal(hp1_l);

      float hp2_l = mp_hp_alpha_ * (mp_hp2_y_l_ + hp1_l - mp_hp2_x_l_);
      mp_hp2_x_l_ = hp1_l;
      mp_hp2_y_l_ = flush_denormal(hp2_l);

      // Left LPF
      mp_lp1_y_l_ = flush_denormal(mp_lp1_y_l_ + mp_lp_alpha_ * (hp2_l - mp_lp1_y_l_));
      mp_lp2_y_l_ = flush_denormal(mp_lp2_y_l_ + mp_lp_alpha_ * (mp_lp1_y_l_ - mp_lp2_y_l_));
      wet_l = mp_lp2_y_l_;

      // Right HPF
      float hp1_r = mp_hp_alpha_ * (mp_hp1_y_r_ + wet_r - mp_hp1_x_r_);
      mp_hp1_x_r_ = wet_r;
      mp_hp1_y_r_ = flush_denormal(hp1_r);

      float hp2_r = mp_hp_alpha_ * (mp_hp2_y_r_ + hp1_r - mp_hp2_x_r_);
      mp_hp2_x_r_ = hp1_r;
      mp_hp2_y_r_ = flush_denormal(hp2_r);

      // Right LPF
      mp_lp1_y_r_ = flush_denormal(mp_lp1_y_r_ + mp_lp_alpha_ * (hp2_r - mp_lp1_y_r_));
      mp_lp2_y_r_ = flush_denormal(mp_lp2_y_r_ + mp_lp_alpha_ * (mp_lp1_y_r_ - mp_lp2_y_r_));
      wet_r = mp_lp2_y_r_;
    } else {
      // Warm & Overdrive: 100 Hz Pre-HPF to keep vocals tight and mud-free
      float hpf_l = pre_hpf_alpha_ * (pre_hpf_y_l_ + wet_l - pre_hpf_x_l_);
      pre_hpf_x_l_ = wet_l;
      pre_hpf_y_l_ = flush_denormal(hpf_l);
      wet_l = hpf_l;

      float hpf_r = pre_hpf_alpha_ * (pre_hpf_y_r_ + wet_r - pre_hpf_x_r_);
      pre_hpf_x_r_ = wet_r;
      pre_hpf_y_r_ = flush_denormal(hpf_r);
      wet_r = hpf_r;
    }

    // Saturation non-linearity
    wet_l = saturate_sample(wet_l, mode, drive_val);
    wet_r = saturate_sample(wet_r, mode, drive_val);

    // Tone filter (post-drive low-pass)
    tone_lp_l_ = flush_denormal(tone_lp_l_ + tone_a * (wet_l - tone_lp_l_));
    tone_lp_r_ = flush_denormal(tone_lp_r_ + tone_a * (wet_r - tone_lp_r_));
    wet_l = tone_lp_l_;
    wet_r = tone_lp_r_;

    // DC Blocker (15 Hz)
    float dc_l = wet_l - dc_x_l_ + dc_r_ * dc_y_l_;
    dc_x_l_ = wet_l;
    dc_y_l_ = flush_denormal(dc_l);
    wet_l = dc_l;

    float dc_r = wet_r - dc_x_r_ + dc_r_ * dc_y_r_;
    dc_x_r_ = wet_r;
    dc_y_r_ = flush_denormal(dc_r);
    wet_r = dc_r;

    // Apply output trim and wet/dry mix
    out_l[i] = (1.0f - mix_val) * xl + mix_val * (wet_l * out_lvl);
    out_r[i] = (1.0f - mix_val) * xr + mix_val * (wet_r * out_lvl);
  }
}
