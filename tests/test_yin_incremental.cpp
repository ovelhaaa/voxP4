#include "yin_detector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr size_t kWindow = 512;
constexpr size_t kHop = 60;
constexpr size_t kTauCount = 185;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "yin incremental test failed: %s\n", message);
    std::exit(1);
  }
}

void fill_stream(std::array<float, kWindow + kHop * 5> &stream) {
  uint32_t state = 0x42344245U;
  for (size_t i = 0; i < stream.size(); ++i) {
    state = state * 1664525U + 1013904223U;
    const float noise = static_cast<int32_t>(state) * (1.0f / 2147483648.0f);
    stream[i] = 0.31f * std::sin(2.0f * 3.14159265358979323846f *
                                 147.0f * i / 12000.0f) +
                0.03f * noise;
  }
}
} // namespace

int main() {
  static_assert(2 * kHop * kTauCount == 22200,
                "frozen update-term count changed");
  require(yin_difference_product_count(kWindow, kTauCount) == 77515,
          "frozen full-product count");

  std::array<float, kWindow + kHop * 5> stream{};
  fill_stream(stream);
  std::array<float, kWindow + 1> oracle{}, candidate{};
  for (const auto variant : {YinDifferenceVariant::IncrementalF32,
                             YinDifferenceVariant::IncrementalDoubleSingle}) {
    YinIncrementalDifference state;
    require(state.init(variant, kWindow, kHop, kTauCount, 0),
            "state initialization");
    for (size_t hop = 0; hop < 5; ++hop) {
      const float *window = stream.data() + hop * kHop;
      yin_difference_fma_8acc(window, kWindow, kTauCount, oracle.data());
      const uint64_t operations = state.compute(window, candidate.data());
      require(operations == (hop == 0 ? 77515U : 22200U),
              "startup/update work count");
      for (size_t tau = 1; tau <= kTauCount; ++tau) {
        const float scale = std::max(std::fabs(oracle[tau]), 1.0f);
        require(std::fabs(candidate[tau] - oracle[tau]) <= 2.0e-5f * scale,
                "sliding identity numerical check");
      }
    }
    require(state.incremental_rebases() == 1, "startup rebase count");
    require(state.update_terms() == 4U * 22200U, "update-term telemetry");
    state.invalidate_for_gap();
    require(state.compute(stream.data() + 5 * kHop, candidate.data()) == 77515,
            "gap forces full rebase");
    require(state.incremental_gap_rebases() == 1, "gap rebase count");
  }

  YinIncrementalDifference periodic;
  require(periodic.init(YinDifferenceVariant::IncrementalDoubleSingle,
                        kWindow, kHop, kTauCount, 4),
          "periodic state initialization");
  for (size_t hop = 0; hop < 5; ++hop)
    periodic.compute(stream.data() + hop * kHop, candidate.data());
  require(periodic.incremental_rebases() == 2, "periodic total rebases");
  require(periodic.periodic_rebases() == 1, "periodic rebase count");
  std::puts("yin incremental formula/state tests passed");
  return 0;
}
