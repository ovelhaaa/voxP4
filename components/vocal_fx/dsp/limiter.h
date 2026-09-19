#pragma once
#include <cstddef>
class Limiter {
public:
  void init(float sr, float ceiling = .98f);
  void reset();
  void set_ceiling(float v);
  void process(float &l, float &r);
  float process_master_block(const float *base_l, const float *base_r,
                            const float *delay_l, const float *delay_r,
                            const float *reverb_l, const float *reverb_r,
                            float *out_l, float *out_r, size_t n);

private:
  float ceiling_ = .98f, gain_ = 1, release_ = .999f;
};
