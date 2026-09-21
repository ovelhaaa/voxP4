#pragma once

#include <cstddef>
#include <cstdint>

enum class DriveMode : uint8_t {
  Warm = 0,
  Overdrive = 1,
  Megaphone = 2,
  Count
};

struct DriveConfig {
  DriveMode mode = DriveMode::Warm;
  float drive = 0.40f;        // Gentle 2nd harmonic saturation (was 0.3f)
  float tone = 0.65f;         // Open, warm presence ~7.5 kHz (was 0.5f)
  float mix = 0.75f;          // Parallel saturation preserving transients (was 1.0f)
  float output_level = 0.95f; // Level-compensated unity gain (was 1.0f)
};

class VocalDrive {
public:
  VocalDrive() = default;

  bool init(float sample_rate);
  void reset();

  void set_mode(DriveMode mode);
  DriveMode mode() const { return mode_; }

  // Applies tuned musical defaults for the specified mode:
  // - Warm: drive 0.40, tone 0.65, mix 0.75, output_level 0.95 (subtle 2nd harmonic warmth)
  // - Overdrive: drive 0.50, tone 0.55, mix 0.80, output_level 0.90 (rock/modern pop saturation)
  // - Megaphone: drive 0.50, tone 0.70, mix 1.00, output_level 0.95 (500Hz-3.2kHz vintage bandpass)
  void apply_mode_defaults(DriveMode mode);

  void set_drive(float drive); // 0.0 to 1.0
  float drive() const { return drive_; }

  void set_tone(float tone); // 0.0 to 1.0 (0=dark, 0.5=neutral, 1.0=bright)
  float tone() const { return tone_; }

  void set_mix(float mix); // 0.0 to 1.0
  float mix() const { return mix_; }

  void set_output_level(float level); // 0.0 to 2.0 (1.0 = unity)
  float output_level() const { return output_level_; }

  void process(const float *in_l, const float *in_r,
               float *out_l, float *out_r,
               size_t frames);

  size_t memory_bytes() const { return sizeof(*this); }

private:
  float sample_rate_ = 44100.0f;
  DriveMode mode_ = DriveMode::Warm;
  float drive_ = 0.40f;
  float tone_ = 0.65f;
  float mix_ = 0.75f;
  float output_level_ = 0.95f;

  // Pre-drive 100 Hz HPF state (1-pole)
  float pre_hpf_x_l_ = 0.0f, pre_hpf_y_l_ = 0.0f;
  float pre_hpf_x_r_ = 0.0f, pre_hpf_y_r_ = 0.0f;
  float pre_hpf_alpha_ = 0.985f;

  // Megaphone bandpass states
  // HPF 500 Hz (2-pole / cascaded 1-pole)
  float mp_hp1_x_l_ = 0.0f, mp_hp1_y_l_ = 0.0f;
  float mp_hp1_x_r_ = 0.0f, mp_hp1_y_r_ = 0.0f;
  float mp_hp2_x_l_ = 0.0f, mp_hp2_y_l_ = 0.0f;
  float mp_hp2_x_r_ = 0.0f, mp_hp2_y_r_ = 0.0f;
  float mp_hp_alpha_ = 0.93f;

  // LPF 3200 Hz (2-pole / cascaded 1-pole)
  float mp_lp1_y_l_ = 0.0f, mp_lp1_y_r_ = 0.0f;
  float mp_lp2_y_l_ = 0.0f, mp_lp2_y_r_ = 0.0f;
  float mp_lp_alpha_ = 0.35f;

  // DC Blocker (15 Hz)
  float dc_x_l_ = 0.0f, dc_y_l_ = 0.0f;
  float dc_x_r_ = 0.0f, dc_y_r_ = 0.0f;
  float dc_r_ = 0.998f;

  // Tone low-pass filter
  float tone_lp_l_ = 0.0f, tone_lp_r_ = 0.0f;
  float tone_alpha_ = 0.5f;

  void update_coefficients();
  float saturate_sample(float x, DriveMode mode, float drive_gain);
};
