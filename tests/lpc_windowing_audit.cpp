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
#include <utility>
#include <vector>

namespace {
constexpr size_t kWindow = 1024;
constexpr size_t kHop = 384;
constexpr uint16_t kOrder = 16;
constexpr float kPreemphasis = 0.97f;
constexpr float kPi = 3.14159265358979323846f;

uint32_t float_bits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint64_t double_bits(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
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

std::vector<float> load_vocal() {
  const std::string path =
      std::string(VOXP4_SOURCE_DIR) +
      "/samples/dry-acapella-leave-this-place_95bpm.wav";
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  std::array<char, 12> riff{};
  input.read(riff.data(), riff.size());
  if (!input || std::memcmp(riff.data(), "RIFF", 4) != 0 ||
      std::memcmp(riff.data() + 8, "WAVE", 4) != 0)
    return {};
  uint16_t format = 0, channels = 0, bits = 0;
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
  if (format != 1 || channels == 0 || bits != 16 || audio.empty())
    return {};
  const size_t frame_bytes = channels * sizeof(int16_t);
  const size_t count = std::min<size_t>(480000, audio.size() / frame_bytes);
  std::vector<float> result(count);
  for (size_t i = 0; i < count; ++i) {
    int16_t sample = 0;
    std::memcpy(&sample, audio.data() + i * frame_bytes, sizeof(sample));
    result[i] = static_cast<float>(sample) / 32768.0f;
  }
  return result;
}

double production_energy(const std::vector<float> &frame) {
  std::array<float, kWindow> hann{};
  std::array<float, kWindow> y{};
  double energy = 0.0;
  SharedLpcAnalysis::prepare_hann(hann.data(), hann.size());
  SharedLpcAnalysis::window_frame(
      frame.data(), kWindow, kPreemphasis,
      LpcWindowVariant::PrecomputedHannDoubleEnergy, hann.data(), y.data(),
      &energy);
  return energy;
}

std::vector<float> scaled_for_energy(const std::vector<float> &shape,
                                     double target) {
  std::vector<float> result(shape);
  const double initial = production_energy(shape);
  const float scale = static_cast<float>(std::sqrt(target / initial));
  for (float &sample : result)
    sample *= scale;
  return result;
}

std::pair<std::vector<float>, std::vector<float>> gate_adjacent_frames(
    const std::vector<float> &shape) {
  constexpr double kGate = 1e-8;
  const double initial = production_energy(shape);
  float scale = static_cast<float>(std::sqrt(kGate / initial));
  auto scaled = [&](float value) {
    std::vector<float> result(shape);
    for (float &sample : result)
      sample *= value;
    return result;
  };
  double energy = production_energy(scaled(scale));
  if (energy > kGate) {
    while (true) {
      const float previous = std::nextafter(scale, 0.0f);
      if (previous == scale || production_energy(scaled(previous)) <= kGate)
        break;
      scale = previous;
    }
    const float below = std::nextafter(scale, 0.0f);
    return {scaled(below), scaled(scale)};
  }
  while (true) {
    const float next = std::nextafter(
        scale, std::numeric_limits<float>::infinity());
    if (next == scale || production_energy(scaled(next)) > kGate)
      return {scaled(scale), scaled(next)};
    scale = next;
  }
}

struct Metrics {
  uint64_t frames = 0;
  uint64_t y_values = 0;
  uint64_t y_bit_mismatch = 0;
  uint64_t energy_bit_mismatch = 0;
  uint64_t valid_mismatch = 0;
  uint64_t coefficient_bit_mismatch = 0;
  uint64_t timestamp_mismatch = 0;
  uint64_t frame_count_mismatch = 0;
  uint64_t nan_inf_regressions = 0;
  double max_energy_abs_error = 0.0;
  double max_energy_relative_error = 0.0;
  long double energy_squared_error = 0.0;
  double max_coefficient_abs_error = 0.0;
  long double coefficient_squared_error = 0.0;
  uint64_t coefficient_values = 0;
  double max_prediction_error_diff = 0.0;
  double max_confidence_diff = 0.0;
  double min_reference_energy = std::numeric_limits<double>::infinity();
  double max_reference_energy = 0.0;
  double first_reference_energy = 0.0;
  double first_candidate_energy = 0.0;
  bool first_reference_valid = false;
  bool first_candidate_valid = false;
};

Metrics compare_fixture(const std::vector<float> &samples,
                        LpcWindowVariant candidate,
                        const std::array<float, kWindow> &hann) {
  Metrics metrics{};
  if (samples.size() < kWindow)
    return metrics;
  uint64_t reference_frames = 0;
  uint64_t candidate_frames = 0;
  for (size_t end = kWindow; end <= samples.size(); end += kHop) {
    const float *frame = samples.data() + end - kWindow;
    std::array<float, kWindow> reference_y{}, candidate_y{};
    double reference_e = 0.0, candidate_e = 0.0;
    SharedLpcAnalysis::window_frame(
        frame, kWindow, kPreemphasis,
        LpcWindowVariant::PrecomputedHannDoubleEnergy, hann.data(),
        reference_y.data(), &reference_e);
    SharedLpcAnalysis::window_frame(frame, kWindow, kPreemphasis, candidate,
                                    hann.data(), candidate_y.data(),
                                    &candidate_e);
    for (size_t i = 0; i < kWindow; ++i) {
      metrics.y_bit_mismatch +=
          float_bits(reference_y[i]) != float_bits(candidate_y[i]);
      ++metrics.y_values;
    }
    metrics.energy_bit_mismatch +=
        double_bits(reference_e) != double_bits(candidate_e);
    const double energy_error = reference_e - candidate_e;
    const double energy_abs_error = std::fabs(energy_error);
    metrics.max_energy_abs_error =
        std::max(metrics.max_energy_abs_error, energy_abs_error);
    metrics.max_energy_relative_error = std::max(
        metrics.max_energy_relative_error,
        reference_e == 0.0 ? (candidate_e == 0.0 ? 0.0 :
                                                std::numeric_limits<double>::infinity())
                           : energy_abs_error / std::fabs(reference_e));
    metrics.energy_squared_error +=
        static_cast<long double>(energy_error) * energy_error;
    metrics.min_reference_energy =
        std::min(metrics.min_reference_energy, reference_e);
    metrics.max_reference_energy =
        std::max(metrics.max_reference_energy, reference_e);

    SharedLpcModel reference{}, result{};
    reference.timestamp = result.timestamp = end - kWindow / 2;
    double solved_reference_e = 0.0, solved_candidate_e = 0.0;
    const bool reference_valid = SharedLpcAnalysis::solve_with_kernels(
        frame, kWindow, kOrder, kPreemphasis,
        LpcAutocorrelationVariant::AutocorrF32DoubleSingle,
        LpcWindowVariant::PrecomputedHannDoubleEnergy, hann.data(), &reference,
        nullptr,
        &solved_reference_e);
    const bool candidate_valid = SharedLpcAnalysis::solve_with_kernels(
        frame, kWindow, kOrder, kPreemphasis,
        LpcAutocorrelationVariant::AutocorrF32DoubleSingle, candidate,
        hann.data(), &result, nullptr, &solved_candidate_e);
    ++reference_frames;
    ++candidate_frames;
    metrics.valid_mismatch += reference_valid != candidate_valid;
    if (metrics.frames == 0) {
      metrics.first_reference_energy = solved_reference_e;
      metrics.first_candidate_energy = solved_candidate_e;
      metrics.first_reference_valid = reference_valid;
      metrics.first_candidate_valid = candidate_valid;
    }
    metrics.timestamp_mismatch += reference.timestamp != result.timestamp;
    if (candidate_valid && !finite_model(result))
      ++metrics.nan_inf_regressions;
    if (reference_valid && candidate_valid) {
      for (size_t i = 0; i <= kOrder; ++i) {
        metrics.coefficient_bit_mismatch +=
            float_bits(reference.coefficients[i]) !=
            float_bits(result.coefficients[i]);
        metrics.max_coefficient_abs_error = std::max(
            metrics.max_coefficient_abs_error,
            std::fabs(static_cast<double>(reference.coefficients[i]) -
                      result.coefficients[i]));
        const double coefficient_error =
            static_cast<double>(reference.coefficients[i]) -
            result.coefficients[i];
        metrics.coefficient_squared_error +=
            static_cast<long double>(coefficient_error) * coefficient_error;
        ++metrics.coefficient_values;
      }
      metrics.max_prediction_error_diff = std::max(
          metrics.max_prediction_error_diff,
          std::fabs(static_cast<double>(reference.prediction_error) -
                    result.prediction_error));
      metrics.max_confidence_diff = std::max(
          metrics.max_confidence_diff,
          std::fabs(static_cast<double>(reference.confidence) -
                    result.confidence));
    }
    ++metrics.frames;
  }
  metrics.frame_count_mismatch += reference_frames != candidate_frames;
  return metrics;
}

void add(Metrics *total, const Metrics &m) {
  total->frames += m.frames;
  total->y_values += m.y_values;
  total->y_bit_mismatch += m.y_bit_mismatch;
  total->energy_bit_mismatch += m.energy_bit_mismatch;
  total->valid_mismatch += m.valid_mismatch;
  total->coefficient_bit_mismatch += m.coefficient_bit_mismatch;
  total->timestamp_mismatch += m.timestamp_mismatch;
  total->frame_count_mismatch += m.frame_count_mismatch;
  total->nan_inf_regressions += m.nan_inf_regressions;
  total->max_energy_abs_error =
      std::max(total->max_energy_abs_error, m.max_energy_abs_error);
  total->max_energy_relative_error = std::max(
      total->max_energy_relative_error, m.max_energy_relative_error);
  total->energy_squared_error += m.energy_squared_error;
  total->max_coefficient_abs_error =
      std::max(total->max_coefficient_abs_error, m.max_coefficient_abs_error);
  total->coefficient_squared_error += m.coefficient_squared_error;
  total->coefficient_values += m.coefficient_values;
  total->max_prediction_error_diff =
      std::max(total->max_prediction_error_diff, m.max_prediction_error_diff);
  total->max_confidence_diff =
      std::max(total->max_confidence_diff, m.max_confidence_diff);
  total->min_reference_energy =
      std::min(total->min_reference_energy, m.min_reference_energy);
  total->max_reference_energy =
      std::max(total->max_reference_energy, m.max_reference_energy);
}
} // namespace

int main() {
  std::array<float, kWindow> hann{};
  if (!SharedLpcAnalysis::prepare_hann(hann.data(), hann.size()))
    return 1;

  std::vector<std::pair<std::string, std::vector<float>>> fixtures;
  fixtures.emplace_back("silence", std::vector<float>(kWindow, 0.0f));
  std::vector<float> sine(kWindow * 8);
  std::vector<float> harmonic(kWindow * 8);
  for (size_t i = 0; i < sine.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * i / 48000.0f;
    sine[i] = .25f * std::sin(phase);
    harmonic[i] = .25f * std::sin(phase) +
                  .0625f * std::sin(2.0f * phase) +
                  .03125f * std::sin(3.0f * phase);
  }
  fixtures.emplace_back("sine_220_hz", sine);
  fixtures.emplace_back("harmonic_tone", harmonic);
  std::vector<float> noise(kWindow * 8);
  uint32_t random = 0x51A7C0DEu;
  for (float &sample : noise) {
    random = random * 1664525u + 1013904223u;
    sample = static_cast<float>(static_cast<int32_t>(random)) /
             2147483648.0f * .2f;
  }
  fixtures.emplace_back("deterministic_noise", noise);

  // A broadband shape keeps the post-gate Levinson solve well conditioned, so
  // the adjacent cases verify both the gate decision and the complete model.
  std::vector<float> threshold_shape(noise.begin(), noise.begin() + kWindow);
  const std::array<double, 9> target_energies{{
      0.25e-8, 0.75e-8, 0.95e-8, 0.999e-8, 1.0e-8,
      1.001e-8, 1.05e-8, 1.25e-8, 4.0e-8}};
  for (size_t i = 0; i < target_energies.size(); ++i)
    fixtures.emplace_back("threshold_level_" + std::to_string(i),
                          scaled_for_energy(threshold_shape,
                                            target_energies[i]));
  auto adjacent_gate = gate_adjacent_frames(threshold_shape);
  fixtures.emplace_back("gate_adjacent_below", std::move(adjacent_gate.first));
  fixtures.emplace_back("gate_adjacent_above", std::move(adjacent_gate.second));
  fixtures.emplace_back("very_low_periodic_tone",
                        scaled_for_energy(sine, 2.0e-8));
  fixtures.emplace_back("very_low_level_noise",
                        scaled_for_energy(
                            std::vector<float>(noise.begin(),
                                               noise.begin() + kWindow),
                            2.0e-8));
  auto vocal = load_vocal();
  if (vocal.empty()) {
    std::fprintf(stderr, "repository vocal WAV unavailable\n");
    return 1;
  }
  fixtures.emplace_back("repository_vocal_wav", std::move(vocal));

  const std::string csv_path = std::string(VOXP4_SOURCE_DIR) +
      "/artifacts/alpha01b/b4b4h_lpc_energy_equivalence.csv";
  std::ofstream csv(csv_path);
  if (!csv)
    return 1;
  csv << "fixture,variant,frames,y_values,y_bit_mismatch,energy_bit_mismatch,"
         "energy_max_abs_error,energy_max_relative_error,energy_rms_error,"
         "valid_mismatch,coefficient_bit_mismatch,coefficient_max_abs_error,"
         "coefficient_rms_error,prediction_error_max_diff,confidence_max_diff,"
         "timestamp_mismatch,frame_count_mismatch,nan_inf_regressions,"
         "min_reference_energy,max_reference_energy,first_reference_energy,"
         "first_candidate_energy,first_reference_valid,first_candidate_valid,"
         "first_distance_from_gate,guardrail\n";

  const std::array<LpcWindowVariant, 1> candidates{{
      LpcWindowVariant::PrecomputedHannCompensatedEnergy}};
  std::array<Metrics, candidates.size()> totals{};
  bool all_pass = true;
  for (size_t candidate_index = 0; candidate_index < candidates.size();
       ++candidate_index) {
    const auto candidate = candidates[candidate_index];
    for (const auto &fixture : fixtures) {
      const Metrics m = compare_fixture(fixture.second, candidate, hann);
      add(&totals[candidate_index], m);
      const bool common = m.y_bit_mismatch == 0 && m.valid_mismatch == 0 &&
          m.timestamp_mismatch == 0 && m.frame_count_mismatch == 0 &&
          m.nan_inf_regressions == 0 &&
          m.max_coefficient_abs_error <= 1e-6 &&
          m.max_prediction_error_diff <= 1e-7 &&
          m.max_confidence_diff <= 1e-7;
      const bool pass = common;
      all_pass &= pass;
      csv.precision(17);
      const double energy_rms = m.frames
          ? std::sqrt(static_cast<double>(m.energy_squared_error / m.frames))
          : 0.0;
      const double coefficient_rms = m.coefficient_values
          ? std::sqrt(static_cast<double>(m.coefficient_squared_error /
                                          m.coefficient_values))
          : 0.0;
      csv << fixture.first << ',' << lpc_window_variant_name(candidate) << ','
          << m.frames << ',' << m.y_values << ',' << m.y_bit_mismatch << ','
          << m.energy_bit_mismatch << ',' << m.max_energy_abs_error << ','
          << m.max_energy_relative_error << ',' << energy_rms << ','
          << m.valid_mismatch << ',' << m.coefficient_bit_mismatch << ','
          << m.max_coefficient_abs_error << ',' << coefficient_rms << ','
          << m.max_prediction_error_diff << ',' << m.max_confidence_diff << ','
          << m.timestamp_mismatch << ',' << m.frame_count_mismatch << ','
          << m.nan_inf_regressions << ',' << m.min_reference_energy << ','
          << m.max_reference_energy << ',' << m.first_reference_energy << ','
          << m.first_candidate_energy << ',' << (m.first_reference_valid ? 1 : 0)
          << ',' << (m.first_candidate_valid ? 1 : 0) << ','
          << (m.first_reference_energy - 1e-8) << ','
          << (pass ? "PASS" : "FAIL")
          << '\n';
    }
  }

  for (size_t i = 0; i < candidates.size(); ++i) {
    const Metrics &m = totals[i];
    const double energy_rms = m.frames
        ? std::sqrt(static_cast<double>(m.energy_squared_error / m.frames))
        : 0.0;
    const double coefficient_rms = m.coefficient_values
        ? std::sqrt(static_cast<double>(m.coefficient_squared_error /
                                        m.coefficient_values))
        : 0.0;
    std::printf(
        "B4B4H_EQUIVALENCE oracle=LPC_WINDOW_HANN_DOUBLE_ENERGY variant=%s "
        "frames=%llu y_bit_mismatch=%llu energy_bit_mismatch=%llu "
        "energy_max_abs=%.17g energy_max_relative=%.17g energy_rms=%.17g "
        "valid_mismatch=%llu coefficient_bit_mismatch=%llu "
        "coefficient_max_abs=%.9g coefficient_rms=%.9g "
        "prediction_error_diff=%.9g confidence_diff=%.9g "
        "timestamp_mismatch=%llu frame_count_mismatch=%llu nan_inf=%llu\n",
        lpc_window_variant_name(candidates[i]),
        static_cast<unsigned long long>(m.frames),
        static_cast<unsigned long long>(m.y_bit_mismatch),
        static_cast<unsigned long long>(m.energy_bit_mismatch),
        m.max_energy_abs_error, m.max_energy_relative_error, energy_rms,
        static_cast<unsigned long long>(m.valid_mismatch),
        static_cast<unsigned long long>(m.coefficient_bit_mismatch),
        m.max_coefficient_abs_error, coefficient_rms,
        m.max_prediction_error_diff,
        m.max_confidence_diff,
        static_cast<unsigned long long>(m.timestamp_mismatch),
        static_cast<unsigned long long>(m.frame_count_mismatch),
        static_cast<unsigned long long>(m.nan_inf_regressions));
  }
  std::printf("B4B.4H host LPC energy qualification: %s\n",
              all_pass ? "PASS" : "FAIL");
  return all_pass ? 0 : 1;
}
