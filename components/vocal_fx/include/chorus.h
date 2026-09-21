#pragma once

#include "tempo.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

enum class ChorusMode : uint8_t {
  Chorus = 0,    // Classic vocal chorus: smooth stereo widening & thickening
  Ensemble,      // Multi-tap dense doubling: 3 taps with decorrelated rates/phases
  Dimension,     // Subtle spatial widening: low depth, asymmetric delay, mono-compatible
  Count
};

enum class ChorusInterpolation : uint8_t {
  Linear = 0,
  CubicHermite = 1
};

struct ChorusConfig {
  float sample_rate = 44100.0f;
  ChorusMode mode = ChorusMode::Chorus;
  ChorusInterpolation interpolation = ChorusInterpolation::CubicHermite;
  bool sync_enabled = false;
  float rate_hz = 0.8f;
  TempoSubdivision subdivision = TempoSubdivision::Half;
  float depth_ms = 2.5f;
  float base_delay_ms = 15.0f;
  float width = 1.0f;
  float mix = 0.35f;
};

class VocalChorus {
public:
  VocalChorus() = default;
  ~VocalChorus() = default;

  bool init(float sample_rate, float max_delay_ms = 50.0f);
  void reset();

  void set_mode(ChorusMode mode);
  ChorusMode mode() const { return mode_; }

  void set_interpolation(ChorusInterpolation interp) { interp_ = interp; }
  ChorusInterpolation interpolation() const { return interp_; }

  void set_mix(float mix) { mix_ = std::clamp(mix, 0.0f, 1.0f); }
  float mix() const { return mix_; }

  void set_sync_enabled(bool enabled) { sync_enabled_ = enabled; }
  bool sync_enabled() const { return sync_enabled_; }

  void set_rate_hz(float hz) { rate_hz_ = std::clamp(hz, 0.05f, 10.0f); }
  float rate_hz() const { return rate_hz_; }

  void set_subdivision(TempoSubdivision sub) { subdivision_ = sub; }
  TempoSubdivision subdivision() const { return subdivision_; }

  void set_depth_ms(float depth_ms) { depth_ms_ = std::clamp(depth_ms, 0.1f, 15.0f); }
  float depth_ms() const { return depth_ms_; }

  void set_base_delay_ms(float delay_ms) { base_delay_ms_ = std::clamp(delay_ms, 2.0f, 40.0f); }
  float base_delay_ms() const { return base_delay_ms_; }

  void set_width(float width) { width_ = std::clamp(width, 0.0f, 1.0f); }
  float width() const { return width_; }

  // Processes a block of stereo audio in-place.
  // When bypass/disabled, caller can skip or mix == 0 is cheap pass-through.
  void process(const float *in_l, const float *in_r, float *out_l, float *out_r,
               size_t frames, float tempo_bpm);

  size_t memory_bytes() const;

private:
  float effective_rate_hz(float tempo_bpm) const;

  // Reads from circular buffer with linear interpolation
  float read_linear(const float *buf, float delay_samples) const;
  // Reads from circular buffer with cubic Hermite interpolation
  float read_cubic(const float *buf, float delay_samples) const;

  float read_sample(const float *buf, float delay_samples) const {
    return (interp_ == ChorusInterpolation::CubicHermite)
               ? read_cubic(buf, delay_samples)
               : read_linear(buf, delay_samples);
  }

  std::unique_ptr<float[]> delay_buf_l_;
  std::unique_ptr<float[]> delay_buf_r_;
  size_t buf_size_ = 0;
  size_t write_pos_ = 0;

  float sample_rate_ = 44100.0f;
  float max_delay_samples_ = 0.0f;

  ChorusMode mode_ = ChorusMode::Chorus;
  ChorusInterpolation interp_ = ChorusInterpolation::CubicHermite;
  bool sync_enabled_ = false;
  float rate_hz_ = 0.8f;
  TempoSubdivision subdivision_ = TempoSubdivision::Half;
  float depth_ms_ = 2.5f;
  float base_delay_ms_ = 15.0f;
  float width_ = 1.0f;
  float mix_ = 0.35f;

  // LFO phase accumulators (normalized [0, 1))
  float lfo_phase_0_ = 0.0f;
  float lfo_phase_1_ = 0.25f;
  float lfo_phase_2_ = 0.67f;
};
