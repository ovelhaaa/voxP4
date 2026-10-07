#include "compressor.h"
#include <algorithm>
#include <cmath>
void Compressor::init(float sr) {
  sr_ = sr;
  set(-18, 3, 10, 100, 3, 6);
  reset();
}
void Compressor::reset() { env_ = 0; }
void Compressor::set(float t, float r, float a, float rel, float m, float k) {
  threshold_ = t;
  ratio_ = std::max(r, 1.0f);
  knee_ = std::max(k, 0.0f);
  makeup_ = std::pow(10.0f, m / 20);
  attack_ = std::exp(-1 / (sr_ * std::max(a, .01f) * .001f));
  release_ = std::exp(-1 / (sr_ * std::max(rel, .01f) * .001f));
  // Linear guard 1 dB below the knee edge. Below it the exact gain computer is
  // 0 dB, so process() may return x*makeup_ directly (bit-identical).
  threshold_lo_linear_ =
      std::pow(10.0f, (threshold_ - knee_ * 0.5f - 1.0f) * 0.05f);
}
float Compressor::gain_db_for(float x) const {
  float over = x - threshold_;
  if (knee_ > 0 && over > -knee_ / 2 && over < knee_ / 2) {
    float v = over + knee_ / 2;
    return (1 / ratio_ - 1) * v * v / (2 * knee_);
  }
  return over > 0 ? (1 / ratio_ - 1) * over : 0;
}
float Compressor::process(float x) {
  float p = std::fabs(x);
  float c = p > env_ ? attack_ : release_;
  env_ = p + (env_ - p) * c;
  // Exact fast path: provably 0 dB gain, so pow(10, 0) == 1 and log10 is not
  // needed. Identical output to the general path.
  if (env_ <= threshold_lo_linear_)
    return x * makeup_;
  const float db = 6.020599913279624f * std::log2(std::max(env_, 1e-12f));
  return x * std::exp2(gain_db_for(db) * 0.16609640474436813f) * makeup_;
}
void Compressor::process_block(float *buffer, size_t n) {
  float env = env_;
  const float attack = attack_, release = release_;
  const float makeup = makeup_, linear_limit = threshold_lo_linear_;
  for (size_t i = 0; i < n; ++i) {
    const float x = buffer[i];
    const float p = std::fabs(x);
    const float c = p > env ? attack : release;
    env = p + (env - p) * c;
    if (env <= linear_limit) {
      buffer[i] = x * makeup;
    } else {
      const float db = 6.020599913279624f * std::log2(std::max(env, 1e-12f));
      buffer[i] = x * std::exp2(gain_db_for(db) * 0.16609640474436813f) * makeup;
    }
  }
  env_ = env;
}
