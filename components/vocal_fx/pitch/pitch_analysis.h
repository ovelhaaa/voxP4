#pragma once
#include "analysis_fifo.h"
#include "decimator.h"
#include "profiling.h"
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include "yin_detector.h"
#include <array>
#include <atomic>

class PitchAnalysis {
public:
  static_assert(ATOMIC_INT_LOCK_FREE == 2,
                "pitch-mark metadata must be lock-free");
  static_assert(std::atomic<float>::is_always_lock_free,
                "pitch-mark confidence must be lock-free");
  static constexpr size_t kFifoCapacity = 2048;
  static constexpr size_t kAudioHistory = 16384;
  static constexpr size_t kMarkCapacity = 64;
  bool init(const PitchAnalysisConfig &);
  void reset();
  void tap(const float *, size_t); // audio-task producer, bounded/non-blocking
  size_t run(size_t max_hops);     // analysis-task consumer
  PitchResult latest() const;
  bool latest_mark(PitchMark *) const;
  size_t marks(uint64_t, uint64_t, PitchMark *, size_t) const;
  PitchTrackState track_state() const;
  uint64_t latency_samples() const { return latency_samples_; }
  ProfileStats profile(PitchAnalysisProfileSection) const;
  size_t memory_bytes() const { return sizeof(*this); }

private:
  struct AtomicPitchMark {
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> position_low{0}, position_high{0};
    std::atomic<float> confidence{0};
  };
  float audio_at(uint64_t position) const;
  uint64_t audio_end() const;
  void publish_audio_end(uint64_t position);
  PitchMark read_mark(size_t index) const;
  void publish(const PitchResult &);
  void update_marks(const PitchResult &);
  void add_mark(PitchMark);
  float median_history() const;
  PitchAnalysisConfig config_{};
  FirDecimator decimator_;
  YinDetector yin_;
  AnalysisFifo<kFifoCapacity> fifo_;
  std::array<float, YinDetector::kMaxWindow> rolling_{}, linear_{};
  size_t rolling_write_ = 0, rolling_count_ = 0, since_hop_ = 0;
  std::array<float, kAudioHistory> audio_{};
  std::atomic<uint32_t> audio_end_sequence_{0}, audio_end_low_{0},
      audio_end_high_{0};
  uint64_t input_position_ = 0, latest_analysis_position_ = 0;
  uint64_t latency_samples_ = 0;
  bool voiced_ = false;
  uint8_t good_frames_ = 0, bad_frames_ = 0, change_frames_ = 0;
  float smoothed_cents_ = 0, previous_energy_ = 0;
  bool have_smoothed_ = false;
  std::array<float, 8> pitch_history_{};
  size_t history_count_ = 0, history_write_ = 0;
  std::array<AtomicPitchMark, kMarkCapacity> mark_ring_{};
  // Packed as count:16 | write:16 so readers acquire coherent ring metadata.
  std::atomic<uint32_t> mark_metadata_{0};
  mutable std::atomic<uint32_t> mark_generation_{0};
  uint64_t previous_mark_ = 0;
  uint8_t coherent_marks_ = 0, mark_failures_ = 0;
  std::atomic<uint8_t> mark_state_{
      static_cast<uint8_t>(PitchTrackState::Unlocked)};
  mutable std::atomic<uint32_t> state_seq_{0};
  PitchResult published_{};
  Profiler profiler_;
};
