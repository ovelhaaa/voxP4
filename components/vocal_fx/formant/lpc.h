#pragma once
#include "analysis_fifo.h"
#include "profiling.h"
#include "vocal_fx_types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr size_t VOCAL_FX_LPC_MAX_ORDER = 20;

struct LpcConfig {
  bool enabled = true;
  uint16_t order = 16;
  uint16_t window_size = 1024;
  uint16_t hop_size = 384;
  float preemphasis = 0.97f;
};

struct SharedLpcModel {
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> coefficients{};
  float prediction_error = 0.0f;
  float confidence = 0.0f;
  uint64_t timestamp = 0;
  uint16_t order = 0;
  bool valid = false;
};

enum class LpcProfileSection : uint8_t {
  Windowing, Autocorrelation, LevinsonDurbin, Publication, Total, Count
};

struct LpcTelemetry {
  uint64_t lpc_frames = 0;
  uint64_t lpc_invalid_frames = 0;
  uint64_t lpc_fallback_frames = 0;
  float max_prediction_error = 0.0f;
  uint64_t voice1_formant_frames = 0;
  uint64_t voice2_formant_frames = 0;
};

class SharedLpcAnalysis {
public:
  static constexpr size_t kModelCount = 16;
  bool init(float sample_rate, const LpcConfig &config);
  void reset();
  void tap(const float *samples, size_t count);
  size_t run(size_t max_frames, const PitchResult &pitch);
  bool model_near(uint64_t timestamp, SharedLpcModel *model) const;
  LpcTelemetry telemetry() const { return telemetry_; }
  ProfileStats profile(LpcProfileSection section) const;
  size_t memory_bytes() const { return sizeof(*this); }

  // Public for deterministic host tests of the numerical core.
  static bool solve(const float *frame, size_t count, uint16_t order,
                    float preemphasis, SharedLpcModel *model);
private:
  struct PublishedModel {
    std::atomic<uint32_t> sequence{0};
    std::array<std::atomic<float>, VOCAL_FX_LPC_MAX_ORDER + 1> coefficients{};
    std::atomic<float> error{0}, confidence{0};
    std::atomic<uint32_t> timestamp_low{0}, timestamp_high{0}, order{0}, valid{0};
  };
  void publish(const SharedLpcModel &model);
  LpcConfig config_{};
  float sample_rate_ = 48000.0f;
  AnalysisFifo<2049> fifo_{};
  std::array<float, 1024> frame_{};
  size_t fill_ = 0, since_frame_ = 0;
  uint64_t last_position_ = 0;
  std::array<PublishedModel, kModelCount> models_{};
  std::atomic<uint32_t> published_{0};
  LpcTelemetry telemetry_{};
  Profiler profiler_{};
};
