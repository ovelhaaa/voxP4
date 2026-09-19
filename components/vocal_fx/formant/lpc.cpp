#include "lpc.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define VOXP4_WARP_IRAM IRAM_ATTR
#else
#define VOXP4_WARP_IRAM
#endif

#ifdef ESP_PLATFORM
TCM_DRAM_ATTR std::array<float, 1024> SharedLpcAnalysis::hann_{};
std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
    SharedLpcAnalysis::gainnorm_cos_basis_{};
std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
    SharedLpcAnalysis::gainnorm_sin_basis_{};
#else
std::array<float, 1024> SharedLpcAnalysis::hann_{};
std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
    SharedLpcAnalysis::gainnorm_cos_basis_{};
std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
    SharedLpcAnalysis::gainnorm_sin_basis_{};
#endif
bool SharedLpcAnalysis::gainnorm_basis_ready_ = false;
float SharedLpcAnalysis::gainnorm_basis_rate_ = 0.0f;
// B4D.4 LPC invalid-reason counters (file scope; no engine .bss growth).
static uint64_t s_lpc_unvoiced = 0;
static uint64_t s_lpc_solve_fail = 0;

// B4D.5 model-lookup decomposition (observability only, opt-in).
static bool s_b4d5_model_audit = false;
static uint64_t s_b4d5_model_calls = 0;
static uint64_t s_b4d5_model_candidates = 0;
static uint64_t s_b4d5_model_scan_cycles = 0;
static uint64_t s_b4d5_model_copies = 0;
static uint64_t s_b4d5_model_repeat = 0;
static uint64_t s_b4d5_model_last_ts = UINT64_MAX;

void SharedLpcAnalysis::invalid_reason_counts(uint64_t *unvoiced,
                                              uint64_t *solve_fail) {
  if (unvoiced) *unvoiced = s_lpc_unvoiced;
  if (solve_fail) *solve_fail = s_lpc_solve_fail;
}
void SharedLpcAnalysis::reset_invalid_reason_counts() {
  s_lpc_unvoiced = 0;
  s_lpc_solve_fail = 0;
}

namespace {
constexpr float kPi = 3.14159265358979323846f;
ProfileSection section(LpcProfileSection s) {
  return static_cast<ProfileSection>(static_cast<size_t>(ProfileSection::LpcFifoDrain) +
                                     static_cast<size_t>(s));
}

#if defined(__GNUC__)
#define VF_LPC_NOINLINE __attribute__((noinline))
#else
#define VF_LPC_NOINLINE
#endif

VF_LPC_NOINLINE void autocorr_reference_double(const float *y, size_t n,
                                                uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k)
    for (size_t i = k; i < n; ++i)
      r[k] += double(y[i]) * y[i - k];
}

VF_LPC_NOINLINE void autocorr_float_scalar(const float *y, size_t n,
                                           uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k) {
    float sum = 0.0f;
    for (size_t i = k; i < n; ++i)
      sum += y[i] * y[i - k];
    r[k] = static_cast<double>(sum);
  }
}

VF_LPC_NOINLINE void autocorr_float_multiacc(const float *y, size_t n,
                                             uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k) {
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    size_t i = k;
    for (; i + 3 < n; i += 4) {
      s0 += y[i] * y[i - k];
      s1 += y[i + 1] * y[i + 1 - k];
      s2 += y[i + 2] * y[i + 2 - k];
      s3 += y[i + 3] * y[i + 3 - k];
    }
    for (; i < n; ++i)
      s0 += y[i] * y[i - k];
    // Fixed pairwise reduction order is part of this variant's definition.
    const float sum = (s0 + s1) + (s2 + s3);
    r[k] = static_cast<double>(sum);
  }
}

VF_LPC_NOINLINE void autocorr_f32_kahan(const float *y, size_t n,
                                        uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k) {
    float sum = 0.0f;
    float compensation = 0.0f;
    for (size_t i = k; i < n; ++i) {
      const float product = y[i] * y[i - k];
      const float corrected = product - compensation;
      const float next = sum + corrected;
      compensation = (next - sum) - corrected;
      sum = next;
    }
    r[k] = static_cast<double>(sum);
  }
}

VF_LPC_NOINLINE void autocorr_f32_fma_product(const float *y, size_t n,
                                              uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k) {
    float sum = 0.0f;
    float product_residual = 0.0f;
    for (size_t i = k; i < n; ++i) {
      const float a = y[i];
      const float b = y[i - k];
      const float product = a * b;
      const float residual = std::fma(a, b, -product);
      sum += product;
      product_residual += residual;
    }
    r[k] = static_cast<double>(sum) +
           static_cast<double>(product_residual);
  }
}

// Error-free transforms for binary32 arithmetic. These functions deliberately
// remain inline so the target disassembly exposes only single-precision FPU
// operations in the autocorrelation inner loop.
inline void two_sum_f32(float a, float b, float *sum, float *error) {
  const float s = a + b;
  const float bb = s - a;
  *error = (a - (s - bb)) + (b - bb);
  *sum = s;
}

inline float runtime_hann(size_t i, size_t n) {
  // This expression intentionally matches the B4B.4F production loop exactly.
  return .5f - .5f * std::cos(2 * kPi * i / (n - 1));
}

VF_LPC_NOINLINE double window_reference(const float *x, size_t n, float pre,
                                         float *y) {
  double energy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float v = x[i] - (i ? pre * x[i - 1] : 0.0f);
    const float w = runtime_hann(i, n);
    y[i] = v * w;
    energy += double(y[i]) * y[i];
  }
  return energy;
}

