#pragma once
#include "profiling.h"
#include "vocal_fx_types.h"
#include "lpc.h"
#include "harmony_unvoiced_articulation.h"
#include "harmony_plosive_bridge.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <algorithm>

// Single-writer, absolute-addressed historical TD-PSOLA renderer. All storage
// is owned by the object and process() performs no allocation.
class SharedPitchShiftResources {
public:
  static constexpr size_t kHistorySize = 16384;
  static constexpr size_t kHannSize = 2048;
  void init();
  void reset();
  void push(const float *input, size_t frames);
  bool available(uint64_t first, uint64_t last) const;
  float sample(uint64_t position) const;
  uint64_t input_end() const { return input_end_; }
  float window(size_t index) const { return hann_[index]; }
  size_t memory_bytes() const { return sizeof(*this); }
  const void* history_ptr() const { return history_.data(); }
  size_t history_bytes() const { return sizeof(history_); }
  const void* hann_ptr() const { return hann_.data(); }
  size_t hann_bytes() const { return sizeof(hann_); }
private:
  std::array<float, kHistorySize> history_{};
  std::array<float, kHannSize> hann_{};
  uint64_t input_end_ = 0;
};

class TdPsola {
public:
  static_assert(ATOMIC_INT_LOCK_FREE == 2,
                "pitch-shift telemetry requires lock-free 32-bit atomics");
  static constexpr size_t kHistorySize = SharedPitchShiftResources::kHistorySize;
  static constexpr size_t kHannSize = SharedPitchShiftResources::kHannSize;
  static constexpr size_t kMaxMarks = 64;
  static constexpr size_t kOlaSize = 2048;
  static constexpr size_t kMaxGrainsPerBlock = 32;
  static constexpr uint32_t kHistoryOffset = 1536; // 32 ms at 48 kHz

