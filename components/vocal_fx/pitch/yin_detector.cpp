#include "yin_detector.h"
#include <algorithm>
#include <cmath>
#include <new>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {
#if defined(__GNUC__) || defined(__clang__)
#define YIN_NOINLINE __attribute__((noinline))
#else
#define YIN_NOINLINE
#endif

ProfileSection profile_section(PitchAnalysisProfileSection s) {
  return static_cast<ProfileSection>(
      static_cast<size_t>(s) +
      static_cast<size_t>(ProfileSection::AnalysisDecimator));
}
} // namespace

const char *yin_difference_variant_name(YinDifferenceVariant variant) {
  switch (variant) {
  case YinDifferenceVariant::ReferenceScalar:
    return "YIN_DIFF_REFERENCE_SCALAR";
  case YinDifferenceVariant::FmaScalar:
    return "YIN_DIFF_FMA_SCALAR";
  case YinDifferenceVariant::Muladd4Acc:
    return "YIN_DIFF_MULADD_4ACC";
  case YinDifferenceVariant::Fma4Acc:
    return "YIN_DIFF_FMA_4ACC";
  case YinDifferenceVariant::Fma8Acc:
    return "YIN_DIFF_FMA_8ACC";
  case YinDifferenceVariant::IncrementalF32:
    return "YIN_DIFF_INCREMENTAL_F32";
  case YinDifferenceVariant::IncrementalDoubleSingle:
    return "YIN_DIFF_INCREMENTAL_DOUBLE_SINGLE";
  }
  return "YIN_DIFF_UNKNOWN";
}

uint64_t yin_difference_product_count(size_t frames, size_t tau_count) {
  if (tau_count >= frames)
    return 0;
  return tau_count * static_cast<uint64_t>(frames) -
         tau_count * static_cast<uint64_t>(tau_count + 1) / 2;
}

extern "C" YIN_NOINLINE void
yin_difference_reference_scalar(const float *x, size_t n, size_t tau_count,
                                float *difference) {
  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    float sum = 0.0f;
    for (size_t i = 0; i + tau < n; ++i) {
      const float d = x[i] - x[i + tau];
      sum += d * d;
    }
    difference[tau] = sum;
  }
}

extern "C" YIN_NOINLINE void
yin_difference_fma_scalar(const float *x, size_t n, size_t tau_count,
                          float *difference) {
  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    float sum = 0.0f;
    for (size_t i = 0; i + tau < n; ++i) {
      const float d = x[i] - x[i + tau];
      sum = __builtin_fmaf(d, d, sum);
    }
    difference[tau] = sum;
  }
}

extern "C" YIN_NOINLINE void
yin_difference_muladd_4acc(const float *x, size_t n, size_t tau_count,
                          float *difference) {
  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    const size_t count = n - tau;
    size_t i = 0;
    for (; i + 3 < count; i += 4) {
      const float d0 = x[i] - x[i + tau];
      const float d1 = x[i + 1] - x[i + 1 + tau];
      const float d2 = x[i + 2] - x[i + 2 + tau];
      const float d3 = x[i + 3] - x[i + 3 + tau];
      s0 += d0 * d0;
      s1 += d1 * d1;
      s2 += d2 * d2;
      s3 += d3 * d3;
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; i < count; ++i) {
      const float d = x[i] - x[i + tau];
      sum += d * d;
    }
    difference[tau] = sum;
  }
}

extern "C" YIN_NOINLINE void
yin_difference_fma_4acc(const float *x, size_t n, size_t tau_count,
                        float *difference) {
  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    const size_t count = n - tau;
    size_t i = 0;
    for (; i + 3 < count; i += 4) {
      const float d0 = x[i] - x[i + tau];
      const float d1 = x[i + 1] - x[i + 1 + tau];
      const float d2 = x[i + 2] - x[i + 2 + tau];
      const float d3 = x[i + 3] - x[i + 3 + tau];
      s0 = __builtin_fmaf(d0, d0, s0);
      s1 = __builtin_fmaf(d1, d1, s1);
      s2 = __builtin_fmaf(d2, d2, s2);
      s3 = __builtin_fmaf(d3, d3, s3);
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; i < count; ++i) {
      const float d = x[i] - x[i + tau];
      sum = __builtin_fmaf(d, d, sum);
    }
    difference[tau] = sum;
  }
}

