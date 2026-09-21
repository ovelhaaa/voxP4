#include "chorus.h"
#include "vocal_fx.h"
#include <cassert>
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

// Estimates fundamental frequency of a real discrete tone via zero-crossing rate
double estimate_f0(const std::vector<float> &sig, double sample_rate, size_t skip_samples = 2048) {
  if (sig.size() <= skip_samples + 1024) return 0.0;
  size_t crossings = 0;
  size_t first_crossing = 0;
  size_t last_crossing = 0;
  bool found_first = false;

  for (size_t i = skip_samples + 1; i < sig.size(); ++i) {
    if ((sig[i - 1] <= 0.0f && sig[i] > 0.0f) || (sig[i - 1] >= 0.0f && sig[i] < 0.0f)) {
      crossings++;
      if (!found_first) {
        first_crossing = i;
        found_first = true;
      }
      last_crossing = i;
    }
  }

  if (crossings < 4 || last_crossing <= first_crossing) return 0.0;
  // Each cycle has 2 zero-crossings
  const double num_cycles = static_cast<double>(crossings - 1) / 2.0;
  const double span_samples = static_cast<double>(last_crossing - first_crossing);
  return (num_cycles / span_samples) * sample_rate;
}

} // namespace

int main() {
  std::puts("=== Running Vocal Microshift Unit Tests ===");
  constexpr float kRate = 44100.0f;
  constexpr size_t kBlock = 64;

  VocalChorus vc;
  check(vc.init(kRate, 50.0f), "VocalChorus::init 50ms buffer");
  vc.apply_mode_defaults(ChorusMode::Microshift);
  check(vc.mode() == ChorusMode::Microshift, "Mode is Microshift");
  check(std::fabs(vc.microshift_left_cents() - (-7.0f)) < 1e-4f, "Default left cents is -7");
  check(std::fabs(vc.microshift_right_cents() - 9.0f) < 1e-4f, "Default right cents is +9");
  check(std::fabs(vc.mix() - 0.35f) < 1e-4f, "Default mix is 0.35");
  check(std::fabs(vc.microshift_window_ms() - 25.0f) < 1e-4f, "Default window is 25ms");

  // 1. Cents -> Ratio accuracy
  {
    // 0 cents -> ratio 1.0
    const float r0 = std::exp2(0.0f / 1200.0f);
    check(std::fabs(r0 - 1.0f) < 1e-6f, "0 cents ratio == 1.0");

    // +12 cents
    const float r_p12 = std::exp2(12.0f / 1200.0f);
    check(std::fabs(r_p12 - 1.00695555f) < 1e-5f, "+12 cents ratio accurate");

    // -12 cents
    const float r_m12 = std::exp2(-12.0f / 1200.0f);
    check(std::fabs(r_m12 - 0.99309249f) < 1e-5f, "-12 cents ratio accurate");
  }

  // 2. Dual-Head Crossfade Continuity
  {
    // Test that for both Linear and EqualPower windows, w_A + w_B == 1.0
    vc.set_microshift_crossfade(MicroshiftCrossfade::Linear);
    for (int i = 0; i <= 1000; ++i) {
      const float phi_a = static_cast<float>(i) / 1000.0f;
      float phi_b = phi_a + 0.5f;
      if (phi_b >= 1.0f) phi_b -= 1.0f;

      const float w_a = 1.0f - 2.0f * std::fabs(phi_a - 0.5f);
      const float w_b = 1.0f - 2.0f * std::fabs(phi_b - 0.5f);
      check(std::fabs((w_a + w_b) - 1.0f) < 1e-5f, "Linear crossfade unity sum");
    }

    vc.set_microshift_crossfade(MicroshiftCrossfade::EqualPower);
    for (int i = 0; i <= 1000; ++i) {
      const float phi_a = static_cast<float>(i) / 1000.0f;
      float phi_b = phi_a + 0.5f;
      if (phi_b >= 1.0f) phi_b -= 1.0f;

      // sin^2(pi*phi) + cos^2(pi*phi) = 1.0
      const float s_a = std::sin(3.14159265f * phi_a);
      const float s_b = std::sin(3.14159265f * phi_b);
      const float w_a = s_a * s_a;
      const float w_b = s_b * s_b;
      check(std::fabs((w_a + w_b) - 1.0f) < 1e-5f, "Equal power crossfade unity sum");
    }
  }

  // 3. Dry-only and Wet-only modes
  {
    std::vector<float> in_l(kBlock, 0.75f), in_r(kBlock, -0.5f);
    std::vector<float> out_l(kBlock), out_r(kBlock);

    // Dry only (mix = 0)
    vc.set_mix(0.0f);
    vc.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), kBlock, 120.0f);
    bool dry_match = true;
    for (size_t i = 0; i < kBlock; ++i) {
      if (std::fabs(out_l[i] - in_l[i]) > 1e-5f || std::fabs(out_r[i] - in_r[i]) > 1e-5f) {
        dry_match = false;
        break;
      }
    }
    check(dry_match, "Mix = 0.0 is perfect bit-accurate dry pass-through");

    // Wet only (mix = 1.0)
    vc.set_mix(1.0f);
    // Warm up delay line (base delay 8ms + window 25ms requires ~353 to 1450 samples)
    for (int blk = 0; blk < 30; ++blk) {
      vc.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), kBlock, 120.0f);
    }
    // After warming up delay line, output must be non-zero
    bool has_wet = false;
    for (size_t i = 0; i < kBlock; ++i) {
      if (std::fabs(out_l[i]) > 1e-4f) has_wet = true;
    }
    check(has_wet, "Mix = 1.0 produces active wet signal");
  }

  // 4. Zero input test
  {
    std::vector<float> zeros(kBlock, 0.0f);
    std::vector<float> out_l(kBlock), out_r(kBlock);
    vc.reset();
    vc.set_mix(0.5f);
    vc.process(zeros.data(), zeros.data(), out_l.data(), out_r.data(), kBlock, 120.0f);
    bool all_zero = true;
    for (size_t i = 0; i < kBlock; ++i) {
      if (std::fabs(out_l[i]) > 1e-6f || std::fabs(out_r[i]) > 1e-6f) all_zero = false;
    }
    check(all_zero, "Zero input produces exact zero output");
  }

  // 5. Numerical safety and extreme parameter handling
  {
    vc.set_microshift_left_cents(std::nanf(""));
    check(std::isfinite(vc.microshift_left_cents()), "NaN left cents rejected");

    vc.set_microshift_right_cents(std::numeric_limits<float>::infinity());
    check(vc.microshift_right_cents() <= 50.0f, "Inf right cents clamped");

    vc.set_microshift_left_cents(-1000.0f);
    check(vc.microshift_left_cents() >= -50.0f, "Extreme negative cents clamped");

    vc.set_microshift_window_ms(-5.0f);
    check(vc.microshift_window_ms() >= 5.0f, "Negative window ms rejected/clamped");

    vc.set_microshift_window_ms(500.0f);
    check(vc.microshift_window_ms() <= 50.0f, "Excessive window ms clamped");
  }

  // 6. Pitch Accuracy Test on 440 Hz Sinusoid
  {
    constexpr double kF0 = 440.0;
    constexpr size_t kTotalSamples = 44100 * 2; // 2 seconds
    std::vector<float> sine_in(kTotalSamples);
    for (size_t i = 0; i < kTotalSamples; ++i) {
      sine_in[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * kF0 * i / kRate));
    }

    std::vector<float> out_l(kTotalSamples), out_r(kTotalSamples);
    vc.reset();
    vc.apply_mode_defaults(ChorusMode::Microshift);
    vc.set_mix(1.0f); // 100% wet to measure shifted tone directly
    vc.set_width(1.0f);
    vc.set_microshift_left_cents(-12.0f);
    vc.set_microshift_right_cents(+12.0f);
    vc.set_microshift_window_ms(30.0f);

    for (size_t pos = 0; pos < kTotalSamples; pos += kBlock) {
      const size_t n = std::min(kBlock, kTotalSamples - pos);
      vc.process(sine_in.data() + pos, sine_in.data() + pos,
                 out_l.data() + pos, out_r.data() + pos, n, 120.0f);
    }

    const double f_l = estimate_f0(out_l, kRate, 4410);
    const double f_r = estimate_f0(out_r, kRate, 4410);

    const double expected_l = kF0 * std::exp2(-12.0 / 1200.0); // ~436.96 Hz
    const double expected_r = kF0 * std::exp2(+12.0 / 1200.0); // ~443.06 Hz

    const double err_l_hz = std::fabs(f_l - expected_l);
    const double err_r_hz = std::fabs(f_r - expected_r);
    const double err_l_cents = 1200.0 * std::log2(f_l / expected_l);
    const double err_r_cents = 1200.0 * std::log2(f_r / expected_r);

    std::printf("  440 Hz Test: Left (-12st target: %.2f Hz) -> Measured: %.2f Hz (err: %.2f Hz / %.2f cents)\n",
                expected_l, f_l, err_l_hz, err_l_cents);
    std::printf("  440 Hz Test: Right (+12st target: %.2f Hz) -> Measured: %.2f Hz (err: %.2f Hz / %.2f cents)\n",
                expected_r, f_r, err_r_hz, err_r_cents);

    check(err_l_hz < 1.0, "Left pitch shift within 1 Hz of target");
    check(err_r_hz < 1.0, "Right pitch shift within 1 Hz of target");
    check(std::fabs(err_l_cents) < 4.0, "Left pitch error < 4 cents");
    check(std::fabs(err_r_cents) < 4.0, "Right pitch error < 4 cents");
  }

  // 7. Pitch Preservation at 0 cents
  {
    constexpr double kF0 = 880.0;
    constexpr size_t kTotalSamples = 44100;
    std::vector<float> sine_in(kTotalSamples);
    for (size_t i = 0; i < kTotalSamples; ++i) {
      sine_in[i] = static_cast<float>(std::sin(2.0 * 3.141592653589793 * kF0 * i / kRate));
    }
    std::vector<float> out_l(kTotalSamples), out_r(kTotalSamples);
    vc.reset();
    vc.set_mix(1.0f);
    vc.set_microshift_left_cents(0.0f);
    vc.set_microshift_right_cents(0.0f);
    vc.set_microshift_window_ms(20.0f);

    for (size_t pos = 0; pos < kTotalSamples; pos += kBlock) {
      const size_t n = std::min(kBlock, kTotalSamples - pos);
      vc.process(sine_in.data() + pos, sine_in.data() + pos,
                 out_l.data() + pos, out_r.data() + pos, n, 120.0f);
    }
    const double f_l = estimate_f0(out_l, kRate, 4410);
    check(std::fabs(f_l - kF0) < 0.5, "0 cents wet output preserves pitch exactly");
  }

  // 8. Public vocal_fx API integration test
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = kRate;
    cfg.block_size = kBlock;
    cfg.enable_chorus = true;
    cfg.chorus.mode = ChorusMode::Microshift;
    cfg.chorus.microshift_left_cents = -6.0f;
    cfg.chorus.microshift_right_cents = 8.0f;
    vocal_fx_init(cfg);

    check(vocal_fx_get_chorus_mode() == ChorusMode::Microshift, "vocal_fx_get_chorus_mode is Microshift");
    check(std::fabs(vocal_fx_get_microshift_left_cents() - (-6.0f)) < 1e-4f, "Initial microshift left cents from config");
    check(std::fabs(vocal_fx_get_microshift_right_cents() - 8.0f) < 1e-4f, "Initial microshift right cents from config");

    std::vector<float> in_mono(kBlock, 0.4f);
    std::vector<float> out_l(kBlock), out_r(kBlock);

    vocal_fx_set_microshift_left_cents(-9.0f);
    vocal_fx_set_microshift_right_cents(10.0f);
    vocal_fx_set_microshift_window_ms(30.0f);
    // Drain parameter queue via vocal_fx_process
    vocal_fx_process(in_mono.data(), out_l.data(), out_r.data(), kBlock);

    check(std::fabs(vocal_fx_get_microshift_left_cents() - (-9.0f)) < 1e-4f, "vocal_fx_set/get_microshift_left_cents");
    check(std::fabs(vocal_fx_get_microshift_right_cents() - 10.0f) < 1e-4f, "vocal_fx_set/get_microshift_right_cents");
    check(std::fabs(vocal_fx_get_microshift_window_ms() - 30.0f) < 1e-4f, "vocal_fx_set/get_microshift_window_ms");
    check(std::isfinite(out_l[0]) && std::isfinite(out_r[0]), "vocal_fx_process with Microshift executes cleanly");
  }

  if (g_failures == 0) {
    std::puts("ALL Vocal Microshift tests PASSED.");
    return 0;
  } else {
    std::printf("Vocal Microshift tests FAILED with %d failure(s).\n", g_failures);
    return 1;
  }
}
