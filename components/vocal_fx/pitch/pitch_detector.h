#pragma once
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include <cstddef>
#include <cstdint>

struct PitchDetectorMeasurement {
  float frequency_hz = 0.0f;
  float period_samples = 0.0f;
  float confidence = 0.0f;
  float rms_db = -160.0f;
};

class PitchDetector {
public:
  virtual ~PitchDetector() = default;
  virtual bool init(const PitchAnalysisConfig &config) = 0;
  virtual PitchDetectorMeasurement analyze(const float *samples, size_t frames,
                                           uint64_t timestamp) = 0;
  virtual void reset() = 0;
};
