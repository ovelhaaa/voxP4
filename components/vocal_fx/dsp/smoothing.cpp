#include "smoothing.h"
#include <algorithm>
#include <cmath>
void SmoothedValue::init(float v, float sr, float ms) {
  current_ = target_ = v;
  set_time(sr, ms);
}
void SmoothedValue::set_target(float v) { target_ = v; }
void SmoothedValue::set_time(float sr, float ms) {
  coefficient_ = ms <= 0 ? 0 : std::exp(-1.0f / (sr * ms * 0.001f));
}
float SmoothedValue::next() {
  current_ = target_ + coefficient_ * (current_ - target_);
  return current_;
}
void SmoothedValue::reset(float v) { current_ = target_ = v; }
