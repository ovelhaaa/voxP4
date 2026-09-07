#pragma once
#include "profiling.h"
#include "vocal_fx_types.h"
#include "lpc.h"
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
  static constexpr size_t kOlaSize = 4096;
  static constexpr size_t kHannSize = SharedPitchShiftResources::kHannSize;
  static constexpr size_t kMaxMarks = 64;
  static constexpr size_t kMaxGrainsPerBlock = 8;
  static constexpr uint32_t kHistoryOffset = 1536; // 32 ms at 48 kHz

  bool init(float sample_rate, const PitchShiftConfig &config,
            SharedPitchShiftResources *shared, const SharedLpcAnalysis *lpc = nullptr);
  void reset();
  void set_enabled(bool enabled) { target_enabled_ = enabled; }
  void set_semitones(float semitones);
  void set_ratio(float ratio);
  void set_smoothing(float milliseconds);
  void set_wet(float wet);
  void set_formants(FormantMode mode, float amount) { formant_mode_=mode; formant_amount_=std::clamp(amount,0.0f,1.0f); }
  FormantMode formant_mode() const { return formant_mode_; }
  float formant_amount() const { return formant_amount_; }
  bool enabled() const { return target_enabled_; }
  bool has_usable_output() const { return block_has_psola_; }
  void process(const float *input, float *output, size_t frames,
               const PitchResult &pitch, PitchTrackState track,
               const PitchMark *marks, size_t mark_count);
  // The owner writes the common history once, then renders every voice.
  void process_shared(const float *input, float *output, size_t frames,
                      const PitchResult &pitch, PitchTrackState track,
                      const PitchMark *marks, size_t mark_count);
  uint32_t latency_samples() const { return history_offset_; }
  size_t memory_bytes() const { return sizeof(*this); }
  PitchShiftTelemetry telemetry() const;
  ProfileStats profile(PitchShiftProfileSection section) const;

private:
  struct Atomic64Parts {
    std::atomic<uint32_t> low{0}, high{0};
  };
  struct PublishedTelemetry {
    std::atomic<uint32_t> sequence{0};
    Atomic64Parts blocks, grains, pitch_mark_underflows,
        audio_history_underflows, psola_resyncs, fallback_frames,
        max_grains_exceeded, invalid_pitch, invalid_mark, formant_frames;
    std::atomic<uint32_t> max_grains_per_block{0}, state{0};
  };
  static void store_atomic64(Atomic64Parts &, uint64_t);
  static uint64_t load_atomic64(const Atomic64Parts &);
  void publish_telemetry();
  bool history_available(uint64_t first, uint64_t last) const;
  float history_at(uint64_t position) const;
  bool select_mark(double source_position, const PitchMark *marks, size_t count,
                   size_t &index, float &period) const;
  bool add_grain(double destination, double source, const PitchMark *marks,
                 size_t count);
  void clear_ola();

  float sample_rate_ = 48000.0f;
  uint32_t history_offset_ = kHistoryOffset;
  std::array<float, kOlaSize> ola_{}, lpc_ola_{}, norm_{};
  SharedPitchShiftResources *resources_ = nullptr;
  uint64_t output_position_ = 0;
  double next_synthesis_mark_ = 0.0;
  bool have_cursor_ = false, target_enabled_ = false;
  bool block_has_psola_ = false;
  float target_semitones_ = 0, current_semitones_ = 0;
  float target_wet_ = 1, current_wet_ = 1;
  float smoothing_ms_ = 30, psola_gain_ = 0, active_mix_ = 0;
  uint32_t onset_hold_ = 0;
  PitchShiftState state_ = PitchShiftState::Bypass;
  PitchShiftTelemetry telemetry_{};
  PublishedTelemetry published_telemetry_{};
  Profiler profiler_;
  const SharedLpcAnalysis *lpc_ = nullptr;
  FormantMode formant_mode_ = FormantMode::Off;
  float formant_amount_ = 1.0f, formant_mix_ = 0.0f;
  SharedLpcModel grain_model_{};
  std::array<float, VOCAL_FX_LPC_MAX_ORDER> synthesis_state_{};
  uint64_t formant_frames_ = 0;
};
