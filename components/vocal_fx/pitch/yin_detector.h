#pragma once
#include "pitch_detector.h"
#include "profiling.h"
#include <array>

const char *yin_difference_variant_name(YinDifferenceVariant variant);
const char *yin_cmnd_variant_name(YinCmndVariant variant);
const char *yin_energy_variant_name(YinEnergyVariant variant);

// The named, noinline entry points are intentionally public: host numerical
// audits and the ESP32-P4 isolated benchmark use the exact production kernels,
// and the ELF assembly audit can address each implementation unambiguously.
extern "C" {
void yin_difference_reference_scalar(const float *samples, size_t frames,
                                     size_t tau_count, float *difference);
void yin_difference_fma_scalar(const float *samples, size_t frames,
                               size_t tau_count, float *difference);
void yin_difference_muladd_4acc(const float *samples, size_t frames,
                               size_t tau_count, float *difference);
void yin_difference_fma_4acc(const float *samples, size_t frames,
                             size_t tau_count, float *difference);
void yin_difference_fma_8acc(const float *samples, size_t frames,
                             size_t tau_count, float *difference);
void YIN_CMND_REFERENCE_DOUBLE(const float *difference, size_t tau_count,
                               float *cmnd);
void YIN_CMND_DOUBLE_SUM_F32_DIV(const float *difference, size_t tau_count,
                                float *cmnd);
void YIN_CMND_F32(const float *difference, size_t tau_count, float *cmnd);
void YIN_CMND_F32_COMPENSATED(const float *difference, size_t tau_count,
                              float *cmnd);
double YIN_ENERGY_REFERENCE_DOUBLE(const float *samples, size_t frames);
double YIN_ENERGY_F32(const float *samples, size_t frames);
double YIN_ENERGY_F32_COMPENSATED(const float *samples, size_t frames);
}

void yin_difference_compute(YinDifferenceVariant variant, const float *samples,
                            size_t frames, size_t tau_count,
                            float *difference);
uint64_t yin_difference_product_count(size_t frames, size_t tau_count);
void yin_cmnd_compute(YinCmndVariant variant, const float *difference,
                      size_t tau_count, float *cmnd);
double yin_energy_compute(YinEnergyVariant variant, const float *samples,
                          size_t frames);

// Stateful exact-window update used by the B4B.4E candidates. For the frozen
// 512/60 geometry only the outgoing 60-sample prefix must survive a hop; all
// other remove/add operands are in the next 512-sample window.
class YinIncrementalDifference {
public:
  static constexpr size_t kMaxWindow = 768;
  static constexpr size_t kHistorySamples = 60;
  ~YinIncrementalDifference();
  YinIncrementalDifference() = default;
  YinIncrementalDifference(const YinIncrementalDifference &) = delete;
  YinIncrementalDifference &operator=(const YinIncrementalDifference &) = delete;
  bool init(YinDifferenceVariant variant, size_t frames, size_t hop,
            size_t tau_count, uint32_t rebase_hops);
  void reset();
  void reset_measurement_telemetry();
  void invalidate_for_gap();
  uint64_t compute(const float *samples, float *difference);
  uint64_t incremental_rebases() const { return incremental_rebases_; }
  uint64_t incremental_gap_rebases() const { return gap_rebases_; }
  uint64_t periodic_rebases() const { return periodic_rebases_; }
  uint64_t update_terms() const { return update_terms_; }
  uint64_t full_rebase_products() const { return full_rebase_products_; }

private:
  void capture_outgoing(const float *samples);
  void add_double_single(size_t tau, float value, float *hi);
  void release_lo();
  YinDifferenceVariant variant_ = YinDifferenceVariant::IncrementalF32;
  size_t frames_ = 0, hop_ = 0, tau_count_ = 0;
  uint32_t rebase_hops_ = 0, updates_since_rebase_ = 0;
  bool valid_ = false, gap_pending_ = false;
  float *lo_ = nullptr;
  float *state_buffer_ = nullptr;
  std::array<float, kHistorySamples> outgoing_{};
  uint64_t incremental_rebases_ = 0, gap_rebases_ = 0;
  uint64_t periodic_rebases_ = 0, update_terms_ = 0;
  uint64_t full_rebase_products_ = 0;
};

class YinDetector final : public PitchDetector {
public:
  static constexpr size_t kMaxWindow = 768;
  bool init(const PitchAnalysisConfig &config) override;
  PitchDetectorMeasurement analyze(const float *samples, size_t frames,
                                   uint64_t timestamp) override;
  void reset() override;
  void reset_measurement_telemetry();
  void invalidate_incremental_gap() { incremental_.invalidate_for_gap(); }
  ProfileStats profile(PitchAnalysisProfileSection section) const;
  ProfileDistributionStats
  profile_distribution(PitchAnalysisProfileSection section) const;
  YinForensicTelemetry forensic_telemetry() const;
  const std::array<float, kMaxWindow + 1> &difference_debug() const {
    return difference_;
  }
  const std::array<float, kMaxWindow + 1> &cmnd_debug() const { return cmnd_; }
  size_t tau_min_debug() const { return tau_min_; }
  size_t tau_max_debug() const { return tau_max_; }
  const void *difference_ptr() const { return difference_.data(); }
  size_t difference_bytes() const { return sizeof(difference_); }
  const void *cmnd_ptr() const { return cmnd_.data(); }
  size_t cmnd_bytes() const { return sizeof(cmnd_); }

private:
  PitchAnalysisConfig config_{};
  size_t tau_min_ = 0, tau_max_ = 0;
  bool initialized_ = false;
  std::array<float, kMaxWindow + 1> difference_{}, cmnd_{};
  YinIncrementalDifference incremental_{};
  Profiler profiler_;
  std::atomic<uint64_t> executions_{0}, inner_iterations_{0};
#if defined(CONFIG_VOXP4_MODE_I2S_B4B_6_RC_VALIDATION) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4I_PITCH_RESIDUAL_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4J_YIN_CMND_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4B_4K_YIN_ENERGY_AUDIT)
  std::atomic<uint64_t> selected_cmnd_sum_nano_{0};
  std::atomic<uint32_t> selected_cmnd_min_nano_{1000000000U};
  std::atomic<uint32_t> closest_threshold_nano_{1000000000U};
  std::atomic<uint32_t> selected_tau_min_{UINT32_MAX};
  std::atomic<uint32_t> selected_tau_max_{0};
#endif
};
