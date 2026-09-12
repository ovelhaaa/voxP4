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
constexpr size_t kTauCount = 185; // tau_max + interpolation look-ahead.

const std::array<YinDifferenceVariant, 5> kVariants{{
    YinDifferenceVariant::ReferenceScalar, YinDifferenceVariant::FmaScalar,
    YinDifferenceVariant::Muladd4Acc, YinDifferenceVariant::Fma4Acc,
    YinDifferenceVariant::Fma8Acc,
}};

struct Fixture {
  std::string name;
  std::vector<float> input_48k;
};

struct ErrorStats {
  double max_abs = 0.0;
  double max_rel = 0.0;
  long double squared = 0.0;
  uint64_t count = 0;
  uint32_t max_ulp = 0;
  std::vector<uint32_t> ulps;
  uint64_t nonfinite = 0;

  void add(float reference, float candidate) {
    if (!std::isfinite(reference) || !std::isfinite(candidate)) {
      ++nonfinite;
      return;
    }
    const double absolute = std::fabs(static_cast<double>(candidate) - reference);
    const double relative = absolute / std::max(std::fabs(static_cast<double>(reference)), 1e-30);
    max_abs = std::max(max_abs, absolute);
    max_rel = std::max(max_rel, relative);
    squared += absolute * absolute;
    ++count;
    uint32_t a = 0, b = 0;
    std::memcpy(&a, &reference, sizeof(a));
    std::memcpy(&b, &candidate, sizeof(b));
    auto ordered = [](uint32_t value) {
      return (value & 0x80000000U) ? ~value + 1U : value | 0x80000000U;
    };
    a = ordered(a);
    b = ordered(b);
    const uint32_t ulp = a > b ? a - b : b - a;
    max_ulp = std::max(max_ulp, ulp);
    ulps.push_back(ulp);
  }

  double rms() const {
    return count ? std::sqrt(static_cast<double>(squared / count)) : 0.0;
  }
  uint32_t percentile(double p) {
    if (ulps.empty())
      return 0;
    std::sort(ulps.begin(), ulps.end());
    const size_t index = static_cast<size_t>(
        std::ceil((p / 100.0) * ulps.size()) - 1.0);
    return ulps[std::min(index, ulps.size() - 1)];
  }
};

struct DirectStats {
  ErrorStats difference;
  ErrorStats cmnd;
  uint64_t hops = 0;
  uint64_t products = 0;
  uint64_t selected_tau_mismatches = 0;
  uint64_t candidate_valley_mismatches = 0;
  float max_period_error = 0.0f;
  float max_f0_error = 0.0f;
  float max_confidence_error = 0.0f;
};

struct TrackerRun {
  std::vector<PitchResult> results;
  std::vector<PitchMark> marks;
};

