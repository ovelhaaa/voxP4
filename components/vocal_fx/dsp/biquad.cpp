#include "biquad.h"
#include <algorithm>
#include <cmath>
void Biquad::configure(BiquadType t, float sr, float hz, float q, float db) {
  hz = std::clamp(hz, 1.0f, sr * 0.499f);
  q = std::max(q, 0.01f);
  const float w = 2 * 3.14159265358979323846f * hz / sr, c = std::cos(w),
              s = std::sin(w), A = std::pow(10.0f, db / 40),
              alpha = s / (2 * q);
  float a0;
  if (t == BiquadType::LowPass) {
    b0_ = (1 - c) / 2;
    b1_ = 1 - c;
    b2_ = b0_;
    a0 = 1 + alpha;
    a1_ = -2 * c;
    a2_ = 1 - alpha;
  } else if (t == BiquadType::HighPass) {
    b0_ = (1 + c) / 2;
    b1_ = -(1 + c);
    b2_ = b0_;
    a0 = 1 + alpha;
    a1_ = -2 * c;
    a2_ = 1 - alpha;
  } else if (t == BiquadType::Peaking) {
    b0_ = 1 + alpha * A;
    b1_ = -2 * c;
    b2_ = 1 - alpha * A;
    a0 = 1 + alpha / A;
    a1_ = -2 * c;
    a2_ = 1 - alpha / A;
  } else {
    const float sa = 2 * std::sqrt(A) * alpha;
    if (t == BiquadType::LowShelf) {
      b0_ = A * ((A + 1) - (A - 1) * c + sa);
      b1_ = 2 * A * ((A - 1) - (A + 1) * c);
      b2_ = A * ((A + 1) - (A - 1) * c - sa);
      a0 = (A + 1) + (A - 1) * c + sa;
      a1_ = -2 * ((A - 1) + (A + 1) * c);
      a2_ = (A + 1) + (A - 1) * c - sa;
    } else {
      b0_ = A * ((A + 1) + (A - 1) * c + sa);
      b1_ = -2 * A * ((A - 1) + (A + 1) * c);
      b2_ = A * ((A + 1) + (A - 1) * c - sa);
      a0 = (A + 1) - (A - 1) * c + sa;
      a1_ = 2 * ((A - 1) - (A + 1) * c);
      a2_ = (A + 1) - (A - 1) * c - sa;
    }
  }
  b0_ /= a0;
  b1_ /= a0;
  b2_ /= a0;
  a1_ /= a0;
  a2_ /= a0;
}
void Biquad::reset() { z1_ = z2_ = 0; }
float Biquad::process(float x) {
  if (bypass_)
    return x;
  float y = b0_ * x + z1_;
  z1_ = b1_ * x - a1_ * y + z2_;
  z2_ = b2_ * x - a2_ * y;
  return y;
}
void Biquad::process(const float *i, float *o, size_t n) {
  for (size_t k = 0; k < n; k++)
    o[k] = process(i[k]);
}
