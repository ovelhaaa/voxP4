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
  FifoDrain,
  RingWrite,
  FrameLinearization,
  SolveWindowing,
  Autocorrelation,
  LevinsonDurbin,
  Publication,
  SolveTotal,
  Total,
  Count
};

struct LpcTelemetry {
  uint64_t lpc_frames = 0;
  uint64_t samples_drained = 0;
  uint64_t lpc_invalid_frames = 0;
  uint64_t lpc_fallback_frames = 0;
  float max_prediction_error = 0.0f;
  uint64_t voice1_formant_frames = 0;
  uint64_t voice2_formant_frames = 0;
  uint64_t voice1_formant_resets = 0;
  uint64_t voice2_formant_resets = 0;
  float voice1_max_restored = 0.0f;
  float voice2_max_restored = 0.0f;
};

struct LpcFrameCostSummary {
  uint32_t count = 0;
  uint64_t total_us = 0;
  uint32_t p50_us = 0;
  uint32_t p95_us = 0;
  uint32_t p99_us = 0;
  uint32_t max_us = 0;
};

class SharedLpcAnalysis {
public:
  static constexpr size_t kModelCount = 16;
  bool init(float sample_rate, const LpcConfig &config);
  void reset();
  void tap(const float *samples, size_t count);
  size_t run(size_t max_frames, const PitchResult &pitch);
  bool model_near(uint64_t timestamp, SharedLpcModel *model) const;
  LpcTelemetry telemetry() const;
  ProfileStats profile(LpcProfileSection section) const;
  bool latest_model(SharedLpcModel *model) const;
  LpcFrameCostSummary frame_cost_summary();
  size_t memory_bytes() const { return sizeof(*this); }
  const void* fifo_ptr() const { return &fifo_; }
  size_t fifo_bytes() const { return sizeof(fifo_); }
  const void* frame_ptr() const { return circular_frame_.data(); }
  size_t frame_bytes() const { return sizeof(circular_frame_); }
  const void* linear_frame_ptr() const { return linear_frame_.data(); }
  size_t linear_frame_bytes() const { return sizeof(linear_frame_); }

  // Public for deterministic host tests of the numerical core.
  static bool solve(const float *frame, size_t count, uint16_t order,
                    float preemphasis, SharedLpcModel *model,
                    Profiler *profiler = nullptr);
  static bool warp_polynomial(const float *a_in, uint16_t order, float lambda,
                              float gamma, float *a_out);
  static float lambda_from_semitones(float semitones);
  static float compute_gain_normalization(const float *a_orig, const float *a_warped,
                                          uint16_t order, FormantNormalizationStrategy strategy,
                                          float sample_rate = 48000.0f);
private:
  struct LpcSample { float value; uint32_t input_position; };
  struct PublishedModel {
    std::atomic<uint32_t> sequence{0};
    std::array<std::atomic<float>, VOCAL_FX_LPC_MAX_ORDER + 1> coefficients{};
    std::atomic<float> error{0}, confidence{0};
    std::atomic<uint32_t> timestamp_low{0}, timestamp_high{0}, order{0}, valid{0};
  };
  void publish(const SharedLpcModel &model);
  void publish_telemetry();
  struct PublishedTelemetry {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> frames_low{0}, frames_high{0};
    std::atomic<uint32_t> invalid_low{0}, invalid_high{0};
    std::atomic<uint32_t> fallback_low{0}, fallback_high{0};
    std::atomic<uint32_t> drained_low{0}, drained_high{0};
    std::atomic<float> max_error{0};
  };
  LpcConfig config_{};
  float sample_rate_ = 48000.0f;
  AnalysisFifo<2049, LpcSample> fifo_{};
  std::array<float, 1024> circular_frame_{};
  std::array<float, 1024> linear_frame_{};
  size_t write_index_ = 0, valid_count_ = 0, since_frame_ = 0;
  uint64_t last_position_ = 0;
  std::array<PublishedModel, kModelCount> models_{};
  std::atomic<uint32_t> published_{0};
  LpcTelemetry telemetry_{};
  PublishedTelemetry published_telemetry_{};
  Profiler profiler_{};
  // Fixed 100-us bins avoid retaining every sample in scarce internal SRAM.
  // Quantiles are reported as the upper bound of the selected bin; total and
  // maximum retain their exact measured microsecond values.
  static constexpr uint32_t kFrameCostBinUs = 100;
  static constexpr size_t kFrameCostBinCount = 512;
  std::array<uint16_t, kFrameCostBinCount> frame_cost_histogram_{};
  uint32_t frame_cost_count_ = 0;
  uint64_t frame_cost_total_us_ = 0;
  uint32_t frame_cost_max_us_ = 0;
};
