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

const char *yin_cmnd_variant_name(YinCmndVariant variant) {
  switch (variant) {
  case YinCmndVariant::ReferenceDouble:
    return "YIN_CMND_REFERENCE_DOUBLE";
  case YinCmndVariant::DoubleSumF32Div:
    return "YIN_CMND_DOUBLE_SUM_F32_DIV";
  case YinCmndVariant::F32:
    return "YIN_CMND_F32";
  case YinCmndVariant::F32Compensated:
    return "YIN_CMND_F32_COMPENSATED";
  }
  return "YIN_CMND_UNKNOWN";
}

const char *yin_energy_variant_name(YinEnergyVariant variant) {
  switch (variant) {
  case YinEnergyVariant::ReferenceDouble:
    return "YIN_ENERGY_REFERENCE_DOUBLE";
  case YinEnergyVariant::F32:
    return "YIN_ENERGY_F32";
  case YinEnergyVariant::F32Compensated:
    return "YIN_ENERGY_F32_COMPENSATED";
  }
  return "YIN_ENERGY_UNKNOWN";
}

extern "C" YIN_NOINLINE double
YIN_ENERGY_REFERENCE_DOUBLE(const float *samples, size_t frames) {
  double energy = 0.0;
  for (size_t i = 0; i < frames; ++i)
    energy += static_cast<double>(samples[i]) * samples[i];
  return energy;
}

extern "C" YIN_NOINLINE double
YIN_ENERGY_F32(const float *samples, size_t frames) {
  float energy = 0.0f;
  for (size_t i = 0; i < frames; ++i)
    energy += samples[i] * samples[i];
  return static_cast<double>(energy);
}

extern "C" YIN_NOINLINE double
YIN_ENERGY_F32_COMPENSATED(const float *samples, size_t frames) {
  float hi = 0.0f;
  float lo = 0.0f;
  for (size_t i = 0; i < frames; ++i) {
    const float value = samples[i];
    const float product_hi = value * value;
    const float product_lo = std::fma(value, value, -product_hi);

    // The same TwoSum + deterministic renormalization qualified for LPC
    // frame energy.  The hot loop contains binary32 arithmetic only.
    const float sum = hi + product_hi;
    const float product_virtual = sum - hi;
    const float sum_error =
        (hi - (sum - product_virtual)) + (product_hi - product_virtual);
    const float tail = (lo + sum_error) + product_lo;
    const float renormalized = sum + tail;
    const float tail_virtual = renormalized - sum;
    lo = (sum - (renormalized - tail_virtual)) + (tail - tail_virtual);
    hi = renormalized;
  }
  return static_cast<double>(hi) + static_cast<double>(lo);
}

double yin_energy_compute(YinEnergyVariant variant, const float *samples,
                          size_t frames) {
  switch (variant) {
  case YinEnergyVariant::ReferenceDouble:
    return YIN_ENERGY_REFERENCE_DOUBLE(samples, frames);
  case YinEnergyVariant::F32:
    return YIN_ENERGY_F32(samples, frames);
  case YinEnergyVariant::F32Compensated:
    return YIN_ENERGY_F32_COMPENSATED(samples, frames);
  }
  return YIN_ENERGY_REFERENCE_DOUBLE(samples, frames);
}

extern "C" YIN_NOINLINE void
YIN_CMND_REFERENCE_DOUBLE(const float *difference, size_t tau_count,
                          float *cmnd) {
  cmnd[0] = 1.0f;
  double cumulative = 0.0;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    cumulative += difference[tau];
    // Usual arithmetic conversions form difference[tau] * tau in binary32,
    // then extend that rounded numerator for the binary64 division.
    cmnd[tau] = cumulative > 1e-20
                    ? static_cast<float>(difference[tau] * tau / cumulative)
                    : 1.0f;
  }
}

extern "C" YIN_NOINLINE void
YIN_CMND_DOUBLE_SUM_F32_DIV(const float *difference, size_t tau_count,
                           float *cmnd) {
  cmnd[0] = 1.0f;
  double cumulative = 0.0;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    cumulative += difference[tau];
    const float denominator = static_cast<float>(cumulative);
    const float numerator = difference[tau] * static_cast<float>(tau);
    cmnd[tau] = denominator > 1e-20f ? numerator / denominator : 1.0f;
  }
}

extern "C" YIN_NOINLINE void
YIN_CMND_F32(const float *difference, size_t tau_count, float *cmnd) {
  cmnd[0] = 1.0f;
  float cumulative = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    cumulative += difference[tau];
    const float numerator = difference[tau] * static_cast<float>(tau);
    cmnd[tau] = cumulative > 1e-20f ? numerator / cumulative : 1.0f;
  }
}

extern "C" YIN_NOINLINE void
YIN_CMND_F32_COMPENSATED(const float *difference, size_t tau_count,
                         float *cmnd) {
  cmnd[0] = 1.0f;
  float hi = 0.0f;
  float lo = 0.0f;
  for (size_t tau = 1; tau <= tau_count; ++tau) {
    // Knuth TwoSum followed by deterministic binary32 renormalization. The
    // hot loop deliberately contains no binary64 arithmetic.
    const float value = difference[tau];
    const float sum = hi + value;
    const float value_virtual = sum - hi;
    const float error = (hi - (sum - value_virtual)) +
                        (value - value_virtual);
    const float residual = lo + error;
    hi = sum + residual;
    lo = residual - (hi - sum);
    const float denominator = hi + lo;
    const float numerator = value * static_cast<float>(tau);
    cmnd[tau] = denominator > 1e-20f ? numerator / denominator : 1.0f;
  }
}

