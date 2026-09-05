#include "delay.h"
#include <algorithm>
#include <cmath>
bool StereoDelay::init(float sr, float maxs) {
  sr_ = sr;
  try {
    l_.assign((size_t)(sr * maxs) + 2, 0);
    r_.assign(l_.size(), 0);
  } catch (...) {
    return false;
  }
  const float maximum_delay = (float)l_.size() - 2.0f;
  dl_.init(std::min(.25f * sr, maximum_delay), sr);
  dr_.init(std::min(.375f * sr, maximum_delay), sr);
  wet_.init(.2f, sr);
  dry_.init(1, sr);
  set_feedback_lowpass(6000);
  return true;
}
void StereoDelay::reset() {
  std::fill(l_.begin(), l_.end(), 0);
  std::fill(r_.begin(), r_.end(), 0);
  pos_ = 0;
  lp_l_ = lp_r_ = 0;
}
void StereoDelay::set_times(float a, float b) {
  dl_.set_target(std::clamp(a * sr_ * .001f, 1.0f, (float)l_.size() - 2));
  dr_.set_target(std::clamp(b * sr_ * .001f, 1.0f, (float)l_.size() - 2));
}
void StereoDelay::set_mix(float d, float w) {
  dry_.set_target(std::clamp(d, 0.0f, 1.0f));
  wet_.set_target(std::clamp(w, 0.0f, 1.0f));
}
void StereoDelay::set_feedback(float f) {
  feedback_ = std::clamp(f, -.95f, .95f);
}
void StereoDelay::set_feedback_lowpass(float hz) {
  lp_alpha_ =
      std::exp(-2 * 3.14159265f * std::clamp(hz, 20.0f, sr_ * .49f) / sr_);
}
float StereoDelay::read(float d) const {
  d = std::clamp(d, 1.0f, (float)l_.size() - 2.0f);
  float p = (float)pos_ - d;
  if (p < 0)
    p += l_.size();
  size_t i = (size_t)p, j = (i + 1) % l_.size();
  float f = p - i;
  return l_[i] + (l_[j] - l_[i]) * f;
}
void StereoDelay::process(float x, float &ol, float &orr) {
  float a = read(dl_.next());
  const float right_delay =
      std::clamp(dr_.next(), 1.0f, (float)r_.size() - 2.0f);
  float p = (float)pos_ - right_delay;
  if (p < 0)
    p += r_.size();
  size_t i = (size_t)p, j = (i + 1) % r_.size();
  float f = p - i, b = r_[i] + (r_[j] - r_[i]) * f;
  lp_l_ = (1 - lp_alpha_) * a + lp_alpha_ * lp_l_;
  lp_r_ = (1 - lp_alpha_) * b + lp_alpha_ * lp_r_;
  l_[pos_] = x + feedback_ * lp_l_;
  r_[pos_] = x + feedback_ * lp_r_;
  pos_ = (pos_ + 1) % l_.size();
  float dry = dry_.next(), wet = wet_.next();
  ol = x * dry + a * wet;
  orr = x * dry + b * wet;
}
size_t StereoDelay::memory_bytes() const {
  return (l_.capacity() + r_.capacity()) * sizeof(float);
}
