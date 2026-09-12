#pragma once
#include "pitch_detector.h"
#include "profiling.h"
#include <array>

class YinDetector final : public PitchDetector {
public:
  static constexpr size_t kMaxWindow = 768;
  bool init(const PitchAnalysisConfig &config) override;
  PitchDetectorMeasurement analyze(const float *samples, size_t frames,
                                   uint64_t timestamp) override;
  void reset() override;
  ProfileStats profile(PitchAnalysisProfileSection section) const;
  YinForensicTelemetry forensic_telemetry() const;

private:
  PitchAnalysisConfig config_{};
  size_t tau_min_ = 0, tau_max_ = 0;
  bool initialized_ = false;
  std::array<float, kMaxWindow + 1> difference_{}, cmnd_{};
  Profiler profiler_;
  std::atomic<uint64_t> executions_{0}, inner_iterations_{0};
};