VF_LPC_NOINLINE double window_cached_double(const float *x, size_t n,
                                             float pre, const float *hann,
                                             float *y) {
  double energy = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float v = x[i] - (i ? pre * x[i - 1] : 0.0f);
    y[i] = v * hann[i];
    energy += double(y[i]) * y[i];
  }
  return energy;
}

VF_LPC_NOINLINE double window_cached_compensated(
    const float *x, size_t n, float pre, const float *hann, float *y) {
  float energy_hi = 0.0f;
  float energy_lo = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float v = x[i] - (i ? pre * x[i - 1] : 0.0f);
    y[i] = v * hann[i];
    const float product_hi = y[i] * y[i];
    const float product_lo = std::fma(y[i], y[i], -product_hi);
    float sum = 0.0f;
    float sum_error = 0.0f;
    two_sum_f32(energy_hi, product_hi, &sum, &sum_error);
    const float tail = (energy_lo + sum_error) + product_lo;
    two_sum_f32(sum, tail, &energy_hi, &energy_lo);
  }
  return static_cast<double>(energy_hi) + static_cast<double>(energy_lo);
}

VF_LPC_NOINLINE void window_cached_only(const float *x, size_t n, float pre,
                                         const float *hann, float *y) {
  for (size_t i = 0; i < n; ++i) {
    const float v = x[i] - (i ? pre * x[i - 1] : 0.0f);
    y[i] = v * hann[i];
  }
}

VF_LPC_NOINLINE void autocorr_f32_double_single(const float *y, size_t n,
                                                uint16_t order, double *r) {
  for (size_t k = 0; k <= order; ++k) {
    float hi = 0.0f;
    float lo = 0.0f;
    for (size_t i = k; i < n; ++i) {
      const float a = y[i];
      const float b = y[i - k];
      const float product_hi = a * b;
      const float product_lo = std::fma(a, b, -product_hi);

      float sum = 0.0f;
      float sum_error = 0.0f;
      two_sum_f32(hi, product_hi, &sum, &sum_error);

      // Use full TwoSum for the final renormalization as autocorrelation terms
      // can cancel; unlike FastTwoSum it needs no magnitude ordering assumption.
      const float tail = (lo + sum_error) + product_lo;
      two_sum_f32(sum, tail, &hi, &lo);
    }
    r[k] = static_cast<double>(hi) + static_cast<double>(lo);
  }
}
}

const char *lpc_window_variant_name(LpcWindowVariant variant) {
  switch (variant) {
  case LpcWindowVariant::Reference:
    return "LPC_WINDOW_REFERENCE";
  case LpcWindowVariant::PrecomputedHannDoubleEnergy:
    return "LPC_WINDOW_HANN_DOUBLE_ENERGY";
  case LpcWindowVariant::PrecomputedHannCompensatedEnergy:
    return "LPC_ENERGY_F32_COMPENSATED";
  case LpcWindowVariant::EnergyFromAutocorrR0:
    return "LPC_ENERGY_FROM_AUTOCORR_R0";
  }
  return "LPC_WINDOW_REFERENCE";
}

const char *lpc_autocorrelation_variant_name(
    LpcAutocorrelationVariant variant) {
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
  return "AUTOCORR_UNKNOWN";
}

bool SharedLpcAnalysis::init(float rate, const LpcConfig &c) {
  if (!std::isfinite(rate) || rate < 8000 || c.order < 1 ||
      c.order > VOCAL_FX_LPC_MAX_ORDER || c.window_size < c.order + 2 ||
      c.window_size > circular_frame_.size() || c.hop_size == 0 ||
      c.hop_size > c.window_size || !std::isfinite(c.preemphasis) ||
      c.preemphasis < 0 || c.preemphasis >= 1)
    return false;
  sample_rate_ = rate;
  prepare_gainnorm_basis(rate);
  config_ = c;
  if (!prepare_hann(hann_.data(), config_.window_size))
    return false;
  reset();
  return true;
}
void SharedLpcAnalysis::reset() {
  fifo_.reset(); circular_frame_.fill(0); linear_frame_.fill(0);
  write_index_ = valid_count_ = since_frame_ = 0; last_position_ = 0;
  telemetry_ = {}; profiler_.reset(); published_.store(0, std::memory_order_release);
  frame_cost_histogram_.fill(0); frame_cost_count_ = 0;
  frame_cost_total_us_ = 0; frame_cost_max_us_ = 0;
  for (auto &m : models_) { m.sequence.store(0); m.valid.store(0); }
  publish_telemetry();
}
void SharedLpcAnalysis::reset_measurement_telemetry() {
  profiler_.reset();
  frame_cost_histogram_.fill(0);
  frame_cost_count_ = 0;
  frame_cost_total_us_ = 0;
  frame_cost_max_us_ = 0;
}

void SharedLpcAnalysis::prepare_gainnorm_basis(float sample_rate) {
  constexpr size_t kPoints = 24;
  static constexpr float kFrequencies[kPoints] = {
      200.0f, 240.0f, 290.0f, 350.0f, 420.0f, 500.0f,
      600.0f, 720.0f, 860.0f, 1030.0f, 1230.0f, 1470.0f,
      1760.0f, 2100.0f, 2500.0f, 2900.0f, 3300.0f, 3700.0f,
      4000.0f, 4200.0f, 4400.0f, 4600.0f, 4800.0f, 5000.0f};
  gainnorm_basis_ready_ = false;
  if (!(sample_rate >= 8000.0f)) return;
  for (size_t p = 0; p < kPoints; ++p) {
    const float w = 2.0f * kPi * (kFrequencies[p] / sample_rate);
    for (size_t k = 0; k <= VOCAL_FX_LPC_MAX_ORDER; ++k) {
      const float phase = -static_cast<float>(k) * w;
      gainnorm_cos_basis_[p][k] = std::cos(phase);
      gainnorm_sin_basis_[p][k] = std::sin(phase);
    }
  }
  gainnorm_basis_rate_ = sample_rate;
  gainnorm_basis_ready_ = true;
}

