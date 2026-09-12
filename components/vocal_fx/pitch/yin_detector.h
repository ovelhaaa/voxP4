#pragma once
#include "pitch_detector.h"
#include "profiling.h"
#include <array>

const char *yin_difference_variant_name(YinDifferenceVariant variant);

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
}

void yin_difference_compute(YinDifferenceVariant variant, const float *samples,
                            size_t frames, size_t tau_count,
                            float *difference);
uint64_t yin_difference_product_count(size_t frames, size_t tau_count);

class YinDetector final : public PitchDetector {
public:
  static constexpr size_t kMaxWindow = 768;
  bool init(const PitchAnalysisConfig &config) override;
  PitchDetectorMeasurement analyze(const float *samples, size_t frames,
                                   uint64_t timestamp) override;
  void reset() override;
  ProfileStats profile(PitchAnalysisProfileSection section) const;
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
  Profiler profiler_;
  std::atomic<uint64_t> executions_{0}, inner_iterations_{0};
};
