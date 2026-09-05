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
