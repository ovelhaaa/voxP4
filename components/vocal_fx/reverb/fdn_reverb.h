#pragma once
#include "diffuser.h"
#include "smoothing.h"
#include <array>
#include <cstddef>
#include <vector>
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

private:
  struct Line {
    std::vector<float> b;
    size_t pos = 0;
    float damping_state = 0, feedback_gain = .8f;
    float modulation_phase = 0, modulation_depth = 0;
  };
  std::array<Line, 8> lines_;
  Diffuser diffuser_;
  float sr_ = 0, damping_coefficient_ = .5f;
  SmoothedValue wet_;
};
