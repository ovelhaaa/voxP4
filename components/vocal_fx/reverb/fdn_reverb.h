#pragma once
#include "diffuser.h"
#include "smoothing.h"
#include <array>
#include <cstddef>
#include <memory>
class FdnReverb {
public:
  bool init(float sr);
  void reset();
  void set_rt60(float seconds);
  void set_damping(float normalized);
  void set_wet(float wet);
  void process(float input, float &l, float &r);
  size_t memory_bytes() const;
  static void hadamard8(float *x);
  size_t line_count() const { return 8; }
  const void* line_ptr(size_t i) const { return i < 8 ? lines_[i].b.get() : nullptr; }
  size_t line_bytes(size_t i) const { return i < 8 ? lines_[i].size * sizeof(float) : 0; }
  const Diffuser& diffuser() const { return diffuser_; }

private:
  struct Line {
    std::unique_ptr<float[]> b;
    size_t size = 0;
    size_t pos = 0;
    float damping_state = 0, feedback_gain = .8f;
    float modulation_phase = 0, modulation_depth = 0;
  };
  std::array<Line, 8> lines_;
  Diffuser diffuser_;
  float sr_ = 0, damping_coefficient_ = .5f;
  SmoothedValue wet_;
};
