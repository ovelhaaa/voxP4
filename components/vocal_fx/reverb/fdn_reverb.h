#pragma once
#include "diffuser.h"
#include "smoothing.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
class FdnReverb {
public:
  bool init(float sr);
  void reset();
  void set_rt60(float seconds);
  void set_damping(float normalized);
  void set_wet(float wet);
  // Per-sample reference path (retained for host tests / API compatibility).
  void process(float input, float &l, float &r);
  // Bit-identical block path used by the production audio loop. Hoists exact
  // per-block invariants and advances ring indices with a single wrap branch
  // instead of a runtime modulo.
  void process_block(const float *in, float *l, float *r, size_t n);
  size_t memory_bytes() const;
  static void hadamard8(float *x);
  size_t line_count() const { return 8; }
  const void* line_ptr(size_t i) const { return i < 8 ? lines_[i].b : nullptr; }
  size_t line_bytes(size_t i) const { return i < 8 ? lines_[i].size * sizeof(float) : 0; }
  const Diffuser& diffuser() const { return diffuser_; }

  // B4D.2 diagnostic subsection accounting (§19/§20). Off by default so the
  // production hot path pays nothing. Cycles are accumulated at block
  // granularity in process_block().
  struct ReverbProfile {
    uint64_t read_damping_cycles = 0;
    uint64_t mix_cycles = 0;
    uint64_t hadamard_cycles = 0;
    uint64_t diffuser_cycles = 0;
    uint64_t write_index_cycles = 0;
    uint64_t wet_mix_cycles = 0;
    uint64_t total_cycles = 0;
    uint64_t samples = 0;
    uint64_t blocks = 0;
  };
  void set_profile_enabled(bool enabled);
  bool profile_enabled() const;
  void reset_profile();
  ReverbProfile profile() const;
  // Buffer placement for the memory audit.
  bool line_in_psram(size_t i) const;
  bool diffuser_in_psram(size_t i) const;

private:
  struct Line {
    float *b = nullptr;   // points into pool_ (64-byte aligned slice)
    size_t size = 0;
    size_t pos = 0;
    float damping_state = 0, feedback_gain = .8f;
  };
  std::array<Line, 8> lines_;
  // B4D.2: single 64-byte-aligned pool for the eight delay lines so their
  // active cache lines do not collide across lines. Same delay lengths and
  // same arithmetic as separate allocations.
  std::unique_ptr<float[]> pool_;
  Diffuser diffuser_;
  float sr_ = 0, damping_coefficient_ = .5f;
  SmoothedValue wet_;
};
