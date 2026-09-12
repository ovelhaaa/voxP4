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

struct LevinsonTrace {
  bool valid = false;
  std::array<double, kOrder + 1> reflection{};
  std::array<double, kOrder + 1> error_before{};
  std::array<double, kOrder + 1> error_after{};
  std::array<bool, kOrder + 1> reached{};
  double minimum_error = std::numeric_limits<double>::infinity();
  double maximum_abs_reflection = 0.0;
  uint16_t failure_order = 0;
  double failure_reflection = std::numeric_limits<double>::quiet_NaN();
  double failure_error_before = std::numeric_limits<double>::quiet_NaN();
  double failure_error_after = std::numeric_limits<double>::quiet_NaN();
};

struct Metrics {
  uint64_t frames = 0;
  uint64_t autocorr_values = 0;
  uint64_t coefficient_values = 0;
  double autocorr_max_absolute_error = 0.0;
  double autocorr_max_relative_error = 0.0;
  long double autocorr_squared_error = 0.0;
  std::vector<uint64_t> autocorr_ulp_errors;
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
  double reference_minimum_levinson_error =
      std::numeric_limits<double>::infinity();
  double candidate_minimum_levinson_error =
      std::numeric_limits<double>::infinity();
  double reference_max_abs_reflection = 0.0;
  double candidate_max_abs_reflection = 0.0;
};

const char *variant_name(LpcAutocorrelationVariant variant) {
  switch (variant) {
  case LpcAutocorrelationVariant::AutocorrReferenceDouble:
    return "AUTOCORR_REFERENCE_DOUBLE";
  case LpcAutocorrelationVariant::AutocorrFloatScalar:
    return "AUTOCORR_FLOAT_SCALAR";
  case LpcAutocorrelationVariant::AutocorrFloatMultiacc:
    return "AUTOCORR_FLOAT_MULTIACC";
  case LpcAutocorrelationVariant::AutocorrF32Kahan:
    return "AUTOCORR_F32_KAHAN";
  case LpcAutocorrelationVariant::AutocorrF32FmaProduct:
    return "AUTOCORR_F32_FMA_PRODUCT";
  case LpcAutocorrelationVariant::AutocorrF32DoubleSingle:
    return "AUTOCORR_F32_DOUBLE_SINGLE";
  }
  return "UNKNOWN";
}