void SharedLpcAnalysis::set_gainnorm_basis_enabled(bool enabled) {
  if (!enabled) {
    gainnorm_basis_ready_ = false;
    return;
  }
  if (!gainnorm_basis_ready_)
    prepare_gainnorm_basis(gainnorm_basis_rate_ >= 8000.0f ? gainnorm_basis_rate_
                                                            : 48000.0f);
}

bool SharedLpcAnalysis::gainnorm_basis_enabled() {
  return gainnorm_basis_ready_;
}

size_t SharedLpcAnalysis::gainnorm_basis_bit_mismatches(float sample_rate) {
  if (!gainnorm_basis_ready_ || sample_rate != gainnorm_basis_rate_) return 0;
  constexpr size_t kPoints = 24;
  static constexpr float kFrequencies[kPoints] = {
      200.0f, 240.0f, 290.0f, 350.0f, 420.0f, 500.0f,
      600.0f, 720.0f, 860.0f, 1030.0f, 1230.0f, 1470.0f,
      1760.0f, 2100.0f, 2500.0f, 2900.0f, 3300.0f, 3700.0f,
      4000.0f, 4200.0f, 4400.0f, 4600.0f, 4800.0f, 5000.0f};
  size_t mismatches = 0;
  for (size_t p = 0; p < kPoints; ++p) {
    const float w = 2.0f * kPi * (kFrequencies[p] / sample_rate);
    for (size_t k = 0; k <= VOCAL_FX_LPC_MAX_ORDER; ++k) {
      const float phase = -static_cast<float>(k) * w;
      const float c = std::cos(phase);
      const float s = std::sin(phase);
      mismatches += std::memcmp(&c, &gainnorm_cos_basis_[p][k], sizeof(float)) != 0;
      mismatches += std::memcmp(&s, &gainnorm_sin_basis_[p][k], sizeof(float)) != 0;
    }
  }
  return mismatches;
}

void SharedLpcAnalysis::tap(const float *x, size_t n) {
  for (size_t i=0;i<n;++i)
    (void)fifo_.push({std::isfinite(x[i]) ? x[i] : 0.0f,
                      static_cast<uint32_t>(last_position_++)});
}
bool SharedLpcAnalysis::solve(const float *x, size_t n, uint16_t order,
                              float pre, SharedLpcModel *out,
                              Profiler *profiler) {
  return solve_with_autocorrelation(
      x, n, order, pre,
      LpcAutocorrelationVariant::AutocorrReferenceDouble, out, profiler);
}

bool SharedLpcAnalysis::autocorrelate(
    const float *y, size_t n, uint16_t order,
    LpcAutocorrelationVariant variant, double *r) {
  if (!y || !r || n == 0 || n < static_cast<size_t>(order) + 1 ||
      order > VOCAL_FX_LPC_MAX_ORDER)
    return false;
  std::fill_n(r, static_cast<size_t>(order) + 1, 0.0);
  switch (variant) {
  case LpcAutocorrelationVariant::AutocorrReferenceDouble:
    autocorr_reference_double(y, n, order, r);
    break;
  case LpcAutocorrelationVariant::AutocorrFloatScalar:
    autocorr_float_scalar(y, n, order, r);
    break;
  case LpcAutocorrelationVariant::AutocorrFloatMultiacc:
    autocorr_float_multiacc(y, n, order, r);
    break;
  case LpcAutocorrelationVariant::AutocorrF32Kahan:
    autocorr_f32_kahan(y, n, order, r);
    break;
  case LpcAutocorrelationVariant::AutocorrF32FmaProduct:
    autocorr_f32_fma_product(y, n, order, r);
    break;
  case LpcAutocorrelationVariant::AutocorrF32DoubleSingle:
    autocorr_f32_double_single(y, n, order, r);
    break;
  default:
    return false;
  }
  return true;
}

bool SharedLpcAnalysis::solve_with_autocorrelation(
    const float *x, size_t n, uint16_t order, float pre,
    LpcAutocorrelationVariant variant, SharedLpcModel *out,
    Profiler *profiler) {
  return solve_with_kernels(
      x, n, order, pre, variant, LpcWindowVariant::Reference, nullptr, out,
      profiler);
}

bool SharedLpcAnalysis::prepare_hann(float *hann, size_t n) {
  if (!hann || n < 2 || n > 1024)
    return false;
  for (size_t i = 0; i < n; ++i)
    hann[i] = runtime_hann(i, n);
  return true;
}

bool SharedLpcAnalysis::window_frame(
    const float *x, size_t n, float pre, LpcWindowVariant variant,
    const float *precomputed_hann, float *y, double *energy) {
  if (!x || !y || !energy || n < 2 || n > 1024 ||
      (variant != LpcWindowVariant::Reference && !precomputed_hann))
    return false;

  switch (variant) {
  case LpcWindowVariant::Reference:
    *energy = window_reference(x, n, pre, y);
    return true;
  case LpcWindowVariant::PrecomputedHannDoubleEnergy:
    *energy = window_cached_double(x, n, pre, precomputed_hann, y);
    return true;
  case LpcWindowVariant::PrecomputedHannCompensatedEnergy:
    *energy = window_cached_compensated(x, n, pre, precomputed_hann, y);
    return true;
  case LpcWindowVariant::EnergyFromAutocorrR0:
    window_cached_only(x, n, pre, precomputed_hann, y);
    *energy = 0.0;
    return true;
  }
  return false;
}

