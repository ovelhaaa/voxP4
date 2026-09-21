#include "tempo.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

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
  // Required cases from specification:
  // 120 BPM + 1/4  = 500 ms
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::Quarter), 500.0f, 1e-4f, "120 BPM + 1/4 = 500 ms");
  // 120 BPM + 1/8  = 250 ms
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::Eighth), 250.0f, 1e-4f, "120 BPM + 1/8 = 250 ms");
  // 120 BPM + 1/8D = 375 ms
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::DottedEighth), 375.0f, 1e-4f, "120 BPM + 1/8D = 375 ms");
  // 120 BPM + 1/8T ≈ 166.6667 ms
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::TripletEighth), 166.66667f, 1e-3f, "120 BPM + 1/8T ≈ 166.6667 ms");
  // 60 BPM + 1/4   = 1000 ms
  check_near(tempo_subdivision_ms(60.0f, TempoSubdivision::Quarter), 1000.0f, 1e-4f, "60 BPM + 1/4 = 1000 ms");
  // 240 BPM + 1/4  = 250 ms
  check_near(tempo_subdivision_ms(240.0f, TempoSubdivision::Quarter), 250.0f, 1e-4f, "240 BPM + 1/4 = 250 ms");

  // BPM bounds and clamping tests
  // Default = 120 BPM
  check_near(tempo_clamp_bpm(120.0f), 120.0f, 1e-4f, "tempo_clamp_bpm(120) == 120");
  check_near(tempo_clamp_bpm(30.0f), 30.0f, 1e-4f, "tempo_clamp_bpm(30) == 30");
  check_near(tempo_clamp_bpm(300.0f), 300.0f, 1e-4f, "tempo_clamp_bpm(300) == 300");

  // Below min (e.g. 10 BPM -> clamps to 30)
  check_near(tempo_clamp_bpm(10.0f), 30.0f, 1e-4f, "tempo_clamp_bpm(10) == 30");
  // Above max (e.g. 500 BPM -> clamps to 300)
  check_near(tempo_clamp_bpm(500.0f), 300.0f, 1e-4f, "tempo_clamp_bpm(500) == 300");

  // NaN / Inf protection -> fallback to TEMPO_BPM_DEFAULT (120)
  const float qnan = std::numeric_limits<float>::quiet_NaN();
  const float pinf = std::numeric_limits<float>::infinity();
  const float ninf = -std::numeric_limits<float>::infinity();
  check_near(tempo_clamp_bpm(qnan), 120.0f, 1e-4f, "tempo_clamp_bpm(NaN) == 120");
  check_near(tempo_clamp_bpm(pinf), 120.0f, 1e-4f, "tempo_clamp_bpm(+Inf) == 120");
  check_near(tempo_clamp_bpm(ninf), 120.0f, 1e-4f, "tempo_clamp_bpm(-Inf) == 120");

  check_near(tempo_subdivision_ms(qnan, TempoSubdivision::Quarter), 500.0f, 1e-4f, "tempo_subdivision_ms(NaN) uses 120 BPM");
  check_near(tempo_subdivision_ms(pinf, TempoSubdivision::Quarter), 500.0f, 1e-4f, "tempo_subdivision_ms(+Inf) uses 120 BPM");

  // Test across all defined ratios at 120 BPM
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::Whole), 2000.0f, 1e-4f, "1/1 = 2000 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::Half), 1000.0f, 1e-4f, "1/2 = 1000 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::Sixteenth), 125.0f, 1e-4f, "1/16 = 125 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::ThirtySecond), 62.5f, 1e-4f, "1/32 = 62.5 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::DottedHalf), 1500.0f, 1e-4f, "1/2D = 1500 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::DottedQuarter), 750.0f, 1e-4f, "1/4D = 750 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::DottedSixteenth), 187.5f, 1e-4f, "1/16D = 187.5 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::TripletQuarter), 333.33334f, 1e-3f, "1/4T ≈ 333.3333 ms");
  check_near(tempo_subdivision_ms(120.0f, TempoSubdivision::TripletSixteenth), 83.33333f, 1e-3f, "1/16T ≈ 83.3333 ms");

  // Invalid subdivision enum test
  auto invalid_sub = static_cast<TempoSubdivision>(99);
  check_near(tempo_subdivision_ms(120.0f, invalid_sub), 500.0f, 1e-4f, "invalid subdivision falls back to quarter (500 ms)");

  if (g_failures == 0) {
    std::puts("test_tempo: PASS");
  }
  return g_failures == 0 ? 0 : 1;
}
