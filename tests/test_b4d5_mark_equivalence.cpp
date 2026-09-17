// B4D.5 §12 mark-selection equivalence gate.
//
// The production nearest-mark search was rewritten from a per-candidate
// software-double `fabs(source - position)` scan to an exact integer-key scan.
// This test drives the *actual* production function
// (td_psola_nearest_mark_index) against a reference implementation of the
// original double loop over a large randomized corpus and reports:
//   selected mark mismatch = 0
//   source position mismatch = 0
// Non-zero mismatches fail the test.
#include "td_psola.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Original algorithm: first occurrence of the strict minimum of
// |source - marks[i].sample_position|.
size_t reference_nearest(double source, const PitchMark *marks, size_t count) {
  size_t best = 0;
  double distance = std::fabs(source - static_cast<double>(marks[0].sample_position));
  for (size_t i = 1; i < count; ++i) {
    const double d = std::fabs(source - static_cast<double>(marks[i].sample_position));
    if (d < distance) {
      distance = d;
      best = i;
    }
  }
  return best;
}

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
  uint64_t next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
};

uint64_t check(const char *tag, const std::vector<PitchMark> &marks,
               const std::vector<double> &sources) {
  uint64_t mismatches = 0;
  for (double s : sources) {
    const size_t got =
        td_psola_nearest_mark_index(s, marks.data(), marks.size());
    const size_t want = reference_nearest(s, marks.data(), marks.size());
    if (got != want) {
      if (mismatches < 8) {
        std::printf("MISMATCH %s source=%.17g got=%zu pos=%llu want=%zu pos=%llu\n",
                    tag, s, got,
                    static_cast<unsigned long long>(marks[got].sample_position),
                    want,
                    static_cast<unsigned long long>(marks[want].sample_position));
      }
      ++mismatches;
    }
  }
  return mismatches;
}

} // namespace

int main() {
  Rng rng(0xB4D5C0FFEEULL);
  uint64_t total_mismatch = 0;
  uint64_t total_cases = 0;

  // 1. Random sorted marks, random fractional sources in [0, 2*max].
  for (int trial = 0; trial < 4000; ++trial) {
    const size_t n = 1 + (rng.next() % 64);
    const uint64_t span = 24 + (rng.next() % 4096);
    std::vector<PitchMark> marks(n);
    uint64_t pos = rng.next() % 1000;
    for (size_t i = 0; i < n; ++i) {
      marks[i].sample_position = pos;
      pos += 1 + (rng.next() % span);
      marks[i].confidence = 0.5f + 0.5f * static_cast<float>(rng.unit());
    }
    std::vector<double> sources;
    sources.reserve(400);
    const double hi = static_cast<double>(pos) + span;
    for (int k = 0; k < 400; ++k) sources.push_back(rng.unit() * hi);
    // exact mark positions
    for (size_t i = 0; i < n && sources.size() < 500; ++i)
      sources.push_back(static_cast<double>(marks[i].sample_position));
    total_mismatch += check("sorted", marks, sources);
    total_cases += sources.size();
  }

  // 2. Exact half-way ties between adjacent marks (frac == 0.5).
  for (int trial = 0; trial < 2000; ++trial) {
    const size_t n = 2 + (rng.next() % 32);
    std::vector<PitchMark> marks(n);
    uint64_t pos = 100 + (rng.next() % 1000);
    for (size_t i = 0; i < n; ++i) {
      marks[i].sample_position = pos;
      pos += 2 + (rng.next() % 64);
    }
    std::vector<double> sources;
    for (size_t i = 0; i + 1 < n; ++i)
      sources.push_back(0.5 * (static_cast<double>(marks[i].sample_position) +
                               static_cast<double>(marks[i + 1].sample_position)));
    total_mismatch += check("half", marks, sources);
    total_cases += sources.size();
  }

  // 3. Unsorted marks (the scan must not assume order).
  for (int trial = 0; trial < 1000; ++trial) {
    const size_t n = 2 + (rng.next() % 40);
    std::vector<PitchMark> marks(n);
    for (size_t i = 0; i < n; ++i)
      marks[i].sample_position = rng.next() % 200000;
    std::vector<double> sources;
    for (int k = 0; k < 200; ++k) sources.push_back(rng.unit() * 200000.0);
    total_mismatch += check("unsorted", marks, sources);
    total_cases += sources.size();
  }

  // 4. Large sample positions (near the practical history limit).
  for (int trial = 0; trial < 200; ++trial) {
    const size_t n = 2 + (rng.next() % 60);
    std::vector<PitchMark> marks(n);
    uint64_t pos = (1ULL << 33) + (rng.next() % 100000);
    for (size_t i = 0; i < n; ++i) {
      marks[i].sample_position = pos;
      pos += 1 + (rng.next() % 500);
    }
    std::vector<double> sources;
    for (int k = 0; k < 300; ++k)
      sources.push_back(static_cast<double>(1ULL << 33) + rng.unit() * 100000.0);
    total_mismatch += check("large", marks, sources);
    total_cases += sources.size();
  }

  std::printf("B4D5_MARK_EQUIVALENCE cases=%llu selected_mismatch=%llu "
              "source_position_mismatch=%llu result=%s\n",
              static_cast<unsigned long long>(total_cases),
              static_cast<unsigned long long>(total_mismatch),
              static_cast<unsigned long long>(total_mismatch),
              total_mismatch == 0 ? "PASS" : "FAIL");
  return total_mismatch == 0 ? 0 : 1;
}
