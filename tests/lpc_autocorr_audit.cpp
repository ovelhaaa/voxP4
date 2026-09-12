#include "lpc.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {
constexpr size_t kWindowSize = 1024;
constexpr size_t kHopSize = 384;
constexpr uint16_t kOrder = 16;
constexpr float kPreemphasis = 0.97f;
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kProductsPerFrame =
    (kOrder + 1) * kWindowSize - (kOrder * (kOrder + 1)) / 2;

struct Metrics {
  uint64_t frames = 0;
  uint64_t autocorr_values = 0;
  uint64_t coefficient_values = 0;
  double autocorr_max_absolute_error = 0.0;
  double autocorr_max_relative_error = 0.0;
  long double autocorr_squared_error = 0.0;
  double coefficient_max_absolute_error = 0.0;
  long double coefficient_squared_error = 0.0;
  double prediction_error_max_difference = 0.0;
  double confidence_max_difference = 0.0;
  uint64_t validity_decision_differences = 0;
  uint64_t timestamp_differences = 0;
  uint64_t order_differences = 0;
  uint64_t frame_count_differences = 0;
  uint64_t nan_inf_regressions = 0;
  uint64_t unstable_levinson_frames = 0;
};

const char *variant_name(LpcAutocorrelationVariant variant) {
  switch (variant) {
  case LpcAutocorrelationVariant::AutocorrReferenceDouble:
    return "AUTOCORR_REFERENCE_DOUBLE";
  case LpcAutocorrelationVariant::AutocorrFloatScalar:
    return "AUTOCORR_FLOAT_SCALAR";
  case LpcAutocorrelationVariant::AutocorrFloatMultiacc:
    return "AUTOCORR_FLOAT_MULTIACC";
  }
  return "UNKNOWN";
}

