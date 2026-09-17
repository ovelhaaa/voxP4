#include "pitch_analysis.h"
#include "yin_detector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kWindow = 512;
constexpr size_t kHop = 60;
constexpr size_t kTauMax = 184;
constexpr size_t kTauCount = kTauMax + 1; // interpolation look-ahead
constexpr size_t kTauMin = 12;
constexpr float kThreshold = 0.15f;

const std::array<YinCmndVariant, 4> kVariants{{
    YinCmndVariant::ReferenceDouble, YinCmndVariant::DoubleSumF32Div,
    YinCmndVariant::F32, YinCmndVariant::F32Compensated,
}};

struct Fixture {
  std::string name;
  std::vector<float> input_48k;
};

uint16_t read_u16(std::istream &in) {
  unsigned char b[2]{};
  in.read(reinterpret_cast<char *>(b), 2);
  return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
uint32_t read_u32(std::istream &in) {
  unsigned char b[4]{};
  in.read(reinterpret_cast<char *>(b), 4);
  return static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) |
                               (b[3] << 24));
}

std::vector<float> load_pcm16_first_channel(const std::filesystem::path &path,
                                            uint32_t &sample_rate) {
  std::ifstream in(path, std::ios::binary);
  char id[4]{}, wave[4]{};
  in.read(id, 4);
  (void)read_u32(in);
  in.read(wave, 4);
  if (!in || std::memcmp(id, "RIFF", 4) || std::memcmp(wave, "WAVE", 4))
    return {};
  uint16_t format = 0, channels = 0, bits = 0;
  std::vector<unsigned char> data;
  while (in.read(id, 4)) {
    const uint32_t size = read_u32(in);
    if (!std::memcmp(id, "fmt ", 4)) {
      format = read_u16(in);
      channels = read_u16(in);
      sample_rate = read_u32(in);
      (void)read_u32(in);
      (void)read_u16(in);
      bits = read_u16(in);
      if (size > 16)
        in.seekg(size - 16, std::ios::cur);
    } else if (!std::memcmp(id, "data", 4)) {
      data.resize(size);
      in.read(reinterpret_cast<char *>(data.data()), size);
    } else {
      in.seekg(size, std::ios::cur);
    }
    if (size & 1U)
      in.seekg(1, std::ios::cur);
  }
  if (format != 1 || bits != 16 || channels == 0 || data.empty())
    return {};
  const size_t frames = data.size() / (channels * 2U);
  std::vector<float> output(frames);
  for (size_t i = 0; i < frames; ++i) {
    const size_t offset = i * channels * 2U;
    const int16_t value = static_cast<int16_t>(
        static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8)));
    output[i] = value * (1.0f / 32768.0f);
  }
  return output;
}

std::vector<float> resample_linear(const std::vector<float> &input,
                                   double source_rate, double target_rate,
                                   size_t maximum_frames = 0) {
  if (input.empty())
    return {};
  size_t count = static_cast<size_t>(input.size() * target_rate / source_rate);
  if (maximum_frames)
    count = std::min(count, maximum_frames);
  std::vector<float> output(count);
  for (size_t i = 0; i < count; ++i) {
    const double position = i * source_rate / target_rate;
    const size_t left = std::min(static_cast<size_t>(position), input.size() - 1);
    const size_t right = std::min(left + 1, input.size() - 1);
    const float fraction = static_cast<float>(position - left);
    output[i] = input[left] + fraction * (input[right] - input[left]);
  }
  return output;
}

Fixture generated(const std::string &name,
                  const std::function<float(uint64_t)> &generator,
                  size_t frames = 48000) {
  Fixture result{name, std::vector<float>(frames)};
  for (uint64_t i = 0; i < result.input_48k.size(); ++i)
    result.input_48k[i] = generator(i);
  return result;
}

