#pragma once
#include <cstddef>
enum class BiquadType { HighPass, LowPass, Peaking, LowShelf, HighShelf };
class Biquad {
public:
  void configure(BiquadType type, float sr, float hz, float q = 0.70710678f,
                 float gain_db = 0);
  void reset();
  void set_bypass(bool b) { bypass_ = b; }
  float process(float x);
  void process(const float *in, float *out, size_t n);

private:
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
  bool bypass_ = false;
};
