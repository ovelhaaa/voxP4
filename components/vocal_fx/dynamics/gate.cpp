#include "gate.h"
#include <algorithm>
#include <cmath>
float Gate::db_to_linear(float d) {
  // std::pow(10.0f, d / 20) == std::exp2(d * log2(10) / 20)
  constexpr float kDbToLog2 = 0.16609640474436813f;
  return std::exp2(d * kDbToLog2);
}
void Gate::init(float sr) {
  sr_ = sr;
  set(-55, 5, 40, 120, -60);
  reset();
}
void Gate::reset() {
  env_ = 0;
  gain_ = 1;
  hold_ = 0;
}
void Gate::set(float t, float a, float h, float r, float range) {
  threshold_ = db_to_linear(t);
  attack_ = std::exp(-1 / (sr_ * std::max(a, .01f) * .001f));
  release_ = std::exp(-1 / (sr_ * std::max(r, .01f) * .001f));
  hold_samples_ = (unsigned)(sr_ * std::max(h, 0.0f) * .001f);
  min_gain_ = db_to_linear(range);
}
float Gate::process(float x) {
  float p = std::fabs(x);
  env_ = p > env_ ? p + (env_ - p) * attack_ : p + (env_ - p) * release_;
  float target;
  if (env_ >= threshold_) {
    hold_ = hold_samples_;
    target = 1;
  } else if (hold_) {
    --hold_;
    target = 1;
  } else
    target = min_gain_;
  float c = target > gain_ ? attack_ : release_;
  gain_ = target + (gain_ - target) * c;
  return x * gain_;
}
void Gate::process_block(float *buffer, size_t n) {
  float env = env_, gain = gain_;
  unsigned hold = hold_;
  const float attack = attack_, release = release_;
  const float threshold = threshold_, min_gain = min_gain_;
  const unsigned hold_samples = hold_samples_;
  for (size_t i = 0; i < n; ++i) {
    const float x = buffer[i];
    const float p = std::fabs(x);
    env = p > env ? p + (env - p) * attack : p + (env - p) * release;
    float target;
    if (env >= threshold) {
      hold = hold_samples;
      target = 1;
    } else if (hold) {
      --hold;
      target = 1;
    } else {
      target = min_gain;
    }
    const float c = target > gain ? attack : release;
    gain = target + (gain - target) * c;
    buffer[i] = x * gain;
  }
  env_ = env;
  gain_ = gain;
  hold_ = hold;
}
