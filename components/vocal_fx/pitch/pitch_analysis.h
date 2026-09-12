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
  void tap(const float *, size_t, bool audit_identity = false); // audio-task producer, bounded/non-blocking
  size_t run(size_t max_hops);     // analysis-task consumer
  PitchResult latest() const;
  bool latest_mark(PitchMark *) const;
  size_t marks(uint64_t, uint64_t, PitchMark *, size_t) const;
  // Single-attempt snapshot for hard real-time readers. Returns false rather
  // than waiting when the analysis writer is publishing a mark.
  bool try_marks(uint64_t, uint64_t, PitchMark *, size_t, size_t *) const;
  PitchTrackState track_state() const;
  uint64_t latency_samples() const { return latency_samples_; }
  ProfileStats profile(PitchAnalysisProfileSection) const;
  PitchAnalysisAuditTelemetry audit_telemetry() const;
  PitchMarkForensicTelemetry mark_forensic_telemetry() const;
  YinForensicTelemetry yin_forensic_telemetry() const {
    return yin_.forensic_telemetry();
  }
  size_t read_audit_events(PitchAuditEvent *, size_t);
  VocalFxInputIdentity tap_identity() const;
  size_t memory_bytes() const { return sizeof(*this); }
  const void* audio_history_ptr() const { return audio_.data(); }
  size_t audio_history_bytes() const { return sizeof(audio_); }
  const void* fifo_ptr() const { return &fifo_; }
  size_t fifo_bytes() const { return sizeof(fifo_); }
  const void *rolling_window_ptr() const { return rolling_.data(); }
  size_t rolling_window_bytes() const { return sizeof(rolling_); }
  const void *linear_window_ptr() const { return linear_.data(); }
  size_t linear_window_bytes() const { return sizeof(linear_); }
  const void *yin_difference_ptr() const { return yin_.difference_ptr(); }
  size_t yin_difference_bytes() const { return yin_.difference_bytes(); }
  const void *yin_cmnd_ptr() const { return yin_.cmnd_ptr(); }
  size_t yin_cmnd_bytes() const { return yin_.cmnd_bytes(); }
  size_t yin_detector_bytes() const { return sizeof(yin_); }

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
  bool try_read_mark(size_t index, PitchMark *) const;
  void publish(const PitchResult &);
  void update_marks(const PitchResult &);
  void add_mark(PitchMark);
  void set_track_state(PitchTrackState, const PitchResult &,
                       PitchMarkResetReason = PitchMarkResetReason::None);
  void record_mark_event(PitchAuditEventType, const PitchResult &,
                         PitchMarkResetReason, uint64_t predicted = 0,
                         int best_offset = 0, float best_correlation = 0.0f);
  void record_mark_reset(const PitchResult &, PitchMarkResetReason);
  void record_correlation(float, bool);
  void record_pitch_age(uint64_t);
  float median_history() const;
  PitchAnalysisConfig config_{};
  FirDecimator decimator_;
  YinDetector yin_;
  AnalysisFifo<kFifoCapacity> fifo_;
  std::array<float, YinDetector::kMaxWindow> rolling_{}, linear_{};
  size_t rolling_write_ = 0, rolling_count_ = 0, since_hop_ = 0;
  uint64_t previous_fifo_position_ = 0;
  bool have_previous_fifo_position_ = false;
  std::array<float, kAudioHistory> audio_{};
  std::atomic<uint32_t> audio_end_sequence_{0}, audio_end_low_{0},
      audio_end_high_{0};
  uint64_t input_position_ = 0, latest_analysis_position_ = 0;
  uint64_t latency_samples_ = 0;
  bool voiced_ = false;
  bool voiced_raw_ = false;
  uint8_t good_frames_ = 0, bad_frames_ = 0, change_frames_ = 0;
  float smoothed_cents_ = 0, previous_energy_ = 0;
  float last_stable_f0_hz_ = 0.0f, last_stable_period_ = 0.0f;
  float last_zcr_ = 0.0f, last_r1_ = 0.0f, last_spectral_centroid_ = 0.0f;
  bool have_smoothed_ = false;
  std::array<float, 8> pitch_history_{};
  size_t history_count_ = 0, history_write_ = 0;
  std::array<AtomicPitchMark, kMarkCapacity> mark_ring_{};
  // Packed as count:16 | write:16 so readers acquire coherent ring metadata.
  std::atomic<uint32_t> mark_metadata_{0};
  mutable std::atomic<uint32_t> mark_generation_{0};
  uint64_t previous_mark_ = 0;
  uint8_t coherent_marks_ = 0, mark_failures_ = 0, coasting_hops_ = 0;
  std::atomic<uint8_t> mark_state_{
      static_cast<uint8_t>(PitchTrackState::Unlocked)};
  mutable std::atomic<uint32_t> state_seq_{0};
  PitchResult published_{};
  Profiler profiler_;
  std::atomic<uint64_t> audit_input_position_{0}, audit_analysis_position_{0},
      audit_published_timestamp_{0}, audit_backlog_max_samples_{0};
  std::atomic<uint64_t> pitch_age_sum_samples_{0}, pitch_age_observations_{0},
      pitch_age_max_samples_{0};
  static constexpr size_t kPitchAgeHistogramBins = 128;
  // 50 ms bins keep the 128-bin histogram useful even during multi-second
  // worker stalls.  This is passive telemetry and does not affect analysis.
  static constexpr uint32_t kPitchAgeBinSamples = 2400;
  std::array<std::atomic<uint32_t>, kPitchAgeHistogramBins>
      pitch_age_histogram_{};
  std::atomic<uint8_t> coherent_marks_audit_{0}, coherent_marks_maximum_{0},
      mark_failures_audit_{0}, mark_failures_maximum_{0};
  std::atomic<uint64_t> coherent_mark_increments_{0}, coherent_mark_resets_{0};
  std::array<std::atomic<uint64_t>, 7> mark_reset_reasons_{};
  std::atomic<int64_t> correlation_sum_micros_{0};
  std::atomic<int32_t> correlation_min_micros_{2000000},
      correlation_max_micros_{-2000000};
  std::atomic<uint64_t> correlation_observations_{0}, accepted_marks_{0},
      rejected_marks_{0};
  static constexpr size_t kAuditEventCapacity = 128;
  std::array<PitchAuditEvent, kAuditEventCapacity> audit_events_{};
  std::atomic<uint32_t> audit_event_head_{0}, audit_event_tail_{0};
  std::atomic<uint64_t> audit_event_drops_{0}, first_locked_input_position_{0};
  std::atomic<uint64_t> tap_identity_sequence_{0};
  std::atomic<uint32_t> tap_rms_bits_{0}, tap_checksum_{0};
  std::atomic<uint64_t> mark_correlation_searches_{0},
      mark_candidate_offsets_{0}, mark_sample_pairs_{0},
      mark_mac_like_operations_{0};
};
