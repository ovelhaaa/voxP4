#include "diffuser.h"
#include <algorithm>
#include <new>
bool AllPass::init(size_t n, float g) {
  const size_t size = std::max<size_t>(n, 1);
  auto buffer = std::unique_ptr<float[]>(new (std::nothrow) float[size]());
  if (!buffer)
    return false;
  buffer_ = std::move(buffer);
  size_ = size;
  gain_ = g;
  pos_ = 0;
  return true;
}
void AllPass::reset() {
  std::fill_n(buffer_.get(), size_, 0.0f);
  pos_ = 0;
}
float AllPass::process(float x) {
  float d = buffer_[pos_];
  float y = d - gain_ * x;
  buffer_[pos_] = x + gain_ * y;
  pos_ = (pos_ + 1) % size_;
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
