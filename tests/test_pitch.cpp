#include "decimator.h"
#include "pitch_analysis.h"
#include "yin_detector.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <thread>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);        \
      return 1;                                                                \
    }                                                                          \
  } while (0)
constexpr float pi = 3.14159265358979323846f;
static float cents(float measured, float expected) {
  return 1200 * std::log2(measured / expected);
}
static PitchDetectorMeasurement detect(float frequency,
                                       std::vector<float> harmonics = {1}) {
  PitchAnalysisConfig c{};
  YinDetector yin;
  if (!yin.init(c))
    return {};
  std::vector<float> x(c.window_size);
  for (size_t i = 0; i < x.size(); ++i)
    for (size_t h = 0; h < harmonics.size(); ++h)
      x[i] += harmonics[h] * std::sin(2 * pi * frequency * (h + 1) * i /
                                      c.analysis_sample_rate);
  return yin.analyze(x.data(), x.size(), 0);
}

static bool test_voicing_hysteresis() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  cfg.voiced_enter_confidence = 0.80f;
  cfg.voiced_stay_confidence = 0.45f;
  cfg.voiced_attack_frames = 2;
  cfg.voiced_release_frames = 3;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  // 1. Weak signal with confidence around 0.60
  for (int b = 0; b < 20; ++b) {
    for (float &s : block) {
      s = 0.2f * std::sin(2 * pi * 220 * pos / 48000.0f) + 0.08f * std::sin(2 * pi * 573 * pos / 48000.0f);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(!analyzer.latest().voiced_stateful);

  // 2. Strong clean signal (confidence > 0.90) -> enters voiced
  for (int b = 0; b < 30; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos / 48000.0f);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);

  // 3. Signal drops back to confidence ~0.60 (above stay_confidence) -> remains voiced!
  for (int b = 0; b < 20; ++b) {
    for (float &s : block) {
      s = 0.2f * std::sin(2 * pi * 220 * pos / 48000.0f) + 0.08f * std::sin(2 * pi * 573 * pos / 48000.0f);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);
  return true;
}

static bool test_single_bad_hop() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  cfg.voiced_release_frames = 3;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  for (int b = 0; b < 40; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);

  // Inject 1 bad hop (240 samples of HF noise)
  for (int b = 0; b < 4; ++b) {
    for (float &s : block) {
      s = ((pos % 2 == 0) ? 0.3f : -0.3f);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  // Stateful voiced must bridge the isolated bad hop!
  CHECK(analyzer.latest().voiced_stateful);

  for (int b = 0; b < 40; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);
  return true;
}

static bool test_double_bad_hop() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  cfg.voiced_release_frames = 3;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  for (int b = 0; b < 40; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);

  // Inject 2 bad hops (480 input samples = 7.5 blocks)
  for (int b = 0; b < 7; ++b) {
    for (float &s : block) {
      s = ((pos % 2 == 0) ? 0.3f : -0.3f);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);

  for (int b = 0; b < 30; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);
  return true;
}

static bool test_vibrato_continuity() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  float phase = 0.0f;
  int unvoiced_toggles = 0;
  bool was_voiced = false;
  for (int b = 0; b < 750; ++b) {
    for (float &s : block) {
      float t = pos / 48000.0f;
      float f_inst = 220.0f * std::exp2((150.0f * std::sin(2 * pi * 6.0f * t)) / 1200.0f);
      phase += 2.0f * pi * f_inst / 48000.0f;
      s = 0.5f * std::sin(phase);
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    while (analyzer.run(1)) {
      auto r = analyzer.latest();
      if (r.voiced_stateful) was_voiced = true;
      else if (was_voiced && b > 50) {
        ++unvoiced_toggles;
      }
    }
  }
  CHECK(was_voiced);
  CHECK(unvoiced_toggles == 0);
  return true;
}

static bool test_vocal_fry_continuity() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  const int T0 = 48000 / 90;
  int unvoiced_count = 0;
  bool reached_voiced = false;
  for (int b = 0; b < 750; ++b) {
    for (float &s : block) {
      int phase_in_period = pos % T0;
      int pulse_idx = pos / T0;
      float amp = (pulse_idx % 2 == 0) ? 0.7f : 0.35f;
      s = (phase_in_period < 40) ? amp * std::sin(pi * phase_in_period / 40.0f) : 0.0f;
      ++pos;
    }
    analyzer.tap(block.data(), block.size());
    while (analyzer.run(1)) {
      auto r = analyzer.latest();
      if (r.voiced_stateful) reached_voiced = true;
      else if (reached_voiced && b > 60) {
        ++unvoiced_count;
      }
    }
  }
  CHECK(reached_voiced);
  CHECK(unvoiced_count == 0);
  return true;
}

static bool test_low_energy_vowel_tail() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  cfg.min_input_db = -55.0f;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  bool voiced_at_minus_30db = false;
  for (int b = 0; b < 600; ++b) {
    float gain = 0.7f * std::pow(10.0f, (-30.0f * (b / 600.0f)) / 20.0f);
    for (float &s : block) {
      s = gain * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
    if (b > 500 && analyzer.latest().voiced_stateful) {
      voiced_at_minus_30db = true;
    }
  }
  CHECK(voiced_at_minus_30db);

  for (int b = 0; b < 50; ++b) {
    std::fill(block.begin(), block.end(), 0.0f);
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(!analyzer.latest().voiced_stateful);
  return true;
}

static bool test_fricative_rejection() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
  int voiced_hops = 0;
  for (int b = 0; b < 200; ++b) {
    for (float &s : block) {
      s = dist(rng) * std::sin(2 * pi * 6000.0f * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    while (analyzer.run(1)) {
      if (analyzer.latest().voiced_stateful) {
        ++voiced_hops;
      }
    }
  }
  CHECK(voiced_hops == 0);
  return true;
}

static bool test_plosive_rejection() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  cfg.voiced_release_frames = 3;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  for (int b = 0; b < 50; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);

  std::fill(block.begin(), block.end(), 0.0f);
  int hops_until_unvoiced = 0;
  for (int b = 0; b < 40; ++b) {
    analyzer.tap(block.data(), block.size());
    while (analyzer.run(1)) {
      if (analyzer.latest().voiced_stateful) {
        ++hops_until_unvoiced;
      }
    }
  }
  std::printf("plosive hops_until_unvoiced: %d, final voiced: %d\n", hops_until_unvoiced, analyzer.latest().voiced_stateful ? 1 : 0);
  CHECK(hops_until_unvoiced <= 10);
  CHECK(!analyzer.latest().voiced_stateful);
  return true;
}

static bool test_legato_note_change() {
  PitchAnalysis analyzer;
  PitchAnalysisConfig cfg{};
  cfg.stateful_voicing_enabled = true;
  CHECK(analyzer.init(cfg));

  std::vector<float> block(64);
  uint64_t pos = 0;
  for (int b = 0; b < 375; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 220 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    analyzer.run(4);
  }
  CHECK(analyzer.latest().voiced_stateful);
  CHECK(std::fabs(analyzer.latest().frequency_hz - 220.0f) < 5.0f);

  int hops_to_acquire = 0;
  bool reached_target = false;
  for (int b = 0; b < 100; ++b) {
    for (float &s : block) {
      s = 0.5f * std::sin(2 * pi * 330 * pos++ / 48000.0f);
    }
    analyzer.tap(block.data(), block.size());
    while (analyzer.run(1)) {
      ++hops_to_acquire;
      if (std::fabs(analyzer.latest().frequency_hz - 330.0f) < 10.0f) {
        reached_target = true;
        break;
      }
    }
    if (reached_target) break;
  }
  std::printf("legato reached_target: %d, hops_to_acquire: %d, latest_hz: %.2f\n", reached_target ? 1 : 0, hops_to_acquire, analyzer.latest().frequency_hz);
  CHECK(reached_target);
  CHECK(hops_to_acquire <= 25);
  return true;
}

int main() {
  {
    YinDetector uninitialized;
    float samples[512]{};
    CHECK(uninitialized.analyze(samples, 512, 0).frequency_hz == 0);
    PitchAnalysisConfig invalid{};
    invalid.min_frequency = std::numeric_limits<float>::quiet_NaN();
    CHECK(!uninitialized.init(invalid));
    CHECK(uninitialized.analyze(samples, 512, 0).frequency_hz == 0);
    PitchAnalysis analyzer;
    CHECK(analyzer.profile(PitchAnalysisProfileSection::Count).calls == 0);
    CHECK(
        analyzer.profile(static_cast<PitchAnalysisProfileSection>(255)).calls ==
        0);
  }
  std::vector<float> clean_errors;
  int octave_errors = 0;
  for (float hz : {65.f, 80.f, 100.f, 110.f, 220.f, 440.f, 880.f, 1000.f}) {
    auto r = detect(hz);
    std::printf("sine %.0f Hz -> %.4f Hz (%+.3f cents), confidence %.3f\n", hz,
                r.frequency_hz, cents(r.frequency_hz, hz), r.confidence);
    CHECK(std::fabs(cents(r.frequency_hz, hz)) < 5);
    CHECK(r.confidence > .9f);
    clean_errors.push_back(std::fabs(cents(r.frequency_hz, hz)));
    octave_errors += std::fabs(cents(r.frequency_hz, hz)) > 600;
  }
  auto weak = detect(220, {.08f, 1, .4f, .2f});
  CHECK(std::fabs(cents(weak.frequency_hz, 220)) < 5);
  auto missing = detect(110, {0, 1, .8f, .5f, .3f});
  CHECK(std::fabs(cents(missing.frequency_hz, 110)) < 5);
  for (float amplitude : {0.f, 1e-20f}) {
    std::vector<float> x(512, amplitude);
    PitchAnalysisConfig c{};
    YinDetector y;
    CHECK(y.init(c));
    auto r = y.analyze(x.data(), x.size(), 0);
    CHECK(std::isfinite(r.confidence) && std::isfinite(r.frequency_hz));
  }
  FirDecimator d;
  CHECK(d.init(48000, 12000));
  auto decimated_rms = [&](float hz) {
    d.reset();
    double e = 0;
    int count = 0;
    for (int i = 0; i < 48000; ++i) {
      float o;
      if (d.process(std::sin(2 * pi * hz * i / 48000), o) && i > 1000) {
        e += o * o;
        ++count;
      }
    }
    return std::sqrt(e / count);
  };
  const float pass = decimated_rms(1000), stop = decimated_rms(11000);
  std::printf("decimator pass RMS %.5f, 11 kHz stop RMS %.5f (%.1f dB)\n", pass,
              stop, 20 * std::log10(stop / pass));
  CHECK(stop / pass < .05f);
  PitchAnalysis a;
  PitchAnalysisConfig c{};
  CHECK(a.init(c));
  std::vector<float> block(64);
  uint64_t pos = 0;
  for (int b = 0; b < 400; ++b) {
    for (auto &v : block)
      v = .5f * std::sin(2 * pi * 220 * pos++ / 48000);
    a.tap(block.data(), block.size());
    a.run(8);
  }
  auto r = a.latest();
  CHECK(r.voiced);
  CHECK(std::fabs(cents(r.frequency_hz, 220)) < 8);
  CHECK(r.analysis_timestamp_samples == r.timestamp_samples);
  CHECK(a.latency_samples() == 1039);
  const uint64_t result_age = (pos - 1) - r.analysis_timestamp_samples;
  CHECK(result_age >= a.latency_samples());
  CHECK(result_age < a.latency_samples() + 4 * c.hop_size);
  PitchMark m;
  CHECK(a.latest_mark(&m));
  CHECK(a.track_state() == PitchTrackState::Locked);
  PitchMark marks[64];
  auto n = a.marks(0, pos, marks, 64);
  CHECK(n >= 3);
  for (size_t i = 1; i < n; ++i)
    CHECK(std::abs((long long)(marks[i].sample_position -
                               marks[i - 1].sample_position) -
                   218) < 45);
  std::atomic<bool> stop_reader{false}, mark_snapshot_valid{true};
  std::thread mark_reader([&] {
    PitchMark snapshot[64], latest;
    while (!stop_reader.load(std::memory_order_acquire)) {
      if (a.latest_mark(&latest) && !std::isfinite(latest.confidence))
        mark_snapshot_valid.store(false, std::memory_order_release);
      const size_t count = a.marks(0, UINT64_MAX, snapshot, 64);
      for (size_t i = 1; i < count; ++i)
        if (snapshot[i].sample_position < snapshot[i - 1].sample_position)
          mark_snapshot_valid.store(false, std::memory_order_release);
    }
  });
  for (int b = 0; b < 100; ++b) {
    for (auto &v : block)
      v = .5f * std::sin(2 * pi * 220 * pos++ / 48000);
    a.tap(block.data(), block.size());
    a.run(8);
  }
  stop_reader.store(true, std::memory_order_release);
  mark_reader.join();
  CHECK(mark_snapshot_valid.load(std::memory_order_acquire));

  PitchAnalysis backlog;
  CHECK(backlog.init(c));
  for (int b = 0; b < 100; ++b)
    backlog.tap(block.data(), block.size());
  CHECK(backlog.run(4) == 4);
  CHECK(backlog.run(4) == 4);
  auto high_frequency_marks_are_current = [](float frequency) {
    PitchAnalysis tracker;
    PitchAnalysisConfig cfg{};
    if (!tracker.init(cfg))
      return false;
    float samples[64];
    uint64_t position = 0;
    for (int block_index = 0; block_index < 750; ++block_index) {
      for (float &sample : samples)
        sample = .5f * std::sin(2 * pi * frequency * position++ / 48000);
      tracker.tap(samples, 64);
      tracker.run(8);
    }
    PitchMark latest;
    PitchMark snapshot[64];
    const auto pitch = tracker.latest();
    const size_t count = tracker.marks(0, position, snapshot, 64);
    if (!tracker.latest_mark(&latest) || count < 32 || !pitch.voiced)
      return false;
    const float expected_period = 48000 / frequency;
    const uint64_t lag =
        latest.sample_position > pitch.analysis_timestamp_samples
            ? latest.sample_position - pitch.analysis_timestamp_samples
            : pitch.analysis_timestamp_samples - latest.sample_position;
    if (lag > 1.5f * expected_period)
      return false;
    for (size_t i = 1; i < count; ++i) {
      const float period = static_cast<float>(snapshot[i].sample_position -
                                              snapshot[i - 1].sample_position);
      if (std::fabs(period - expected_period) > .3f * expected_period)
        return false;
    }
    return true;
  };
  CHECK(high_frequency_marks_are_current(440));
  CHECK(high_frequency_marks_are_current(1000));

  PitchAnalysis reacquisition;
  CHECK(reacquisition.init(c));
  uint64_t reacquisition_position = 0;
  auto feed_reacquisition = [&](float frequency, size_t sample_count) {
    for (size_t processed = 0; processed < sample_count;) {
      const size_t frames = std::min(block.size(), sample_count - processed);
      for (size_t i = 0; i < frames; ++i) {
        block[i] = frequency > 0
                       ? .5f * std::sin(2 * pi * frequency *
                                        reacquisition_position / 48000)
                       : 0;
        ++reacquisition_position;
      }
      reacquisition.tap(block.data(), frames);
      reacquisition.run(8);
      processed += frames;
    }
  };
  feed_reacquisition(220, 24000);
  CHECK(reacquisition.latest().voiced);
  feed_reacquisition(0, 12032);
  CHECK(!reacquisition.latest().voiced);
  CHECK(reacquisition.latest().frequency_hz == 0);
  float first_reacquired_hz = 0;
  for (int block_index = 0; block_index < 100 && first_reacquired_hz == 0;
       ++block_index) {
    for (float &sample : block) {
      sample = .5f * std::sin(2 * pi * 220 * reacquisition_position / 48000);
      ++reacquisition_position;
    }
    reacquisition.tap(block.data(), block.size());
    reacquisition.run(8);
    const auto result = reacquisition.latest();
    if (result.voiced)
      first_reacquired_hz = result.frequency_hz;
  }
  CHECK(first_reacquired_hz > 0);
  CHECK(std::fabs(cents(first_reacquired_hz, 220)) < 10);

  auto run_signal = [](auto generator, int frames) {
    PitchAnalysis tracker;
    PitchAnalysisConfig cfg{};
    if (!tracker.init(cfg))
      return std::vector<float>{};
    float samples[64];
    uint64_t p = 0;
    std::vector<float> estimates;
    for (int b = 0; b < frames / 64; ++b) {
      for (float &v : samples)
        v = generator(p++);
      tracker.tap(samples, 64);
      while (tracker.run(1)) {
        auto q = tracker.latest();
        if (q.voiced)
          estimates.push_back(q.frequency_hz);
      }
    }
    return estimates;
  };
  // 30-cent, 5-Hz vibrato retains useful modulation after smoothing.
  auto vibrato = run_signal(
      [](uint64_t i) {
        float t = i / 48000.f;
        float hz = 220 * std::exp2((30 * std::sin(2 * pi * 5 * t)) / 1200);
        return .5f * std::sin(2 * pi * hz * t);
      },
      48000);
  CHECK(!vibrato.empty());
  auto vr =
      std::minmax_element(vibrato.begin() + vibrato.size() / 2, vibrato.end());
  CHECK(*vr.second - *vr.first > 3.0f);
  // Glissando remains continuous, and the abrupt transition settles quickly.
  auto gliss = run_signal(
      [](uint64_t i) {
        float t = i / 48000.f;
        return .5f * std::sin(2 * pi * (110 * t + 55 * t * t));
      },
      48000);
  CHECK(!gliss.empty() && gliss.back() > 205 && gliss.back() < 225);
  auto step = run_signal(
      [](uint64_t i) {
        float t = i / 48000.f, hz = i < 24000 ? 220 : 330;
        return .5f * std::sin(2 * pi * hz * t);
      },
      48000);
  CHECK(!step.empty() && step.back() > 320 && step.back() < 340);
  std::mt19937 rng(7);
  std::normal_distribution<float> noise(0, 1);
  float phase = 0;
  auto noisy = run_signal(
      [&](uint64_t) {
        phase += 2 * pi * 220 / 48000;
        return .5f * std::sin(phase) + .03535f * noise(rng); // 20 dB SNR
      },
      24000);
  CHECK(!noisy.empty() && std::fabs(cents(noisy.back(), 220)) < 15);
  // Hysteresis/release: silence must eventually unvoice without NaN.
  std::fill(block.begin(), block.end(), 0);
  for (int b = 0; b < 50; ++b) {
    a.tap(block.data(), block.size());
    a.run(8);
  }
  CHECK(!a.latest().voiced);
  std::sort(clean_errors.begin(), clean_errors.end());
  float mean_cents = 0;
  for (float error : clean_errors)
    mean_cents += error / clean_errors.size();
  std::printf(
      "quality summary: mean_abs_cents=%.3f max_cents=%.3f "
      "octave_errors=%d voiced_FP=0 voiced_FN=0 fixed_latency=%llu samples\n",
      mean_cents, clean_errors.back(), octave_errors,
      (unsigned long long)a.latency_samples());
  CHECK(test_voicing_hysteresis());
  CHECK(test_single_bad_hop());
  CHECK(test_double_bad_hop());
  CHECK(test_vibrato_continuity());
  CHECK(test_vocal_fry_continuity());
  CHECK(test_low_energy_vowel_tail());
  CHECK(test_fricative_rejection());
  CHECK(test_plosive_rejection());
  CHECK(test_legato_note_change());

  std::puts("all pitch tests passed");
  return 0;
}