  bool init(float sample_rate, const PitchShiftConfig &config,
            SharedPitchShiftResources *shared, const SharedLpcAnalysis *lpc = nullptr,
            uint32_t voice_index = 0);
  void reset();
  void set_voice_index(uint32_t voice_index) { voice_index_ = voice_index; }
  void set_enabled(bool enabled) {
    if (enabled) {
      target_enabled_ = true;
      short_pitch_loss_remaining_ = short_pitch_loss_samples_;
      short_pitch_loss_holding_ = false;
    } else if (!short_pitch_loss_remaining_) {
      target_enabled_ = false;
      short_pitch_loss_holding_ = false;
    } else {
      short_pitch_loss_holding_ = true;
    }
  }
  void set_semitones(float semitones);
  void set_ratio(float ratio);
  void set_smoothing(float milliseconds);
  void set_wet(float wet);
  void set_ola_normalization(OlaNormalizationMode mode) {
    ola_normalization_ = mode;
  }
  void set_onset_unvoiced_attenuation(bool enabled) {
    onset_unvoiced_attenuation_enabled_ = enabled;
    if (!enabled)
      psola_gain_ = 1.0f;
  }
  void set_short_pitch_loss_grace(uint32_t samples) {
    short_pitch_loss_samples_ = samples;
    short_pitch_loss_remaining_ = 0;
  }
  bool short_pitch_loss_grace_active() const {
    return short_pitch_loss_holding_ && short_pitch_loss_remaining_ != 0;
  }
  void set_formants(FormantMode mode, float amount, float shift_semitones = 0.0f) {
    formant_mode_ = mode;
    formant_amount_ = std::clamp(amount, 0.0f, 1.0f);
    formant_shift_semitones_ = std::clamp(shift_semitones, -12.0f, 12.0f);
  }
  FormantMode formant_mode() const { return formant_mode_; }
  float formant_amount() const { return formant_amount_; }
  float formant_shift_semitones() const { return formant_shift_semitones_; }
  void set_formant_shift_semitones(float st) { formant_shift_semitones_ = std::clamp(st, -12.0f, 12.0f); }
  float formant_bandwidth_expansion() const { return formant_bandwidth_expansion_; }
  void set_formant_bandwidth_expansion(float bw) { formant_bandwidth_expansion_ = std::clamp(bw, 0.90f, 1.0f); }
  FormantNormalizationStrategy formant_normalization_strategy() const { return formant_normalization_strategy_; }
  void set_formant_normalization_strategy(FormantNormalizationStrategy s) { formant_normalization_strategy_ = s; }
  bool enabled() const { return target_enabled_; }
  bool is_releasing() const { return release_remaining_ > 0; }
  bool has_usable_output() const {
    return block_has_psola_ || (release_remaining_ > 0) ||
           unvoiced_articulation_.is_active() ||
           plosive_bridge_.is_active();
  }
  bool articulation_active() const {
    return unvoiced_articulation_.is_active();
  }
  bool unvoiced_articulation_enabled() const {
    return unvoiced_config_.enabled &&
           unvoiced_config_.mode != UnvoicedArticulationMode::U0_Silence;
  }
  bool plosive_bridge_active() const {
    return plosive_bridge_.is_active();
  }
  bool plosive_bridge_enabled() const {
    return plosive_config_.enabled &&
           plosive_config_.policy != PlosiveBridgePolicy::B0_None;
  }
  void process(const float *input, float *output, size_t frames,
               const PitchResult &pitch, PitchTrackState track,
               const PitchMark *marks, size_t mark_count);
  // The owner writes the common history once, then renders every voice.
  void process_shared(const float *input, float *output, size_t frames,
                      const PitchResult &pitch, PitchTrackState track,
                      const PitchMark *marks, size_t mark_count
#ifndef ESP_PLATFORM
                      , float *psola_component = nullptr,
                      float *fallback_component = nullptr,
                      float *reference_delayed = nullptr,
                      uint8_t *fallback_reason = nullptr,
                      uint8_t *fallback_active = nullptr,
                      float *measured_component = nullptr,
                      float *coasted_component = nullptr,
                      uint8_t *measured_active = nullptr,
                      uint8_t *coasted_active = nullptr,
                      float *articulation_component = nullptr,
                      uint8_t *articulation_active = nullptr,
                      uint8_t *unvoiced_acoustic_class = nullptr,
                      float *plosive_bridge_component = nullptr,
                      uint8_t *plosive_bridge_active = nullptr,
                      SampleTelemetryRecord *sample_telemetry = nullptr
#endif
                      );
  uint32_t latency_samples() const { return history_offset_; }
  size_t memory_bytes() const { return sizeof(*this); }
  PitchShiftTelemetry telemetry() const;
  GrainRejectionTelemetry grain_rejection_telemetry() const;
  PitchShiftDebug debug() const { return debug_; }
  ProfileStats profile(PitchShiftProfileSection section) const;
#ifndef ESP_PLATFORM
  void set_sample_telemetry_buffer(SampleTelemetryRecord *buf) { sample_telemetry_ = buf; }
#endif

private:
  struct Atomic64Parts {
    std::atomic<uint32_t> low{0}, high{0};
  };
  struct PublishedTelemetry {
    std::atomic<uint32_t> sequence{0};
    Atomic64Parts blocks, grains, pitch_mark_underflows,
        audio_history_underflows, psola_resyncs, fallback_frames,
        max_grains_exceeded, invalid_pitch, invalid_mark, formant_frames,
        formant_resets;
    std::atomic<uint32_t> max_grains_per_block{0}, state{0};
  };
  static void store_atomic64(Atomic64Parts &, uint64_t);
  static uint64_t load_atomic64(const Atomic64Parts &);
  void publish_telemetry();
  bool history_available(uint64_t first, uint64_t last) const;
  float history_at(uint64_t position) const;
  bool select_mark(double source_position, const PitchMark *marks, size_t count,
                   size_t &index, float &period, GrainFailureReason *reason,
                   double *distance, double *allowed_distance) const;
  bool add_grain(double destination, double source, const PitchMark *marks,
                 size_t count);
  void record_grain_failure(GrainFailureReason reason, double destination,
                            double source, uint64_t center = 0,
                            uint32_t half = 0, double distance = 0.0,
                            double allowed_distance = 0.0);
  void clear_ola();
  PitchShiftFallbackReason fallback_reason_for_sample(
      const PitchResult &pitch, PitchTrackState track, bool usable,
      bool sample_has_psola) const;
  float fallback_policy_gain(PitchShiftFallbackReason reason) const;
#ifndef ESP_PLATFORM
  void finish_source_mark_run();
#endif

  float sample_rate_ = 48000.0f;
  uint32_t history_offset_ = kHistoryOffset;
  uint32_t voice_index_ = 0;
  std::array<float, kOlaSize> ola_{}, lpc_ola_{}, norm_{};
#ifndef ESP_PLATFORM
  // Host-only experimental COLA telemetry/method-C storage.
  std::array<float, kOlaSize> norm_square_{};
  std::array<uint16_t, kOlaSize> overlap_{};
  std::array<float, kOlaSize> measured_ola_{}, coasted_ola_{};
  std::array<float, kOlaSize> measured_norm_{}, coasted_norm_{};
#endif
  SharedPitchShiftResources *resources_ = nullptr;
  uint64_t output_position_ = 0;
  double next_synthesis_mark_ = 0.0;
  bool have_cursor_ = false, target_enabled_ = false;
  bool block_has_psola_ = false;
  float target_semitones_ = 0, current_semitones_ = 0;
  float target_wet_ = 1, current_wet_ = 1;
  float smoothing_ms_ = 30, psola_gain_ = 0, active_mix_ = 0;
  uint32_t onset_hold_ = 0;
  uint32_t short_pitch_loss_samples_ = 0, short_pitch_loss_remaining_ = 0;
  bool short_pitch_loss_holding_ = false;
  bool onset_unvoiced_attenuation_enabled_ = true;
  OlaNormalizationMode ola_normalization_ = OlaNormalizationMode::ColaEnergyHybrid;
  float source_energy_ = 0.0f, ola_energy_ = 0.0f, energy_alpha_ = 0.0f;
  HarmonyFallbackPolicy fallback_policy_ = HarmonyFallbackPolicy::CurrentDry;
  PsolaContinuityPolicy continuity_policy_ = PsolaContinuityPolicy::Baseline;
  float unvoiced_fallback_gain_ = 0.25f, onset_fallback_gain_ = 0.15f;
  float fallback_hpf_alpha_ = 0.0f, fallback_hpf_input_ = 0.0f;
  float fallback_hpf_output_ = 0.0f;

