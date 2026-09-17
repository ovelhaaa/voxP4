// B4D.2 exact reverb equivalence: the production block path must be
// bit-identical to the per-sample reference, including with the diagnostic
// subsection profiler enabled, across many ring-wrap events.
#include "fdn_reverb.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int failures = 0;
#define CHECK(x)                                                              \
  do {                                                                        \
    if (!(x)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);       \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

static bool bits_equal(float a, float b) {
  uint32_t x, y;
  std::memcpy(&x, &a, sizeof(x));
  std::memcpy(&y, &b, sizeof(y));
  return x == y;
}

int main() {
  constexpr size_t kBlocks = 4000;
  constexpr size_t kBlock = 64;
  constexpr size_t kN = kBlocks * kBlock;
  constexpr float kFs = 48000.0f;

  std::vector<float> in(kN), ref_l(kN), ref_r(kN), blk_l(kN), blk_r(kN),
      prof_l(kN), prof_r(kN);
  uint32_t seed = 0x1234567u;
  for (size_t i = 0; i < kN; ++i) {
    seed = seed * 1664525u + 1013904223u;
    in[i] = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.4f;
  }

  // Reference per-sample path.
  FdnReverb ref;
  CHECK(ref.init(kFs));
  ref.set_rt60(2.0f);
  ref.set_damping(0.45f);
  ref.set_wet(0.5f);
  for (size_t i = 0; i < kN; ++i) ref.process(in[i], ref_l[i], ref_r[i]);

  // Production block path.
  FdnReverb blk;
  CHECK(blk.init(kFs));
  blk.set_rt60(2.0f);
  blk.set_damping(0.45f);
  blk.set_wet(0.5f);
  for (size_t b = 0; b < kBlocks; ++b)
    blk.process_block(in.data() + b * kBlock, blk_l.data() + b * kBlock,
                      blk_r.data() + b * kBlock, kBlock);

  // Profiled block path (identical arithmetic; adds cycle accounting only).
  FdnReverb pr;
  CHECK(pr.init(kFs));
  pr.set_rt60(2.0f);
  pr.set_damping(0.45f);
  pr.set_wet(0.5f);
  pr.set_profile_enabled(true);
  for (size_t b = 0; b < kBlocks; ++b)
    pr.process_block(in.data() + b * kBlock, prof_l.data() + b * kBlock,
                     prof_r.data() + b * kBlock, kBlock);

  size_t ref_mismatch = 0, prof_mismatch = 0;
  for (size_t i = 0; i < kN; ++i) {
    if (!bits_equal(ref_l[i], blk_l[i]) || !bits_equal(ref_r[i], blk_r[i]))
      ++ref_mismatch;
    if (!bits_equal(ref_l[i], prof_l[i]) || !bits_equal(ref_r[i], prof_r[i]))
      ++prof_mismatch;
  }
  CHECK(ref_mismatch == 0);
  CHECK(prof_mismatch == 0);
  CHECK(pr.profile().samples == kN);
  CHECK(pr.profile().blocks == kBlocks);
  CHECK(pr.profile().total_cycles > 0);

  // Buffer placement / size accounting is coherent.
  size_t total = 0;
  for (size_t i = 0; i < 8; ++i) total += ref.line_bytes(i);
  for (size_t i = 0; i < 3; ++i) total += ref.diffuser().stage_bytes(i);
  CHECK(total == ref.memory_bytes());
  CHECK(ref.memory_bytes() > 0);

  if (failures) {
    std::fprintf(stderr, "reverb equivalence: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("reverb equivalence OK (%zu samples, %zu block mismatches, "
              "%zu profiled mismatches)\n",
              kN, ref_mismatch, prof_mismatch);
  return 0;
}
