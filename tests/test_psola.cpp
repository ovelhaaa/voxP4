#include "td_psola.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace {
constexpr float kRate = 48000.0f, kPi = 3.14159265358979323846f;
void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
size_t marks_for(uint64_t available, float period,
                 std::array<PitchMark, 64> &marks) {
  const uint64_t last_index =
      static_cast<uint64_t>(std::floor(available / period));
  const uint64_t first_index = last_index > 62 ? last_index - 62 : 0;
  size_t n = 0;
  for (uint64_t index = first_index; index <= last_index && n < marks.size();
       ++index)
    marks[n++] = {static_cast<uint64_t>(std::llround(index * period)), 1};
  return n;
}
float estimate(const std::vector<float> &x, size_t begin, float expected) {
  float best = -1, frequency = expected;
  for (float f = expected * .98f; f <= expected * 1.02f; f += .02f) {
    double re = 0, im = 0;
    for (size_t i = begin; i < x.size(); ++i) {
      const double phase = 2 * kPi * f * i / kRate;
      re += x[i] * std::cos(phase);
      im -= x[i] * std::sin(phase);
    }
    const float power = static_cast<float>(re * re + im * im);
    if (power > best) {
      best = power;
      frequency = f;
    }
  }
  return frequency;
}
void accuracy(float semitones) {
  TdPsola p;
  PitchShiftConfig c{true, semitones, 1, 1};
  require(p.init(kRate, c), "init");
  constexpr size_t total = 48000 * 3, block = 64;
  std::vector<float> in(total), out(total);
  const float f = 220, period = kRate / f;
  for (size_t i = 0; i < total; ++i)
    for (int h = 1; h <= 5; ++h)
      in[i] += .12f / h * std::sin(2 * kPi * f * h * i / kRate);
  for (size_t pos = 0; pos < total; pos += block) {
    std::array<PitchMark, 64> m{};
    const uint64_t analyzed = pos > 1100 ? pos - 1100 : 0;
    const size_t count = marks_for(analyzed, period, m);
    PitchResult r{};
    r.voiced = true;
    r.confidence = 1;
    r.frequency_hz = f;
    r.period_samples = period;
    r.analysis_timestamp_samples = analyzed;
    p.process(in.data() + pos, out.data() + pos, std::min(block, total - pos),
              r,
              count >= 3 ? PitchTrackState::Locked : PitchTrackState::Acquiring,
              m.data(), count);
  }
  const float want = f * std::exp2(semitones / 12),
              got = estimate(out, total - 24000, want);
  const float cents = 1200 * std::log2(got / want);
  std::printf("%+.0f st: %.3f Hz target %.3f Hz, error %.3f cents\n", semitones,
              got, want, cents);
  if (std::fabs(cents) >= 10)
    std::fprintf(stderr, "%+.0f st: got %.3f want %.3f (%.2f cents)\n",
                 semitones, got, want, cents);
  require(std::fabs(cents) < 10, "pitch accuracy");
  require(
      std::all_of(out.begin(), out.end(),
                  [](float v) { return std::isfinite(v) && std::fabs(v) < 2; }),
      "finite bounded output");
}
void transition_safety() {
  constexpr size_t total = 72000, block = 64;
  std::vector<float> in(total), out(total);
  double phase = 0;
  std::vector<uint64_t> crossings;
  for (size_t i = 0; i < total; ++i) {
    float f =
        i < 24000 ? 220.0f : (i < 28800 ? 0.0f : 330.0f); // voiced/noise/voiced
    if (!f)
      in[i] = .08f * std::sin(i * 12.9898f);
    else {
      double old = phase;
      phase += 2 * kPi * f / kRate;
      if (std::floor(old / (2 * kPi)) != std::floor(phase / (2 * kPi)))
        crossings.push_back(i);
      in[i] = .3f * std::sin(phase);
    }
  }
  TdPsola p;
  PitchShiftConfig c{true, 4, 1, 30};
  require(p.init(kRate, c), "transition init");
  for (size_t pos = 0; pos < total; pos += block) {
    if (pos == 12000)
      p.set_semitones(7);
    if (pos == 18000)
      p.set_semitones(-3);
    std::array<PitchMark, 64> m{};
    size_t n = 0;
    uint64_t available = pos > 1100 ? pos - 1100 : 0;
    auto it = std::lower_bound(crossings.begin(), crossings.end(),
                               available > 14000 ? available - 14000 : 0);
    for (; it != crossings.end() && *it <= available && n < m.size(); ++it)
      m[n++] = {*it, 1};
    const bool voiced = !(pos >= 24000 && pos < 28800);
    const float f = pos < 24000 ? 220 : 330;
    PitchResult r{};
    r.voiced = voiced;
    r.confidence = voiced ? 1 : 0;
    r.period_samples = kRate / f;
    r.analysis_timestamp_samples = available;
    r.onset = (pos == 28800);
    p.process(
        in.data() + pos, out.data() + pos, std::min(block, total - pos), r,
        voiced && n >= 3 ? PitchTrackState::Locked : PitchTrackState::Unlocked,
        m.data(), n);
  }
  float jump = 0;
  for (size_t i = 1; i < out.size(); ++i)
    jump = std::max(jump, std::fabs(out[i] - out[i - 1]));
  require(jump < 1.0f, "voiced/noise/voiced transition has no click burst");
}
} // namespace
int main() {
  for (int field = 0; field < 3; ++field) {
    TdPsola invalid;
    PitchShiftConfig bad{true, 4, 1, 30};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (field == 0)
      bad.semitones = nan;
    else if (field == 1)
      bad.wet = nan;
    else
      bad.smoothing_ms = nan;
    require(invalid.init(kRate, bad),
            "replace non-finite config with defaults");
    std::array<float, 64> input{}, output{};
    PitchResult no_pitch{};
    invalid.process(input.data(), output.data(), output.size(), no_pitch,
                    PitchTrackState::Unlocked, nullptr, 0);
    require(std::all_of(output.begin(), output.end(),
                        [](float value) { return std::isfinite(value); }),
            "defaulted configuration remains finite");
  }
  for (float st : {-7.f, -5.f, -4.f, -3.f, 3.f, 4.f, 5.f, 7.f})
    accuracy(st);
  TdPsola p;
  PitchShiftConfig c{true, 4, 1, 30};
  require(p.init(kRate, c), "safety init");
  std::array<float, 64> z{}, o{};
  PitchResult r{};
  for (int i = 0; i < 5000; ++i)
    p.process(z.data(), o.data(), z.size(), r, PitchTrackState::Unlocked,
              nullptr, 0);
  require(std::all_of(o.begin(), o.end(), [](float v) { return v == 0; }),
          "silence and repeated ring wrap");
  require(p.telemetry().fallback_frames > 0, "fallback telemetry");
  TdPsola startup;
  require(startup.init(kRate, c), "startup init");
  std::array<float, 64> nonzero{}, startup_out{};
  nonzero.fill(0.25f);
  startup.process(nonzero.data(), startup_out.data(), nonzero.size(), r,
                  PitchTrackState::Unlocked, nullptr, 0);
  require(std::all_of(startup_out.begin(), startup_out.end(),
                      [](float value) { return value == 0.0f; }),
          "pre-history fallback must be silence, not sample zero replay");
  std::atomic<bool> reading{true};
  std::thread telemetry_reader([&] {
    while (reading.load(std::memory_order_relaxed)) {
      const auto telemetry = startup.telemetry();
      require(telemetry.max_grains_per_block <= TdPsola::kMaxGrainsPerBlock,
              "coherent telemetry snapshot");
    }
  });
  for (int i = 0; i < 100; ++i)
    startup.process(nonzero.data(), startup_out.data(), nonzero.size(), r,
                    PitchTrackState::Unlocked, nullptr, 0);
  reading.store(false, std::memory_order_relaxed);
  telemetry_reader.join();
  transition_safety();
  std::puts("TD-PSOLA tests passed");
}