void yin_cmnd_compute(YinCmndVariant variant, const float *difference,
                      size_t tau_count, float *cmnd) {
  switch (variant) {
  case YinCmndVariant::ReferenceDouble:
    YIN_CMND_REFERENCE_DOUBLE(difference, tau_count, cmnd);
    break;
  case YinCmndVariant::DoubleSumF32Div:
    YIN_CMND_DOUBLE_SUM_F32_DIV(difference, tau_count, cmnd);
    break;
  case YinCmndVariant::F32:
    YIN_CMND_F32(difference, tau_count, cmnd);
    break;
  case YinCmndVariant::F32Compensated:
    YIN_CMND_F32_COMPENSATED(difference, tau_count, cmnd);
    break;
  }
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

void YinIncrementalDifference::reset_measurement_telemetry() {
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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  if (!profiler_.enable_distribution_range(
          profile_section(PitchAnalysisProfileSection::YinEnergy),
          profile_section(PitchAnalysisProfileSection::YinTotal)))
    return false;
#endif
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
  const double energy = yin_energy_compute(config_.yin_energy, x, n);
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
  yin_cmnd_compute(config_.yin_cmnd, difference_.data(), tau_max_ + 1,
                   cmnd_.data());
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
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  const uint32_t cmnd_nano = static_cast<uint32_t>(std::lround(
      std::clamp(out.yin_min, 0.0f, 1.0f) * 1000000000.0f));
  const uint32_t threshold_nano = static_cast<uint32_t>(std::lround(
      std::fabs(out.yin_min - config_.yin_threshold) * 1000000000.0f));
  selected_cmnd_sum_nano_.fetch_add(cmnd_nano, std::memory_order_relaxed);
  uint32_t observed = selected_cmnd_min_nano_.load(std::memory_order_relaxed);
  while (cmnd_nano < observed &&
         !selected_cmnd_min_nano_.compare_exchange_weak(
             observed, cmnd_nano, std::memory_order_relaxed)) {
  }
  observed = closest_threshold_nano_.load(std::memory_order_relaxed);
  while (threshold_nano < observed &&
         !closest_threshold_nano_.compare_exchange_weak(
             observed, threshold_nano, std::memory_order_relaxed)) {
  }
  const uint32_t selected_tau = static_cast<uint32_t>(candidate);
  observed = selected_tau_min_.load(std::memory_order_relaxed);
  while (selected_tau < observed &&
         !selected_tau_min_.compare_exchange_weak(
             observed, selected_tau, std::memory_order_relaxed)) {
  }
  observed = selected_tau_max_.load(std::memory_order_relaxed);
  while (selected_tau > observed &&
         !selected_tau_max_.compare_exchange_weak(
             observed, selected_tau, std::memory_order_relaxed)) {
  }
#endif
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

void YinDetector::reset_measurement_telemetry() {
  profiler_.reset();
  executions_.store(0, std::memory_order_relaxed);
  inner_iterations_.store(0, std::memory_order_relaxed);
  incremental_.reset_measurement_telemetry();
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  selected_cmnd_sum_nano_.store(0, std::memory_order_relaxed);
  selected_cmnd_min_nano_.store(1000000000U, std::memory_order_relaxed);
  closest_threshold_nano_.store(1000000000U, std::memory_order_relaxed);
  selected_tau_min_.store(UINT32_MAX, std::memory_order_relaxed);
  selected_tau_max_.store(0, std::memory_order_relaxed);
#endif
}
void YinDetector::reset() {
  difference_.fill(0);
  cmnd_.fill(0);
  profiler_.reset();
  incremental_.reset();
  executions_.store(0, std::memory_order_relaxed);
  inner_iterations_.store(0, std::memory_order_relaxed);
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  selected_cmnd_sum_nano_.store(0, std::memory_order_relaxed);
  selected_cmnd_min_nano_.store(1000000000U, std::memory_order_relaxed);
  closest_threshold_nano_.store(1000000000U, std::memory_order_relaxed);
  selected_tau_min_.store(UINT32_MAX, std::memory_order_relaxed);
  selected_tau_max_.store(0, std::memory_order_relaxed);
#endif
}
ProfileStats YinDetector::profile(PitchAnalysisProfileSection s) const {
  if (s < PitchAnalysisProfileSection::YinEnergy ||
      s > PitchAnalysisProfileSection::YinTotal)
    return {};
  return profiler_.stats(profile_section(s));
}
ProfileDistributionStats
YinDetector::profile_distribution(PitchAnalysisProfileSection s) const {
  if (s < PitchAnalysisProfileSection::YinEnergy ||
      s > PitchAnalysisProfileSection::YinTotal)
    return {};
  return profiler_.distribution_stats(profile_section(s));
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
  result.yin_threshold = config_.yin_threshold;
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  result.selected_cmnd_minimum =
      selected_cmnd_min_nano_.load(std::memory_order_relaxed) * 1.0e-9f;
  result.closest_threshold_distance =
      closest_threshold_nano_.load(std::memory_order_relaxed) * 1.0e-9f;
  result.selected_cmnd_average = result.executions
      ? static_cast<float>(selected_cmnd_sum_nano_.load(
            std::memory_order_relaxed) * 1.0e-9 / result.executions)
      : 0.0f;
  const uint32_t tau_min = selected_tau_min_.load(std::memory_order_relaxed);
  result.selected_tau_minimum = tau_min == UINT32_MAX ? 0 : tau_min;
  result.selected_tau_maximum =
      selected_tau_max_.load(std::memory_order_relaxed);
#endif
  return result;
}
