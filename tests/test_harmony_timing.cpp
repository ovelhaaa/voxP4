#include "vocal_fx.h"
#include "pitch_analysis.h"
#include "td_psola.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

int find_energy_rise(const std::vector<float> &sig, float thresh_ratio = 0.05f, size_t win = 96) {
  std::vector<float> env(sig.size(), 0.0f);
  float peak = 0.0f;
  for (size_t i = 0; i < sig.size(); ++i) {
    float sum = 0.0f;
    size_t count = 0;
    for (size_t j = (i >= win ? i - win : 0); j <= std::min(sig.size() - 1, i + win); ++j) {
      sum += sig[j] * sig[j];
      ++count;
    }
    env[i] = std::sqrt(sum / count);
    if (env[i] > peak) peak = env[i];
  }
  if (peak < 1e-6f) return -1;
  const float thresh = peak * thresh_ratio;
  for (size_t i = 0; i < env.size(); ++i) {
    if (env[i] >= thresh) return static_cast<int>(i);
  }
  return -1;
}

int find_energy_fall(const std::vector<float> &sig, float thresh_ratio = 0.05f, size_t win = 96) {
  std::vector<float> env(sig.size(), 0.0f);
  float peak = 0.0f;
  for (size_t i = 0; i < sig.size(); ++i) {
    float sum = 0.0f;
    size_t count = 0;
    for (size_t j = (i >= win ? i - win : 0); j <= std::min(sig.size() - 1, i + win); ++j) {
      sum += sig[j] * sig[j];
      ++count;
    }
    env[i] = std::sqrt(sum / count);
    if (env[i] > peak) peak = env[i];
  }
  if (peak < 1e-6f) return -1;
  const float thresh = peak * thresh_ratio;
  for (int i = static_cast<int>(env.size()) - 1; i >= 0; --i) {
    if (env[i] >= thresh) return i;
  }
  return -1;
}

std::vector<float> process_signal(const std::vector<float> &input, float semitones, PsolaContinuityPolicy policy) {
  VocalFxConfig cfg{};
  cfg.sample_rate = kRate;
  cfg.block_size = 64;
  cfg.enable_gate = cfg.enable_compressor = cfg.enable_delay = cfg.enable_reverb = false;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = semitones;
  cfg.pitch_shift.wet = 1.0f;
  cfg.pitch_shift.continuity_policy = policy;
  cfg.pitch_shift.fallback_policy = HarmonyFallbackPolicy::Muted;
  cfg.isolate_pitch_shift_output = true;

  require(vocal_fx_init(cfg), "vocal_fx_init failed");

  const size_t frames = input.size();
  std::vector<float> out(frames, 0.0f), right(64, 0.0f);

  for (size_t i = 0; i < frames; i += 64) {
    size_t n = std::min<size_t>(64, frames - i);
    vocal_fx_process(input.data() + i, out.data() + i, right.data(), n);
    while (vocal_fx_run_pitch_analysis(8)) {}
  }
  return out;
}

void test_synthetic_tone_burst_onset() {
  std::puts("--- Test 1: Synthetic Tone Burst Onset Latency ---");
  const size_t pre = 4800; // 100 ms silence
  const size_t dur = 9600; // 200 ms tone
  const size_t post = 4800; // 100 ms silence
  const size_t total = pre + dur + post;

  std::vector<float> input(total, 0.0f);
  for (size_t i = 0; i < dur; ++i) {
    input[pre + i] = 0.5f * std::sin(2.0f * kPi * 440.0f * i / kRate);
  }
  // Smooth 2 ms ramp
  for (size_t i = 0; i < 96; ++i) {
    float r = 0.5f * (1.0f - std::cos(kPi * i / 96.0f));
    input[pre + i] *= r;
    input[pre + dur - 1 - i] *= r;
  }

  const auto out = process_signal(input, 7.0f, PsolaContinuityPolicy::OnsetContinuity);

  const uint64_t expected_onset = pre + 1536; // L_global = 1536 samples (32.0 ms)
  const int actual_onset = find_energy_rise(out, 0.05f);

  require(actual_onset > 0, "tone burst output must have energy rise");
  const float error_samples = static_cast<float>(actual_onset) - static_cast<float>(expected_onset);
  const float error_ms = error_samples * 1000.0f / kRate;

  std::printf("  Expected onset: %llu, Actual onset: %d, Error: %.2f ms\n",
              (unsigned long long)expected_onset, actual_onset, error_ms);
  require(std::fabs(error_ms) < 6.0f, "onset jitter must be < 6.0 ms after L_global compensation");
  std::puts("  PASS: Synthetic tone burst onset latency < 6 ms");
}