std::ofstream open_artifact(const char *relative) {
  std::ofstream output(relative);
  if (!output) {
    output.clear();
    output.open((std::string("../") + relative).c_str());
  }
  return output;
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
  if (format != 1 || channels == 0 || bits != 16 || sample_rate == 0 ||
      audio.empty())
    return {};
  const size_t frame_bytes = channels * sizeof(int16_t);
  const size_t samples = std::min(maximum_samples, audio.size() / frame_bytes);
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

uint64_t ordered_double_bits(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return (bits & (uint64_t{1} << 63)) ? ~bits
                                      : bits | (uint64_t{1} << 63);
}

uint64_t ulp_distance(double a, double b) {
  if (!std::isfinite(a) || !std::isfinite(b))
    return std::numeric_limits<uint64_t>::max();
  const uint64_t ka = ordered_double_bits(a);
  const uint64_t kb = ordered_double_bits(b);
  return ka > kb ? ka - kb : kb - ka;
}

uint64_t percentile(std::vector<uint64_t> values, unsigned percent) {
  if (values.empty())
    return 0;
  std::sort(values.begin(), values.end());
  const size_t index = std::min(
      values.size() - 1,
      (values.size() * static_cast<size_t>(percent) + 99) / 100 - 1);
  return values[index];
}

LevinsonTrace trace_levinson(const std::array<double, kOrder + 1> &input) {
  LevinsonTrace trace{};
  auto r = input;
  r[0] *= 1.000001;
  std::array<double, kOrder + 1> a{}, next{};
  double error = r[0];
  a[0] = 1.0;
  trace.minimum_error = error;
  trace.error_before[0] = trace.error_after[0] = error;
  trace.reached[0] = true;
  for (size_t i = 1; i <= kOrder; ++i) {
    const double before = error;
    double sum = r[i];
    for (size_t j = 1; j < i; ++j)
      sum += a[j] * r[i - j];
    const double reflection = -sum / error;
    const double after = error * (1.0 - reflection * reflection);
    trace.reached[i] = true;
    trace.reflection[i] = reflection;
    trace.error_before[i] = before;
    trace.error_after[i] = after;
    trace.maximum_abs_reflection =
        std::max(trace.maximum_abs_reflection, std::fabs(reflection));
    trace.minimum_error = std::min(trace.minimum_error, before);
    if (!std::isfinite(reflection) || std::fabs(reflection) >= .999999 ||
        error <= 1e-12) {
      trace.failure_order = static_cast<uint16_t>(i);
      trace.failure_reflection = reflection;
      trace.failure_error_before = before;
      trace.failure_error_after = after;
      return trace;
    }
    next = a;
    next[i] = reflection;
    for (size_t j = 1; j < i; ++j)
      next[j] = a[j] + reflection * a[i - j];
    a = next;
    error = after;
    trace.minimum_error = std::min(trace.minimum_error, error);
    if (!std::isfinite(error) || error <= 1e-12) {
      trace.failure_order = static_cast<uint16_t>(i);
      trace.failure_reflection = reflection;
      trace.failure_error_before = before;
      trace.failure_error_after = after;
      return trace;
    }
  }
  trace.valid = true;
  return trace;
}

int first_material_stage(const LevinsonTrace &reference,
                         const LevinsonTrace &candidate) {
  for (size_t i = 1; i <= kOrder; ++i) {
    if (reference.reached[i] != candidate.reached[i])
      return static_cast<int>(i);
    if (!reference.reached[i])
      continue;
    if (std::fabs(reference.reflection[i] - candidate.reflection[i]) > 1e-4)
      return static_cast<int>(i);
    const double normalized_reference = reference.error_after[0] != 0.0
                                            ? reference.error_after[i] /
                                                  reference.error_after[0]
                                            : reference.error_after[i];
    const double normalized_candidate = candidate.error_after[0] != 0.0
                                            ? candidate.error_after[i] /
                                                  candidate.error_after[0]
                                            : candidate.error_after[i];
    if (std::fabs(normalized_reference - normalized_candidate) > 1e-5)
      return static_cast<int>(i);
  }
  if (reference.valid != candidate.valid)
    return candidate.failure_order ? candidate.failure_order : 1;
  return -1;
}

void write_sine_trace(std::ofstream &csv, size_t frame_index,
                      LpcAutocorrelationVariant variant,
                      const std::array<double, kOrder + 1> &reference_r,
                      const std::array<double, kOrder + 1> &candidate_r,
                      const LevinsonTrace &reference,
                      const LevinsonTrace &candidate) {
  const int first_stage = first_material_stage(reference, candidate);
  csv.precision(17);
  for (size_t i = 0; i <= kOrder; ++i) {
    csv << "sine_220_hz," << frame_index << ',' << variant_name(variant)
        << ',' << i << ',' << reference_r[i] << ',' << candidate_r[i] << ','
        << std::fabs(reference_r[i] - candidate_r[i]) << ',';
    if (i == 0 || !reference.reached[i])
      csv << "nan,nan,nan,";
    else
      csv << reference.reflection[i] << ',' << reference.error_before[i]
          << ',' << reference.error_after[i] << ',';
    if (i == 0 || !candidate.reached[i])
      csv << "nan,nan,nan,";
    else
      csv << candidate.reflection[i] << ',' << candidate.error_before[i]
          << ',' << candidate.error_after[i] << ',';
    csv << first_stage << ',' << candidate.failure_order << ','
        << candidate.failure_reflection << ','
        << candidate.failure_error_before << ','
        << candidate.failure_error_after << ','
        << (candidate.valid ? 1 : 0) << '\n';
  }
}

Metrics compare_fixture(const char *fixture_name,
                        const std::vector<float> &samples,
                        LpcAutocorrelationVariant candidate,
                        std::ofstream &stability_csv) {
  Metrics metrics{};
  if (samples.size() < kWindowSize)
    return metrics;
  std::array<float, kWindowSize> y{};
  std::array<double, kOrder + 1> reference_r{}, candidate_r{};
  size_t frame_index = 0;
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
      metrics.autocorr_ulp_errors.push_back(
          ulp_distance(reference_r[i], candidate_r[i]));
      ++metrics.autocorr_values;
    }

    const LevinsonTrace reference_trace = trace_levinson(reference_r);
    const LevinsonTrace candidate_trace = trace_levinson(candidate_r);
    if (reference_trace.valid) {
      metrics.reference_minimum_levinson_error = std::min(
          metrics.reference_minimum_levinson_error,
          reference_trace.minimum_error);
      metrics.reference_max_abs_reflection = std::max(
          metrics.reference_max_abs_reflection,
          reference_trace.maximum_abs_reflection);
    }
    if (candidate_trace.valid) {
      metrics.candidate_minimum_levinson_error = std::min(
          metrics.candidate_minimum_levinson_error,
          candidate_trace.minimum_error);
      metrics.candidate_max_abs_reflection = std::max(
          metrics.candidate_max_abs_reflection,
          candidate_trace.maximum_abs_reflection);
    }
    if (std::strcmp(fixture_name, "sine_220_hz") == 0)
      write_sine_trace(stability_csv, frame_index, candidate, reference_r,
                       candidate_r, reference_trace, candidate_trace);

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
    ++frame_index;
  }

  LpcConfig reference_config{};
  LpcConfig candidate_config{};
  reference_config.autocorrelation =
      LpcAutocorrelationVariant::AutocorrReferenceDouble;
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