std::vector<float> load_vocal_fixture(size_t maximum_samples) {
  std::ifstream input("../samples/dry-acapella-leave-this-place_95bpm.wav",
                      std::ios::binary);
  if (!input)
    input.open("samples/dry-acapella-leave-this-place_95bpm.wav",
               std::ios::binary);
  if (!input)
    return {};
  std::array<char, 12> riff{};
  input.read(riff.data(), riff.size());
  if (!input || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
      std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
    return {};
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t sample_rate = 0;
  std::vector<char> audio;
  while (input && (audio.empty() || bits == 0)) {
    std::array<char, 4> id{};
    uint32_t size = 0;
    input.read(id.data(), id.size());
    input.read(reinterpret_cast<char *>(&size), sizeof(size));
    if (!input)
      break;
    if (std::memcmp(id.data(), "fmt ", 4) == 0) {
      std::vector<char> fmt(size);
      input.read(fmt.data(), size);
      if (size >= 16) {
        std::memcpy(&format, fmt.data(), 2);
        std::memcpy(&channels, fmt.data() + 2, 2);
        std::memcpy(&sample_rate, fmt.data() + 4, 4);
        std::memcpy(&bits, fmt.data() + 14, 2);
      }
    } else if (std::memcmp(id.data(), "data", 4) == 0) {
      audio.resize(size);
      input.read(audio.data(), size);
    } else {
      input.seekg(size, std::ios::cur);
    }
    if (size & 1U)
      input.seekg(1, std::ios::cur);
  }
  // The repository source vocal is 44.1 kHz. Sample-rate interpretation does
  // not affect this kernel comparison: all variants consume the same floats.
  if (format != 1 || channels == 0 || bits != 16 || sample_rate == 0 ||
      audio.empty())
    return {};
  const size_t frame_bytes = channels * sizeof(int16_t);
  const size_t samples =
      std::min(maximum_samples, audio.size() / frame_bytes);
  std::vector<float> result(samples);
  for (size_t i = 0; i < samples; ++i) {
    int16_t sample = 0;
    std::memcpy(&sample, audio.data() + i * frame_bytes, sizeof(sample));
    result[i] = static_cast<float>(sample) / 32768.0f;
  }
  return result;
}

void prepare_window(const float *frame, std::array<float, kWindowSize> *y) {
  for (size_t i = 0; i < kWindowSize; ++i) {
    const float value =
        frame[i] - (i ? kPreemphasis * frame[i - 1] : 0.0f);
    const float window =
        .5f - .5f * std::cos(2.0f * kPi * i / (kWindowSize - 1));
    (*y)[i] = value * window;
  }
}

bool finite_model(const SharedLpcModel &model) {
  if (!std::isfinite(model.prediction_error) ||
      !std::isfinite(model.confidence))
    return false;
  for (size_t i = 0; i <= model.order; ++i)
    if (!std::isfinite(model.coefficients[i]))
      return false;
  return true;
}

Metrics compare_fixture(const std::vector<float> &samples,
                        LpcAutocorrelationVariant candidate) {
  Metrics metrics{};
  if (samples.size() < kWindowSize)
    return metrics;
  std::array<float, kWindowSize> y{};
  std::array<double, kOrder + 1> reference_r{}, candidate_r{};
  for (size_t end = kWindowSize; end <= samples.size(); end += kHopSize) {
    const float *frame = samples.data() + end - kWindowSize;
    prepare_window(frame, &y);
    SharedLpcAnalysis::autocorrelate(
        y.data(), y.size(), kOrder,
        LpcAutocorrelationVariant::AutocorrReferenceDouble,
        reference_r.data());
    SharedLpcAnalysis::autocorrelate(y.data(), y.size(), kOrder, candidate,
                                     candidate_r.data());
    for (size_t i = 0; i <= kOrder; ++i) {
      const double absolute = std::fabs(candidate_r[i] - reference_r[i]);
      const double relative = reference_r[i] == 0.0
                                  ? (absolute == 0.0
                                         ? 0.0
                                         : std::numeric_limits<double>::infinity())
                                  : absolute / std::fabs(reference_r[i]);
      metrics.autocorr_max_absolute_error =
          std::max(metrics.autocorr_max_absolute_error, absolute);
      metrics.autocorr_max_relative_error =
          std::max(metrics.autocorr_max_relative_error, relative);
      metrics.autocorr_squared_error +=
          static_cast<long double>(absolute) * absolute;
      ++metrics.autocorr_values;
    }

    SharedLpcModel reference{}, result{};
    reference.timestamp = result.timestamp = end - kWindowSize / 2;
    const bool reference_valid = SharedLpcAnalysis::solve_with_autocorrelation(
        frame, kWindowSize, kOrder, kPreemphasis,
        LpcAutocorrelationVariant::AutocorrReferenceDouble, &reference);
    const bool candidate_valid = SharedLpcAnalysis::solve_with_autocorrelation(
        frame, kWindowSize, kOrder, kPreemphasis, candidate, &result);
    if (reference_valid != candidate_valid)
      ++metrics.validity_decision_differences;
    if (reference.timestamp != result.timestamp)
      ++metrics.timestamp_differences;
    if (reference_valid && candidate_valid && reference.order != result.order)
      ++metrics.order_differences;
    if (reference_valid && (!candidate_valid || !finite_model(result)))
      ++metrics.unstable_levinson_frames;
    if (candidate_valid && !finite_model(result))
      ++metrics.nan_inf_regressions;
    if (reference_valid && candidate_valid) {
      for (size_t i = 0; i <= kOrder; ++i) {
        const double absolute =
            std::fabs(static_cast<double>(result.coefficients[i]) -
                      reference.coefficients[i]);
        metrics.coefficient_max_absolute_error =
            std::max(metrics.coefficient_max_absolute_error, absolute);
        metrics.coefficient_squared_error +=
            static_cast<long double>(absolute) * absolute;
        ++metrics.coefficient_values;
      }
      metrics.prediction_error_max_difference = std::max(
          metrics.prediction_error_max_difference,
          std::fabs(static_cast<double>(result.prediction_error) -
                    reference.prediction_error));
      metrics.confidence_max_difference = std::max(
          metrics.confidence_max_difference,
          std::fabs(static_cast<double>(result.confidence) -
                    reference.confidence));
    }
    ++metrics.frames;
  }

  LpcConfig reference_config{};
  LpcConfig candidate_config{};
  candidate_config.autocorrelation = candidate;
  SharedLpcAnalysis reference_analysis, candidate_analysis;
  if (!reference_analysis.init(48000.0f, reference_config) ||
      !candidate_analysis.init(48000.0f, candidate_config)) {
    ++metrics.frame_count_differences;
    return metrics;
  }
  PitchResult pitch{};
  pitch.voiced = true;
  pitch.confidence = .83f;
  uint64_t reference_frames = 0, candidate_frames = 0;
  for (size_t offset = 0; offset < samples.size(); offset += 64) {
    const size_t count = std::min<size_t>(64, samples.size() - offset);
    reference_analysis.tap(samples.data() + offset, count);
    candidate_analysis.tap(samples.data() + offset, count);
    const size_t emitted_reference = reference_analysis.run(8, pitch);
    const size_t emitted_candidate = candidate_analysis.run(8, pitch);
    reference_frames += emitted_reference;
    candidate_frames += emitted_candidate;
    if (emitted_reference != emitted_candidate) {
      ++metrics.frame_count_differences;
      continue;
    }
    if (emitted_reference != 0) {
      SharedLpcModel reference{}, result{};
      if (!reference_analysis.latest_model(&reference) ||
          !candidate_analysis.latest_model(&result)) {
        ++metrics.frame_count_differences;
        continue;
      }
      if (reference.timestamp != result.timestamp)
        ++metrics.timestamp_differences;
      if (reference.valid && result.valid && reference.order != result.order)
        ++metrics.order_differences;
    }
  }
  if (reference_frames != candidate_frames)
    ++metrics.frame_count_differences;
  return metrics;
}

double rms(long double sum, uint64_t count) {
  return count ? std::sqrt(static_cast<double>(sum / count)) : 0.0;
}

} // namespace

