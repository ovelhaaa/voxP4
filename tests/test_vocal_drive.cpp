#include "vocal_drive.h"
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
  VocalDrive drive;
  const float sample_rate = 44100.0f;
  check(drive.init(sample_rate), "VocalDrive init succeeds");

  constexpr size_t N = 64;
  std::vector<float> in_l(N), in_r(N);
  std::vector<float> out_l(N), out_r(N);

  // 1. Dry pass-through with mix = 0
  drive.set_mix(0.0f);
  for (size_t i = 0; i < N; ++i) {
    in_l[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * i / sample_rate);
    in_r[i] = -in_l[i];
  }
  drive.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N);
  for (size_t i = 0; i < N; ++i) {
    check_near(out_l[i], in_l[i], 1e-6f, "Mix = 0 passes left channel bit-exact");
    check_near(out_r[i], in_r[i], 1e-6f, "Mix = 0 passes right channel bit-exact");
  }

  // 2. Harmonic growth with increasing drive (Warm and Overdrive)
  drive.set_mix(1.0f);
  drive.set_tone(1.0f); // fully open tone
  drive.set_mode(DriveMode::Warm);

  auto measure_thd_energy = [&](DriveMode mode, float drive_amt) -> float {
    drive.reset();
    drive.set_mode(mode);
    drive.set_drive(drive_amt);
    // Feed 1 kHz sine
    constexpr size_t kLen = 2048;
    std::vector<float> buf_in(kLen), buf_out_l(kLen), buf_out_r(kLen);
    for (size_t i = 0; i < kLen; ++i) {
      buf_in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * i / sample_rate);
    }
    drive.process(buf_in.data(), buf_in.data(), buf_out_l.data(), buf_out_r.data(), kLen);

    // Compute discrete Fourier coefficient at fundamental (1 kHz) and harmonics (2 kHz, 3 kHz)
    double f1_re = 0.0, f1_im = 0.0;
    double f2_re = 0.0, f2_im = 0.0;
    double f3_re = 0.0, f3_im = 0.0;
    // Skip initial transient
    const size_t start = 512;
    const size_t count = kLen - start;
    for (size_t i = start; i < kLen; ++i) {
      double t = static_cast<double>(i) / sample_rate;
      double s = buf_out_l[i];
      f1_re += s * std::cos(2.0 * 3.14159265 * 1000.0 * t);
      f1_im += s * std::sin(2.0 * 3.14159265 * 1000.0 * t);
      f2_re += s * std::cos(2.0 * 3.14159265 * 2000.0 * t);
      f2_im += s * std::sin(2.0 * 3.14159265 * 2000.0 * t);
      f3_re += s * std::cos(2.0 * 3.14159265 * 3000.0 * t);
      f3_im += s * std::sin(2.0 * 3.14159265 * 3000.0 * t);
    }
    double mag_f2 = std::hypot(f2_re, f2_im) / count;
    double mag_f3 = std::hypot(f3_re, f3_im) / count;
    return static_cast<float>(mag_f2 + mag_f3);
  };

  float harmonics_low = measure_thd_energy(DriveMode::Warm, 0.1f);
  float harmonics_high = measure_thd_energy(DriveMode::Warm, 0.9f);
  check(harmonics_high > harmonics_low * 1.5f, "Warm mode: harmonic energy increases with drive amount");

  float od_low = measure_thd_energy(DriveMode::Overdrive, 0.1f);
  float od_high = measure_thd_energy(DriveMode::Overdrive, 0.9f);
  check(od_high > od_low * 1.5f, "Overdrive mode: harmonic energy increases with drive amount");

  // 3. Megaphone bandpass frequency response
  // Feed 100 Hz (sub-bass), 1000 Hz (mid), and 10000 Hz (high) through Megaphone
  auto measure_response = [&](float freq) -> float {
    drive.reset();
    drive.set_mode(DriveMode::Megaphone);
    drive.set_drive(0.0f);
    drive.set_mix(1.0f);
    drive.set_tone(1.0f);
    constexpr size_t kLen = 2048;
    std::vector<float> buf_in(kLen), buf_out_l(kLen), buf_out_r(kLen);
    for (size_t i = 0; i < kLen; ++i) {
      buf_in[i] = 0.2f * std::sin(2.0f * 3.14159265f * freq * i / sample_rate);
    }
    drive.process(buf_in.data(), buf_in.data(), buf_out_l.data(), buf_out_r.data(), kLen);
    double sum_sq = 0.0;
    for (size_t i = 1024; i < kLen; ++i) {
      sum_sq += buf_out_l[i] * buf_out_l[i];
    }
    return static_cast<float>(std::sqrt(sum_sq / 1024.0));
  };

  float resp_100hz = measure_response(100.0f);
  float resp_1000hz = measure_response(1000.0f);
  float resp_10000hz = measure_response(10000.0f);
  check(resp_1000hz > resp_100hz * 2.0f, "Megaphone rolls off low frequencies (100 Hz vs 1000 Hz)");
  check(resp_1000hz > resp_10000hz * 2.0f, "Megaphone rolls off high frequencies (10000 Hz vs 1000 Hz)");

  // 4. DC Offset elimination
  // Feed a signal with massive DC bias and verify DC blocker eliminates it
  {
    drive.reset();
    drive.set_mode(DriveMode::Warm);
    drive.set_drive(0.8f);
    drive.set_mix(1.0f);
    constexpr size_t kLen = 16000;
    std::vector<float> buf_in(kLen), buf_out_l(kLen), buf_out_r(kLen);
    // 441 Hz at 44100 Hz has period exactly 100 samples
    for (size_t i = 0; i < kLen; ++i) {
      buf_in[i] = 0.3f + 0.3f * std::sin(2.0f * 3.14159265f * 441.0f * i / sample_rate);
    }
    drive.process(buf_in.data(), buf_in.data(), buf_out_l.data(), buf_out_r.data(), kLen);
    double dc_sum = 0.0;
    // Measure across exactly 20 cycles (2000 samples) after settling
    for (size_t i = 12000; i < 14000; ++i) {
      dc_sum += buf_out_l[i];
    }
    float dc_mean = static_cast<float>(std::fabs(dc_sum / 2000.0));
    check(dc_mean < 0.001f, "DC blocker eliminates DC bias after non-linearity (<0.001)");
  }

  // 5. Numerical stability under silence and extremes
  {
    drive.reset();
    std::vector<float> silent(N, 0.0f);
    drive.process(silent.data(), silent.data(), out_l.data(), out_r.data(), N);
    for (size_t i = 0; i < N; ++i) {
      check(std::isfinite(out_l[i]) && std::fabs(out_l[i]) < 1e-6f, "Zero input yields zero output");
    }
  }

  // 6. Execution benchmark (10,000 blocks each mode)
  {
    constexpr int kBenchBlocks = 10000;
    const DriveMode modes[] = {DriveMode::Warm, DriveMode::Overdrive, DriveMode::Megaphone};
    const char *names[] = {"Warm", "Overdrive", "Megaphone"};
    for (size_t m = 0; m < 3; ++m) {
      drive.set_mode(modes[m]);
      auto start = std::chrono::steady_clock::now();
      for (int b = 0; b < kBenchBlocks; ++b) {
        drive.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), N);
      }
      auto elapsed_us = std::chrono::duration<double, std::micro>(
          std::chrono::steady_clock::now() - start).count();
      double us_per_block = elapsed_us / kBenchBlocks;
      std::printf("  Benchmark %s: %.3f us/block (64 frames)\n", names[m], us_per_block);
      check(us_per_block < 500.0, "Drive block execution within real-time budget");
    }
  }

  if (g_failures == 0) {
    std::puts("test_vocal_drive: PASS");
  }
  return g_failures == 0 ? 0 : 1;
}
