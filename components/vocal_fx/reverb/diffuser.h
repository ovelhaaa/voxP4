#pragma once
#include <cstddef>
#include <vector>
class AllPass {
public:
  bool init(size_t samples, float gain);
  void reset();
  float process(float x);
  size_t memory_bytes() const { return buffer_.capacity() * sizeof(float); }

private:
  std::vector<float> buffer_;
  size_t pos_ = 0;
  float gain_ = .6f;
};
class Diffuser {
public:
  bool init(float sr);
  void reset();
  float process(float x);
  size_t memory_bytes() const;

private:
  AllPass stages_[3];
};
