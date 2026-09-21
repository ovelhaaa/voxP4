#include "chorus.h"
#include <cmath>
#include <new>

namespace {
constexpr size_t kSineLutSize = 512;
float s_sine_lut[kSineLutSize + 1];
bool s_sine_lut_initialized = false;

void init_sine_lut() {
  if (s_sine_lut_initialized) return;
  constexpr float kTwoPi = 6.28318530717958647692f;
  for (size_t i = 0; i < kSineLutSize; ++i) {
    s_sine_lut[i] = std::sin(kTwoPi * static_cast<float>(i) / static_cast<float>(kSineLutSize));
  }
  s_sine_lut[kSineLutSize] = s_sine_lut[0]; // guard for interpolation
  s_sine_lut_initialized = true;
}

// Fast sine lookup from normalized phase [0.0, 1.0) with linear interpolation
inline float fast_sin(float phase) {
  phase = phase - std::floor(phase); // wrap to [0, 1)
  const float pos = phase * static_cast<float>(kSineLutSize);
  const size_t idx = static_cast<size_t>(pos);
  const float frac = pos - static_cast<float>(idx);
  return s_sine_lut[idx] + frac * (s_sine_lut[idx + 1] - s_sine_lut[idx]);
}
} // namespace

bool VocalChorus::init(float sample_rate, float max_delay_ms) {
  if (!std::isfinite(sample_rate) || sample_rate < 8000.0f || max_delay_ms <= 0.0f) {
    return false;
  }
  init_sine_lut();
  sample_rate_ = sample_rate;
  max_delay_samples_ = max_delay_ms * 0.001f * sample_rate;

  // Power-of-two size greater than requested max delay + 4 guard samples
  size_t size = 64;
  while (size < static_cast<size_t>(max_delay_samples_) + 8) {
    size <<= 1;
  }
  buf_size_ = size;

  delay_buf_l_ = std::unique_ptr<float[]>(new (std::nothrow) float[buf_size_]());
  delay_buf_r_ = std::unique_ptr<float[]>(new (std::nothrow) float[buf_size_]());
  if (!delay_buf_l_ || !delay_buf_r_) {
    return false;
  }

  reset();
  return true;
}

void VocalChorus::reset() {
  if (delay_buf_l_) std::fill_n(delay_buf_l_.get(), buf_size_, 0.0f);
  if (delay_buf_r_) std::fill_n(delay_buf_r_.get(), buf_size_, 0.0f);
  write_pos_ = 0;
  lfo_phase_0_ = 0.0f;
  lfo_phase_1_ = 0.25f;
  lfo_phase_2_ = 0.67f;
}

void VocalChorus::set_mode(ChorusMode mode) {
  if (static_cast<uint8_t>(mode) < static_cast<uint8_t>(ChorusMode::Count)) {
    mode_ = mode;
  }
}

float VocalChorus::effective_rate_hz(float tempo_bpm) const {
  if (sync_enabled_) {
    const float period_ms = tempo_subdivision_ms(tempo_bpm, subdivision_);
    return (period_ms > 0.0f) ? (1000.0f / period_ms) : rate_hz_;
  }
  return rate_hz_;
}

float VocalChorus::read_linear(const float *buf, float delay_samples) const {
  const float clamped_delay = std::clamp(delay_samples, 1.0f, static_cast<float>(buf_size_ - 4));
  float read_pos = static_cast<float>(write_pos_) - clamped_delay;
  if (read_pos < 0.0f) {
    read_pos += static_cast<float>(buf_size_);
  }
  const size_t i0 = static_cast<size_t>(read_pos);
  const size_t i1 = (i0 + 1) & (buf_size_ - 1);
  const float frac = read_pos - static_cast<float>(i0);
  return buf[i0] + frac * (buf[i1] - buf[i0]);
}

float VocalChorus::read_cubic(const float *buf, float delay_samples) const {
  const float clamped_delay = std::clamp(delay_samples, 2.0f, static_cast<float>(buf_size_ - 4));
  float read_pos = static_cast<float>(write_pos_) - clamped_delay;
  if (read_pos < 0.0f) {
    read_pos += static_cast<float>(buf_size_);
  }
  const size_t i1 = static_cast<size_t>(read_pos);
  const size_t mask = buf_size_ - 1;
  const size_t i0 = (i1 + buf_size_ - 1) & mask;
  const size_t i2 = (i1 + 1) & mask;
  const size_t i3 = (i1 + 2) & mask;
  const float t = read_pos - static_cast<float>(i1);

  // 4-point, 3rd-order Hermite interpolation
  const float y0 = buf[i0], y1 = buf[i1], y2 = buf[i2], y3 = buf[i3];
  const float c0 = y1;
  const float c1 = 0.5f * (y2 - y0);
  const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

  return ((c3 * t + c2) * t + c1) * t + c0;
}