struct PitchStats {
  uint64_t reference_hops = 0;
  uint64_t candidate_hops = 0;
  uint64_t selected_tau_mismatches = 0;
  uint64_t voiced_raw_mismatches = 0;
  uint64_t voiced_stateful_mismatches = 0;
  uint64_t voiced_mismatches = 0;
  uint64_t onset_mismatches = 0;
  uint64_t pitch_changed_mismatches = 0;
  uint64_t track_state_mismatches = 0;
  uint64_t coast_mismatches = 0;
  uint64_t nonfinite_regressions = 0;
  uint64_t mark_count_mismatches = 0;
  uint64_t mark_position_mismatches = 0;
  uint64_t coherent_mark_mismatches = 0;
  float max_period_error = 0.0f;
  float max_f0_error = 0.0f;
  float max_confidence_error = 0.0f;
  float max_yin_min_error = 0.0f;
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
  char id[4]{};
  in.read(id, 4);
  (void)read_u32(in);
  char wave[4]{};
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
  if (input.empty() || source_rate <= 0.0 || target_rate <= 0.0)
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
                  size_t frames = 12000) {
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
                   700.0f, 900.0f})
    fixtures.push_back(generated("sine_" + std::to_string(static_cast<int>(hz)),
                                 sine(hz)));
  fixtures.push_back(generated("harmonic_220", [](uint64_t i) {
    const float phase = 2.0f * kPi * 220.0f * i / 48000.0f;
    return 0.45f * std::sin(phase) + 0.24f * std::sin(2.0f * phase) +
           0.12f * std::sin(3.0f * phase) + 0.06f * std::sin(5.0f * phase);
  }));
  std::mt19937 noise_rng(0x59494e44U);
  std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
  fixtures.push_back(generated("deterministic_noise", [&](uint64_t) {
    return uniform(noise_rng);
  }));
  fixtures.push_back(generated("boundary_low_amplitude", sine(220.0f, 0.0018f)));
  std::mt19937 moderate_rng(0x42344244U);
  std::normal_distribution<float> normal(0.0f, 0.11f);
  fixtures.push_back(generated("boundary_moderate_noise", [&](uint64_t i) {
    return 0.13f * std::sin(2.0f * kPi * 220.0f * i / 48000.0f) +
           normal(moderate_rng);
  }));
  fixtures.push_back(generated("boundary_low_edge_66", sine(66.0f, 0.08f)));
  fixtures.push_back(generated("boundary_high_edge_990", sine(990.0f, 0.08f)));

  uint32_t vocal_rate = 0;
  auto vocal = load_pcm16_first_channel(vocal_path, vocal_rate);
  if (!vocal.empty()) {
    // Start after the opening silence and keep the deterministic audit bounded.
    const size_t start = std::min<size_t>(vocal.size(), vocal_rate * 2U);
    std::vector<float> excerpt(vocal.begin() + start, vocal.end());
    Fixture fixture{"repository_vocal_wav",
                    resample_linear(excerpt, vocal_rate, 48000.0, 24000)};
    if (fixture.input_48k.size() >= kWindow * 4)
      fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

void calculate_cmnd(const std::array<float, kWindow + 1> &difference,
                    std::array<float, kWindow + 1> &cmnd) {
  cmnd[0] = 1.0f;
  double cumulative = 0.0;
  for (size_t tau = 1; tau <= kTauCount; ++tau) {
    cumulative += difference[tau];
    cmnd[tau] = cumulative > 1e-20
                    ? static_cast<float>(difference[tau] * tau / cumulative)
                    : 1.0f;
  }
}

size_t select_tau(const std::array<float, kWindow + 1> &cmnd) {
  constexpr size_t tau_min = 12;
  constexpr size_t tau_max = 184;
  size_t candidate = 0;
  for (size_t tau = tau_min; tau <= tau_max; ++tau) {
    if (cmnd[tau] < 0.15f) {
      while (tau < tau_max && cmnd[tau + 1] < cmnd[tau])
        ++tau;
      candidate = tau;
      break;
    }
  }
  if (!candidate) {
    candidate = tau_min;
    for (size_t tau = tau_min + 1; tau <= tau_max; ++tau)
      if (cmnd[tau] < cmnd[candidate])
        candidate = tau;
  }
  return candidate;
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

DirectStats direct_audit(const Fixture &fixture, YinDifferenceVariant variant) {
  DirectStats stats{};
  const auto analysis = resample_linear(fixture.input_48k, 48000.0, 12000.0);
  if (analysis.size() < kWindow)
    return stats;
  const size_t available = 1 + (analysis.size() - kWindow) / kHop;
  const size_t hops = std::min<size_t>(available, 32);
  std::array<float, kWindow + 1> reference{}, candidate{};
  std::array<float, kWindow + 1> reference_cmnd{}, candidate_cmnd{};
  for (size_t hop = 0; hop < hops; ++hop) {
    const float *window = analysis.data() + hop * kHop;
    yin_difference_compute(YinDifferenceVariant::ReferenceScalar, window,
                           kWindow, kTauCount, reference.data());
    yin_difference_compute(variant, window, kWindow, kTauCount,
                           candidate.data());
    calculate_cmnd(reference, reference_cmnd);
    calculate_cmnd(candidate, candidate_cmnd);
    for (size_t tau = 0; tau <= kTauCount; ++tau) {
      stats.difference.add(reference[tau], candidate[tau]);
      stats.cmnd.add(reference_cmnd[tau], candidate_cmnd[tau]);
    }
    const size_t reference_tau = select_tau(reference_cmnd);
    const size_t candidate_tau = select_tau(candidate_cmnd);
    stats.selected_tau_mismatches += reference_tau != candidate_tau;
    stats.candidate_valley_mismatches += reference_tau != candidate_tau;
    float rp = 0, rf = 0, rc = 0, cp = 0, cf = 0, cc = 0;
    period_result(reference, reference_cmnd, reference_tau, rp, rf, rc);
    period_result(candidate, candidate_cmnd, candidate_tau, cp, cf, cc);
    stats.max_period_error = std::max(stats.max_period_error, std::fabs(cp - rp));
    stats.max_f0_error = std::max(stats.max_f0_error, std::fabs(cf - rf));
    stats.max_confidence_error =
        std::max(stats.max_confidence_error, std::fabs(cc - rc));
    ++stats.hops;
    stats.products += yin_difference_product_count(kWindow, kTauCount);
  }
  return stats;
}

TrackerRun tracker_run(const Fixture &fixture, YinDifferenceVariant variant) {
  PitchAnalysisConfig config{};
  config.yin_difference = variant;
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

PitchStats compare_tracker(const TrackerRun &reference,
                           const TrackerRun &candidate) {
  PitchStats stats{};
  stats.reference_hops = reference.results.size();
  stats.candidate_hops = candidate.results.size();
  const size_t count = std::min(reference.results.size(), candidate.results.size());
  for (size_t i = 0; i < count; ++i) {
    const auto &a = reference.results[i];
    const auto &b = candidate.results[i];
    stats.selected_tau_mismatches += a.yin_tau != b.yin_tau;
    stats.voiced_raw_mismatches += a.voiced_raw != b.voiced_raw;
    stats.voiced_stateful_mismatches += a.voiced_stateful != b.voiced_stateful;
    stats.voiced_mismatches += a.voiced != b.voiced;
    stats.onset_mismatches += a.onset != b.onset;
    stats.pitch_changed_mismatches += a.pitch_changed != b.pitch_changed;
    stats.track_state_mismatches += a.pitch_track_state != b.pitch_track_state;
    stats.coast_mismatches += a.coast_remaining != b.coast_remaining;
    stats.coherent_mark_mismatches += a.coherent_marks != b.coherent_marks;
    stats.max_period_error =
        std::max(stats.max_period_error, std::fabs(a.period_samples - b.period_samples));
    stats.max_f0_error =
        std::max(stats.max_f0_error, std::fabs(a.frequency_hz - b.frequency_hz));
    stats.max_confidence_error =
        std::max(stats.max_confidence_error, std::fabs(a.confidence - b.confidence));
    stats.max_yin_min_error =
        std::max(stats.max_yin_min_error, std::fabs(a.yin_min - b.yin_min));
    if (std::isfinite(a.frequency_hz) && !std::isfinite(b.frequency_hz))
      ++stats.nonfinite_regressions;
  }
  stats.mark_count_mismatches = reference.marks.size() != candidate.marks.size();
  const size_t mark_count = std::min(reference.marks.size(), candidate.marks.size());
  for (size_t i = 0; i < mark_count; ++i)
    stats.mark_position_mismatches +=
        reference.marks[i].sample_position != candidate.marks[i].sample_position;
  return stats;
}

bool eligible(const DirectStats &direct, const PitchStats &pitch) {
  return direct.difference.nonfinite == 0 && direct.cmnd.nonfinite == 0 &&
         direct.selected_tau_mismatches == 0 &&
         direct.max_period_error <= 1e-3f && direct.max_f0_error <= 0.01f &&
         direct.max_confidence_error <= 1e-5f &&
         pitch.nonfinite_regressions == 0 &&
         pitch.selected_tau_mismatches == 0 &&
         pitch.voiced_raw_mismatches == 0 &&
         pitch.voiced_stateful_mismatches == 0 &&
         pitch.voiced_mismatches == 0 && pitch.track_state_mismatches == 0 &&
         pitch.mark_count_mismatches == 0 &&
         pitch.mark_position_mismatches == 0;
}
} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path root = VOXP4_SOURCE_DIR;
  std::filesystem::path artifact_dir = root / "artifacts" / "alpha01b";
  bool verify_only = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--verify")
      verify_only = true;
    else if (std::string(argv[i]) == "--artifacts" && i + 1 < argc)
      artifact_dir = argv[++i];
  }
  const auto fixtures = make_fixtures(
      root / "samples" / "dry-acapella-leave-this-place_95bpm.wav");
  if (fixtures.size() < 14) {
    std::fprintf(stderr, "YIN audit: repository vocal fixture unavailable\n");
    return 1;
  }
  std::filesystem::create_directories(artifact_dir);
  std::ofstream numeric;
  std::ofstream pitch_csv;
  if (!verify_only) {
    numeric.open(artifact_dir / "b4b4d_yin_numeric_equivalence.csv");
    pitch_csv.open(artifact_dir / "b4b4d_pitch_equivalence.csv");
    numeric << "fixture,variant,hops,taus_evaluated,products_per_hop,"
               "reference_products,candidate_products,difference_max_abs,"
               "difference_max_rel,difference_rms,max_ulp,median_ulp,p95_ulp,"
               "p99_ulp,cmnd_max_abs,cmnd_max_rel,cmnd_rms,nonfinite,"
               "selected_tau_mismatches,candidate_valley_mismatches,"
               "max_period_error_samples,max_f0_error_hz,"
               "max_confidence_error,eligible\n";
    pitch_csv << "fixture,variant,reference_hops,candidate_hops,"
                 "selected_tau_mismatches,voiced_raw_mismatches,"
                 "voiced_stateful_mismatches,voiced_mismatches,onset_mismatches,"
                 "pitch_changed_mismatches,track_state_mismatches,coast_mismatches,"
                 "coherent_mark_mismatches,pitch_mark_count_mismatches,"
                 "pitch_mark_position_mismatches,nonfinite_regressions,"
                 "max_period_error_samples,max_f0_error_hz,"
                 "max_confidence_error,max_yin_min_error,eligible\n";
    numeric << std::setprecision(12);
    pitch_csv << std::setprecision(12);
  }

  bool production_pass = true;
  for (const auto &fixture : fixtures) {
    const auto reference_tracker =
        tracker_run(fixture, YinDifferenceVariant::ReferenceScalar);
    for (const auto variant : kVariants) {
      auto direct = direct_audit(fixture, variant);
      const auto candidate_tracker = tracker_run(fixture, variant);
      const auto pitch = compare_tracker(reference_tracker, candidate_tracker);
      const bool pass = eligible(direct, pitch);
      if (variant == YinDifferenceVariant::Fma8Acc)
        production_pass = production_pass && pass;
      if (!verify_only) {
        numeric << fixture.name << ',' << yin_difference_variant_name(variant)
                << ',' << direct.hops << ',' << kTauCount << ','
                << yin_difference_product_count(kWindow, kTauCount) << ','
                << direct.products << ',' << direct.products << ','
                << direct.difference.max_abs << ',' << direct.difference.max_rel
                << ',' << direct.difference.rms() << ','
                << direct.difference.max_ulp << ','
                << direct.difference.percentile(50) << ','
                << direct.difference.percentile(95) << ','
                << direct.difference.percentile(99) << ','
                << direct.cmnd.max_abs << ',' << direct.cmnd.max_rel << ','
                << direct.cmnd.rms() << ','
                << direct.difference.nonfinite + direct.cmnd.nonfinite << ','
                << direct.selected_tau_mismatches << ','
                << direct.candidate_valley_mismatches << ','
                << direct.max_period_error << ',' << direct.max_f0_error << ','
                << direct.max_confidence_error << ',' << (pass ? "yes" : "no")
                << '\n';
        pitch_csv << fixture.name << ',' << yin_difference_variant_name(variant)
                  << ',' << pitch.reference_hops << ',' << pitch.candidate_hops
                  << ',' << pitch.selected_tau_mismatches << ','
                  << pitch.voiced_raw_mismatches << ','
                  << pitch.voiced_stateful_mismatches << ','
                  << pitch.voiced_mismatches << ',' << pitch.onset_mismatches
                  << ',' << pitch.pitch_changed_mismatches << ','
                  << pitch.track_state_mismatches << ',' << pitch.coast_mismatches
                  << ',' << pitch.coherent_mark_mismatches << ','
                  << pitch.mark_count_mismatches << ','
                  << pitch.mark_position_mismatches << ','
                  << pitch.nonfinite_regressions << ',' << pitch.max_period_error
                  << ',' << pitch.max_f0_error << ','
                  << pitch.max_confidence_error << ',' << pitch.max_yin_min_error
                  << ',' << (pass ? "yes" : "no") << '\n';
      }
    }
  }
  std::printf("YIN kernel audit: fixtures=%zu products/hop=%llu production=%s\n",
              fixtures.size(),
              static_cast<unsigned long long>(
                  yin_difference_product_count(kWindow, kTauCount)),
              production_pass ? "YIN_DIFF_FMA_8ACC PASS"
                              : "YIN_DIFF_FMA_8ACC FAIL");
  return production_pass ? 0 : 1;
}
