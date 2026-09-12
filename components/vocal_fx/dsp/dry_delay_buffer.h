#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

class DryDelayBuffer {
public:
  static constexpr size_t kCapacity = 4096; // Power of two: 85.3 ms @ 48 kHz
  static constexpr size_t kMask = kCapacity - 1;

  void init(float sample_rate, float delay_ms = 32.0f, bool enabled = false) {
    sample_rate_ = sample_rate > 0.0f ? sample_rate : 48000.0f;
    enabled_ = enabled;
    set_delay_ms(delay_ms);
    reset();
  }

  void reset() {
    std::memset(buffer_, 0, sizeof(buffer_));
    write_pos_ = 0;
  }

  void set_enabled(bool enabled) {
    enabled_ = enabled;
  }

  bool enabled() const {
    return enabled_;
  }

  void set_delay_ms(float delay_ms) {
    const float clamped_ms = std::clamp(delay_ms, 0.0f, 50.0f);
    delay_ms_ = clamped_ms;
    const uint32_t samples = static_cast<uint32_t>(std::lround(clamped_ms * sample_rate_ * 0.001f));
    set_delay_samples(samples);
  }

  void set_delay_samples(uint32_t samples) {
    delay_samples_ = std::min<uint32_t>(samples, static_cast<uint32_t>(kMask));
  }

  uint32_t delay_samples() const {
    return delay_samples_;
  }

  float delay_ms() const {
    return delay_ms_;
  }

  const void* buffer_ptr() const { return buffer_; }
  size_t buffer_bytes() const { return sizeof(buffer_); }

  inline float process(float in) {
    buffer_[write_pos_] = in;
    if (!enabled_ || delay_samples_ == 0) {
      write_pos_ = (write_pos_ + 1) & kMask;
      return in;
    }
    const size_t read_pos = (write_pos_ + kCapacity - delay_samples_) & kMask;
    write_pos_ = (write_pos_ + 1) & kMask;
    return buffer_[read_pos];
  }

private:
  float sample_rate_ = 48000.0f;
  float delay_ms_ = 32.0f;
  uint32_t delay_samples_ = 1536;
  bool enabled_ = false;
  size_t write_pos_ = 0;
  float buffer_[kCapacity]{};
};
