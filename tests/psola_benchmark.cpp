#include "vocal_fx.h"
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
int main(int argc, char **argv) {
  int seconds = 10;
  if (argc > 1) {
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(argv[1], &end, 10);
    if (errno == ERANGE || end == argv[1] || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX / 48000) {
      std::fprintf(stderr, "seconds must be an integer in [1, %d]\n",
                   INT_MAX / 48000);
      return 2;
    }
    seconds = static_cast<int>(parsed);
  }
  VocalFxConfig c{};
  bool full = false;
  OlaNormalizationMode normalization = OlaNormalizationMode::ColaEnergyHybrid;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "full"))
      full = true;
    else if (!std::strcmp(argv[i], "current"))
      normalization = OlaNormalizationMode::Current;
    else if (!std::strcmp(argv[i], "hybrid"))
      normalization = OlaNormalizationMode::ColaEnergyHybrid;
    else if (!std::strcmp(argv[i], "continuity")) {
      c.psola_continuity_policy =
          PsolaContinuityPolicy::OnsetContinuityCoasting;
      c.psola_coast_ms = 60.0f;
    }
    else {
      std::fprintf(stderr, "mode must be current, hybrid, or full\n");
      return 2;
    }
  }
  c.enable_gate = c.enable_compressor = c.enable_delay = c.enable_reverb = full;
  c.pitch_shift = {true, 4, 1, 30};
  c.pitch_shift.ola_normalization = normalization;
  if (!vocal_fx_init(c))
    return 1;
  std::array<float, 64> in{}, l{}, r{};
  uint64_t sample = 0, blocks = 0;
  std::vector<double> block_us;
  block_us.reserve(static_cast<size_t>(seconds) * 750);
  const auto start = std::chrono::steady_clock::now();
  for (int b = 0; b < seconds * 750; ++b) {
    for (size_t i = 0; i < 64; ++i, ++sample) {
      float p = 2 * 3.14159265358979323846f * 220 * sample / 48000;
      in[i] = .25f * std::sin(p) + .1f * std::sin(2 * p);
    }
    const auto block_start = std::chrono::steady_clock::now();
    vocal_fx_process(in.data(), l.data(), r.data(), 64);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
    block_us.push_back(std::chrono::duration<double, std::micro>(
                           std::chrono::steady_clock::now() - block_start)
                           .count());
    ++blocks;
  }
  const double us = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  std::sort(block_us.begin(), block_us.end());
  const double p95_us = block_us.empty()
                            ? 0.0
                            : block_us[static_cast<size_t>(
                                  .95 * static_cast<double>(block_us.size() - 1))];
  const auto t = vocal_fx_pitch_shift_telemetry();
  const auto p =
      vocal_fx_pitch_shift_profile_stats(PitchShiftProfileSection::Total);
  PitchMark m{};
  auto pr = vocal_fx_latest_pitch();
  vocal_fx_get_latest_pitch_mark(&m);
  std::printf(
      "seconds=%d wall_us/block=%.3f wall_p95_us=%.3f wall_max_us=%.3f "
      "profiled_us/block=%.3f grains/block=%.3f "
      "max_grains=%u fallback_pct=%.3f mark_underflow=%llu "
      "history_underflow=%llu ram=%zu latest_pitch_ts=%llu latest_mark=%llu "
      "state=%u\n",
      seconds, us / blocks, p95_us, block_us.empty() ? 0.0 : block_us.back(),
      p.blocks ? (double)p.total_us / p.blocks : 0,
      t.blocks ? (double)t.grains / t.blocks : 0, t.max_grains_per_block,
      100.0 * t.fallback_frames / (seconds * 48000),
      (unsigned long long)t.pitch_mark_underflows,
      (unsigned long long)t.audio_history_underflows,
      vocal_fx_dsp_memory_bytes(),
      (unsigned long long)pr.analysis_timestamp_samples,
      (unsigned long long)m.sample_position,
      (unsigned)vocal_fx_pitch_track_state());
  for (unsigned s = 0; s < (unsigned)PitchShiftProfileSection::Count; ++s) {
    auto q = vocal_fx_pitch_shift_profile_stats((PitchShiftProfileSection)s);
    std::printf("stage=%u calls=%llu avg_us=%.3f max_us=%llu\n", s,
                (unsigned long long)q.blocks,
                q.blocks ? (double)q.total_us / q.blocks : 0,
                (unsigned long long)q.worst_us);
  }
}
