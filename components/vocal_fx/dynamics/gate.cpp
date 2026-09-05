#include "gate.h"
#include <algorithm>
#include <cmath>
float Gate::db_to_linear(float d) { return std::pow(10.0f, d / 20); }
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