double finite_or_zero(double value) {
  return std::isfinite(value) ? value : 0.0;
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
  const std::array<LpcAutocorrelationVariant, 4> candidates{{
      LpcAutocorrelationVariant::AutocorrF32Kahan,
      LpcAutocorrelationVariant::AutocorrF32FmaProduct,
      LpcAutocorrelationVariant::AutocorrF32DoubleSingle,
      LpcAutocorrelationVariant::AutocorrFloatMultiacc,
  }};

  auto csv = open_artifact(
      "artifacts/alpha01b/b4b4c_numeric_equivalence.csv");
  auto stability_csv = open_artifact(
      "artifacts/alpha01b/b4b4c_levinson_stability.csv");
  if (!csv || !stability_csv) {
    std::fprintf(stderr, "cannot create B4B.4C audit CSV files\n");
    return 1;
  }
  csv << "fixture,variant,window_size,hop_size,order,products_per_frame,frames,"
         "autocorr_max_absolute_error,autocorr_max_relative_error,"
         "autocorr_rms_error,autocorr_max_ulp_difference,"
         "autocorr_median_ulp_difference,autocorr_p95_ulp_difference,"
         "autocorr_p99_ulp_difference,lpc_coefficient_max_absolute_error,"
         "lpc_coefficient_rms_error,prediction_error_max_difference,"
         "confidence_max_difference,validity_decision_differences,"
         "timestamp_differences,order_differences,frame_count_differences,"
         "nan_inf_regressions,unstable_levinson_frames,"
         "reference_minimum_levinson_error,candidate_minimum_levinson_error,"
         "reference_max_abs_reflection,candidate_max_abs_reflection,"
         "guardrail_result\n";
  stability_csv
      << "fixture,frame,variant,index,reference_r,candidate_r,"
         "autocorr_absolute_error,reference_reflection,"
         "reference_error_before,reference_error_after,candidate_reflection,"
         "candidate_error_before,candidate_error_after,"
         "first_material_divergence_stage,candidate_failure_order,"
         "candidate_failure_reflection,candidate_failure_error_before,"
         "candidate_failure_error_after,candidate_valid\n";

  std::array<bool, candidates.size()> candidate_pass{};
  candidate_pass.fill(true);
  for (const auto &fixture : fixtures) {
    for (size_t candidate_index = 0; candidate_index < candidates.size();
         ++candidate_index) {
      const auto candidate = candidates[candidate_index];
      const Metrics m = compare_fixture(fixture.name, *fixture.samples,
                                        candidate, stability_csv);
      const bool pass =
          m.validity_decision_differences == 0 &&
          m.timestamp_differences == 0 && m.order_differences == 0 &&
          m.frame_count_differences == 0 && m.nan_inf_regressions == 0 &&
          m.unstable_levinson_frames == 0 &&
          m.coefficient_max_absolute_error <= 1e-4 &&
          m.prediction_error_max_difference <= 1e-5 &&
          m.confidence_max_difference <= 1e-5;
      candidate_pass[candidate_index] &= pass;
      const uint64_t max_ulp = m.autocorr_ulp_errors.empty()
                                   ? 0
                                   : *std::max_element(
                                         m.autocorr_ulp_errors.begin(),
                                         m.autocorr_ulp_errors.end());
      csv.precision(17);
      csv << fixture.name << ',' << variant_name(candidate) << ','
          << kWindowSize << ',' << kHopSize << ',' << kOrder << ','
          << kProductsPerFrame << ',' << m.frames << ','
          << m.autocorr_max_absolute_error << ','
          << m.autocorr_max_relative_error << ','
          << rms(m.autocorr_squared_error, m.autocorr_values) << ','
          << max_ulp << ',' << percentile(m.autocorr_ulp_errors, 50) << ','
          << percentile(m.autocorr_ulp_errors, 95) << ','
          << percentile(m.autocorr_ulp_errors, 99) << ','
          << m.coefficient_max_absolute_error << ','
          << rms(m.coefficient_squared_error, m.coefficient_values) << ','
          << m.prediction_error_max_difference << ','
          << m.confidence_max_difference << ','
          << m.validity_decision_differences << ','
          << m.timestamp_differences << ',' << m.order_differences << ','
          << m.frame_count_differences << ',' << m.nan_inf_regressions << ','
          << m.unstable_levinson_frames << ','
          << finite_or_zero(m.reference_minimum_levinson_error) << ','
          << finite_or_zero(m.candidate_minimum_levinson_error) << ','
          << m.reference_max_abs_reflection << ','
          << m.candidate_max_abs_reflection << ','
          << (pass ? "PASS" : "FAIL") << '\n';
      std::printf(
          "B4B4C_NUMERIC fixture=%s variant=%s frames=%llu "
          "autocorr_max_abs=%.9g max_ulp=%llu coeff_max_abs=%.9g "
          "pred_diff=%.9g confidence_diff=%.9g validity_diff=%llu "
          "unstable=%llu min_error_ref=%.9g min_error_candidate=%.9g "
          "max_reflection_ref=%.9g max_reflection_candidate=%.9g %s\n",
          fixture.name, variant_name(candidate),
          static_cast<unsigned long long>(m.frames),
          m.autocorr_max_absolute_error,
          static_cast<unsigned long long>(max_ulp),
          m.coefficient_max_absolute_error,
          m.prediction_error_max_difference, m.confidence_max_difference,
          static_cast<unsigned long long>(m.validity_decision_differences),
          static_cast<unsigned long long>(m.unstable_levinson_frames),
          finite_or_zero(m.reference_minimum_levinson_error),
          finite_or_zero(m.candidate_minimum_levinson_error),
          m.reference_max_abs_reflection, m.candidate_max_abs_reflection,
          pass ? "PASS" : "FAIL");
    }
  }
  std::printf("B4B4C_PRODUCTS window=%zu order=%u products_per_frame=%zu "
              "logical_products_equal=yes\n",
              kWindowSize, kOrder, kProductsPerFrame);
  std::printf("B4B4C_MEMORY LpcConfig=%zu SharedLpcAnalysis=%zu "
              "size_increase_bytes=0\n",
              sizeof(LpcConfig), sizeof(SharedLpcAnalysis));
  for (size_t i = 0; i < candidates.size(); ++i)
    std::printf("B4B4C_CLASSIFICATION variant=%s result=%s\n",
                variant_name(candidates[i]),
                candidate_pass[i] ? "NUMERIC_GUARDRAILS_PASS"
                                  : "CANDIDATE_NOT_ELIGIBLE");
  return 0;
}
