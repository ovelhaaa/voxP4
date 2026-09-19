#pragma once
#include "analysis_fifo.h"
#include "profiling.h"
#include "vocal_fx_types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr size_t VOCAL_FX_LPC_MAX_ORDER = 20;

enum class LpcAutocorrelationVariant : uint8_t {
  AutocorrReferenceDouble,
  AutocorrFloatScalar,
  AutocorrFloatMultiacc,
  AutocorrF32Kahan,
  AutocorrF32FmaProduct,
  AutocorrF32DoubleSingle,
};

// Stage B4B.4G keeps the original per-frame Hann implementation as an oracle.
// The production variant changes only Hann coefficient generation/lookup; the
// experimental variants change only how the already-windowed frame energy is
// accumulated or sourced.
enum class LpcWindowVariant : uint8_t {
  Reference,
  PrecomputedHannDoubleEnergy,
  PrecomputedHannCompensatedEnergy,
  EnergyFromAutocorrR0,
};

const char *lpc_window_variant_name(LpcWindowVariant variant);
const char *lpc_autocorrelation_variant_name(
    LpcAutocorrelationVariant variant);

struct LpcConfig {
  bool enabled = true;
  LpcAutocorrelationVariant autocorrelation =
      LpcAutocorrelationVariant::AutocorrF32DoubleSingle;
  // B4B.4H qualifies compensated F32 energy against the retained double
  // oracle. Keep this selectable so host renders can exercise both paths.
  LpcWindowVariant windowing =
      LpcWindowVariant::PrecomputedHannCompensatedEnergy;
  uint16_t order = 16;
  uint16_t window_size = 1024;
  uint16_t hop_size = 384;
  float preemphasis = 0.97f;
};
static_assert(sizeof(LpcConfig) == 16,
              "Unexpected LPC configuration layout");

struct SharedLpcModel {
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> coefficients{};
  float prediction_error = 0.0f;
  float confidence = 0.0f;
  uint64_t timestamp = 0;
  uint16_t order = 0;
  bool valid = false;
  // Publication serial is observational. Zero means a model that has not
  // been read from the published ring (for example, a solver result).
  uint32_t publication_serial = 0;
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
  void reset_measurement_telemetry();
  void tap(const float *samples, size_t count);
  size_t run(size_t max_frames, const PitchResult &pitch);
  bool model_near(uint64_t timestamp, SharedLpcModel *model) const;
  // One bounded, read-only attempt. False means a publication crossed the
  // snapshot; callers must label that observation ambiguous.
  bool snapshot_model_refs(LpcPublishedRef *out, size_t capacity,
                           size_t *count) const;
  LpcTelemetry telemetry() const;
  ProfileStats profile(LpcProfileSection section) const;
  bool latest_model(SharedLpcModel *model) const;
  LpcFrameCostSummary frame_cost_summary();
  // B4D.9 diagnostic prewarm: read-only touches of the model ring and the
  // GainNorm basis tables. No state mutation, no DSP effect.
  void warm_model_ring() const;
  static void warm_gainnorm_basis();
  // B4D.5 test-only: snapshot the currently published valid models (newest
  // first) so the exact model_near selection can be checked against a
  // reference implementation.
  size_t snapshot_models(SharedLpcModel *out, size_t cap) const;
  const LpcConfig &effective_config() const { return config_; }
  size_t memory_bytes() const { return sizeof(*this); }
  const void* fifo_ptr() const { return &fifo_; }
  size_t fifo_bytes() const { return sizeof(fifo_); }
  const void* frame_ptr() const { return circular_frame_.data(); }
  size_t frame_bytes() const { return sizeof(circular_frame_); }
  const void* linear_frame_ptr() const { return linear_frame_.data(); }
  size_t linear_frame_bytes() const { return sizeof(linear_frame_); }
  const void* hann_ptr() const { return hann_.data(); }
  size_t hann_bytes() const { return sizeof(hann_); }

  // Public for deterministic host tests of the numerical core.
  static bool solve(const float *frame, size_t count, uint16_t order,
                    float preemphasis, SharedLpcModel *model,
                    Profiler *profiler = nullptr);
  static bool solve_with_autocorrelation(
      const float *frame, size_t count, uint16_t order, float preemphasis,
      LpcAutocorrelationVariant variant, SharedLpcModel *model,
      Profiler *profiler = nullptr);
  static bool solve_with_kernels(
      const float *frame, size_t count, uint16_t order, float preemphasis,
      LpcAutocorrelationVariant autocorrelation,
      LpcWindowVariant windowing, const float *precomputed_hann,
      SharedLpcModel *model, Profiler *profiler = nullptr,
      double *frame_energy = nullptr);
  static bool prepare_hann(float *hann, size_t count);
  static bool window_frame(const float *frame, size_t count,
                           float preemphasis, LpcWindowVariant variant,
                           const float *precomputed_hann, float *windowed,
                           double *energy);
  static bool autocorrelate(const float *windowed, size_t count,
                            uint16_t order,
                            LpcAutocorrelationVariant variant,
                            double *result);
  static bool warp_polynomial(const float *a_in, uint16_t order, float lambda,
                              float gamma, float *a_out);
  static float lambda_from_semitones(float semitones);
  static float compute_gain_normalization(const float *a_orig, const float *a_warped,
                                          uint16_t order, FormantNormalizationStrategy strategy,
                                          float sample_rate = 48000.0f);
  // B4C.6 exact invariant basis.  The production LPC path is fixed at 24
  // frequencies and 48 kHz; other rates retain the reference trig path.
  static void prepare_gainnorm_basis(float sample_rate = 48000.0f);
  static void set_gainnorm_basis_enabled(bool enabled);
  static bool gainnorm_basis_enabled();
  static size_t gainnorm_basis_bit_mismatches(float sample_rate = 48000.0f);
  // B4D.4: invalid LPC frame reasons (unvoiced vs voiced solve failure).
  static void invalid_reason_counts(uint64_t *unvoiced, uint64_t *solve_fail);
  static void reset_invalid_reason_counts();
  static size_t gainnorm_basis_bytes() {
    return sizeof(gainnorm_cos_basis_) + sizeof(gainnorm_sin_basis_);
  }
private:
  struct LpcSample { float value; uint32_t input_position; };
  struct PublishedModel {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> serial{0};
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
  // One fixed table is sufficient because B4B.4G freezes a single 1024-sample
  // LPC analyzer. Its P4 definition is placed in fast internal TCM, never
  // PSRAM; the host definition remains ordinary fixed storage.
  static std::array<float, 1024> hann_;
  static std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
      gainnorm_cos_basis_;
  static std::array<std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1>, 24>
      gainnorm_sin_basis_;
  static bool gainnorm_basis_ready_;
  // B4D.3S: the sample rate the basis was prepared for (rate-agnostic basis).
  static float gainnorm_basis_rate_;
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

// B4D.5 model-lookup decomposition accessors (file scope, diagnostic).
void shared_lpc_b4d5_model_audit_reset();
void shared_lpc_b4d5_model_audit_enable(bool on);
bool shared_lpc_b4d5_model_audit_enabled();
void shared_lpc_b4d5_model_audit(uint64_t *calls, uint64_t *candidates,
                                 uint64_t *scan_cycles, uint64_t *copies,
                                 uint64_t *repeat);
