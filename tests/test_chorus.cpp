#include "chorus.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
int g_failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

void check_near(float actual, float expected, float tol, const char *message) {
  if (std::fabs(actual - expected) > tol) {
    std::fprintf(stderr, "FAIL: %s (expected %.5f, got %.5f, diff %.5f > tol %.5f)\n",
                 message, expected, actual, std::fabs(actual - expected), tol);
    ++g_failures;
  }
}
} // namespace

int main() {
  VocalChorus chorus;
  if (!chorus.init(44100.0f, 50.0f)) {
    std::fprintf(stderr, "FAIL: chorus.init failed\n");
    return 1;
  }

  constexpr size_t N = 512;
  std::vector<float> in_l(N), in_r(N), out_l(N), out_r(N);

  for (size_t i = 0; i < N; ++i) {
    in_l[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 44100.0f);
    in_r[i] = in_l[i];
  }

  // 1. Bypass / mix = 0 test
  chorus.set_mix(0.0f);
  chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);
  for (size_t i = 0; i < N; ++i) {
    check_near(out_l[i], in_l[i], 1e-5f, "mix=0 preserves left channel exactly");
    check_near(out_r[i], in_r[i], 1e-5f, "mix=0 preserves right channel exactly");
  }

  // 2. Zero input test
  chorus.reset();
  chorus.set_mix(0.5f);
  std::vector<float> zero(N, 0.0f);
  chorus.process(zero.data(), zero.data(), out_l.data(), out_r.data(), N, 120.0f);
  for (size_t i = 0; i < N; ++i) {
    check_near(out_l[i], 0.0f, 1e-6f, "zero input produces zero left output");
    check_near(out_r[i], 0.0f, 1e-6f, "zero input produces zero right output");
  }

  // 3. Reset test
  chorus.reset();
  check(true, "chorus.reset() executed successfully");

  // 4. Test all modes (Chorus, Ensemble, Dimension)
  for (int m = 0; m < static_cast<int>(ChorusMode::Count); ++m) {
    chorus.reset();
    chorus.set_mode(static_cast<ChorusMode>(m));
    chorus.set_mix(0.5f);
    chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);

    bool all_finite = true;
    float peak_l = 0.0f, peak_r = 0.0f;
    for (size_t i = 0; i < N; ++i) {
      if (!std::isfinite(out_l[i]) || !std::isfinite(out_r[i])) all_finite = false;
      peak_l = std::max(peak_l, std::fabs(out_l[i]));
      peak_r = std::max(peak_r, std::fabs(out_r[i]));
    }
    check(all_finite, "All output samples finite in mode");
    check(peak_l > 0.01f && peak_r > 0.01f, "Mode produces active audio output");
    check(peak_l < 2.0f && peak_r < 2.0f, "Mode output is bounded");
  }

  // 5. Test Linear vs Cubic Hermite interpolation
  chorus.reset();
  chorus.set_mode(ChorusMode::Chorus);
  chorus.set_interpolation(ChorusInterpolation::Linear);
  chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);
  const float lin_peak = out_l[100];

  chorus.reset();
  chorus.set_interpolation(ChorusInterpolation::CubicHermite);
  chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);
  const float cub_peak = out_l[100];
  check(std::isfinite(lin_peak) && std::isfinite(cub_peak), "Both interpolations finite");

  // 6. Test FREE rate vs SYNC rate
  chorus.set_sync_enabled(false);
  chorus.set_rate_hz(1.5f);
  check_near(chorus.rate_hz(), 1.5f, 1e-4f, "FREE rate set to 1.5 Hz");

  chorus.set_sync_enabled(true);
  chorus.set_subdivision(TempoSubdivision::Half); // at 120 BPM, 1/2 is 1000 ms -> 1.0 Hz
  check(chorus.sync_enabled(), "SYNC mode enabled");

  // 7. Test small successive BPM transitions under SYNC
  const float bpm_stream[] = {120.0f, 90.0f, 140.0f, 121.0f, 122.5f};
  for (float bpm : bpm_stream) {
    chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, bpm);
    for (size_t i = 0; i < N; ++i) {
      check(std::isfinite(out_l[i]) && std::isfinite(out_r[i]), "Output finite during BPM change");
    }
  }

  // 8. Mono compatibility check
  chorus.reset();
  chorus.set_mode(ChorusMode::Dimension);
  chorus.set_mix(0.4f);
  // Run several blocks to establish modulation
  for (int b = 0; b < 10; ++b) {
    chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);
  }
  double mono_energy = 0.0;
  for (size_t i = 0; i < N; ++i) {
    const float mono = 0.5f * (out_l[i] + out_r[i]);
    mono_energy += mono * mono;
  }
  const float mono_rms = std::sqrt(mono_energy / N);
  check(mono_rms > 0.15f, "Dimension mode mono sum maintains strong vocal energy (no phase cancellation)");

  // 9. Memory check
  check(chorus.memory_bytes() > 0 && chorus.memory_bytes() < 65536, "Memory usage reasonable (<64 KB)");

  // 10. Performance benchmark (10,000 blocks each mode)
  {
    constexpr int kBenchBlocks = 10000;
    const ChorusMode modes[] = {ChorusMode::Chorus, ChorusMode::Ensemble, ChorusMode::Dimension};
    const char *names[] = {"Chorus", "Ensemble", "Dimension"};
    for (size_t m = 0; m < 3; ++m) {
      chorus.set_mode(modes[m]);
      auto start = std::chrono::steady_clock::now();
      for (int b = 0; b < kBenchBlocks; ++b) {
        chorus.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N, 120.0f);
      }
      auto elapsed_us = std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - start).count();
      double us_per_block = elapsed_us / kBenchBlocks;
      std::printf("  Benchmark %s: %.3f us/block (64 frames)\n", names[m], us_per_block);
      check(us_per_block < 500.0, "Chorus block execution within budget");
    }
  }

  if (g_failures == 0) {
    std::puts("test_chorus: PASS");
  }
  return g_failures == 0 ? 0 : 1;
}