std::vector<Fixture> make_fixtures(const std::filesystem::path &vocal_path) {
  std::vector<Fixture> fixtures;
  fixtures.push_back(generated("silence", [](uint64_t) { return 0.0f; }));
  auto sine = [](float hz, float amplitude = 0.5f) {
    return [=](uint64_t i) {
      return amplitude * std::sin(2.0f * kPi * hz * i / 48000.0f);
    };
  };
  for (float hz : {80.0f, 110.0f, 147.0f, 220.0f, 330.0f, 440.0f,
                   700.0f, 900.0f, 66.0f, 990.0f})
    fixtures.push_back(generated("sine_" + std::to_string(static_cast<int>(hz)),
                                 sine(hz, hz == 66.0f || hz == 990.0f ? 0.08f
                                                                     : 0.5f)));
  fixtures.push_back(generated("harmonic_220", [](uint64_t i) {
    const float phase = 2.0f * kPi * 220.0f * i / 48000.0f;
    return 0.45f * std::sin(phase) + 0.24f * std::sin(2.0f * phase) +
           0.12f * std::sin(3.0f * phase) + 0.06f * std::sin(5.0f * phase);
  }));
  uint32_t noise_state = 0x59494e44U;
  fixtures.push_back(generated("deterministic_noise", [&](uint64_t) {
    noise_state = noise_state * 1664525U + 1013904223U;
    return static_cast<int32_t>(noise_state) * (0.5f / 2147483648.0f);
  }));
  fixtures.push_back(generated("low_level_tone", sine(220.0f, 0.0018f)));
  for (size_t index = 0; index < 9; ++index) {
    const float noise_gain = 0.025f + static_cast<float>(index) * 0.00625f;
    uint32_t state = 0x42344a00U + static_cast<uint32_t>(index);
    fixtures.push_back(generated(
        "threshold_tone_noise_" + std::to_string(index),
        [state, noise_gain](uint64_t i) mutable {
          state = state * 1664525U + 1013904223U;
          const float noise = static_cast<int32_t>(state) /
                              2147483648.0f;
          return 0.13f * std::sin(2.0f * kPi * 220.0f * i / 48000.0f) +
                 noise_gain * noise;
        }));
  }
  uint32_t vocal_rate = 0;
  auto vocal = load_pcm16_first_channel(vocal_path, vocal_rate);
  if (!vocal.empty()) {
    const size_t start = std::min<size_t>(vocal.size(), vocal_rate * 2U);
    std::vector<float> excerpt(vocal.begin() + start, vocal.end());
    Fixture fixture{"repository_vocal_wav",
                    resample_linear(excerpt, vocal_rate, 48000.0, 96000)};
    if (fixture.input_48k.size() >= kWindow * 4)
      fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

uint32_t ulp_distance(float a, float b) {
  uint32_t ai = 0, bi = 0;
  std::memcpy(&ai, &a, sizeof(ai));
  std::memcpy(&bi, &b, sizeof(bi));
  const auto ordered = [](uint32_t value) {
    return (value & 0x80000000U) ? ~value + 1U : value | 0x80000000U;
  };
  ai = ordered(ai);
  bi = ordered(bi);
  return ai > bi ? ai - bi : bi - ai;
}

struct ErrorStats {
  double max_abs = 0.0, max_rel = 0.0;
  long double squared = 0.0;
  uint64_t count = 0, nonfinite = 0;
  std::vector<uint32_t> ulps;
  void add(float reference, float candidate) {
    if (!std::isfinite(reference) || !std::isfinite(candidate)) {
      ++nonfinite;
      return;
    }
    const double absolute = std::fabs(static_cast<double>(candidate) - reference);
    const double relative = absolute /
        std::max(std::fabs(static_cast<double>(reference)), 1e-30);
    max_abs = std::max(max_abs, absolute);
    max_rel = std::max(max_rel, relative);
    squared += absolute * absolute;
    ++count;
    ulps.push_back(ulp_distance(reference, candidate));
  }
  double rms() const {
    return count ? std::sqrt(static_cast<double>(squared / count)) : 0.0;
  }
  uint32_t percentile(unsigned p) {
    if (ulps.empty())
      return 0;
    std::sort(ulps.begin(), ulps.end());
    const size_t index = std::min(
        ulps.size() - 1, (ulps.size() * static_cast<size_t>(p) + 99) / 100 - 1);
    return ulps[index];
  }
};

struct SearchResult {
  size_t first_crossing = 0, valley = 0, selected = 0, runner_up = 0;
  float minimum = 1.0f, selected_value = 1.0f, runner_up_value = 1.0f;
  float closest_threshold = 1.0f;
};

SearchResult search(const std::array<float, kWindow + 1> &cmnd) {
  SearchResult result{};
  size_t candidate = 0;
  for (size_t tau = kTauMin; tau <= kTauMax; ++tau) {
    result.minimum = std::min(result.minimum, cmnd[tau]);
    result.closest_threshold = std::min(result.closest_threshold,
                                        std::fabs(cmnd[tau] - kThreshold));
    if (!candidate && cmnd[tau] < kThreshold) {
      result.first_crossing = tau;
      while (tau < kTauMax && cmnd[tau + 1] < cmnd[tau])
        ++tau;
      result.valley = tau;
      candidate = tau;
    }
  }
  if (!candidate) {
    candidate = kTauMin;
    for (size_t tau = kTauMin + 1; tau <= kTauMax; ++tau)
      if (cmnd[tau] < cmnd[candidate])
        candidate = tau;
    result.valley = candidate;
  }
  result.selected = candidate;
  result.selected_value = cmnd[candidate];
  result.runner_up = candidate == kTauMin ? kTauMin + 1 : kTauMin;
  for (size_t tau = kTauMin; tau <= kTauMax; ++tau)
    if (tau != candidate && cmnd[tau] < cmnd[result.runner_up])
      result.runner_up = tau;
  result.runner_up_value = cmnd[result.runner_up];
  return result;
}

void period_result(const std::array<float, kWindow + 1> &difference,
                   const std::array<float, kWindow + 1> &cmnd, size_t tau,
                   float &period, float &frequency, float &confidence) {
  const float y0 = difference[tau - 1], y1 = difference[tau];
  const float y2 = difference[tau + 1];
  const float denominator = y0 - 2.0f * y1 + y2;
  float offset = std::fabs(denominator) > 1e-12f
                     ? 0.5f * (y0 - y2) / denominator
                     : 0.0f;
  offset = std::clamp(offset, -0.5f, 0.5f);
  period = tau + offset;
  frequency = period > 0.0f ? 12000.0f / period : 0.0f;
  confidence = std::clamp(1.0f - cmnd[tau], 0.0f, 1.0f);
}

struct DirectStats {
  ErrorStats error;
  uint64_t hops = 0, threshold_crossing_mismatches = 0;
  uint64_t descent_path_mismatches = 0, valley_mismatches = 0;
  uint64_t selected_tau_mismatches = 0, nonfinite = 0;
  float max_period_error = 0.0f, max_f0_error = 0.0f;
  float max_confidence_error = 0.0f;
  float min_threshold_margin = std::numeric_limits<float>::infinity();
  float min_runner_up_margin = std::numeric_limits<float>::infinity();
};

DirectStats direct_audit(const Fixture &fixture, YinCmndVariant variant,
                         std::ofstream *threshold_csv = nullptr) {
  DirectStats stats{};
  const auto analysis = resample_linear(fixture.input_48k, 48000.0, 12000.0);
  if (analysis.size() < kWindow)
    return stats;
  const size_t available = 1 + (analysis.size() - kWindow) / kHop;
  const size_t hops = std::min<size_t>(available, 128);
  std::array<float, kWindow + 1> difference{}, reference{}, candidate{};
  YinIncrementalDifference incremental;
  if (!incremental.init(YinDifferenceVariant::IncrementalF32, kWindow, kHop,
                        kTauCount, 64))
    return stats;
  for (size_t hop = 0; hop < hops; ++hop) {
    incremental.compute(analysis.data() + hop * kHop, difference.data());
    YIN_CMND_REFERENCE_DOUBLE(difference.data(), kTauCount, reference.data());
    yin_cmnd_compute(variant, difference.data(), kTauCount, candidate.data());
    for (size_t tau = 0; tau <= kTauCount; ++tau)
      stats.error.add(reference[tau], candidate[tau]);
    const SearchResult a = search(reference), b = search(candidate);
    stats.threshold_crossing_mismatches += a.first_crossing != b.first_crossing;
    stats.descent_path_mismatches +=
        a.first_crossing != b.first_crossing || a.valley != b.valley;
    stats.valley_mismatches += a.valley != b.valley;
    stats.selected_tau_mismatches += a.selected != b.selected;
    stats.min_threshold_margin = std::min(
        stats.min_threshold_margin,
        std::min(std::fabs(a.selected_value - kThreshold),
                 std::fabs(a.minimum - kThreshold)));
    stats.min_runner_up_margin = std::min(
        stats.min_runner_up_margin, a.runner_up_value - a.selected_value);
    float ap = 0, af = 0, ac = 0, bp = 0, bf = 0, bc = 0;
    period_result(difference, reference, a.selected, ap, af, ac);
    period_result(difference, candidate, b.selected, bp, bf, bc);
    stats.max_period_error = std::max(stats.max_period_error, std::fabs(bp - ap));
    stats.max_f0_error = std::max(stats.max_f0_error, std::fabs(bf - af));
    stats.max_confidence_error =
        std::max(stats.max_confidence_error, std::fabs(bc - ac));
    stats.nonfinite += !std::isfinite(bp) || !std::isfinite(bf) ||
                       !std::isfinite(bc);
    if (threshold_csv)
      *threshold_csv << fixture.name << ',' << yin_cmnd_variant_name(variant)
                     << ',' << hop << ',' << a.minimum << ','
                     << a.selected_value << ','
                     << std::fabs(a.minimum - kThreshold) << ','
                     << std::fabs(a.selected_value - kThreshold) << ','
                     << a.closest_threshold << ',' << a.selected << ','
                     << a.runner_up << ',' << a.runner_up_value << ','
                     << a.runner_up_value - a.selected_value << '\n';
    ++stats.hops;
  }
  return stats;
}

struct TrackerRun {
  std::vector<PitchResult> results;
  std::vector<PitchMark> marks;
};
TrackerRun tracker_run(const Fixture &fixture, YinCmndVariant variant) {
  PitchAnalysisConfig config{};
  config.yin_difference = YinDifferenceVariant::IncrementalF32;
  config.yin_incremental_rebase_hops = 64;
  config.yin_cmnd = variant;
  config.pitch_mark_ncc = PitchMarkNccVariant::ReuseAa;
  PitchAnalysis tracker;
  TrackerRun run;
  if (!tracker.init(config))
    return run;
  for (size_t offset = 0; offset < fixture.input_48k.size(); offset += 64) {
    const size_t count = std::min<size_t>(64, fixture.input_48k.size() - offset);
    tracker.tap(fixture.input_48k.data() + offset, count);
    while (tracker.run(1))
      run.results.push_back(tracker.latest());
  }
  std::array<PitchMark, 64> marks{};
  const size_t count = tracker.marks(0, std::numeric_limits<uint64_t>::max(),
                                     marks.data(), marks.size());
  run.marks.assign(marks.begin(), marks.begin() + count);
  return run;
}

struct PitchStats {
  uint64_t reference_hops = 0, candidate_hops = 0;
  uint64_t selected_tau = 0, voiced_raw = 0, voiced_stateful = 0, voiced = 0;
  uint64_t onset = 0, pitch_changed = 0, track_state = 0;
  uint64_t mark_count = 0, mark_position = 0, nonfinite = 0;
  float period = 0.0f, f0 = 0.0f, confidence = 0.0f, yin_min = 0.0f;
};
PitchStats compare_tracker(const TrackerRun &a, const TrackerRun &b) {
  PitchStats s{};
  s.reference_hops = a.results.size();
  s.candidate_hops = b.results.size();
  const size_t count = std::min(a.results.size(), b.results.size());
  for (size_t i = 0; i < count; ++i) {
    const auto &x = a.results[i];
    const auto &y = b.results[i];
    s.selected_tau += x.yin_tau != y.yin_tau;
    s.voiced_raw += x.voiced_raw != y.voiced_raw;
    s.voiced_stateful += x.voiced_stateful != y.voiced_stateful;
    s.voiced += x.voiced != y.voiced;
    s.onset += x.onset != y.onset;
    s.pitch_changed += x.pitch_changed != y.pitch_changed;
    s.track_state += x.pitch_track_state != y.pitch_track_state;
    s.period = std::max(s.period, std::fabs(x.period_samples - y.period_samples));
    s.f0 = std::max(s.f0, std::fabs(x.frequency_hz - y.frequency_hz));
    s.confidence = std::max(s.confidence, std::fabs(x.confidence - y.confidence));
    s.yin_min = std::max(s.yin_min, std::fabs(x.yin_min - y.yin_min));
    s.nonfinite += std::isfinite(x.frequency_hz) && !std::isfinite(y.frequency_hz);
  }
  s.mark_count = a.marks.size() != b.marks.size();
  const size_t marks = std::min(a.marks.size(), b.marks.size());
  for (size_t i = 0; i < marks; ++i)
    s.mark_position += a.marks[i].sample_position != b.marks[i].sample_position;
  return s;
}

bool eligible(const DirectStats &d, const PitchStats &p) {
  return d.error.max_abs <= 1e-5 && d.max_period_error <= 1e-3f &&
         d.max_f0_error <= 0.01f && d.max_confidence_error <= 1e-5f &&
         d.threshold_crossing_mismatches == 0 &&
         d.descent_path_mismatches == 0 && d.valley_mismatches == 0 &&
         d.selected_tau_mismatches == 0 && d.nonfinite == 0 &&
         d.error.nonfinite == 0 && p.selected_tau == 0 && p.voiced_raw == 0 &&
         p.voiced_stateful == 0 && p.voiced == 0 && p.onset == 0 &&
         p.pitch_changed == 0 && p.track_state == 0 && p.mark_count == 0 &&
         p.mark_position == 0 && p.nonfinite == 0;
}

void write_long_audit(std::ofstream &numeric, std::ofstream &threshold,
                      YinCmndVariant variant) {
  constexpr size_t kSamples = 600 * 12000 + kWindow;
  std::vector<float> stream(kSamples);
  uint32_t state = 0x42344245U;
  for (size_t i = 0; i < stream.size(); ++i) {
    state = state * 1664525U + 1013904223U;
    const float noise = static_cast<int32_t>(state) / 2147483648.0f;
    const float seconds = static_cast<float>(i) / 12000.0f;
    const float f0 = seconds < 300.0f ? 147.0f : 220.0f;
    const float phase = 2.0f * kPi * f0 * seconds;
    stream[i] = 0.30f * std::sin(phase) + 0.11f * std::sin(2.0f * phase) +
                0.02f * noise;
  }
  std::array<float, kWindow + 1> difference{}, reference{}, candidate{};
  YinIncrementalDifference incremental;
  if (!incremental.init(YinDifferenceVariant::IncrementalF32, kWindow, kHop,
                        kTauCount, 64))
    return;
  DirectStats stats{};
  const size_t hops = 1 + (stream.size() - kWindow) / kHop;
  for (size_t hop = 0; hop < hops; ++hop) {
    incremental.compute(stream.data() + hop * kHop, difference.data());
    YIN_CMND_REFERENCE_DOUBLE(difference.data(), kTauCount, reference.data());
    yin_cmnd_compute(variant, difference.data(), kTauCount, candidate.data());
    for (size_t tau = 0; tau <= kTauCount; ++tau)
      stats.error.add(reference[tau], candidate[tau]);
    const SearchResult a = search(reference), b = search(candidate);
    stats.threshold_crossing_mismatches += a.first_crossing != b.first_crossing;
    stats.descent_path_mismatches +=
        a.first_crossing != b.first_crossing || a.valley != b.valley;
    stats.valley_mismatches += a.valley != b.valley;
    stats.selected_tau_mismatches += a.selected != b.selected;
    stats.min_threshold_margin = std::min(
        stats.min_threshold_margin,
        std::min(std::fabs(a.minimum - kThreshold),
                 std::fabs(a.selected_value - kThreshold)));
    stats.min_runner_up_margin = std::min(
        stats.min_runner_up_margin, a.runner_up_value - a.selected_value);
    ++stats.hops;
  }
  numeric << "long_10min," << yin_cmnd_variant_name(variant) << ',' << stats.hops
          << ',' << stats.error.max_abs << ',' << stats.error.max_rel << ','
          << stats.error.rms() << ',' << stats.error.percentile(50) << ','
          << stats.error.percentile(95) << ',' << stats.error.percentile(99)
          << ',' << (stats.error.ulps.empty() ? 0 : stats.error.ulps.back())
          << ',' << stats.threshold_crossing_mismatches << ','
          << stats.descent_path_mismatches << ',' << stats.valley_mismatches
          << ',' << stats.selected_tau_mismatches << ",0,0,0,"
          << stats.error.nonfinite << ',' << (eligible(stats, {}) ? "yes" : "no")
          << '\n';
  threshold << "long_10min," << yin_cmnd_variant_name(variant)
            << ",summary,,," << stats.min_threshold_margin
            << ",,,summary,,," << stats.min_runner_up_margin << '\n';
}
} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path root = VOXP4_SOURCE_DIR;
  std::filesystem::path output = root / "artifacts" / "alpha01b";
  bool verify = false, long_stream = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--verify")
      verify = true;
    else if (std::string(argv[i]) == "--long")
      long_stream = true;
    else if (std::string(argv[i]) == "--artifacts" && i + 1 < argc)
      output = argv[++i];
  }
  const auto fixtures = make_fixtures(
      root / "samples" / "dry-acapella-leave-this-place_95bpm.wav");
  if (fixtures.size() < 23) {
    std::fprintf(stderr, "B4B.4J: fixture corpus incomplete (%zu)\n",
                 fixtures.size());
    return 1;
  }
  std::filesystem::create_directories(output);
  std::ofstream numeric, threshold, pitch;
  if (!verify) {
    numeric.open(output / "b4b4j_yin_cmnd_equivalence.csv");
    threshold.open(output / "b4b4j_threshold_margin.csv");
    pitch.open(output / "b4b4j_pitch_equivalence.csv");
    numeric << "fixture,variant,hops,max_abs_error,max_relative_error,rms_error,"
               "ulp_p50,ulp_p95,ulp_p99,ulp_max,threshold_crossing_mismatches,"
               "descent_path_mismatches,valley_mismatches,selected_tau_mismatches,"
               "max_period_error_samples,max_f0_error_hz,max_confidence_error,"
               "nonfinite_regressions,eligible\n";
    threshold << "fixture,variant,hop,minimum_cmnd,selected_cmnd,"
                 "minimum_distance_to_threshold,selected_distance_to_threshold,"
                 "closest_cmnd_distance_to_threshold,selected_tau,runner_up_tau,"
                 "runner_up_cmnd,selected_runner_up_margin\n";
    pitch << "fixture,variant,reference_hops,candidate_hops,"
             "selected_tau_mismatches,voiced_raw_mismatches,"
             "voiced_stateful_mismatches,final_voiced_mismatches,onset_mismatches,"
             "pitch_changed_mismatches,track_state_mismatches,"
             "pitch_mark_count_mismatches,pitch_mark_position_mismatches,"
             "max_period_error_samples,max_f0_error_hz,max_confidence_error,"
             "max_yin_min_error,nonfinite_regressions,eligible\n";
    numeric << std::setprecision(12);
    threshold << std::setprecision(12);
    pitch << std::setprecision(12);
  }
  bool pass = true;
  for (const auto &fixture : fixtures) {
    const TrackerRun reference =
        tracker_run(fixture, YinCmndVariant::ReferenceDouble);
    for (const auto variant : kVariants) {
      DirectStats direct = direct_audit(
          fixture, variant, verify ? nullptr : &threshold);
      const PitchStats tracked =
          compare_tracker(reference, tracker_run(fixture, variant));
      const bool candidate_pass = eligible(direct, tracked);
      if (variant == YinCmndVariant::F32Compensated)
        pass = pass && candidate_pass;
      if (!verify) {
        const uint32_t p50 = direct.error.percentile(50);
        const uint32_t p95 = direct.error.percentile(95);
        const uint32_t p99 = direct.error.percentile(99);
        const uint32_t maximum = direct.error.ulps.empty()
                                     ? 0
                                     : *std::max_element(direct.error.ulps.begin(),
                                                         direct.error.ulps.end());
        numeric << fixture.name << ',' << yin_cmnd_variant_name(variant) << ','
                << direct.hops << ',' << direct.error.max_abs << ','
                << direct.error.max_rel << ',' << direct.error.rms() << ','
                << p50 << ',' << p95 << ',' << p99 << ',' << maximum << ','
                << direct.threshold_crossing_mismatches << ','
                << direct.descent_path_mismatches << ','
                << direct.valley_mismatches << ','
                << direct.selected_tau_mismatches << ','
                << direct.max_period_error << ',' << direct.max_f0_error << ','
                << direct.max_confidence_error << ','
                << direct.error.nonfinite + direct.nonfinite << ','
                << (candidate_pass ? "yes" : "no") << '\n';
        pitch << fixture.name << ',' << yin_cmnd_variant_name(variant) << ','
              << tracked.reference_hops << ',' << tracked.candidate_hops << ','
              << tracked.selected_tau << ',' << tracked.voiced_raw << ','
              << tracked.voiced_stateful << ',' << tracked.voiced << ','
              << tracked.onset << ',' << tracked.pitch_changed << ','
              << tracked.track_state << ',' << tracked.mark_count << ','
              << tracked.mark_position << ',' << tracked.period << ','
              << tracked.f0 << ',' << tracked.confidence << ','
              << tracked.yin_min << ',' << tracked.nonfinite << ','
              << (candidate_pass ? "yes" : "no") << '\n';
      }
    }
  }
  if (!verify && long_stream)
    for (const auto variant : kVariants)
      write_long_audit(numeric, threshold, variant);
  std::printf("B4B.4J host CMND audit: fixtures=%zu taus/hop=%zu long=%s "
              "F32_COMPENSATED=%s\n",
              fixtures.size(), kTauCount, long_stream ? "yes" : "no",
              pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