bool SharedLpcAnalysis::solve_with_kernels(
    const float *x, size_t n, uint16_t order, float pre,
    LpcAutocorrelationVariant variant, LpcWindowVariant windowing,
    const float *precomputed_hann, SharedLpcModel *out, Profiler *profiler,
    double *frame_energy) {
  if (!x || !out || n > 1024 || n < static_cast<size_t>(order) + 2 ||
      order > VOCAL_FX_LPC_MAX_ORDER)
    return false;
  if (profiler)
    profiler->begin(section(LpcProfileSection::SolveTotal));
  std::array<float, 1024> y{};
  if (profiler)
    profiler->begin(section(LpcProfileSection::SolveWindowing));
  double energy = 0.0;
  if (!window_frame(x, n, pre, windowing, precomputed_hann, y.data(),
                    &energy)) {
    if (profiler) profiler->end(section(LpcProfileSection::SolveWindowing));
    if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
    return false;
  }
  if (profiler)
    profiler->end(section(LpcProfileSection::SolveWindowing));
  if (windowing != LpcWindowVariant::EnergyFromAutocorrR0 &&
      (!(energy > 1e-8) || !std::isfinite(energy))) {
    if (frame_energy) *frame_energy = energy;
    if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
    return false;
  }
  std::array<double,VOCAL_FX_LPC_MAX_ORDER+1> r{},a{},next{};
  if (profiler)
    profiler->begin(section(LpcProfileSection::Autocorrelation));
  if (!autocorrelate(y.data(), n, order, variant, r.data())) {
    if (profiler) profiler->end(section(LpcProfileSection::Autocorrelation));
    if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
    return false;
  }
  if (windowing == LpcWindowVariant::EnergyFromAutocorrR0) {
    energy = r[0];
    if (!(energy > 1e-8) || !std::isfinite(energy)) {
      if (frame_energy) *frame_energy = energy;
      if (profiler) profiler->end(section(LpcProfileSection::Autocorrelation));
      if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
      return false;
    }
  }
  if (frame_energy) *frame_energy = energy;
  // Tiny diagonal loading prevents perfectly periodic synthetic frames from
  // producing a singular Toeplitz system without materially flattening speech.
  r[0] *= 1.000001;
  if (profiler)
    profiler->end(section(LpcProfileSection::Autocorrelation));
  if (profiler)
    profiler->begin(section(LpcProfileSection::LevinsonDurbin));
  double error=r[0]; a[0]=1.0;
  for(size_t i=1;i<=order;++i){
    double sum=r[i]; for(size_t j=1;j<i;++j) sum+=a[j]*r[i-j];
    const double reflection=-sum/error;
    if(!std::isfinite(reflection)||std::fabs(reflection)>=.999999||error<=1e-12) {
      if (profiler) profiler->end(section(LpcProfileSection::LevinsonDurbin));
      if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
      return false;
    }
    next=a; next[i]=reflection;
    for(size_t j=1;j<i;++j) next[j]=a[j]+reflection*a[i-j];
    a=next; error*=1.0-reflection*reflection;
    if(!std::isfinite(error)||error<=1e-12) {
      if (profiler) profiler->end(section(LpcProfileSection::LevinsonDurbin));
      if (profiler) profiler->end(section(LpcProfileSection::SolveTotal));
      return false;
    }
  }
  if (profiler)
    profiler->end(section(LpcProfileSection::LevinsonDurbin));
  out->coefficients.fill(0); for(size_t i=0;i<=order;++i) out->coefficients[i]=float(a[i]);
  out->order=order; out->prediction_error=float(error/r[0]);
  out->confidence=std::clamp(1.0f-out->prediction_error,0.0f,1.0f);
  out->valid=std::isfinite(out->prediction_error);
  if (profiler)
    profiler->end(section(LpcProfileSection::SolveTotal));
  return out->valid;
}

float SharedLpcAnalysis::lambda_from_semitones(float semitones) {
  if (std::fabs(semitones) < 1e-4f) return 0.0f;
  const float beta = std::exp2(std::clamp(semitones, -12.0f, 12.0f) / 12.0f);
  return (1.0f - beta) / (1.0f + beta);
}

