#pragma once
#include "smoothing.h"
#include <cstddef>
#include <vector>
class StereoDelay {
public:
  bool init(float sr, float max_seconds);
  void reset();
  void set_times(float l_ms, float r_ms);
  void set_mix(float dry, float wet);
  void set_feedback(float f);
  void set_feedback_lowpass(float hz);
  void process(float input, float &l, float &r);
  size_t memory_bytes() const;

private:
  float read(float delay) const;
  std::vector<float> l_, r_;
  size_t pos_ = 0;
  float sr_ = 0, feedback_ = .25f, lp_alpha_ = 0, lp_l_ = 0, lp_r_ = 0;
  SmoothedValue dl_, dr_, wet_, dry_;
};
