#pragma once
#include "profiling.h"
#include "vocal_fx_types.h"
#include <array>
#include <cstddef>
#include <cstdint>

// Single-writer, absolute-addressed historical TD-PSOLA renderer. All storage
// is owned by the object and process() performs no allocation.
class TdPsola {
public:
  static constexpr size_t kHistorySize = 16384;
  static constexpr size_t kOlaSize = 4096;
  static constexpr size_t kHannSize = 2048;
  static constexpr size_t kMaxMarks = 64;
  static constexpr size_t kMaxGrainsPerBlock = 8;
  static constexpr uint32_t kHistoryOffset = 1536; // 32 ms at 48 kHz

  bool init(float sample_rate, const PitchShiftConfig &config);
  void reset();
  void set_enabled(bool enabled) { target_enabled_ = enabled; }
  void set_semitones(float semitones);
  void set_wet(float wet);
  void process(const float *input, float *output, size_t frames,
               const PitchResult &pitch, PitchTrackState track,
               const PitchMark *marks, size_t mark_count);
  uint32_t latency_samples() const { return history_offset_; }
  size_t memory_bytes() const { return sizeof(*this); }
  PitchShiftTelemetry telemetry() const;
  ProfileStats profile(PitchShiftProfileSection section) const;

private:
  bool history_available(uint64_t first, uint64_t last) const;
  float history_at(uint64_t position) const;
  bool select_mark(double source_position, const PitchMark *marks, size_t count,
                   size_t &index, float &period) const;
  bool add_grain(double destination, double source, const PitchMark *marks,
                 size_t count);
  void clear_ola();

  float sample_rate_ = 48000.0f;
  uint32_t history_offset_ = kHistoryOffset;
  std::array<float, kHistorySize> history_{};
  std::array<float, kOlaSize> ola_{}, norm_{};
  std::array<float, kHannSize> hann_{};
  uint64_t input_end_ = 0, output_position_ = 0;
  double next_synthesis_mark_ = 0.0;
  bool have_cursor_ = false, target_enabled_ = false;
  float target_semitones_ = 0, current_semitones_ = 0;
  float target_wet_ = 1, current_wet_ = 1;
  float smoothing_ms_ = 30, psola_gain_ = 0, active_mix_ = 0;
  uint32_t onset_hold_ = 0;
  PitchShiftState state_ = PitchShiftState::Bypass;
  PitchShiftTelemetry telemetry_{};
  Profiler profiler_;
};
