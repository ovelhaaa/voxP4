#include "yin_detector.h"
#include <algorithm>
#include <cmath>

namespace {
ProfileSection profile_section(PitchAnalysisProfileSection s) {
  return static_cast<ProfileSection>(
      static_cast<size_t>(s) +
      static_cast<size_t>(ProfileSection::AnalysisDecimator));
}
} // namespace

bool YinDetector::init(const PitchAnalysisConfig &c) {
  initialized_ = false;
  if (!std::isfinite(c.analysis_sample_rate) || c.analysis_sample_rate < 4000 ||
      c.window_size < 64 || c.window_size > kMaxWindow || c.hop_size == 0 ||
      c.hop_size >= c.window_size || !std::isfinite(c.min_frequency) ||
      !std::isfinite(c.max_frequency) || c.min_frequency <= 0 ||
      c.max_frequency <= c.min_frequency || !std::isfinite(c.yin_threshold) ||
      c.yin_threshold <= 0 || c.yin_threshold >= 1)
    return false;
  const double tau_min = c.analysis_sample_rate / c.max_frequency;
  const double tau_max = c.analysis_sample_rate / c.min_frequency;
  if (!std::isfinite(tau_min) || !std::isfinite(tau_max) || tau_min < 2.0 ||
      tau_max < tau_min || tau_max > kMaxWindow ||
      tau_max + 1.0 >= c.window_size)
    return false;
  config_ = c;
  tau_min_ = static_cast<size_t>(tau_min);
  tau_max_ = static_cast<size_t>(tau_max);
  initialized_ = true;
  return true;
}

PitchDetectorMeasurement YinDetector::analyze(const float *x, size_t n,
                                              uint64_t) {
  PitchDetectorMeasurement out;
  if (!initialized_ || !x || n < config_.window_size)
    return out;
  VF_PROFILE_BEGIN(profiler_,
                   profile_section(PitchAnalysisProfileSection::YinTotal));
  VF_PROFILE_BEGIN(profiler_,
                   profile_section(PitchAnalysisProfileSection::YinEnergy));
  double energy = 0;
  for (size_t i = 0; i < n; ++i)
    energy += static_cast<double>(x[i]) * x[i];
  out.rms_db = 10.0f * std::log10(static_cast<float>(energy / n) + 1e-20f);
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinEnergy), 0);
  VF_PROFILE_BEGIN(profiler_,
                   profile_section(PitchAnalysisProfileSection::YinDifference));
  difference_[0] = 0;
  uint64_t inner_iterations = 0;
  for (size_t tau = 1; tau <= tau_max_ + 1; ++tau) {
    inner_iterations += n - tau;
    float sum = 0;
    for (size_t i = 0; i + tau < n; ++i) {
      const float d = x[i] - x[i + tau];
      sum += d * d;
    }
    difference_[tau] = sum;
  }
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinDifference),
                 0);
  VF_PROFILE_BEGIN(profiler_,
                   profile_section(PitchAnalysisProfileSection::YinCmnd));
  cmnd_[0] = 1;
  double cumulative = 0;
  for (size_t tau = 1; tau <= tau_max_ + 1; ++tau) {
    cumulative += difference_[tau];
    cmnd_[tau] = cumulative > 1e-20
                     ? static_cast<float>(difference_[tau] * tau / cumulative)
                     : 1.0f;
  }
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinCmnd), 0);
  VF_PROFILE_BEGIN(profiler_,
                   profile_section(PitchAnalysisProfileSection::YinSearch));
  size_t candidate = 0;
  for (size_t tau = tau_min_; tau <= tau_max_; ++tau) {
    if (cmnd_[tau] < config_.yin_threshold) {
      while (tau < tau_max_ && cmnd_[tau + 1] < cmnd_[tau])
        ++tau;
      candidate = tau;
      break;
    }
  }
  if (!candidate) {
    candidate = tau_min_;
    for (size_t tau = tau_min_ + 1; tau <= tau_max_; ++tau)
      if (cmnd_[tau] < cmnd_[candidate])
        candidate = tau;
  }
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinSearch), 0);
  VF_PROFILE_BEGIN(
      profiler_,
      profile_section(PitchAnalysisProfileSection::YinInterpolation));
  // Interpolate the raw difference valley: unlike CMND it is locally
  // symmetric for an integer-period sinusoid and avoids high-F0 bias.
  const float y0 = difference_[candidate - 1], y1 = difference_[candidate],
              y2 = difference_[candidate + 1];
  const float denom = y0 - 2.0f * y1 + y2;
  float offset = std::fabs(denom) > 1e-12f ? .5f * (y0 - y2) / denom : 0;
  offset = std::clamp(offset, -.5f, .5f);
  const float period = candidate + offset;
  out.yin_min = cmnd_[candidate];
  out.yin_tau = static_cast<float>(candidate);
  out.confidence = std::clamp(1.0f - cmnd_[candidate], 0.0f, 1.0f);
  if (period > 0 && std::isfinite(period)) {
    out.period_samples = period;
    out.frequency_hz = config_.analysis_sample_rate / period;
  }
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinInterpolation),
                 0);
  executions_.fetch_add(1, std::memory_order_relaxed);
  inner_iterations_.fetch_add(inner_iterations, std::memory_order_relaxed);
  VF_PROFILE_END(profiler_,
                 profile_section(PitchAnalysisProfileSection::YinTotal), 0);
  return out;
}
void YinDetector::reset() {
  difference_.fill(0);
  cmnd_.fill(0);
  profiler_.reset();
  executions_.store(0, std::memory_order_relaxed);
  inner_iterations_.store(0, std::memory_order_relaxed);
}
ProfileStats YinDetector::profile(PitchAnalysisProfileSection s) const {
  if (s < PitchAnalysisProfileSection::YinEnergy ||
      s > PitchAnalysisProfileSection::YinTotal)
    return {};
  return profiler_.stats(profile_section(s));
}
YinForensicTelemetry YinDetector::forensic_telemetry() const {
  YinForensicTelemetry result{};
  result.tau_min = static_cast<uint32_t>(tau_min_);
  result.tau_max = static_cast<uint32_t>(tau_max_);
  result.window_size = config_.window_size;
  result.tau_values_per_execution = static_cast<uint32_t>(tau_max_ + 1);
  result.executions = executions_.load(std::memory_order_relaxed);
  result.actual_inner_iterations =
      inner_iterations_.load(std::memory_order_relaxed);
  const uint64_t per_execution =
      (tau_max_ + 1) * static_cast<uint64_t>(config_.window_size) -
      ((tau_max_ + 1) * (tau_max_ + 2)) / 2;
  result.expected_inner_iterations = result.executions * per_execution;
  return result;
}
