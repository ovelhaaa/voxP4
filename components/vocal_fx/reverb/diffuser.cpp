#include "diffuser.h"
#include <algorithm>
bool AllPass::init(size_t n, float g) {
  try {
    buffer_.assign(std::max<size_t>(n, 1), 0);
  } catch (...) {
    return false;
  }
  gain_ = g;
  pos_ = 0;
  return true;
}
void AllPass::reset() {
  std::fill(buffer_.begin(), buffer_.end(), 0);
  pos_ = 0;
}
float AllPass::process(float x) {
  float d = buffer_[pos_];
  float y = d - gain_ * x;
  buffer_[pos_] = x + gain_ * y;
  pos_ = (pos_ + 1) % buffer_.size();
  return y;
}
bool Diffuser::init(float sr) {
  const float ms[3] = {3.1f, 4.7f, 7.3f};
  for (int i = 0; i < 3; i++)
    if (!stages_[i].init((size_t)(sr * ms[i] * .001f), .62f - i * .04f))
      return false;
  return true;
}
void Diffuser::reset() {
  for (auto &s : stages_)
    s.reset();
}
float Diffuser::process(float x) {
  for (auto &s : stages_)
    x = s.process(x);
  return x;
}
size_t Diffuser::memory_bytes() const {
  size_t n = 0;
  for (auto &s : stages_)
    n += s.memory_bytes();
  return n;
}
