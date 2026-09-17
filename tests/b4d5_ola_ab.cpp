// B4D.5 §19 isolated OLA index-progression benchmark.
//
// Compares the pre-B4D.4 per-sample 64-bit division against the retained
// quotient/remainder accumulator used by the LPC window/OLA path, over the
// same input, grain lengths and Hann table.  Verifies bit-identical indices
// and reports cycles/sample, us/grain and speedup.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
constexpr size_t kHannSize = 2048;

// Old path: one 64-bit division per sample.
size_t old_index(int64_t m, int denom) {
  return static_cast<size_t>(
      (static_cast<int64_t>(m) * static_cast<int64_t>(kHannSize - 1)) / denom);
}

// New path: quotient/remainder accumulator (production B4D.4 code).
struct Accumulator {
  int64_t wi, rem, q, r;
  int denom;
  Accumulator(int n_min, int half, int denom_in)
      : denom(denom_in) {
    const int64_t step = static_cast<int64_t>(kHannSize - 1);
    q = step / denom;
    r = step % denom;
    const int64_t num0 = static_cast<int64_t>(n_min + half) * step;
    wi = num0 / denom;
    rem = num0 % denom;
  }
  size_t next() {
    const size_t value = static_cast<size_t>(wi);
    wi += q;
    rem += r;
    if (rem >= denom) {
      ++wi;
      rem -= denom;
    }
    return value;
  }
};

uint64_t checksum_old(int n_min, int half, int count) {
  const int denom = 2 * half;
  uint64_t h = 1469598103934665603ULL;
  for (int k = 0; k < count; ++k) {
    const size_t wi = old_index(n_min + half + k, denom);
    h = (h ^ wi) * 1099511628211ULL;
  }
  return h;
}

uint64_t checksum_new(int n_min, int half, int count) {
  const int denom = 2 * half;
  Accumulator acc(n_min, half, denom);
  uint64_t h = 1469598103934665603ULL;
  for (int k = 0; k < count; ++k) {
    const size_t wi = acc.next();
    h = (h ^ wi) * 1099511628211ULL;
  }
  return h;
}

} // namespace

int main() {
  // Hann table stand-in (values are irrelevant to the index arithmetic).
  std::vector<float> hann(kHannSize);
  for (size_t i = 0; i < kHannSize; ++i)
    hann[i] = 0.5f - 0.5f * std::cos(6.283185307179586f *
                                     static_cast<float>(i) /
                                     static_cast<float>(kHannSize - 1));

  // Representative grain half-widths (period/2) for the production workload.
  const int halves[] = {24, 48, 100, 200, 400, 800};
  const int iterations = 20000;

  uint64_t mismatches = 0;
  double total_old_us = 0.0;
  double total_new_us = 0.0;
  uint64_t total_samples = 0;

  std::printf("half,count,old_us,new_us,cycles_per_sample_old,"
              "cycles_per_sample_new,us_per_grain_old,us_per_grain_new,"
              "speedup,bit_identical\n");

  for (int half : halves) {
    const int count = 2 * half; // full grain window
    for (int n_min = -half; n_min <= 0; n_min += half) {
      if (checksum_old(n_min, half, count) != checksum_new(n_min, half, count))
        ++mismatches;

      volatile uint64_t sink = 0;
      const auto t0 = std::chrono::steady_clock::now();
      for (int it = 0; it < iterations; ++it) {
        const int denom = 2 * half;
        for (int k = 0; k < count; ++k)
          sink ^= old_index(n_min + half + k, denom);
      }
      const auto t1 = std::chrono::steady_clock::now();
      for (int it = 0; it < iterations; ++it) {
        const int denom = 2 * half;
        Accumulator acc(n_min, half, denom);
        for (int k = 0; k < count; ++k)
          sink ^= acc.next();
      }
      const auto t2 = std::chrono::steady_clock::now();
      (void)sink;

      const double old_us =
          std::chrono::duration<double, std::micro>(t1 - t0).count();
      const double new_us =
          std::chrono::duration<double, std::micro>(t2 - t1).count();
      total_old_us += old_us;
      total_new_us += new_us;
      total_samples += static_cast<uint64_t>(count) * iterations;

      const double old_per_grain = old_us / iterations;
      const double new_per_grain = new_us / iterations;
      std::printf("%d,%d,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%.3f,%s\n", half, count,
                  old_us, new_us,
                  old_us * 1000.0 / (static_cast<double>(count) * iterations),
                  new_us * 1000.0 / (static_cast<double>(count) * iterations),
                  old_per_grain, new_per_grain,
                  new_us > 0.0 ? old_us / new_us : 0.0,
                  checksum_old(n_min, half, count) ==
                          checksum_new(n_min, half, count)
                      ? "YES"
                      : "NO");
    }
  }

  const double speedup = total_new_us > 0.0 ? total_old_us / total_new_us : 0.0;
  std::printf("B4D5_OLA_AB samples=%llu old_us=%.2f new_us=%.2f speedup=%.3f "
              "bit_identical=%s\n",
              static_cast<unsigned long long>(total_samples), total_old_us,
              total_new_us, speedup, mismatches == 0 ? "YES" : "NO");
  return mismatches == 0 ? 0 : 1;
}
