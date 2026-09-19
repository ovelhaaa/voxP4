#include "limiter.h"
#include <algorithm>
#include <cmath>
void Limiter::init(float sr, float c) {
  release_ = std::exp(-1 / (sr * .05f));
  set_ceiling(c);
  reset();
}
void Limiter::reset() { gain_ = 1; }
void Limiter::set_ceiling(float v) { ceiling_ = std::clamp(v, .1f, 1.0f); }
void Limiter::process(float &l, float &r) {
  float p = std::max(std::fabs(l), std::fabs(r));
  float target = p > ceiling_ ? ceiling_ / p : 1;
  gain_ = target < gain_ ? target : 1 - (1 - gain_) * release_;
  l *= gain_;
  r *= gain_;
}

void Limiter::process_block(float *l, float *r, size_t n) {
  float g = gain_;
  const float c = ceiling_;
  const float rel = release_;
  for (size_t i = 0; i < n; ++i) {
    float p = std::max(std::fabs(l[i]), std::fabs(r[i]));
    float target = p > c ? c / p : 1.0f;
    g = target < g ? target : 1.0f - (1.0f - g) * rel;
    l[i] *= g;
    r[i] *= g;
  }
  gain_ = g;
}
