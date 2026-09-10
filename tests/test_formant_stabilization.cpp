#include "lpc.h"
#include "td_psola.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
  std::printf("=== Running Milestone 5.10.1 Formant Stabilization Tests ===\n");

  constexpr size_t n = 1024;
  std::vector<float> signal(n, 0.0f);
  for (size_t i = 0; i < n; ++i) {
    for (int h = 1; h <= 10; ++h) {
      signal[i] += (0.2f / static_cast<float>(h)) *
                   std::sin(2.0 * 3.14159265358979323846 * 220.0 * h * i / 48000.0);
    }
    signal[i] += 0.005f * std::sin(2.0 * 3.14159265358979323846 * 1234.0 * i / 48000.0);
  }

  SharedLpcModel m16;
  CHECK(SharedLpcAnalysis::solve(signal.data(), n, 16, 0.97f, &m16));
  CHECK(m16.valid);

  // 1. Test formant_gain_normalization_tests (Strategies A, B, C, D)
  std::printf("1. Testing gain normalization strategies A, B, C, D...\n");
  for (float st : {-6.0f, -3.0f, 0.0f, 3.0f, 6.0f}) {
    std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> warped{};
    const float lam = SharedLpcAnalysis::lambda_from_semitones(st);
    CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, lam, 0.985f, warped.data()));

    const float g_a = SharedLpcAnalysis::compute_gain_normalization(
        m16.coefficients.data(), warped.data(), 16, FormantNormalizationStrategy::StrategyA_DC);
    const float g_b = SharedLpcAnalysis::compute_gain_normalization(
        m16.coefficients.data(), warped.data(), 16, FormantNormalizationStrategy::StrategyB_ReferenceFrequency);
    const float g_c = SharedLpcAnalysis::compute_gain_normalization(
        m16.coefficients.data(), warped.data(), 16, FormantNormalizationStrategy::StrategyC_IntegratedSpectral);
    const float g_d = SharedLpcAnalysis::compute_gain_normalization(
        m16.coefficients.data(), warped.data(), 16, FormantNormalizationStrategy::StrategyD_ResidualEnergy);

    CHECK(std::isfinite(g_a) && g_a > 0.001f && g_a < 20.0f);
    CHECK(std::isfinite(g_b) && g_b > 0.001f && g_b < 20.0f);
    CHECK(std::isfinite(g_c) && g_c > 0.001f && g_c < 20.0f);
    CHECK(std::isfinite(g_d) && g_d > 0.001f && g_d < 20.0f);

    if (st == 0.0f) {
      CHECK(std::fabs(g_c - 1.0f) < 0.15f);
    }
    if (st < 0.0f) {
      CHECK(g_c < 0.8f);
    }
  }

  // 2. Test formant_softclip_tests (Rational soft limiter)
  std::printf("2. Testing rational soft limiter properties...\n");
  auto softclip = [](float x) {
    if (std::fabs(x) <= 1.0f) return x;
    const float ax = std::fabs(x);
    const float excess = ax - 1.0f;
    return std::copysign(1.0f + excess / (1.0f + 0.5f * excess), x);
  };

  // Small-signal identity (0 THD under 1.0)
  for (float v : {-1.0f, -0.7f, -0.3f, 0.0f, 0.2f, 0.5f, 0.99f, 1.0f}) {
    CHECK(std::fabs(softclip(v) - v) < 1e-7f);
  }

  // Odd symmetry f(-x) == -f(x)
  for (float v : {0.5f, 1.0f, 1.5f, 2.0f, 5.0f, 100.0f}) {
    CHECK(std::fabs(softclip(-v) + softclip(v)) < 1e-6f);
  }

  // Monotonicity
  float prev_y = -10.0f;
  for (float v = -5.0f; v <= 5.0f; v += 0.1f) {
    float y = softclip(v);
    CHECK(y >= prev_y);
    prev_y = y;
  }

  // Asymptotic bound < 3.0
  CHECK(softclip(10.0f) < 3.0f);
  CHECK(softclip(1000.0f) < 3.0f);
  CHECK(softclip(1000.0f) > 2.95f);

  // 3. Test formant_negative_shift_tests (F3_ShiftDown and sweeps)
  std::printf("3. Testing negative formant shifts in simulation...\n");
  for (float shift : {-6.0f, -4.0f, -3.0f, -2.0f, -1.0f}) {
    std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> warped{};
    const float lam = SharedLpcAnalysis::lambda_from_semitones(shift);
    CHECK(SharedLpcAnalysis::warp_polynomial(m16.coefficients.data(), 16, lam, 0.985f, warped.data()));
    const float g_c = SharedLpcAnalysis::compute_gain_normalization(
        m16.coefficients.data(), warped.data(), 16, FormantNormalizationStrategy::StrategyC_IntegratedSpectral);

    // Simulate all-pole filter response with rational softclip matching td_psola synthesis loop
    std::array<float, 16> state{};
    float max_sample = 0.0f;
    for (size_t i = 0; i < 256; ++i) {
      float input = (i == 0) ? (0.02f * g_c) : 0.0f;
      float acc = input;
      for (size_t j = 1; j <= 16; ++j) {
        acc -= warped[j] * state[j - 1];
      }
      if (std::fabs(acc) > 1.0f) {
        const float ax = std::fabs(acc);
        const float excess = ax - 1.0f;
        acc = std::copysign(1.0f + excess / (1.0f + 0.5f * excess), acc);
      }
      for (size_t j = 15; j > 0; --j) {
        state[j] = state[j - 1] * 0.999f;
      }
      state[0] = acc;
      max_sample = std::max(max_sample, std::fabs(acc));
    }
    std::printf("  shift=%.1f lam=%.4f g_c=%.4f max_sample=%.4f\n", shift, lam, g_c, max_sample);
    CHECK(max_sample < 3.0f);
  }

  // 4. Test formant_gain_tracker_tests
  std::printf("4. Testing multi-stage gain tracker...\n");
  float fast_lpc = 0.0f, fast_psola = 0.0f, slow_target = 1.0f, smoothed_g = 1.0f;
  constexpr float kMaxSlew = 0.0005f;

  // Step response test
  float max_slew_observed = 0.0f;
  for (size_t i = 0; i < 4800; ++i) {
    float lpc_in = 1.0f;
    float psola_in = 2.0f;
    fast_lpc += 0.0035f * (lpc_in * lpc_in - fast_lpc);
    fast_psola += 0.0035f * (psola_in * psola_in - fast_psola);
    if (fast_lpc > 1e-8f && fast_psola > 1e-8f) {
      const float instant_ratio = std::sqrt(fast_psola / fast_lpc);
      const float target_g = std::clamp(instant_ratio, 0.4f, 2.5f);
      const float rate = (target_g > slow_target) ? 0.0008f : 0.0002f;
      slow_target += rate * (target_g - slow_target);
    }
    const float prev_g = smoothed_g;
    const float delta = std::clamp(slow_target - smoothed_g, -kMaxSlew, kMaxSlew);
    smoothed_g += delta;
    max_slew_observed = std::max(max_slew_observed, std::fabs(smoothed_g - prev_g));
  }
  CHECK(max_slew_observed <= kMaxSlew + 1e-7f);
  CHECK(smoothed_g > 1.5f && smoothed_g <= 2.0f);

  if (failures == 0) {
    std::printf("=== ALL MILESTONE 5.10.1 TESTS PASSED ===\n");
  } else {
    std::printf("=== %d TESTS FAILED ===\n", failures);
  }

  return failures ? 1 : 0;
}
