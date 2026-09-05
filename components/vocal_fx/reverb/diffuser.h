#pragma once
#include <cstddef>
#include <memory>
class AllPass {
public:
  bool init(size_t samples, float gain);
  void reset();
  float process(float x);
  size_t memory_bytes() const { return size_ * sizeof(float); }

private:
  std::unique_ptr<float[]> buffer_;
  size_t size_ = 0;
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
