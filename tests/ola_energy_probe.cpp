#include "td_psola.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <vector>

namespace {
constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979323846f;
constexpr size_t kBlock = 64, kTotal = 4 * 48000, kMeasureBegin = 2 * 48000;

struct RunResult {
  std::vector<float> input, output;
  PitchShiftDebug debug{};
  PitchShiftTelemetry telemetry{};
  double norm_sum = 0, norm_square_sum = 0, ola_sum = 0, overlap_sum = 0;
  float norm_min = 1e9f, norm_max = 0, norm_square_min = 1e9f,
        norm_square_max = 0;
  uint32_t overlap_min = UINT32_MAX, overlap_max = 0;
  uint64_t measured_blocks = 0;
};

size_t marks_for(uint64_t available, float period,
                 std::array<PitchMark, 64> &marks) {
  const uint64_t last = static_cast<uint64_t>(std::floor(available / period));
  const uint64_t first = last > 62 ? last - 62 : 0;
  size_t count = 0;
  for (uint64_t index = first; index <= last && count < marks.size(); ++index)
    marks[count++] =
        {static_cast<uint64_t>(std::llround(index * period)), 1.0f};
  return count;
}

const char *mode_name(OlaNormalizationMode mode) {
  switch (mode) {
  case OlaNormalizationMode::Current: return "current";
  case OlaNormalizationMode::WindowSum: return "window_sum";
  case OlaNormalizationMode::WindowEnergy: return "window_energy";
  case OlaNormalizationMode::ColaEnergyHybrid: return "cola_energy_hybrid";
  }
  return "unknown";
}

std::vector<float> make_signal(const std::string &name) {
  std::vector<float> signal(kTotal);
  for (size_t i = 0; i < signal.size(); ++i) {
    const float phase = 2.0f * kPi * 220.0f * i / kRate;
    if (name == "constant")
      signal[i] = 1.0f;
    else if (name == "sine")
      signal[i] = 0.25f * std::sin(phase);
    else
      for (int harmonic = 1; harmonic <= 5; ++harmonic)
        signal[i] += 0.12f / harmonic * std::sin(harmonic * phase);
  }
  return signal;
}

RunResult run(const std::string &signal_name, float semitones,
              OlaNormalizationMode mode, std::ofstream &timeseries) {
  RunResult result;
  result.input = make_signal(signal_name);
  result.output.resize(result.input.size());
  SharedPitchShiftResources shared;
  shared.init();
  TdPsola psola;
  PitchShiftConfig config{true, semitones, 1.0f, 1.0f};
  config.ola_normalization = mode;
  if (!psola.init(kRate, config, &shared))
    std::exit(2);
  psola.set_onset_unvoiced_attenuation(false);
  const float period = kRate / 220.0f;
  for (size_t position = 0; position < result.input.size(); position += kBlock) {
    const size_t frames = std::min(kBlock, result.input.size() - position);
    const uint64_t analyzed = position > 1100 ? position - 1100 : 0;
    std::array<PitchMark, 64> marks{};
    const size_t mark_count = marks_for(analyzed, period, marks);
    PitchResult pitch{};
    pitch.voiced = true;
    pitch.confidence = 1.0f;
    pitch.frequency_hz = 220.0f;
    pitch.period_samples = period;
    pitch.analysis_timestamp_samples = analyzed;
    psola.process(result.input.data() + position,
                  result.output.data() + position, frames, pitch,
                  mark_count >= 3 ? PitchTrackState::Locked
                                  : PitchTrackState::Acquiring,
                  marks.data(), mark_count);
    const auto debug = psola.debug();
    if (position >= kMeasureBegin) {
      result.norm_sum += debug.ola_norm_mean;
      result.norm_square_sum += debug.ola_norm_square_mean;
      result.ola_sum += debug.ola_sum_mean;
      result.overlap_sum += debug.ola_overlap_mean;
      result.norm_min = std::min(result.norm_min, debug.ola_norm_min);
      result.norm_max = std::max(result.norm_max, debug.ola_norm_max);
      result.norm_square_min =
          std::min(result.norm_square_min, debug.ola_norm_square_min);
      result.norm_square_max =
          std::max(result.norm_square_max, debug.ola_norm_square_max);
      result.overlap_min = std::min(result.overlap_min, debug.ola_overlap_min);
      result.overlap_max = std::max(result.overlap_max, debug.ola_overlap_max);
      ++result.measured_blocks;
      if (signal_name == "harmonic" && mode == OlaNormalizationMode::Current)
        timeseries << position / kRate << ',' << semitones << ','
                   << debug.ola_sum_mean << ',' << debug.ola_norm_mean << ','
                   << debug.ola_norm_square_mean << ','
                   << debug.ola_norm_min << ',' << debug.ola_norm_max << ','
                   << debug.ola_norm_square_min << ','
                   << debug.ola_norm_square_max << ','
                   << debug.ola_overlap_mean << ',' << debug.ola_overlap_min
                   << ',' << debug.ola_overlap_max << '\n';
    }
  }
  result.debug = psola.debug();
  result.telemetry = psola.telemetry();
  return result;
}

double rms(const std::vector<float> &values, size_t begin) {
  double sum = 0;
  for (size_t i = begin; i < values.size(); ++i)
    sum += static_cast<double>(values[i]) * values[i];
  return std::sqrt(sum / (values.size() - begin));
}

double mean(const std::vector<float> &values, size_t begin) {
  return std::accumulate(values.begin() + begin, values.end(), 0.0) /
         (values.size() - begin);
}

double percentile(std::vector<double> values, double quantile) {
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(
      std::lround(quantile * static_cast<double>(values.size() - 1)));
  return values[index];
}

struct Modulation {
  double depth = 0, p5_p95 = 0, peak_average_variation = 0;
};

Modulation modulation(const std::vector<float> &input,
                      const std::vector<float> &output) {
  constexpr size_t window = 960;
  std::vector<double> gains;
  for (size_t begin = kMeasureBegin; begin + window <= output.size();
       begin += window / 2) {
    double input_power = 0, output_power = 0;
    for (size_t i = begin; i < begin + window; ++i) {
      input_power += static_cast<double>(input[i]) * input[i];
      output_power += static_cast<double>(output[i]) * output[i];
    }
    if (input_power > 1e-12)
      gains.push_back(std::sqrt(output_power / input_power));
  }
  const double p5 = percentile(gains, 0.05), p95 = percentile(gains, 0.95);
  const double average =
      std::accumulate(gains.begin(), gains.end(), 0.0) / gains.size();
  double variation = 0;
  for (double gain : gains)
    variation = std::max(variation, std::fabs(gain - average));
  return {average > 0 ? (p95 - p5) / average : 0,
          p95 > 0 ? p5 / p95 : 0,
          average > 0 ? variation / average : 0};
}

double max_step(const std::vector<float> &values) {
  double result = 0;
  for (size_t i = kMeasureBegin + 1; i < values.size(); ++i)
    result = std::max(result,
                      std::fabs(static_cast<double>(values[i] - values[i - 1])));
  return result;
}

double estimate_pitch(const std::vector<float> &values, double expected) {
  const size_t begin = values.size() - 48000;
  const int expected_lag = static_cast<int>(std::lround(kRate / expected));
  const int radius = std::max(3, static_cast<int>(std::ceil(expected_lag * .03)));
  const int first = std::max(2, expected_lag - radius);
  const int last = expected_lag + radius;
  std::vector<double> correlation(last - first + 1);
  for (int lag = first; lag <= last; ++lag) {
    double sum = 0;
    for (size_t i = begin + lag; i < values.size(); ++i)
      sum += static_cast<double>(values[i]) * values[i - lag];
    correlation[lag - first] = sum;
  }
  const auto best = std::max_element(correlation.begin(), correlation.end());
  const size_t index = static_cast<size_t>(best - correlation.begin());
  double lag = first + index;
  if (index && index + 1 < correlation.size()) {
    const double left = correlation[index - 1], center = correlation[index];
    const double right = correlation[index + 1];
    const double denominator = left - 2.0 * center + right;
    if (std::fabs(denominator) > 1e-12)
      lag += 0.5 * (left - right) / denominator;
  }
  return kRate / lag;
}

void put16(std::ofstream &file, uint16_t value) {
  file.put(static_cast<char>(value));
  file.put(static_cast<char>(value >> 8));
}
void put32(std::ofstream &file, uint32_t value) {
  put16(file, static_cast<uint16_t>(value));
  put16(file, static_cast<uint16_t>(value >> 16));
}
void write_wav(const std::filesystem::path &path,
               const std::vector<float> &samples) {
  std::ofstream file(path, std::ios::binary);
  const uint32_t bytes = static_cast<uint32_t>(samples.size() * sizeof(float));
  file.write("RIFF", 4); put32(file, 36 + bytes); file.write("WAVEfmt ", 8);
  put32(file, 16); put16(file, 3); put16(file, 1); put32(file, 48000);
  put32(file, 48000 * 4); put16(file, 4); put16(file, 32);
  file.write("data", 4); put32(file, bytes);
  file.write(reinterpret_cast<const char *>(samples.data()), bytes);
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: ola_energy_probe output-directory\n");
    return 2;
  }
  const std::filesystem::path root(argv[1]);
  std::filesystem::create_directories(root / "renders" / "synthetic");
  std::ofstream metrics(root / "synthetic_metrics.csv");
  std::ofstream reuse(root / "source_mark_reuse.csv");
  std::ofstream timeseries(root / "ola_norm_timeseries.csv");
  metrics << "signal,method,shift_st,input_rms,output_rms,rms_ratio,output_mean,"
             "ola_sum_mean,ola_norm_mean,ola_norm_min,ola_norm_max,"
             "ola_norm_square_mean,ola_norm_square_min,ola_norm_square_max,"
             "overlap_mean,overlap_min,overlap_max,reuse_pct,grain_rate_hz,"
             "modulation_depth,p5_p95_rms_ratio,peak_average_gain_variation,"
             "max_sample_step,pitch_error_cents,norm_holes,nan_or_inf\n";
  reuse << "signal,method,shift_st,source_marks_1x,source_marks_2x,"
           "source_marks_3x,source_marks_4x_or_more,reuses_total,reuse_pct\n";
  timeseries << "time_s,shift_st,ola_sum_mean,sum_w_mean,sum_w2_mean,"
                "sum_w_min,sum_w_max,sum_w2_min,sum_w2_max,overlap_mean,"
                "overlap_min,overlap_max\n";
  const std::array<std::string, 3> signals{"constant", "sine", "harmonic"};
  const std::array<float, 7> shifts{-12, -7, -4, 0, 4, 7, 12};
  const std::array<OlaNormalizationMode, 4> modes{
      OlaNormalizationMode::Current, OlaNormalizationMode::WindowSum,
      OlaNormalizationMode::WindowEnergy, OlaNormalizationMode::ColaEnergyHybrid};
  for (const auto &signal : signals) {
    for (auto mode : modes) {
      for (float shift : shifts) {
        RunResult result = run(signal, shift, mode, timeseries);
        const double input_rms = rms(result.input, kMeasureBegin);
        const double output_rms = rms(result.output, kMeasureBegin);
        const Modulation mod = modulation(result.input, result.output);
        const uint64_t source_runs = result.debug.source_marks_1x +
                                     result.debug.source_marks_2x +
                                     result.debug.source_marks_3x +
                                     result.debug.source_marks_4x_or_more;
        const uint64_t grain_selections = source_runs +
                                          result.debug.source_mark_reuses_total;
        const double reuse_pct = grain_selections
                                     ? 100.0 * result.debug.source_mark_reuses_total /
                                           grain_selections
                                     : 0.0;
        const double blocks = static_cast<double>(result.measured_blocks);
        const bool nonfinite = std::any_of(
            result.output.begin(), result.output.end(),
            [](float value) { return !std::isfinite(value); });
        double pitch_error = 0;
        if (signal != "constant") {
          const double expected = 220.0 * std::exp2(shift / 12.0);
          pitch_error = 1200.0 *
                        std::log2(estimate_pitch(result.output, expected) /
                                  expected);
        }
        metrics << signal << ',' << mode_name(mode) << ',' << shift << ','
                << input_rms << ',' << output_rms << ','
                << output_rms / input_rms << ','
                << mean(result.output, kMeasureBegin) << ','
                << result.ola_sum / blocks << ',' << result.norm_sum / blocks
                << ',' << result.norm_min << ',' << result.norm_max << ','
                << result.norm_square_sum / blocks << ','
                << result.norm_square_min << ',' << result.norm_square_max
                << ',' << result.overlap_sum / blocks << ','
                << result.overlap_min << ',' << result.overlap_max << ','
                << reuse_pct << ',' << result.telemetry.grains / 4.0 << ','
                << mod.depth << ',' << mod.p5_p95 << ','
                << mod.peak_average_variation << ',' << max_step(result.output)
                << ',' << pitch_error << ','
                << result.debug.ola_norm_samples_below_threshold << ','
                << nonfinite << '\n';
        reuse << signal << ',' << mode_name(mode) << ',' << shift << ','
              << result.debug.source_marks_1x << ','
              << result.debug.source_marks_2x << ','
              << result.debug.source_marks_3x << ','
              << result.debug.source_marks_4x_or_more << ','
              << result.debug.source_mark_reuses_total << ',' << reuse_pct
              << '\n';
        if (signal == "harmonic" &&
            (mode == OlaNormalizationMode::Current ||
             mode == OlaNormalizationMode::WindowEnergy ||
             mode == OlaNormalizationMode::ColaEnergyHybrid)) {
          const std::string filename = signal + "_" + mode_name(mode) + "_" +
                                       (shift < 0 ? "m" : "p") +
                                       std::to_string(static_cast<int>(std::fabs(shift))) +
                                       ".wav";
          write_wav(root / "renders" / "synthetic" / filename, result.output);
        }
      }
    }
  }
  return 0;
}