  HarmonyUnvoicedArticulation unvoiced_articulation_{};
  HarmonyUnvoicedArticulationConfig unvoiced_config_{};
  HarmonyPlosiveBridge plosive_bridge_{};
  HarmonyPlosiveBridgeConfig plosive_config_{};

#ifndef ESP_PLATFORM
  uint64_t source_mark_run_value_ = 0;
  uint32_t source_mark_run_count_ = 0;
  uint64_t source_marks_1x_ = 0, source_marks_2x_ = 0, source_marks_3x_ = 0,
           source_marks_4x_or_more_ = 0, source_mark_reuses_total_ = 0;
#endif
  PitchShiftState state_ = PitchShiftState::Bypass;
  PitchShiftTelemetry telemetry_{};
  GrainRejectionTelemetry grain_rejection_{};
  PublishedTelemetry published_telemetry_{};
  Profiler profiler_;
  const SharedLpcAnalysis *lpc_ = nullptr;
  FormantMode formant_mode_ = FormantMode::Off;
  float formant_amount_ = 1.0f, formant_mix_ = 0.0f;
  float formant_shift_semitones_ = 0.0f;
  float formant_bandwidth_expansion_ = 0.985f;
  FormantNormalizationStrategy formant_normalization_strategy_ =
      FormantNormalizationStrategy::StrategyC_IntegratedSpectral;
  float formant_filter_gain_ = 1.0f;
  float fast_lpc_energy_ = 0.0f, fast_psola_energy_ = 0.0f;
  float slow_gain_target_ = 1.0f, smoothed_gain_ = 1.0f;
  uint32_t softclip_events_ = 0;
  uint32_t gain_rail_events_ = 0;
  SharedLpcModel grain_model_{};
  std::array<float, VOCAL_FX_LPC_MAX_ORDER> synthesis_state_{};
  uint64_t formant_frames_ = 0;
  uint64_t formant_resets_ = 0;
  float max_restored_ = 0.0f;
  float max_pre_tanh_ = 0.0f;
  float last_pre_tanh_ = 0.0f;
  float last_post_tanh_ = 0.0f;
  float last_gain_scale_ = 1.0f;
  PitchTrackState previous_track_state_ = PitchTrackState::Unlocked;
  float last_coasted_period_ = 0.0f;
  float last_reliable_f0_before_coast_ = 0.0f;
  PsolaRecoveryMode recovery_mode_ = PsolaRecoveryMode::Soft;
  float recovery_crossfade_ms_ = 5.0f;
  float recovery_small_error_cents_ = 50.0f;
  float recovery_note_change_cents_ = 150.0f;
  PsolaRecoveryClass active_recovery_class_ = PsolaRecoveryClass::None;
  PsolaRecoveryType active_recovery_type_ = PsolaRecoveryType::None;
  float recovery_period_error_cents_ = 0.0f;
  float recovery_mark_error_fraction_ = 0.0f;
  uint32_t recovery_mark_error_samples_ = 0;
  static constexpr size_t kMaxCrossfadeSamples = 1024;
  std::array<float, kMaxCrossfadeSamples> crossfade_old_ola_{};
  std::array<float, kMaxCrossfadeSamples> crossfade_old_norm_{};
  uint32_t crossfade_total_ = 0;
  uint32_t crossfade_pos_ = 0;
  uint32_t crossfade_remaining_ = 0;
  float current_synthesis_period_ = 0.0f;
  uint32_t slew_grains_remaining_ = 0;
  float slew_period_step_ = 0.0f;
  float pre_recovery_rms_ = 0.0f;
  float transition_min_rms_ = 1.0f;
  uint32_t release_total_ = 0;
  uint32_t release_remaining_ = 0;
  uint64_t last_scheduled_mark_ = 0;
  bool new_grain_scheduled_this_block_ = false;
#ifndef ESP_PLATFORM
  SampleTelemetryRecord *sample_telemetry_ = nullptr;
#endif
  PitchShiftDebug debug_{};
};