bool VOXP4_WARP_IRAM SharedLpcAnalysis::warp_polynomial(const float *a_in, uint16_t order,
                                        float lambda, float gamma, float *a_out) {
  if (!a_in || !a_out || order == 0 || order > VOCAL_FX_LPC_MAX_ORDER)
    return false;
  if (std::fabs(lambda) < 1e-5f && (gamma >= 0.9999f || gamma <= 0.0f)) {
    for (size_t i = 0; i <= order; ++i) a_out[i] = a_in[i];
    return true;
  }
  if (std::fabs(lambda) < 1e-5f) {
    float g = 1.0f;
    for (size_t i = 0; i <= order; ++i) {
      a_out[i] = a_in[i] * g;
      g *= gamma;
    }
    return true;
  }

  lambda = std::clamp(lambda, -0.4f, 0.4f);

  std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, VOCAL_FX_LPC_MAX_ORDER + 1> N_pow{};
  std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, VOCAL_FX_LPC_MAX_ORDER + 1> D_pow{};

  N_pow[0][0] = 1.0f;
  D_pow[0][0] = 1.0f;

  for (size_t k = 1; k <= order; ++k) {
    N_pow[k][0] = -lambda * N_pow[k - 1][0];
    for (size_t j = 1; j <= k; ++j) {
      N_pow[k][j] = -lambda * N_pow[k - 1][j] + N_pow[k - 1][j - 1];
    }
    D_pow[k][0] = D_pow[k - 1][0];
    for (size_t j = 1; j <= k; ++j) {
      D_pow[k][j] = D_pow[k - 1][j] - lambda * D_pow[k - 1][j - 1];
    }
  }

  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> res{};
  float g = 1.0f;
  for (size_t k = 0; k <= order; ++k) {
    const float coeff = a_in[k] * g;
    if (gamma > 0.0f && gamma < 1.0f) g *= gamma;
    const size_t p_minus_k = order - k;
    for (size_t i = 0; i <= k; ++i) {
      const float ni = N_pow[k][i];
      for (size_t j = 0; j <= p_minus_k; ++j) {
        res[i + j] += coeff * ni * D_pow[p_minus_k][j];
      }
    }
  }

  if (std::fabs(res[0]) < 1e-8f || !std::isfinite(res[0])) {
    for (size_t i = 0; i <= order; ++i) a_out[i] = a_in[i];
    return false;
  }

  const float inv_r0 = 1.0f / res[0];
  for (size_t i = 0; i <= order; ++i) {
    a_out[i] = res[i] * inv_r0;
  }
  return true;
}

