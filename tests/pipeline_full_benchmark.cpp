// HOST FUNCTIONAL / COMPARATIVE BENCHMARK
//
// NOTE: This benchmark runs on the x86_64 host CPU using std::chrono.
// It measures algorithmic DSP throughput of the active pipeline (Gate, Compressor,
// Drive, Chorus, Delay, Reverb, Limiter) in host user space without the asynchronous
// FreeRTOS Core 1 pitch worker task. Because no pitch candidate is generated,
// TD-PSOLA operates in quiescent/fallback mode (0 grains scheduled/rendered).
// Host timings serve as regression/comparative indicators only and MUST NOT be
// conflated with ESP32-P4 hardware real-time execution times (~341-708 us).
// Physical qualification must always be performed on real ESP32-P4 silicon.

#include "vocal_fx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

void run_soak(float sample_rate, const char *label) {
  VocalFxConfig config{};
  config.sample_rate = sample_rate;
  config.block_size = 64;
  config.enable_gate = true;
  config.enable_compressor = true;
  config.enable_pitch_analysis = true;
  config.enable_delay = true;
  config.delay_sync_enabled = true;
  config.delay_left_subdivision = TempoSubdivision::Eighth;
  config.delay_right_subdivision = TempoSubdivision::DottedEighth;
  config.enable_reverb = true;
  config.enable_chorus = true;
  config.chorus.mode = ChorusMode::Chorus;
  config.chorus.mix = 0.4f;
  config.enable_drive = true;
  config.drive.mode = DriveMode::Warm;
  config.drive.drive = 0.4f;
  config.drive.mix = 0.8f;
  config.tempo_bpm = 120.0f;

  if (!vocal_fx_init(config)) {
    std::fprintf(stderr, "Failed to init vocal_fx for %s\n", label);
    return;
  }

  // Enable harmony voice
  vocal_fx_set_harmony_mode(HarmonyMode::FixedInterval);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 7.0f); // fifth up
  vocal_fx_set_harmony_gain(0, 0.7f);

  const double deadline_us = (64.0 / sample_rate) * 1000000.0;
  constexpr int kWarmupBlocks = 1000;
  constexpr int kTotalBlocks = 50000;

  std::vector<float> input(64), left(64), right(64);
  for (int i = 0; i < 64; ++i) {
    // 220 Hz vocal-like fundamental with harmonics
    input[i] = 0.3f * std::sin(2.0f * 3.14159265f * 220.0f * i / sample_rate) +
               0.15f * std::sin(2.0f * 3.14159265f * 440.0f * i / sample_rate);
  }

  // Warmup
  for (int b = 0; b < kWarmupBlocks; ++b) {
    vocal_fx_process(input.data(), left.data(), right.data(), 64);
  }

  std::vector<double> block_times(kTotalBlocks);
  uint32_t deadline_misses = 0;
  bool finite_output = true;

  auto total_start = std::chrono::steady_clock::now();
  for (int b = 0; b < kTotalBlocks; ++b) {
    auto b_start = std::chrono::steady_clock::now();
    vocal_fx_process(input.data(), left.data(), right.data(), 64);
    auto b_elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - b_start).count();

    block_times[b] = b_elapsed;
    if (b_elapsed > deadline_us) {
      ++deadline_misses;
    }
    for (int i = 0; i < 64; ++i) {
      if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
        finite_output = false;
      }
    }
  }
  auto total_elapsed = std::chrono::duration<double, std::micro>(
      std::chrono::steady_clock::now() - total_start).count();

  std::sort(block_times.begin(), block_times.end());
  double avg_us = total_elapsed / kTotalBlocks;
  double p50_us = block_times[kTotalBlocks * 50 / 100];
  double p95_us = block_times[kTotalBlocks * 95 / 100];
  double p99_us = block_times[kTotalBlocks * 99 / 100];
  double max_us = block_times.back();

  std::printf("\n=== HOST SOAK BENCHMARK: %s (%.1f kHz, 64 frames, deadline: %.1f us) ===\n",
              label, sample_rate * 0.001f, deadline_us);
  std::printf("  [NOTE] Host x86 comparative metric. Real-time deadline qualification is on ESP32-P4.\n");
  std::printf("  Blocks: %d | Finite: %s | Deadline misses: %u\n",
              kTotalBlocks, finite_output ? "YES" : "NO", deadline_misses);
  std::printf("  Avg: %.2f us (%.2f%%) | p50: %.2f us | p95: %.2f us | p99: %.2f us | Max: %.2f us\n",
              avg_us, (avg_us / deadline_us) * 100.0, p50_us, p95_us, p99_us, max_us);
  std::printf("  DSP buffers: %zu bytes (%.2f KB)\n",
              vocal_fx_dsp_memory_bytes(), vocal_fx_dsp_memory_bytes() / 1024.0);
}

int main() {
  std::puts("Running Full Pipeline HOST Benchmark (Harmony + Drive + Chorus + Sync Delay + Reverb + Limiter)...");
  std::puts("Target: Host x86_64 algorithmic comparative throughput (PSOLA quiescent/fallback)");
  run_soak(44100.0f, "Full Pipeline 44.1 kHz");
  run_soak(48000.0f, "Full Pipeline 48.0 kHz");
  return 0;
}
