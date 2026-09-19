#pragma once

#include <cstddef>

class Limiter {
public:
  void init(float sr, float ceiling = .98f);
  void reset();
  void set_ceiling(float v);
  void process(float &l, float &r);
  void process_block(float *l, float *r, size_t n);

private:
  float ceiling_ = .98f, gain_ = 1, release_ = .999f;
};
