#include "fdn_reverb.h"
#include <algorithm>
#include <cmath>
void FdnReverb::hadamard8(float *x) {
  for (int step = 1; step < 8; step *= 2)
    for (int i = 0; i < 8; i += 2 * step)
      for (int j = 0; j < step; j++) {
        float a = x[i + j], b = x[i + j + step];
        x[i + j] = a + b;
        x[i + j + step] = a - b;
      }
  constexpr float n = .3535533905932738f;
  for (int i = 0; i < 8; i++)
    x[i] *= n;
}
bool FdnReverb::init(float sr) {
  sr_ = sr; /* Pairwise-incommensurate millisecond values spread modes and avoid
               a common audible period. */
  const float ms[8] = {29.7f, 32.9f, 36.1f, 39.7f, 43.3f, 47.9f, 52.1f, 58.3f};
  try {
    for (int i = 0; i < 8; i++)
      lines_[i].b.assign((size_t)(sr * ms[i] * .001f), 0);
  } catch (...) {
    return false;
  }
  if (!diffuser_.init(sr))
    return false;
  wet_.init(.18f, sr, 30);
  set_rt60(2);
  set_damping(.45f);
  return true;
}
void FdnReverb::reset() {
  for (auto &x : lines_) {
    std::fill(x.b.begin(), x.b.end(), 0);
    x.pos = 0;
    x.damping_state = 0;
  }
  diffuser_.reset();
}
void FdnReverb::set_rt60(float s) {
  s = std::clamp(s, .15f, 20.0f);
  for (auto &x : lines_)
    x.feedback_gain = std::pow(10.0f, -3.0f * ((float)x.b.size() / sr_) / s);
}
void FdnReverb::set_damping(float n) {
  n = std::clamp(n, 0.0f, 1.0f);
  float hz = 18000.0f * std::pow(500.0f / 18000.0f, n);
  damping_coefficient_ = std::exp(-2 * 3.14159265f * hz / sr_);
}
void FdnReverb::set_wet(float w) { wet_.set_target(std::clamp(w, 0.0f, 1.0f)); }
void FdnReverb::process(float in, float &l, float &r) {
  float x[8];
  for (int i = 0; i < 8; i++) {
    auto &q = lines_[i];
    float d = q.b[q.pos];
    q.damping_state =
        (1 - damping_coefficient_) * d + damping_coefficient_ * q.damping_state;
    x[i] = q.damping_state;
  }
  float taps[8];
  for (int i = 0; i < 8; i++)
    taps[i] = x[i];
  hadamard8(x);
  float feed = diffuser_.process(in) * .35355339f;
  for (int i = 0; i < 8; i++) {
    auto &q = lines_[i];
    q.b[q.pos] = feed + x[i] * q.feedback_gain;
    q.pos = (q.pos + 1) % q.b.size();
  }
  constexpr float scale = .25f;
  l = (taps[0] + taps[1] - taps[2] + taps[3] - taps[4] - taps[5] + taps[6] -
       taps[7]) *
      scale;
  r = (taps[0] - taps[1] + taps[2] + taps[3] - taps[4] + taps[5] - taps[6] -
       taps[7]) *
      scale;
  float w = wet_.next();
  l *= w;
  r *= w;
}
size_t FdnReverb::memory_bytes() const {
  size_t n = diffuser_.memory_bytes();
  for (auto &q : lines_)
    n += q.b.capacity() * sizeof(float);
  return n;
}