extern "C" YIN_NOINLINE void
yin_difference_fma_8acc(const float *x, size_t n, size_t tau_count,
                        float *difference) {
  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    float s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
    const size_t count = n - tau;
    size_t i = 0;
    for (; i + 7 < count; i += 8) {
      const float d0 = x[i] - x[i + tau];
      const float d1 = x[i + 1] - x[i + 1 + tau];
      const float d2 = x[i + 2] - x[i + 2 + tau];
      const float d3 = x[i + 3] - x[i + 3 + tau];
      const float d4 = x[i + 4] - x[i + 4 + tau];
      const float d5 = x[i + 5] - x[i + 5 + tau];
      const float d6 = x[i + 6] - x[i + 6 + tau];
      const float d7 = x[i + 7] - x[i + 7 + tau];
      s0 = __builtin_fmaf(d0, d0, s0);
      s1 = __builtin_fmaf(d1, d1, s1);
      s2 = __builtin_fmaf(d2, d2, s2);
      s3 = __builtin_fmaf(d3, d3, s3);
      s4 = __builtin_fmaf(d4, d4, s4);
      s5 = __builtin_fmaf(d5, d5, s5);
      s6 = __builtin_fmaf(d6, d6, s6);
      s7 = __builtin_fmaf(d7, d7, s7);
    }
    const float s01 = s0 + s1;
    const float s23 = s2 + s3;
    const float s45 = s4 + s5;
    const float s67 = s6 + s7;
    float sum = (s01 + s23) + (s45 + s67);
    for (; i < count; ++i) {
      const float d = x[i] - x[i + tau];
      sum = __builtin_fmaf(d, d, sum);
    }
    difference[tau] = sum;
  }
}

void yin_difference_compute(YinDifferenceVariant variant, const float *samples,
                            size_t frames, size_t tau_count,
                            float *difference) {
  switch (variant) {
  case YinDifferenceVariant::ReferenceScalar:
    return yin_difference_reference_scalar(samples, frames, tau_count,
                                           difference);
  case YinDifferenceVariant::FmaScalar:
    return yin_difference_fma_scalar(samples, frames, tau_count, difference);
  case YinDifferenceVariant::Muladd4Acc:
    return yin_difference_muladd_4acc(samples, frames, tau_count, difference);
  case YinDifferenceVariant::Fma4Acc:
    return yin_difference_fma_4acc(samples, frames, tau_count, difference);
  case YinDifferenceVariant::Fma8Acc:
    return yin_difference_fma_8acc(samples, frames, tau_count, difference);
  case YinDifferenceVariant::IncrementalF32:
  case YinDifferenceVariant::IncrementalDoubleSingle:
    // Stateful variants require YinIncrementalDifference. Preserve a safe
    // oracle result if this stateless API is called with either selector.
    return yin_difference_fma_8acc(samples, frames, tau_count, difference);
  }
  yin_difference_reference_scalar(samples, frames, tau_count, difference);
}

bool YinIncrementalDifference::init(YinDifferenceVariant variant,
                                    size_t frames, size_t hop,
                                    size_t tau_count, uint32_t rebase_hops) {
  if ((variant != YinDifferenceVariant::IncrementalF32 &&
       variant != YinDifferenceVariant::IncrementalDoubleSingle) ||
      frames == 0 || frames > kMaxWindow || hop != kHistorySamples ||
      tau_count == 0 || tau_count >= frames || hop + tau_count > frames)
    return false;
  release_lo();
  variant_ = variant;
  frames_ = frames;
  hop_ = hop;
  tau_count_ = tau_count;
  rebase_hops_ = rebase_hops;
  if (variant == YinDifferenceVariant::IncrementalDoubleSingle) {
#ifdef ESP_PLATFORM
    lo_ = static_cast<float *>(heap_caps_calloc(kMaxWindow + 1, sizeof(float),
                                                MALLOC_CAP_SPIRAM));
#else
    lo_ = new (std::nothrow) float[kMaxWindow + 1]{};
#endif
    if (!lo_)
      return false;
  }
  reset();
  return true;
}

YinIncrementalDifference::~YinIncrementalDifference() { release_lo(); }

void YinIncrementalDifference::release_lo() {
  if (!lo_)
    return;
#ifdef ESP_PLATFORM
  heap_caps_free(lo_);
#else
  delete[] lo_;
#endif
  lo_ = nullptr;
}

void YinIncrementalDifference::reset() {
  valid_ = false;
  gap_pending_ = false;
  state_buffer_ = nullptr;
  updates_since_rebase_ = 0;
  if (lo_)
    std::fill_n(lo_, kMaxWindow + 1, 0.0f);
  outgoing_.fill(0.0f);
  incremental_rebases_ = 0;
  gap_rebases_ = 0;
  periodic_rebases_ = 0;
  update_terms_ = 0;
  full_rebase_products_ = 0;
}

void YinIncrementalDifference::invalidate_for_gap() {
  if (valid_)
    gap_pending_ = true;
  valid_ = false;
  updates_since_rebase_ = 0;
}

void YinIncrementalDifference::capture_outgoing(const float *samples) {
  std::copy_n(samples, hop_, outgoing_.begin());
}

