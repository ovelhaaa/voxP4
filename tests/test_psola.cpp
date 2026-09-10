#include "td_psola.h"
#include "vocal_fx.h"
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
  SharedPitchShiftResources shared; shared.init();
  TdPsola p;
  PitchShiftConfig c{true, semitones, 1, 1};
  require(p.init(kRate, c, &shared), "init");
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
double tone_power(const std::vector<float> &x, size_t begin, float hz) {
  double re = 0, im = 0;
  for (size_t i = begin; i < x.size(); ++i) {
    const double phase = 2 * kPi * hz * i / kRate;
    re += x[i] * std::cos(phase);
    im -= x[i] * std::sin(phase);
  }
  return re * re + im * im;
}
void isolated_engine_output() {
  VocalFxConfig c{};
  c.enable_gate = c.enable_compressor = c.enable_delay = c.enable_reverb = false;
  c.isolate_pitch_shift_output = true;
  c.pitch_shift = {true, 4, 1, 1};
  require(vocal_fx_init(c), "isolated engine init");
  constexpr size_t total = 48000 * 3, block = 64;
  std::vector<float> in(total), out(total);
  std::array<float, block> right{};
  for (size_t i = 0; i < total; ++i)
    for (int h = 1; h <= 5; ++h)
      in[i] += .12f / h * std::sin(2 * kPi * 220 * h * i / kRate);
  for (size_t i = 0; i < total; i += block) {
    vocal_fx_process(in.data() + i, out.data() + i, right.data(),
                     std::min(block, total - i));
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }
  const double shifted =
      tone_power(out, total - 24000, 220 * std::exp2(4.0 / 12));
  const double dry = tone_power(out, total - 24000, 220);
  require(shifted > dry * 10,
          "isolated host path must not contain dominant dry pitch");
}
double stationary_energy_ratio(float semitones, int signal_kind) {
  constexpr size_t total = 48000 * 3, block = 64, measure_begin = 48000 * 2;
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{true, semitones, 1, 1};
  config.ola_normalization = OlaNormalizationMode::ColaEnergyHybrid;
  require(psola.init(kRate, config, &shared), "energy normalization init");
  psola.set_onset_unvoiced_attenuation(false);
  std::vector<float> input(total), output(total);
  const float period = kRate / 220.0f;
  for (size_t i = 0; i < total; ++i) {
    const float phase = 2 * kPi * 220 * i / kRate;
    if (signal_kind == 0)
      input[i] = 1.0f;
    else if (signal_kind == 1)
      input[i] = .25f * std::sin(phase);
    else
      for (int harmonic = 1; harmonic <= 5; ++harmonic)
        input[i] += .12f / harmonic * std::sin(harmonic * phase);
  }
  for (size_t position = 0; position < total; position += block) {
    std::array<PitchMark, 64> marks{};
    const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
    const size_t count = marks_for(analyzed, period, marks);
    PitchResult pitch{};
    pitch.voiced = true;
    pitch.confidence = 1;
    pitch.frequency_hz = 220;
    pitch.period_samples = period;
    pitch.analysis_timestamp_samples = analyzed;
    psola.process(input.data() + position, output.data() + position,
                  std::min(block, total - position), pitch,
                  count >= 3 ? PitchTrackState::Locked
                             : PitchTrackState::Acquiring,
                  marks.data(), count);
  }
  double input_power = 0, output_power = 0;
  for (size_t i = measure_begin; i < total; ++i) {
    input_power += static_cast<double>(input[i]) * input[i];
    output_power += static_cast<double>(output[i]) * output[i];
  }
  require(std::all_of(output.begin(), output.end(), [](float value) {
            return std::isfinite(value) && std::fabs(value) < 4.0f;
          }),
          "energy normalization finite and bounded");
  return std::sqrt(output_power / input_power);
}
void energy_normalization_regression() {
  for (int signal_kind = 0; signal_kind < 3; ++signal_kind)
    for (float semitones : {-7.0f, -4.0f, 0.0f, 4.0f, 7.0f}) {
      const double ratio = stationary_energy_ratio(semitones, signal_kind);
      if (ratio < .95 || ratio > 1.05)
        std::fprintf(stderr, "energy signal=%d shift=%+.0f ratio=%.6f\n",
                     signal_kind, semitones, ratio);
      require(ratio >= .95 && ratio <= 1.05,
              "stationary OLA energy must remain within five percent");
    }
}
void ola_ring_wrap_regression() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{true, 7, 1, 1};
  config.ola_normalization = OlaNormalizationMode::ColaEnergyHybrid;
  require(psola.init(kRate, config, &shared), "OLA wrap regression init");
  psola.set_onset_unvoiced_attenuation(false);
  constexpr size_t block = 64;
  std::array<float, block> input{}, output{};
  const float period = kRate / 220.0f;
  for (size_t position = 0; position < 48000 * 2; position += block) {
    for (size_t i = 0; i < block; ++i)
      input[i] = .25f * std::sin(2 * kPi * 220 * (position + i) / kRate);
    std::array<PitchMark, 64> marks{};
    const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
    const size_t count = marks_for(analyzed, period, marks);
    PitchResult pitch{};
    pitch.voiced = true;
    pitch.confidence = 1;
    pitch.frequency_hz = 220;
    pitch.period_samples = period;
    pitch.analysis_timestamp_samples = analyzed;
    psola.process(input.data(), output.data(), block, pitch,
                  count >= 3 ? PitchTrackState::Locked
                             : PitchTrackState::Acquiring,
                  marks.data(), count);
  }
  psola.reset();
  shared.reset();
  input.fill(0);
  PitchResult no_pitch{};
  for (int wrap = 0; wrap < 100; ++wrap) {
    psola.process(input.data(), output.data(), block, no_pitch,
                  PitchTrackState::Unlocked, nullptr, 0);
    require(std::all_of(output.begin(), output.end(),
                        [](float value) { return value == 0.0f; }),
            "reset OLA rings cannot leak across wrap");
  }
}
void component_stem_regression() {
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{true, 7, 1, 1};
  require(psola.init(kRate, config, &shared), "component stem init");
  psola.set_onset_unvoiced_attenuation(false);
  constexpr size_t block = 64;
  std::array<float, block> input{}, output{}, psola_part{}, fallback_part{};
  const float period = kRate / 220.0f;
  for (size_t position = 0; position < 48000 * 2; position += block) {
    for (size_t i = 0; i < block; ++i) {
      const float phase = 2 * kPi * 220 * (position + i) / kRate;
      input[i] = .2f * std::sin(phase) + .08f * std::sin(2 * phase);
    }
    shared.push(input.data(), block);
    std::array<PitchMark, 64> marks{};
    const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
    const size_t count = marks_for(analyzed, period, marks);
    PitchResult pitch{};
    pitch.voiced = true;
    pitch.confidence = 1;
    pitch.frequency_hz = 220;
    pitch.period_samples = period;
    pitch.analysis_timestamp_samples = analyzed;
    psola.process_shared(input.data(), output.data(), block, pitch,
                         count >= 3 ? PitchTrackState::Locked
                                    : PitchTrackState::Acquiring,
                         marks.data(), count, psola_part.data(),
                         fallback_part.data());
    for (size_t i = 0; i < block; ++i)
      require(std::fabs(output[i] - psola_part[i] - fallback_part[i]) < 1e-6f,
              "PSOLA plus fallback stems reconstruct final output");
  }
}
void isolated_second_voice_output() {
  VocalFxConfig c{};
  c.enable_gate = c.enable_compressor = c.enable_delay = c.enable_reverb = false;
  c.isolate_pitch_shift_output = true;
  c.isolated_pitch_shift_voice = 1;
  c.pitch_shift = {false, 0, 1, 1};
  require(vocal_fx_init(c), "isolated voice-2 engine init");
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7);
  vocal_fx_set_harmony_gain(1, 1);
  constexpr size_t total = 48000 * 3, block = 64;
  std::vector<float> in(total), out(total);
  std::array<float, block> right{};
  for (size_t i = 0; i < total; ++i)
    for (int h = 1; h <= 5; ++h)
      in[i] += .12f / h * std::sin(2 * kPi * 220 * h * i / kRate);
  for (size_t i = 0; i < total; i += block) {
    vocal_fx_process(in.data() + i, out.data() + i, right.data(),
                     std::min(block, total - i));
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }
  const double shifted =
      tone_power(out, total - 24000, 220 * std::exp2(7.0 / 12));
  const double dry = tone_power(out, total - 24000, 220);
  require(shifted > dry * 10,
          "isolated voice-2 path must contain its configured pitch");
  const auto debug = vocal_fx_harmony_debug(1);
  require(std::fabs(debug.requested_semitones - 7.0f) < .01f,
          "voice-2 debug selects voice 2");
}
void attenuation_bypass_debug() {
  SharedPitchShiftResources normal_shared, bypass_shared;
  normal_shared.init();
  bypass_shared.init();
  TdPsola normal, bypass;
  PitchShiftConfig c{true, 7, 1, 30};
  require(normal.init(kRate, c, &normal_shared), "normal attenuation init");
  require(bypass.init(kRate, c, &bypass_shared), "bypass attenuation init");
  bypass.set_onset_unvoiced_attenuation(false);
  std::array<float, 64> input{}, normal_output{}, bypass_output{};
  PitchResult unvoiced{};
  normal.process(input.data(), normal_output.data(), input.size(), unvoiced,
                 PitchTrackState::Unlocked, nullptr, 0);
  bypass.process(input.data(), bypass_output.data(), input.size(), unvoiced,
                 PitchTrackState::Unlocked, nullptr, 0);
  const auto normal_debug = normal.debug();
  const auto bypass_debug = bypass.debug();
  require(normal_debug.onset_unvoiced_attenuation_enabled,
          "transition attenuation defaults on");
  require(normal_debug.psola_gain == 0.0f,
          "unvoiced default keeps acquisition envelope down");
  require(!bypass_debug.onset_unvoiced_attenuation_enabled,
          "diagnostic transition attenuation bypass is reported");
  require(bypass_debug.psola_gain == 1.0f && bypass_debug.active_mix == 1.0f,
          "diagnostic bypass keeps transition envelopes open");
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
  SharedPitchShiftResources shared; shared.init();
  TdPsola p;
  PitchShiftConfig c{true, 4, 1, 30};
  require(p.init(kRate, c, &shared), "transition init");
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
  energy_normalization_regression();
  ola_ring_wrap_regression();
  component_stem_regression();
  isolated_engine_output();
  isolated_second_voice_output();
  attenuation_bypass_debug();
  for (int field = 0; field < 3; ++field) {
    SharedPitchShiftResources invalid_shared; invalid_shared.init();
    TdPsola invalid;
    PitchShiftConfig bad{true, 4, 1, 30};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (field == 0)
      bad.semitones = nan;
    else if (field == 1)
      bad.wet = nan;
    else
      bad.smoothing_ms = nan;
    require(invalid.init(kRate, bad, &invalid_shared),
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
  // Two independent synthesis timelines share one aggressively wrapping source
  // history; neither voice owns or writes a duplicate history.
  SharedPitchShiftResources dual_shared;
  dual_shared.init();
  TdPsola lower, upper;
  PitchShiftConfig lower_cfg{true, -3, 1, 30}, upper_cfg{true, 7, 1, 30};
  require(lower.init(kRate, lower_cfg, &dual_shared) &&
              upper.init(kRate, upper_cfg, &dual_shared),
          "dual shared init");
  std::array<float,64> dual_in{}, dual_a{}, dual_b{};
  PitchResult dual_pitch{};
  for(int i=0;i<5000;++i){
    dual_shared.push(dual_in.data(),dual_in.size());
    lower.process_shared(dual_in.data(),dual_a.data(),dual_in.size(),dual_pitch,
                         PitchTrackState::Unlocked,nullptr,0);
    upper.process_shared(dual_in.data(),dual_b.data(),dual_in.size(),dual_pitch,
                         PitchTrackState::Unlocked,nullptr,0);
  }
  require(std::all_of(dual_a.begin(),dual_a.end(),[](float x){return std::isfinite(x);}) &&
              std::all_of(dual_b.begin(),dual_b.end(),[](float x){return std::isfinite(x);}),
          "two voice independent ring wrap");
  SharedPitchShiftResources shared; shared.init();
  TdPsola p;
  PitchShiftConfig c{true, 4, 1, 30};
  require(p.init(kRate, c, &shared), "safety init");
  std::array<float, 64> z{}, o{};
  PitchResult r{};
  for (int i = 0; i < 5000; ++i)
    p.process(z.data(), o.data(), z.size(), r, PitchTrackState::Unlocked,
              nullptr, 0);
  require(std::all_of(o.begin(), o.end(), [](float v) { return v == 0; }),
          "silence and repeated ring wrap");
  require(p.telemetry().fallback_frames > 0, "fallback telemetry");
  SharedPitchShiftResources startup_shared; startup_shared.init();
  TdPsola startup;
  require(startup.init(kRate, c, &shared), "startup init");
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
