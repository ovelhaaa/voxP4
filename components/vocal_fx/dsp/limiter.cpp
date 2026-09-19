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
float Limiter::process_master_block(const float *base_l, const float *base_r,
                                    const float *delay_l, const float *delay_r,
                                    const float *reverb_l, const float *reverb_r,
                                    float *out_l, float *out_r, size_t n) {
  float gain = gain_;
  const float ceiling = ceiling_, release = release_;
  float output_peak = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    float l = base_l[i];
    float r = base_r[i];
    l += delay_l[i] + reverb_l[i];
    r += delay_r[i] + reverb_r[i];
    const float p = std::max(std::fabs(l), std::fabs(r));
    const float target = p > ceiling ? ceiling / p : 1;
    gain = target < gain ? target : 1 - (1 - gain) * release;
    l *= gain;
    r *= gain;
    out_l[i] = l;
    out_r[i] = r;
    const float la = std::fabs(l), ra = std::fabs(r);
    if (la > output_peak) output_peak = la;
    if (ra > output_peak) output_peak = ra;
  }
  gain_ = gain;
  return output_peak;
}