void VocalChorus::process(const float *in_l, const float *in_r, float *out_l, float *out_r,
                          size_t frames, float tempo_bpm) {
  if (!delay_buf_l_ || !delay_buf_r_ || frames == 0) {
    return;
  }

  // Fast bypass when mix is zero
  if (mix_ <= 0.0001f) {
    if (in_l != out_l) std::copy_n(in_l, frames, out_l);
    if (in_r != out_r) std::copy_n(in_r, frames, out_r);
    // Still write to delay line to keep history warm and update LFO phase
    for (size_t s = 0; s < frames; ++s) {
      delay_buf_l_[write_pos_] = in_l[s];
      delay_buf_r_[write_pos_] = in_r[s];
      write_pos_ = (write_pos_ + 1) & (buf_size_ - 1);
    }
    const float rate = effective_rate_hz(tempo_bpm);
    const float phase_inc = rate / sample_rate_;
    lfo_phase_0_ = std::fmod(lfo_phase_0_ + phase_inc * static_cast<float>(frames), 1.0f);
    lfo_phase_1_ = std::fmod(lfo_phase_1_ + phase_inc * 1.37f * static_cast<float>(frames), 1.0f);
    lfo_phase_2_ = std::fmod(lfo_phase_2_ + phase_inc * 0.73f * static_cast<float>(frames), 1.0f);
    return;
  }

  const float ms_to_samples = sample_rate_ * 0.001f;
  const float base_samples = base_delay_ms_ * ms_to_samples;
  const float depth_samples = depth_ms_ * ms_to_samples;

  const float rate = effective_rate_hz(tempo_bpm);
  const float phase_inc_0 = rate / sample_rate_;
  const float phase_inc_1 = phase_inc_0 * 1.37f;
  const float phase_inc_2 = phase_inc_0 * 0.73f;

  const float dry_gain = 1.0f - mix_;
  const float wet_gain = mix_;
  const float width = width_;

  float *const l_buf = delay_buf_l_.get();
  float *const r_buf = delay_buf_r_.get();
  const size_t mask = buf_size_ - 1;

  for (size_t s = 0; s < frames; ++s) {
    l_buf[write_pos_] = in_l[s];
    r_buf[write_pos_] = in_r[s];

    float wet_l = 0.0f;
    float wet_r = 0.0f;

    switch (mode_) {
    case ChorusMode::Chorus: {
      // Classic vocal chorus: smooth stereo widening with 90° quadrature LFO
      const float mod_l = fast_sin(lfo_phase_0_);
      const float mod_r = fast_sin(lfo_phase_0_ + 0.25f);

      const float d_l = base_samples + depth_samples * mod_l;
      const float d_r = base_samples + depth_samples * mod_r;

      const float s_l = read_sample(l_buf, d_l);
      const float s_r = read_sample(r_buf, d_r);

      // Width panning: 0 = mono wet, 1 = full stereo
      const float mid = 0.5f * (s_l + s_r);
      wet_l = mid + width * (s_l - mid);
      wet_r = mid + width * (s_r - mid);
      break;
    }

    case ChorusMode::Ensemble: {
      // 3 taps with decorrelated rates and staggered base delays for dense pseudo-doubling
      const float d0_l = (base_samples * 0.85f) + depth_samples * fast_sin(lfo_phase_0_);
      const float d1_l = (base_samples * 1.25f) + (depth_samples * 0.8f) * fast_sin(lfo_phase_1_);
      const float d2_l = (base_samples * 1.65f) + (depth_samples * 0.6f) * fast_sin(lfo_phase_2_);

      const float d0_r = (base_samples * 0.85f) + depth_samples * fast_sin(lfo_phase_0_ + 0.33f);
      const float d1_r = (base_samples * 1.25f) + (depth_samples * 0.8f) * fast_sin(lfo_phase_1_ + 0.33f);
      const float d2_r = (base_samples * 1.65f) + (depth_samples * 0.6f) * fast_sin(lfo_phase_2_ + 0.33f);

      const float tap0_l = read_sample(l_buf, d0_l);
      const float tap1_l = read_sample(l_buf, d1_l);
      const float tap2_l = read_sample(l_buf, d2_l);

      const float tap0_r = read_sample(r_buf, d0_r);
      const float tap1_r = read_sample(r_buf, d1_r);
      const float tap2_r = read_sample(r_buf, d2_r);

      // Normalized 3-tap sum with stereo spread
      const float ens_l = 0.45f * tap0_l + 0.35f * tap1_l + 0.25f * tap2_r;
      const float ens_r = 0.45f * tap0_r + 0.35f * tap1_r + 0.25f * tap2_l;

      const float mid = 0.5f * (ens_l + ens_r);
      wet_l = mid + width * (ens_l - mid);
      wet_r = mid + width * (ens_r - mid);
      break;
    }

    case ChorusMode::Dimension: {
      // Dimension mode: subtle widening, asymmetric base delays, low depth, excellent mono sum
      const float dim_depth = depth_samples * 0.4f; // low depth for transparency
      const float mod = fast_sin(lfo_phase_0_);

      const float d_l = (base_samples * 0.8f) + dim_depth * mod;
      const float d_r = (base_samples * 1.25f) - dim_depth * mod; // opposite phase

      const float s_l = read_sample(l_buf, d_l);
      const float s_r = read_sample(r_buf, d_r);

      // Cross-feed matrix for dimension effect: cancels pleasantly in mono
      const float dim_l = s_l - 0.22f * s_r;
      const float dim_r = s_r - 0.22f * s_l;

      const float mid = 0.5f * (dim_l + dim_r);
      wet_l = mid + width * (dim_l - mid);
      wet_r = mid + width * (dim_r - mid);
      break;
    }

    default:
      wet_l = in_l[s];
      wet_r = in_r[s];
      break;
    }

    out_l[s] = dry_gain * in_l[s] + wet_gain * wet_l;
    out_r[s] = dry_gain * in_r[s] + wet_gain * wet_r;

    write_pos_ = (write_pos_ + 1) & mask;

    lfo_phase_0_ += phase_inc_0;
    if (lfo_phase_0_ >= 1.0f) lfo_phase_0_ -= 1.0f;

    lfo_phase_1_ += phase_inc_1;
    if (lfo_phase_1_ >= 1.0f) lfo_phase_1_ -= 1.0f;

    lfo_phase_2_ += phase_inc_2;
    if (lfo_phase_2_ >= 1.0f) lfo_phase_2_ -= 1.0f;
  }
}

size_t VocalChorus::memory_bytes() const {
  return buf_size_ * 2 * sizeof(float) + sizeof(*this);
}