void test_short_segment_preservation() {
  std::puts("--- Test 2: Short Segment Preservation (50 ms) ---");
  const size_t pre = 4800;
  const size_t dur = 2400; // 50 ms
  const size_t post = 4800;
  const size_t total = pre + dur + post;

  std::vector<float> input(total, 0.0f);
  for (size_t i = 0; i < dur; ++i) {
    input[pre + i] = 0.5f * std::sin(2.0f * kPi * 220.0f * i / kRate);
  }
  for (size_t i = 0; i < 48; ++i) {
    float r = 0.5f * (1.0f - std::cos(kPi * i / 48.0f));
    input[pre + i] *= r;
    input[pre + dur - 1 - i] *= r;
  }

  const auto out = process_signal(input, 7.0f, PsolaContinuityPolicy::OnsetContinuity);

  const int actual_on = find_energy_rise(out, 0.05f);
  const int actual_off = find_energy_fall(out, 0.05f);

  require(actual_on > 0 && actual_off > actual_on, "50 ms segment must be detected in output");
  const float synth_dur_ms = (actual_off - actual_on) * 1000.0f / kRate;
  const float dur_ratio = synth_dur_ms / 50.0f;

  std::printf("  50 ms input -> Synth onset: %d, offset: %d, Duration: %.1f ms (ratio: %.3f)\n",
              actual_on, actual_off, synth_dur_ms, dur_ratio);
  require(synth_dur_ms >= 40.0f, "50 ms segment must not be dropped (duration >= 40 ms)");
  std::puts("  PASS: 50 ms segment preserved and synthesized");
}

void test_offset_continuity_and_no_clicks() {
  std::puts("--- Test 3: Offset Continuity & Smooth Decay ---");
  const size_t pre = 4800;
  const size_t dur = 7200; // 150 ms
  const size_t post = 4800;
  const size_t total = pre + dur + post;

  std::vector<float> input(total, 0.0f);
  for (size_t i = 0; i < dur; ++i) {
    input[pre + i] = 0.5f * std::sin(2.0f * kPi * 330.0f * i / kRate);
  }
  for (size_t i = 0; i < 96; ++i) {
    float r = 0.5f * (1.0f - std::cos(kPi * i / 96.0f));
    input[pre + i] *= r;
  }

  const auto out = process_signal(input, 7.0f, PsolaContinuityPolicy::OnsetContinuity);

  const uint64_t expected_offset = pre + dur + 1536;
  const int actual_off = find_energy_fall(out, 0.05f);

  require(actual_off > 0, "valid offset must be found");
  const float offset_err_ms = (actual_off - static_cast<int>(expected_offset)) * 1000.0f / kRate;

  float max_step = 0.0f;
  for (size_t i = std::max(0, actual_off - 500); i < std::min(out.size() - 1, static_cast<size_t>(actual_off + 500)); ++i) {
    float step = std::fabs(out[i + 1] - out[i]);
    if (step > max_step) max_step = step;
  }

  std::printf("  Expected offset: %llu, Actual offset: %d, Error: %.2f ms, Max step: %.4f\n",
              (unsigned long long)expected_offset, actual_off, offset_err_ms, max_step);
  require(max_step < 0.25f, "no discontinuity clicks at offset");
  require(offset_err_ms >= -5.0f && offset_err_ms <= 15.0f, "offset error within [-5 ms, +15 ms]");
  std::puts("  PASS: Offset continuity verified, zero clicks");
}

void test_f0_invariance() {
  std::puts("--- Test 4: Latency Determinism Across F0 (110 Hz - 440 Hz) ---");
  const float freqs[] = {110.0f, 220.0f, 330.0f, 440.0f};

  for (float f0 : freqs) {
    const size_t pre = 4800;
    const size_t dur = 9600;
    const size_t post = 4800;
    const size_t total = pre + dur + post;

    std::vector<float> input(total, 0.0f);
    for (size_t i = 0; i < dur; ++i) {
      input[pre + i] = 0.5f * std::sin(2.0f * kPi * f0 * i / kRate);
    }
    for (size_t i = 0; i < 96; ++i) {
      float r = 0.5f * (1.0f - std::cos(kPi * i / 96.0f));
      input[pre + i] *= r;
      input[pre + dur - 1 - i] *= r;
    }

    const auto out = process_signal(input, 4.0f, PsolaContinuityPolicy::OnsetContinuity);
    const uint64_t expected_onset = pre + 1536;
    const int actual_onset = find_energy_rise(out, 0.05f);

    require(actual_onset > 0, "onset must be found");
    const float err_ms = (actual_onset - static_cast<int>(expected_onset)) * 1000.0f / kRate;
    std::printf("  F0 = %5.1f Hz: Onset Error = %5.2f ms\n", f0, err_ms);
    require(std::fabs(err_ms) < 6.0f, "onset error must remain < 6.0 ms across all F0");
  }
  std::puts("  PASS: F0 determinism validated across 110-440 Hz");
}

} // namespace

int main() {
  std::puts("==========================================================");
  std::puts("=== Milestone 5.11: Harmony Timing & Continuity Tests ===");
  std::puts("==========================================================");

  test_synthetic_tone_burst_onset();
  test_short_segment_preservation();
  test_offset_continuity_and_no_clicks();
  test_f0_invariance();

  std::puts("\nAll harmony timing tests PASSED successfully!\n");
  return 0;
}
