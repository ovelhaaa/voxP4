#include "vocal_fx.h"
#include "tempo.h"
#include <cmath>
#include <cstdio>
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
  VocalFxConfig cfg{};
  cfg.sample_rate = 44100.0f;
  cfg.block_size = 64;
  cfg.enable_delay = true;
  cfg.enable_reverb = false;
  cfg.enable_pitch_analysis = false;
  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "FAIL: vocal_fx_init failed\n");
    return 1;
  }

  // 1. Verify defaults
  check_near(vocal_fx_get_tempo_bpm(), 120.0f, 1e-4f, "Default Tempo is 120 BPM");
  check(!vocal_fx_get_delay_sync_enabled(), "DelaySyncEnabled is false by default");

  // Run a block to drain parameter queue
  float in[64] = {0}, out_l[64] = {0}, out_r[64] = {0};
  for (int i = 0; i < 64; ++i) in[i] = 0.1f * std::sin(0.05f * i);
  vocal_fx_process(in, out_l, out_r, 64);

  // 2. Test FREE mode parameter updates
  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 200.0f);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 400.0f);
  vocal_fx_process(in, out_l, out_r, 64);

  float applied_l = 0, applied_r = 0;
  check(vocal_fx_last_applied_parameter(VocalFxParameter::DelayLeftMs, &applied_l) && applied_l == 200.0f,
        "FREE mode DelayLeftMs applied");
  check(vocal_fx_last_applied_parameter(VocalFxParameter::DelayRightMs, &applied_r) && applied_r == 400.0f,
        "FREE mode DelayRightMs applied");

  // 3. Test SYNC mode activation
  vocal_fx_set_delay_sync_enabled(true);
  vocal_fx_set_delay_subdivisions(TempoSubdivision::Eighth, TempoSubdivision::DottedEighth);
  vocal_fx_process(in, out_l, out_r, 64);
  check(vocal_fx_get_delay_sync_enabled(), "DelaySyncEnabled is true");

  // At 120 BPM: 1/8 = 250 ms, 1/8D = 375 ms
  check_near(tempo_subdivision_ms(vocal_fx_get_tempo_bpm(), TempoSubdivision::Eighth), 250.0f, 1e-4f,
             "120 BPM 1/8 is 250 ms");
  check_near(tempo_subdivision_ms(vocal_fx_get_tempo_bpm(), TempoSubdivision::DottedEighth), 375.0f, 1e-4f,
             "120 BPM 1/8D is 375 ms");

  // 4. Test Tempo modulation: 120 -> 90 BPM
  vocal_fx_set_tempo_bpm(90.0f);
  vocal_fx_process(in, out_l, out_r, 64);
  check_near(vocal_fx_get_tempo_bpm(), 90.0f, 1e-4f, "Tempo updated to 90 BPM");
  // At 90 BPM: 1/8 = (60000 / 90) * 0.5 = 333.3333 ms, 1/8D = (60000 / 90) * 0.75 = 500 ms
  check_near(tempo_subdivision_ms(vocal_fx_get_tempo_bpm(), TempoSubdivision::Eighth), 333.3333f, 1e-3f,
             "90 BPM 1/8 is 333.33 ms");
  check_near(tempo_subdivision_ms(vocal_fx_get_tempo_bpm(), TempoSubdivision::DottedEighth), 500.0f, 1e-4f,
             "90 BPM 1/8D is 500 ms");

  // 5. Test 90 -> 140 BPM
  vocal_fx_set_tempo_bpm(140.0f);
  vocal_fx_process(in, out_l, out_r, 64);
  check_near(vocal_fx_get_tempo_bpm(), 140.0f, 1e-4f, "Tempo updated to 140 BPM");

  // 6. Test subdivision transitions: 1/8 -> 1/4 (left) and 1/8D -> 1/16 (right)
  vocal_fx_set_delay_subdivisions(TempoSubdivision::Quarter, TempoSubdivision::Sixteenth);
  vocal_fx_process(in, out_l, out_r, 64);

  // 7. Test small successive BPM updates (controller filtering simulation)
  const float bpm_stream[] = {120.0f, 120.5f, 121.0f, 121.5f, 122.0f, 121.8f, 122.2f, 122.5f, 123.0f};
  for (float bpm : bpm_stream) {
    vocal_fx_set_tempo_bpm(bpm);
    vocal_fx_process(in, out_l, out_r, 64);
    for (int s = 0; s < 64; ++s) {
      check(std::isfinite(out_l[s]) && std::isfinite(out_r[s]), "Output finite during BPM stream");
    }
  }

  // 8. Test return to FREE mode
  vocal_fx_set_delay_sync_enabled(false);
  vocal_fx_process(in, out_l, out_r, 64);
  check(!vocal_fx_get_delay_sync_enabled(), "DelaySyncEnabled returned to false");

  if (g_failures == 0) {
    std::puts("test_delay_sync: PASS");
  }
  return g_failures == 0 ? 0 : 1;
}
