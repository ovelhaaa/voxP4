#pragma once
#include "pitch_detector.h"
#include "profiling.h"
#include <array>

class YinDetector final : public PitchDetector {
public:
  static constexpr size_t kMaxWindow = 768;
  bool init(const PitchAnalysisConfig &) override;
  PitchDetectorMeasurement analyze(const float *, size_t, uint64_t) override;
  void reset() override;
  ProfileStats profile(PitchAnalysisProfileSection section) const;

private:
  PitchAnalysisConfig config_{};
  size_t tau_min_ = 0, tau_max_ = 0;
  std::array<float, kMaxWindow + 1> difference_{}, cmnd_{};
  Profiler profiler_;
};