void YinIncrementalDifference::add_double_single(size_t tau, float value,
                                                 float *hi) {
  // Knuth TwoSum followed by a deterministic renormalization. All operations
  // remain binary32; the expansion represents hi + lo without a hot-path
  // binary64 accumulator.
  const float a = hi[tau];
  const float sum = a + value;
  const float value_virtual = sum - a;
  const float error = (a - (sum - value_virtual)) + (value - value_virtual);
  const float residual = lo_[tau] + error;
  const float normalized = sum + residual;
  lo_[tau] = residual - (normalized - sum);
  hi[tau] = normalized;
}

uint64_t YinIncrementalDifference::compute(const float *x,
                                           float *difference) {
  if (!x || !difference || frames_ == 0)
    return 0;
  // The output buffer is the persistent hi/F32 accumulator. A caller that
  // changes buffers receives a safe oracle rebase rather than stale state.
  if (state_buffer_ != difference) {
    valid_ = false;
    state_buffer_ = difference;
  }
  const bool periodic = valid_ && rebase_hops_ != 0 &&
                        updates_since_rebase_ + 1 >= rebase_hops_;
  if (!valid_ || periodic) {
    yin_difference_fma_8acc(x, frames_, tau_count_, difference);
    if (lo_)
      std::fill_n(lo_, tau_count_ + 1, 0.0f);
    capture_outgoing(x);
    valid_ = true;
    updates_since_rebase_ = 0;
    ++incremental_rebases_;
    if (gap_pending_) {
      ++gap_rebases_;
      gap_pending_ = false;
    } else if (periodic) {
      ++periodic_rebases_;
    }
    const uint64_t products =
        yin_difference_product_count(frames_, tau_count_);
    full_rebase_products_ += products;
    return products;
  }

  difference[0] = 0.0f;
  for (size_t tau = 1; tau <= tau_count_; ++tau) {
    if (variant_ == YinDifferenceVariant::IncrementalF32) {
      float sum = difference[tau];
      for (size_t j = 0; j < hop_; ++j) {
        const float right = j + tau < hop_
                                ? outgoing_[j + tau]
                                : x[j + tau - hop_];
        const float delta = outgoing_[j] - right;
        sum = __builtin_fmaf(-delta, delta, sum);
      }
      const size_t left = frames_ - tau - hop_;
      const size_t right = frames_ - hop_;
      for (size_t j = 0; j < hop_; ++j) {
        const float delta = x[left + j] - x[right + j];
        sum = __builtin_fmaf(delta, delta, sum);
      }
      difference[tau] = sum;
    } else {
      for (size_t j = 0; j < hop_; ++j) {
        const float right = j + tau < hop_
                                ? outgoing_[j + tau]
                                : x[j + tau - hop_];
        const float delta = outgoing_[j] - right;
        add_double_single(tau, -__builtin_fmaf(delta, delta, 0.0f),
                          difference);
      }
      const size_t left = frames_ - tau - hop_;
      const size_t right = frames_ - hop_;
      for (size_t j = 0; j < hop_; ++j) {
        const float delta = x[left + j] - x[right + j];
        add_double_single(tau, __builtin_fmaf(delta, delta, 0.0f),
                          difference);
      }
      // TwoSum renormalization keeps lo below half an ulp of hi, so the
      // binary32 materialization is exactly the normalized hi component.
    }
  }
  capture_outgoing(x);
  ++updates_since_rebase_;
  const uint64_t terms = 2 * hop_ * static_cast<uint64_t>(tau_count_);
  update_terms_ += terms;
  return terms;
}

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
  if ((c.yin_difference == YinDifferenceVariant::IncrementalF32 ||
       c.yin_difference == YinDifferenceVariant::IncrementalDoubleSingle) &&
      !incremental_.init(c.yin_difference, c.window_size, c.hop_size,
                         tau_max_ + 1, c.yin_incremental_rebase_hops))
    return false;
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
  const size_t tau_count = tau_max_ + 1;
  const bool incremental =
      config_.yin_difference == YinDifferenceVariant::IncrementalF32 ||
      config_.yin_difference == YinDifferenceVariant::IncrementalDoubleSingle;
  const uint64_t inner_iterations =
      incremental
          ? incremental_.compute(x, difference_.data())
          : (yin_difference_compute(config_.yin_difference, x, n, tau_count,
                                    difference_.data()),
             yin_difference_product_count(n, tau_count));
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
  incremental_.reset();
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
  result.incremental_rebases = incremental_.incremental_rebases();
  result.incremental_gap_rebases = incremental_.incremental_gap_rebases();
  result.periodic_rebases = incremental_.periodic_rebases();
  result.incremental_update_terms = incremental_.update_terms();
  result.full_rebase_products = incremental_.full_rebase_products();
  return result;
}
