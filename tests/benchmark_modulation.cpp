#include "chorus.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

void benchmark_mode(const char *name, ChorusMode mode, ChorusInterpolation interp,
                    MicroshiftCrossfade xfade, float rate, float depth, float mix) {
  constexpr float kRate = 44100.0f;
  constexpr size_t kBlock = 64;
  constexpr int kWarmup = 2000;
  constexpr int kIterations = 100000;

  VocalChorus vc;
  vc.init(kRate, 50.0f);
  vc.set_mode(mode);
  vc.set_interpolation(interp);
  vc.set_microshift_crossfade(xfade);
  vc.set_rate_hz(rate);
  vc.set_depth_ms(depth);
  vc.set_mix(mix);
  vc.set_microshift_cents(-7.0f, 9.0f);
  vc.set_microshift_window_ms(25.0f);

  std::vector<float> in_l(kBlock), in_r(kBlock), out_l(kBlock), out_r(kBlock);
  for (size_t i = 0; i < kBlock; ++i) {
    in_l[i] = 0.5f * std::sin(2.0f * 3.14159265f * 330.0f * i / kRate);
    in_r[i] = in_l[i];
  }

  for (int i = 0; i < kWarmup; ++i) {
    vc.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), kBlock, 120.0f);
  }

  std::vector<double> timings(kIterations);
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < kIterations; ++i) {
    auto start = std::chrono::steady_clock::now();
    vc.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), kBlock, 120.0f);
    auto end = std::chrono::steady_clock::now();
    timings[i] = std::chrono::duration<double, std::micro>(end - start).count();
  }
  auto t1 = std::chrono::steady_clock::now();
  double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

  std::sort(timings.begin(), timings.end());
  double mean_us = total_us / kIterations;
  double p50_us = timings[kIterations * 50 / 100];
  double p95_us = timings[kIterations * 95 / 100];
  double p99_us = timings[kIterations * 99 / 100];
  double max_us = timings.back();
  double ns_per_sample = (mean_us * 1000.0) / kBlock;

  std::printf("%-32s | %7.3f us | %7.3f us | %7.3f us | %7.3f us | %7.1f ns/samp\n",
              name, mean_us, p50_us, p99_us, max_us, ns_per_sample);
}

int main() {
  std::printf("=================================================================================================\n");
  std::printf("%-32s | %-10s | %-10s | %-10s | %-10s | %-12s\n",
              "Modulation Mode", "Mean", "P50", "P99", "Max", "Cost");
  std::printf("=================================================================================================\n");

  benchmark_mode("Bypass (Mix = 0.0)", ChorusMode::Chorus, ChorusInterpolation::Linear,
                 MicroshiftCrossfade::EqualPower, 1.2f, 0.35f, 0.0f);
  benchmark_mode("Chorus (Linear)", ChorusMode::Chorus, ChorusInterpolation::Linear,
                 MicroshiftCrossfade::EqualPower, 1.2f, 0.35f, 0.35f);
  benchmark_mode("Chorus (Hermite)", ChorusMode::Chorus, ChorusInterpolation::CubicHermite,
                 MicroshiftCrossfade::EqualPower, 1.2f, 0.35f, 0.35f);
  benchmark_mode("Ensemble (Multi-tap)", ChorusMode::Ensemble, ChorusInterpolation::Linear,
                 MicroshiftCrossfade::EqualPower, 0.8f, 0.5f, 0.35f);
  benchmark_mode("Dimension (Dual-LFO)", ChorusMode::Dimension, ChorusInterpolation::CubicHermite,
                 MicroshiftCrossfade::EqualPower, 0.5f, 0.4f, 0.35f);
  benchmark_mode("Microshift (Linear Xfade)", ChorusMode::Microshift, ChorusInterpolation::Linear,
                 MicroshiftCrossfade::Linear, 0.0f, 0.0f, 0.35f);
  benchmark_mode("Microshift (EqualPower)", ChorusMode::Microshift, ChorusInterpolation::Linear,
                 MicroshiftCrossfade::EqualPower, 0.0f, 0.0f, 0.35f);
  benchmark_mode("Microshift (Hermite Interp)", ChorusMode::Microshift, ChorusInterpolation::CubicHermite,
                 MicroshiftCrossfade::EqualPower, 0.0f, 0.0f, 0.35f);

  std::printf("=================================================================================================\n");
  return 0;
}
