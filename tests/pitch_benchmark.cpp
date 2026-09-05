#include "pitch_analysis.h"
#include <chrono>
#include <cmath>
#include <cstdio>
int main() {
  PitchAnalysis a;
  PitchAnalysisConfig c{};
  if (!a.init(c))
    return 1;
  float block[64];
  uint64_t p = 0;
  constexpr float pi = 3.14159265358979323846f;
  auto begin = std::chrono::steady_clock::now();
  size_t hops = 0;
  for (int b = 0; b < 12000; ++b) {
    for (float &v : block)
      v = .3f * std::sin(2 * pi * 220 * p++ / 48000);
    a.tap(block, 64);
    hops += a.run(8);
  }
  double us = std::chrono::duration<double, std::micro>(
                  std::chrono::steady_clock::now() - begin)
                  .count(),
         hop_us = 1e6 * c.hop_size / c.analysis_sample_rate;
  std::printf("pitch host: %zu hops, %.2f us/hop, %.1f%% of %.2f ms hop, RAM "
              "%zu bytes\n",
              hops, us / hops, (us / hops) / hop_us * 100, hop_us / 1000,
              a.memory_bytes());
  for (int i = 0; i < (int)PitchAnalysisProfileSection::Count; ++i) {
    auto s = a.profile((PitchAnalysisProfileSection)i);
    std::printf("stage %d calls=%llu avg_us=%.2f max_us=%llu\n", i,
                (unsigned long long)s.calls,
                s.calls ? (double)s.total_us / s.calls : 0,
                (unsigned long long)s.max_us);
  }
  return 0;
}