float VOXP4_WARP_IRAM SharedLpcAnalysis::compute_gain_normalization(const float *a_orig, const float *a_warped,
                                                    uint16_t order, FormantNormalizationStrategy strategy,
                                                    float sample_rate) {
  if (!a_orig || !a_warped || order == 0 || order > VOCAL_FX_LPC_MAX_ORDER)
    return 1.0f;

  switch (strategy) {
  case FormantNormalizationStrategy::StrategyA_DC: {
    float sum_orig = 0.0f, sum_warped = 0.0f;
    for (size_t i = 0; i <= order; ++i) {
      sum_orig += a_orig[i];
      sum_warped += a_warped[i];
    }
    const float abs_orig = std::fabs(sum_orig);
    const float abs_warped = std::fabs(sum_warped);
    if (abs_orig > 1e-4f && abs_warped > 1e-4f) {
      return std::clamp(abs_warped / abs_orig, 0.01f, 10.0f);
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyB_ReferenceFrequency: {
    // Reference frequency: 800 Hz (typical vocal vowel formant center)
    const float f_ref = 800.0f;
    const float w = 2.0f * kPi * (f_ref / sample_rate);
    float re_orig = 0.0f, im_orig = 0.0f;
    float re_warp = 0.0f, im_warp = 0.0f;
    for (size_t k = 0; k <= order; ++k) {
      const float phase = -static_cast<float>(k) * w;
      const float c = std::cos(phase);
      const float s = std::sin(phase);
      re_orig += a_orig[k] * c;
      im_orig += a_orig[k] * s;
      re_warp += a_warped[k] * c;
      im_warp += a_warped[k] * s;
    }
    const float mag_orig_sq = re_orig * re_orig + im_orig * im_orig;
    const float mag_warp_sq = re_warp * re_warp + im_warp * im_warp;
    if (mag_orig_sq > 1e-8f && mag_warp_sq > 1e-8f) {
      return std::clamp(std::sqrt(mag_warp_sq / mag_orig_sq), 0.01f, 10.0f);
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyC_IntegratedSpectral: {
    // 24 grid points log-spaced between 200 Hz and 4500 Hz
    constexpr size_t kPoints = 24;
    static constexpr float kFrequencies[kPoints] = {
      200.0f, 240.0f, 290.0f, 350.0f, 420.0f, 500.0f,
      600.0f, 720.0f, 860.0f, 1030.0f, 1230.0f, 1470.0f,
      1760.0f, 2100.0f, 2500.0f, 2900.0f, 3300.0f, 3700.0f,
      4000.0f, 4200.0f, 4400.0f, 4600.0f, 4800.0f, 5000.0f
    };
    double sum_mag_orig_sq = 0.0;
    double sum_mag_warp_sq = 0.0;

    const bool use_basis = gainnorm_basis_ready_ && sample_rate == gainnorm_basis_rate_;
    for (size_t p = 0; p < kPoints; ++p) {
      const float w = 2.0f * kPi * (kFrequencies[p] / sample_rate);
      float re_o = 0.0f, im_o = 0.0f;
      float re_w = 0.0f, im_w = 0.0f;
      if (use_basis) {
        // Basis precomputed: branch-free inner loop, identical order/values.
        const float *cb = gainnorm_cos_basis_[p].data();
        const float *sb = gainnorm_sin_basis_[p].data();
        for (size_t k = 0; k <= order; ++k) {
          const float c = cb[k];
          const float s = sb[k];
          re_o += a_orig[k] * c;
          im_o += a_orig[k] * s;
          re_w += a_warped[k] * c;
          im_w += a_warped[k] * s;
        }
      } else {
        for (size_t k = 0; k <= order; ++k) {
          const float phase = -static_cast<float>(k) * w;
          const float c = std::cos(phase);
          const float s = std::sin(phase);
          re_o += a_orig[k] * c;
          im_o += a_orig[k] * s;
          re_w += a_warped[k] * c;
          im_w += a_warped[k] * s;
        }
      }
      sum_mag_orig_sq += (re_o * re_o + im_o * im_o);
      sum_mag_warp_sq += (re_w * re_w + im_w * im_w);
    }
    if (sum_mag_orig_sq > 1e-12 && sum_mag_warp_sq > 1e-12) {
      return static_cast<float>(std::clamp(std::sqrt(sum_mag_warp_sq / sum_mag_orig_sq), 0.01, 10.0));
    }
    return 1.0f;
  }

  case FormantNormalizationStrategy::StrategyD_ResidualEnergy: {
    // Residual / Excitation energy: ratio of polynomial norms sum(a_k^2)
    float norm_orig = 0.0f, norm_warp = 0.0f;
    for (size_t i = 0; i <= order; ++i) {
      norm_orig += a_orig[i] * a_orig[i];
      norm_warp += a_warped[i] * a_warped[i];
    }
    if (norm_orig > 1e-6f && norm_warp > 1e-6f) {
      return std::clamp(std::sqrt(norm_warp / norm_orig), 0.01f, 10.0f);
    }
    return 1.0f;
  }

  default:
    return 1.0f;
  }
}

void SharedLpcAnalysis::publish(const SharedLpcModel &m) {
  const uint32_t serial=published_.load(std::memory_order_relaxed)+1;
  auto &p=models_[serial%kModelCount]; p.sequence.fetch_add(1,std::memory_order_acq_rel);
  for(size_t i=0;i<p.coefficients.size();++i)p.coefficients[i].store(m.coefficients[i],std::memory_order_relaxed);
  p.error.store(m.prediction_error); p.confidence.store(m.confidence);
  p.timestamp_low.store(uint32_t(m.timestamp)); p.timestamp_high.store(uint32_t(m.timestamp>>32));
  p.order.store(m.order); p.valid.store(m.valid?1:0); p.sequence.fetch_add(1,std::memory_order_release);
  published_.store(serial,std::memory_order_release);
}
size_t SharedLpcAnalysis::run(size_t maximum,const PitchResult &pitch) {
  VF_PROFILE_BEGIN(profiler_, section(LpcProfileSection::Total));
  size_t made=0; LpcSample s;
  uint64_t fifo_cycles = 0, ring_cycles = 0;
  uint64_t fifo_calls = 0, drained = 0;
  while(made<maximum){
    uint32_t cycle_start = Profiler::now_cycles();
    const bool have_sample = fifo_.pop(s);
    fifo_cycles += static_cast<uint32_t>(Profiler::now_cycles() - cycle_start);
    ++fifo_calls;
    if (!have_sample) {
      break;
    }
    ++drained;
    cycle_start = Profiler::now_cycles();
    circular_frame_[write_index_] = s.value;
    if (++write_index_ == config_.window_size)
      write_index_ = 0;
    valid_count_ = std::min<size_t>(valid_count_ + 1, config_.window_size);
    ring_cycles += static_cast<uint32_t>(Profiler::now_cycles() - cycle_start);
    if(valid_count_==config_.window_size && (++since_frame_>=config_.hop_size || telemetry_.lpc_frames==0)){
      const uint64_t frame_start_us = Profiler::now_us();
      since_frame_=0; SharedLpcModel m; m.timestamp=s.input_position-config_.window_size/2;
      VF_PROFILE_BEGIN(profiler_, section(LpcProfileSection::FrameLinearization));
      const size_t first_count = config_.window_size - write_index_;
      std::copy(circular_frame_.begin() + write_index_,
                circular_frame_.begin() + config_.window_size,
                linear_frame_.begin());
      std::copy(circular_frame_.begin(),
                circular_frame_.begin() + write_index_,
                linear_frame_.begin() + first_count);
      VF_PROFILE_END(profiler_, section(LpcProfileSection::FrameLinearization), 0);
      const bool voiced=pitch.voiced && pitch.confidence>.25f;
      if (!voiced) ++s_lpc_unvoiced;
      m.valid=config_.enabled && voiced &&
          solve_with_kernels(
              linear_frame_.data(), config_.window_size, config_.order,
              config_.preemphasis, config_.autocorrelation,
              config_.windowing, hann_.data(), &m,
              &profiler_);
      m.confidence*=std::clamp(pitch.confidence,0.0f,1.0f);
      if(!m.valid){++telemetry_.lpc_invalid_frames; if (voiced) ++s_lpc_solve_fail;}
      telemetry_.max_prediction_error=std::max(telemetry_.max_prediction_error,m.prediction_error);
      VF_PROFILE_BEGIN(profiler_, section(LpcProfileSection::Publication));
      publish(m); ++telemetry_.lpc_frames; ++made;
      publish_telemetry();
      VF_PROFILE_END(profiler_, section(LpcProfileSection::Publication), 0);
      const uint32_t frame_cost_us = static_cast<uint32_t>(
          Profiler::now_us() - frame_start_us);
      const size_t frame_cost_bin = std::min<size_t>(
          frame_cost_us / kFrameCostBinUs, frame_cost_histogram_.size() - 1);
      if (frame_cost_histogram_[frame_cost_bin] != UINT16_MAX)
        ++frame_cost_histogram_[frame_cost_bin];
      ++frame_cost_count_;
      frame_cost_total_us_ += frame_cost_us;
      frame_cost_max_us_ = std::max(frame_cost_max_us_, frame_cost_us);
    }
  }
  telemetry_.samples_drained += drained;
  profiler_.record_cycles(section(LpcProfileSection::FifoDrain), fifo_cycles,
                          fifo_calls);
  profiler_.record_cycles(section(LpcProfileSection::RingWrite), ring_cycles,
                          drained);
  publish_telemetry();
  VF_PROFILE_END(profiler_, section(LpcProfileSection::Total), 0);
  return made;
}
size_t SharedLpcAnalysis::snapshot_models(SharedLpcModel *out,
                                          size_t cap) const {
  if (!out || cap == 0) return 0;
  const uint32_t newest = published_.load(std::memory_order_acquire);
  const uint32_t count = std::min<uint32_t>(newest, kModelCount);
  size_t n = 0;
  for (uint32_t k = 0; k < count && n < cap; ++k) {
    const auto &p = models_[(newest - k) % kModelCount];
    SharedLpcModel m;
    uint32_t before, after;
    do {
      before = p.sequence.load(std::memory_order_acquire);
      if (before & 1U) { after = before; continue; }
      for (size_t i = 0; i < m.coefficients.size(); ++i)
        m.coefficients[i] = p.coefficients[i].load(std::memory_order_relaxed);
      m.prediction_error = p.error.load(std::memory_order_relaxed);
      m.confidence = p.confidence.load(std::memory_order_relaxed);
      m.timestamp = uint64_t(p.timestamp_low.load(std::memory_order_relaxed)) |
                    (uint64_t(p.timestamp_high.load(std::memory_order_relaxed)) << 32);
      m.order = uint16_t(p.order.load(std::memory_order_relaxed));
      m.valid = p.valid.load(std::memory_order_relaxed) != 0;
      after = p.sequence.load(std::memory_order_acquire);
    } while (before != after || (after & 1U));
    if (!m.valid) continue;
    out[n++] = m;
  }
  return n;
}
bool SharedLpcAnalysis::latest_model(SharedLpcModel *out) const {
  if (!out) return false;
  const uint32_t newest = published_.load(std::memory_order_acquire);
  if (newest == 0) return false;
  const auto &p = models_[newest % kModelCount];
  uint32_t before, after;
  do {
    before = p.sequence.load(std::memory_order_acquire);
    if (before & 1U) { after = before; continue; }
    for (size_t i=0;i<out->coefficients.size();++i)
      out->coefficients[i]=p.coefficients[i].load(std::memory_order_relaxed);
    out->prediction_error=p.error.load(std::memory_order_relaxed);
    out->confidence=p.confidence.load(std::memory_order_relaxed);
    out->timestamp=uint64_t(p.timestamp_low.load(std::memory_order_relaxed))|
                   (uint64_t(p.timestamp_high.load(std::memory_order_relaxed))<<32);
    out->order=uint16_t(p.order.load(std::memory_order_relaxed));
    out->valid=p.valid.load(std::memory_order_relaxed)!=0;
    after=p.sequence.load(std::memory_order_acquire);
  } while(before!=after || (after&1U));
  return true;
}
LpcFrameCostSummary SharedLpcAnalysis::frame_cost_summary() {
  LpcFrameCostSummary result{};
  if (frame_cost_count_ == 0) return result;
  const auto percentile_upper_bound = [&](uint32_t value) {
    const uint32_t target =
        (frame_cost_count_ * value + 99U) / 100U;
    uint32_t accumulated = 0;
    for (size_t i = 0; i < frame_cost_histogram_.size(); ++i) {
      accumulated += frame_cost_histogram_[i];
      if (accumulated >= target)
        return std::min<uint32_t>(
            static_cast<uint32_t>((i + 1) * kFrameCostBinUs),
            frame_cost_max_us_);
    }
    return frame_cost_max_us_;
  };
  result.count = frame_cost_count_;
  result.total_us = frame_cost_total_us_;
  result.p50_us = percentile_upper_bound(50);
  result.p95_us = percentile_upper_bound(95);
  result.p99_us = percentile_upper_bound(99);
  result.max_us = frame_cost_max_us_;
  return result;
}
bool SharedLpcAnalysis::model_near(uint64_t ts,SharedLpcModel *out) const {
  if(!out) return false;
  const uint32_t newest=published_.load(std::memory_order_acquire);
  bool found=false; uint64_t best=UINT64_MAX;
  const uint32_t count=std::min<uint32_t>(newest,kModelCount);
  const uint64_t b4d5_scan_start =
      s_b4d5_model_audit ? Profiler::now_cycles() : 0;
  for(uint32_t k=0;k<count;++k){const auto &p=models_[(newest-k)%kModelCount]; const uint32_t before=p.sequence.load(std::memory_order_acquire); if(before&1)continue;
    // B4D.5: read the cheap fields first and copy the coefficient block only
    // when this candidate can actually beat the current best.  This is exactly
    // equivalent to the previous "copy every candidate, keep if d < best"
    // (same strict comparison and newest-first tie-break) but avoids ~16
    // coefficient-block copies per lookup.
    if(p.valid.load(std::memory_order_relaxed)==0) continue;
    const uint64_t t=uint64_t(p.timestamp_low.load(std::memory_order_relaxed))|
                     (uint64_t(p.timestamp_high.load(std::memory_order_relaxed))<<32);
    if(before!=p.sequence.load(std::memory_order_acquire)) continue;
    const uint64_t d=ts>t?ts-t:t-ts;
    if(d>=best) continue;
    SharedLpcModel m; for(size_t i=0;i<m.coefficients.size();++i)m.coefficients[i]=p.coefficients[i].load();
    m.prediction_error=p.error.load();m.confidence=p.confidence.load();m.timestamp=t;m.order=uint16_t(p.order.load());m.valid=true;
    if(before!=p.sequence.load(std::memory_order_acquire)) continue;
    best=d;*out=m;found=true;
    if(s_b4d5_model_audit){++s_b4d5_model_copies;
      if(s_b4d5_model_last_ts==t) ++s_b4d5_model_repeat;}
  }
  if(s_b4d5_model_audit){
    s_b4d5_model_scan_cycles += Profiler::now_cycles() - b4d5_scan_start;
    ++s_b4d5_model_calls;
    s_b4d5_model_candidates += count;
    if(found) s_b4d5_model_last_ts = out->timestamp;
  }
  return found && best<=uint64_t(config_.window_size+config_.hop_size);
}
ProfileStats SharedLpcAnalysis::profile(LpcProfileSection s) const{return profiler_.stats(section(s));}
void SharedLpcAnalysis::publish_telemetry() {
  auto store=[](std::atomic<uint32_t> &lo,std::atomic<uint32_t> &hi,uint64_t v){lo.store(uint32_t(v),std::memory_order_relaxed);hi.store(uint32_t(v>>32),std::memory_order_relaxed);};
  published_telemetry_.sequence.fetch_add(1,std::memory_order_acq_rel);
  store(published_telemetry_.frames_low,published_telemetry_.frames_high,telemetry_.lpc_frames);
  store(published_telemetry_.invalid_low,published_telemetry_.invalid_high,telemetry_.lpc_invalid_frames);
  store(published_telemetry_.fallback_low,published_telemetry_.fallback_high,telemetry_.lpc_fallback_frames);
  store(published_telemetry_.drained_low,published_telemetry_.drained_high,telemetry_.samples_drained);
  published_telemetry_.max_error.store(telemetry_.max_prediction_error,std::memory_order_relaxed);
  published_telemetry_.sequence.fetch_add(1,std::memory_order_release);
}
LpcTelemetry SharedLpcAnalysis::telemetry() const {
  LpcTelemetry result; uint32_t before,after;
  auto load=[](const std::atomic<uint32_t> &lo,const std::atomic<uint32_t> &hi){return uint64_t(lo.load(std::memory_order_relaxed))|(uint64_t(hi.load(std::memory_order_relaxed))<<32);};
  do { before=published_telemetry_.sequence.load(std::memory_order_acquire); if(before&1U){after=before;continue;}
    result.lpc_frames=load(published_telemetry_.frames_low,published_telemetry_.frames_high);
    result.lpc_invalid_frames=load(published_telemetry_.invalid_low,published_telemetry_.invalid_high);
    result.lpc_fallback_frames=load(published_telemetry_.fallback_low,published_telemetry_.fallback_high);
    result.samples_drained=load(published_telemetry_.drained_low,published_telemetry_.drained_high);
    result.max_prediction_error=published_telemetry_.max_error.load(std::memory_order_relaxed);
    after=published_telemetry_.sequence.load(std::memory_order_acquire);
  } while(before!=after || (after&1U)); return result;
}

// B4D.9 diagnostic prewarm helpers (read-only, no state change).
static volatile uint64_t s_b4d9_warm_sink_u64 = 0;
static volatile float s_b4d9_warm_sink_f32 = 0.0f;
void SharedLpcAnalysis::warm_model_ring() const {
  uint64_t acc = 0;
  const uint32_t newest = published_.load(std::memory_order_acquire);
  const uint32_t count = std::min<uint32_t>(newest, kModelCount);
  for (uint32_t k = 0; k < count; ++k) {
    const auto &p = models_[(newest - k) % kModelCount];
    acc ^= p.timestamp_low.load(std::memory_order_relaxed);
    acc ^= p.timestamp_high.load(std::memory_order_relaxed);
    acc += p.sequence.load(std::memory_order_relaxed);
    acc += p.valid.load(std::memory_order_relaxed);
  }
  s_b4d9_warm_sink_u64 = acc;
}
void SharedLpcAnalysis::warm_gainnorm_basis() {
  float acc = 0.0f;
  for (const auto &row : gainnorm_cos_basis_)
    for (float f : row) acc += f;
  for (const auto &row : gainnorm_sin_basis_)
    for (float f : row) acc += f;
  s_b4d9_warm_sink_f32 = acc;
}

// B4D.5 model-lookup decomposition accessors.
void shared_lpc_b4d5_model_audit_reset() {
  s_b4d5_model_calls = 0;
  s_b4d5_model_candidates = 0;
  s_b4d5_model_scan_cycles = 0;
  s_b4d5_model_copies = 0;
  s_b4d5_model_repeat = 0;
  s_b4d5_model_last_ts = UINT64_MAX;
}
void shared_lpc_b4d5_model_audit_enable(bool on) { s_b4d5_model_audit = on; }
bool shared_lpc_b4d5_model_audit_enabled() { return s_b4d5_model_audit; }
void shared_lpc_b4d5_model_audit(uint64_t *calls, uint64_t *candidates,
                                 uint64_t *scan_cycles, uint64_t *copies,
                                 uint64_t *repeat) {
  if (calls) *calls = s_b4d5_model_calls;
  if (candidates) *candidates = s_b4d5_model_candidates;
  if (scan_cycles) *scan_cycles = s_b4d5_model_scan_cycles;
  if (copies) *copies = s_b4d5_model_copies;
  if (repeat) *repeat = s_b4d5_model_repeat;
}