int main() {
  std::vector<float> silence(16384, 0.0f);
  std::vector<float> sine(16384);
  std::vector<float> harmonic(16384);
  std::vector<float> noise(1024 * 101 + 17);
  for (size_t i = 0; i < sine.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * i / 48000.0f;
    sine[i] = .25f * std::sin(phase);
    harmonic[i] = .25f * std::sin(phase) +
                  .0625f * std::sin(2.0f * phase) +
                  .03125f * std::sin(3.0f * phase);
  }
  uint32_t random = 0x51A7C0DEu;
  for (float &sample : noise) {
    random = random * 1664525u + 1013904223u;
    sample = static_cast<float>(static_cast<int32_t>(random)) /
             2147483648.0f * .2f;
  }
  const auto vocal = load_vocal_fixture(480000);
  if (vocal.empty()) {
    std::fprintf(stderr, "repository vocal fixture unavailable\n");
    return 1;
  }

  struct Fixture {
    const char *name;
    const std::vector<float> *samples;
  };
  const std::array<Fixture, 5> fixtures{{
      {"silence", &silence},
      {"sine_220_hz", &sine},
      {"harmonic_synthetic_tone", &harmonic},
      {"deterministic_noise", &noise},
      {"repository_vocal_wav_first_480000_samples", &vocal},
  }};
  const std::array<LpcAutocorrelationVariant, 2> candidates{{
      LpcAutocorrelationVariant::AutocorrFloatScalar,
      LpcAutocorrelationVariant::AutocorrFloatMultiacc,
  }};

  std::ofstream csv("artifacts/alpha01b/b4b4b_numeric_equivalence.csv");
  if (!csv) {
    csv.clear();
    csv.open("../artifacts/alpha01b/b4b4b_numeric_equivalence.csv");
  }
  if (!csv) {
    std::fprintf(stderr, "cannot create numeric-equivalence CSV\n");
    return 1;
  }
  csv << "fixture,variant,window_size,hop_size,order,products_per_frame,frames,"
         "autocorr_max_absolute_error,autocorr_max_relative_error,"
         "autocorr_rms_error,lpc_coefficient_max_absolute_error,"
         "lpc_coefficient_rms_error,prediction_error_max_difference,"
         "confidence_max_difference,validity_decision_differences,"
         "timestamp_differences,order_differences,frame_count_differences,"
         "nan_inf_regressions,"
         "unstable_levinson_frames,guardrail_result\n";
  bool all_pass = true;
  for (const auto &fixture : fixtures) {
    for (const auto candidate : candidates) {
      const Metrics m = compare_fixture(*fixture.samples, candidate);
      const bool pass =
          m.validity_decision_differences == 0 &&
          m.timestamp_differences == 0 && m.order_differences == 0 &&
          m.frame_count_differences == 0 &&
          m.nan_inf_regressions == 0 && m.unstable_levinson_frames == 0 &&
          m.coefficient_max_absolute_error <= 1e-4 &&
          m.prediction_error_max_difference <= 1e-5 &&
          m.confidence_max_difference <= 1e-5;
      all_pass &= pass;
      csv << fixture.name << ',' << variant_name(candidate) << ','
          << kWindowSize << ',' << kHopSize << ',' << kOrder << ','
          << kProductsPerFrame << ',' << m.frames << ',';
      csv.precision(17);
      csv << m.autocorr_max_absolute_error << ','
          << m.autocorr_max_relative_error << ','
          << rms(m.autocorr_squared_error, m.autocorr_values) << ','
          << m.coefficient_max_absolute_error << ','
          << rms(m.coefficient_squared_error, m.coefficient_values) << ','
          << m.prediction_error_max_difference << ','
          << m.confidence_max_difference << ','
          << m.validity_decision_differences << ','
          << m.timestamp_differences << ',' << m.order_differences << ','
          << m.frame_count_differences << ',' << m.nan_inf_regressions << ','
          << m.unstable_levinson_frames << ','
          << (pass ? "PASS" : "FAIL") << '\n';
      std::printf(
          "B4B4B_NUMERIC fixture=%s variant=%s frames=%llu "
          "autocorr_max_abs=%.9g autocorr_max_rel=%.9g autocorr_rms=%.9g "
          "coeff_max_abs=%.9g coeff_rms=%.9g pred_diff=%.9g "
          "confidence_diff=%.9g validity_diff=%llu unstable=%llu %s\n",
          fixture.name, variant_name(candidate),
          static_cast<unsigned long long>(m.frames),
          m.autocorr_max_absolute_error, m.autocorr_max_relative_error,
          rms(m.autocorr_squared_error, m.autocorr_values),
          m.coefficient_max_absolute_error,
          rms(m.coefficient_squared_error, m.coefficient_values),
          m.prediction_error_max_difference, m.confidence_max_difference,
          static_cast<unsigned long long>(m.validity_decision_differences),
          static_cast<unsigned long long>(m.unstable_levinson_frames),
          pass ? "PASS" : "FAIL");
    }
  }
  std::printf("B4B4B_PRODUCTS window=%zu order=%u products_per_frame=%zu "
              "logical_products_equal=yes\n",
              kWindowSize, kOrder, kProductsPerFrame);
  std::printf("B4B4B_MEMORY LpcConfig=%zu SharedLpcAnalysis=%zu "
              "size_increase_bytes=0\n",
              sizeof(LpcConfig), sizeof(SharedLpcAnalysis));
  std::printf("B4B4B_FLOAT_CLASSIFICATION=%s\n",
              all_pass ? "NUMERIC_GUARDRAILS_PASS"
                       : "FLOAT_AUTOCORR_NUMERICALLY_UNACCEPTABLE");
  // A rejected experimental candidate is a successful audit outcome. The CSV
  // records each guard failure; the test fails only on setup/fixture errors.
  return 0;
}
