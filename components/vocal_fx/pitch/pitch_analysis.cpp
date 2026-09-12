#include "pitch_analysis.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kReferenceHz = 440.0f;
ProfileSection ps(PitchAnalysisProfileSection s) {
  return static_cast<ProfileSection>(
      static_cast<size_t>(s) +
      static_cast<size_t>(ProfileSection::AnalysisDecimator));
}
float hz_to_cents(float hz) { return 1200.0f * std::log2(hz / kReferenceHz); }
float cents_to_hz(float cents) {
  return kReferenceHz * std::exp2(cents / 1200.0f);
}
} // namespace

bool PitchAnalysis::init(const PitchAnalysisConfig &c) {
  if (!std::isfinite(c.input_sample_rate) || c.input_sample_rate < 8000 ||
      c.input_sample_rate > 192000 || c.voiced_attack_frames == 0 ||
      c.voiced_release_frames == 0 || c.voiced_exit_confidence < 0 ||
      c.voiced_enter_confidence > 1 ||
      c.voiced_exit_confidence > c.voiced_enter_confidence ||
      c.smoothing_ms < 0 || c.algorithm != PitchDetectorAlgorithm::Yin ||
      !decimator_.init(c.input_sample_rate, c.analysis_sample_rate) ||
      !yin_.init(c))
    return false;
  config_ = c;
  latency_samples_ = static_cast<uint64_t>(std::llround(
      decimator_.group_delay_input_samples() +
      .5 * c.window_size * c.input_sample_rate / c.analysis_sample_rate));
  reset();
  return true;
}
void PitchAnalysis::reset() {
  decimator_.reset();
  yin_.reset();
  fifo_.reset();
  profiler_.reset();
  rolling_.fill(0);
  linear_.fill(0);
  audio_.fill(0);
  pitch_history_.fill(0);
  for (auto &mark : mark_ring_) {
    mark.sequence.store(0, std::memory_order_relaxed);
    mark.position_low.store(0, std::memory_order_relaxed);
    mark.position_high.store(0, std::memory_order_relaxed);
    mark.confidence.store(0, std::memory_order_relaxed);
  }
  rolling_write_ = rolling_count_ = since_hop_ = 0;
  input_position_ = latest_analysis_position_ = 0;
  audio_end_sequence_.store(0, std::memory_order_relaxed);
  audio_end_low_.store(0, std::memory_order_relaxed);
  audio_end_high_.store(0, std::memory_order_relaxed);
  voiced_ = voiced_raw_ = have_smoothed_ = false;
  good_frames_ = bad_frames_ = change_frames_ = 0;
  smoothed_cents_ = previous_energy_ = 0;
  last_stable_f0_hz_ = last_stable_period_ = 0.0f;
  last_zcr_ = last_r1_ = last_spectral_centroid_ = 0.0f;
  history_count_ = history_write_ = 0;
  mark_metadata_.store(0, std::memory_order_relaxed);
  mark_generation_.store(0, std::memory_order_relaxed);
  previous_mark_ = 0;
  coherent_marks_ = mark_failures_ = coasting_hops_ = 0;
  mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked));
  publish({});
}
void PitchAnalysis::tap(const float *samples, size_t n) {
  if (!samples)
    return;
  VF_PROFILE_BEGIN(profiler_, ps(PitchAnalysisProfileSection::Decimator));
  for (size_t i = 0; i < n; ++i) {
    const float x = std::isfinite(samples[i]) ? samples[i] : 0;
    audio_[input_position_ % kAudioHistory] = x;
    float y;
    if (decimator_.process(x, y))
      (void)fifo_.push({y, input_position_}); // drop newest on overload
    ++input_position_;
  }
  publish_audio_end(input_position_);
  VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::Decimator), 0);
}
float PitchAnalysis::median_history() const {
  if (!history_count_)
    return smoothed_cents_;
  std::array<float, 8> v = pitch_history_;
  for (size_t i = 1; i < history_count_; ++i) {
    const float value = v[i];
    size_t j = i;
    while (j > 0 && v[j - 1] > value) {
      v[j] = v[j - 1];
      --j;
    }
    v[j] = value;
  }
  return v[history_count_ / 2];
}
size_t PitchAnalysis::run(size_t max_hops) {
  size_t completed = 0;
  AnalysisSample sample;
  while (completed < max_hops && fifo_.pop(sample)) {
    rolling_[rolling_write_] = sample.value;
    rolling_write_ = (rolling_write_ + 1) % config_.window_size;
    rolling_count_ = std::min<size_t>(rolling_count_ + 1, config_.window_size);
    latest_analysis_position_ = sample.input_position;
    if (rolling_count_ < config_.window_size || ++since_hop_ < config_.hop_size)
      continue;
    since_hop_ = 0;
    for (size_t i = 0; i < config_.window_size; ++i)
      linear_[i] = rolling_[(rolling_write_ + i) % config_.window_size];
    VF_PROFILE_BEGIN(profiler_, ps(PitchAnalysisProfileSection::Total));
    auto measurement = yin_.analyze(linear_.data(), config_.window_size, 0);
    VF_PROFILE_BEGIN(profiler_,
                     ps(PitchAnalysisProfileSection::VoicedClassifier));

    // Time-domain feature extraction (scalar, fixed-cost, ESP32-P4 friendly)
    float peak = 0.0f;
    float sum_sq = 0.0f;
    float sum_cross = 0.0f;
    size_t zcr_count = 0;
    const size_t N = config_.window_size;
    for (size_t i = 0; i < N; ++i) {
      const float x = linear_[i];
      const float ax = std::fabs(x);
      if (ax > peak) peak = ax;
      sum_sq += x * x;
      if (i > 0) {
        sum_cross += x * linear_[i - 1];
        if ((x >= 0.0f && linear_[i - 1] < 0.0f) || (x < 0.0f && linear_[i - 1] >= 0.0f))
          ++zcr_count;
      }
    }
    const float zcr = N > 1 ? static_cast<float>(zcr_count) / static_cast<float>(N - 1) : 0.0f;
    const float r1 = sum_sq > 1e-12f ? std::clamp(sum_cross / sum_sq, -1.0f, 1.0f) : 0.0f;
    const float hfr = std::clamp(1.0f - r1, 0.0f, 2.0f);
    const float spectral_centroid = (config_.analysis_sample_rate / (2.0f * 3.14159265358979323846f)) * std::acos(r1);
    last_zcr_ = zcr;
    last_r1_ = r1;
    last_spectral_centroid_ = spectral_centroid;

    const bool level_ok = measurement.rms_db > config_.min_input_db;
    const bool raw_spectral_ok = (zcr <= config_.max_unvoiced_zcr) && (r1 >= config_.min_unvoiced_r1);
    const bool raw_freq_ok = measurement.frequency_hz >= config_.min_frequency &&
                             measurement.frequency_hz <= config_.max_frequency;
    const bool voiced_raw = level_ok && (measurement.confidence >= config_.voiced_enter_confidence) &&
                            raw_spectral_ok && raw_freq_ok;
    voiced_raw_ = voiced_raw;

    if (!config_.stateful_voicing_enabled) {
      const float threshold = voiced_ ? config_.voiced_exit_confidence
                                      : config_.voiced_enter_confidence;
      if (level_ok && measurement.confidence >= threshold) {
        good_frames_ = static_cast<uint8_t>(std::min<int>(255, good_frames_ + 1));
        bad_frames_ = 0;
        if (!voiced_ && good_frames_ >= config_.voiced_attack_frames)
          voiced_ = true;
      } else {
        bad_frames_ = static_cast<uint8_t>(std::min<int>(255, bad_frames_ + 1));
        good_frames_ = 0;
        if (voiced_ && bad_frames_ >= config_.voiced_release_frames)
          voiced_ = false;
      }
    } else {
      if (!voiced_) {
        // UNVOICED -> VOICED transition: strict criterion + attack debounce
        const bool enter_candidate = level_ok &&
                                     (measurement.confidence >= config_.voiced_enter_confidence) &&
                                     raw_spectral_ok && raw_freq_ok;
        if (enter_candidate) {
          good_frames_ = static_cast<uint8_t>(std::min<int>(255, good_frames_ + 1));
          bad_frames_ = 0;
          if (good_frames_ >= config_.voiced_attack_frames) {
            voiced_ = true;
            last_stable_f0_hz_ = measurement.frequency_hz;
            last_stable_period_ = measurement.period_samples;
          }
        } else {
          good_frames_ = 0;
          bad_frames_ = static_cast<uint8_t>(std::min<int>(255, bad_frames_ + 1));
        }
      } else {
        // VOICED -> remain VOICED / exit VOICED
        if (!level_ok) {
          // True silence: exit rapidly (2 hops)
          bad_frames_ = static_cast<uint8_t>(std::min<int>(255, bad_frames_ + 1));
          good_frames_ = 0;
          if (bad_frames_ >= 2) {
            voiced_ = false;
            last_stable_f0_hz_ = 0.0f;
            last_stable_period_ = 0.0f;
          }
        } else {
          // Check for fricative characteristics (high ZCR and low r1)
          const bool is_fricative = (zcr > config_.max_unvoiced_zcr) && (r1 < config_.min_unvoiced_r1);

          // Path A: Confidence above stay threshold
          const bool stay_confidence_ok = measurement.confidence >= config_.voiced_stay_confidence;

          // Path B: F0 continuity support when confidence dips
          bool continuity_ok = false;
          if (last_stable_f0_hz_ > 0.0f && measurement.frequency_hz > 0.0f) {
            const float delta_cents = std::fabs(1200.0f * std::log2(measurement.frequency_hz / last_stable_f0_hz_));
            if (delta_cents <= config_.f0_continuity_tolerance_cents ||
                std::fabs(delta_cents - 1200.0f) <= 100.0f) {
              continuity_ok = true;
            }
          }

          // Decide whether current hop is acceptable to remain voiced
          bool accept_hop = false;
          if (!is_fricative) {
            if (stay_confidence_ok && raw_freq_ok) {
              accept_hop = true;
            } else if (continuity_ok && measurement.confidence >= 0.25f && raw_freq_ok) {
              // Continuity bridge: confidence dipped (e.g. vocal fry / vibrato trough), but F0 is continuous
              accept_hop = true;
            }
          }

          if (accept_hop) {
            good_frames_ = static_cast<uint8_t>(std::min<int>(255, good_frames_ + 1));
            bad_frames_ = 0;
            if (measurement.confidence >= config_.voiced_stay_confidence && raw_freq_ok) {
              last_stable_f0_hz_ = measurement.frequency_hz;
              last_stable_period_ = measurement.period_samples;
            }
          } else {
            // Bad hop: debounce with release persistence
            bad_frames_ = static_cast<uint8_t>(std::min<int>(255, bad_frames_ + 1));
            good_frames_ = 0;
            if (bad_frames_ >= config_.voiced_release_frames) {
              voiced_ = false;
              last_stable_f0_hz_ = 0.0f;
              last_stable_period_ = 0.0f;
            }
          }
        }
      }
    }

    const float energy = std::pow(10.0f, measurement.rms_db / 10.0f);
    const bool onset = energy > 1e-12f && previous_energy_ > 1e-12f &&
                       energy > previous_energy_ * config_.onset_ratio;
    previous_energy_ = .8f * previous_energy_ + .2f * energy;
    VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::VoicedClassifier),
                   0);
    VF_PROFILE_BEGIN(profiler_, ps(PitchAnalysisProfileSection::PitchSmoother));
    PitchResult result{};
    result.confidence = measurement.confidence;
    result.voiced = voiced_;
    result.voiced_raw = voiced_raw_;
    result.voiced_stateful = voiced_;
    result.onset = onset;
    result.yin_min = measurement.yin_min;
    result.yin_tau = measurement.yin_tau;
    result.input_rms = static_cast<float>(std::sqrt(std::max(0.0, static_cast<double>(energy))));
    result.input_peak = peak;
    result.spectral_centroid = spectral_centroid;
    result.high_frequency_ratio = hfr;
    result.zero_crossing_rate = zcr;

    const bool reliable_measurement =
        level_ok &&
        (measurement.confidence >= (config_.stateful_voicing_enabled ? config_.voiced_stay_confidence : config_.voiced_exit_confidence) ||
         (voiced_ && last_stable_f0_hz_ > 0.0f && measurement.confidence >= 0.25f)) &&
        measurement.frequency_hz > 0 && std::isfinite(measurement.frequency_hz);
    if (!voiced_) {
      have_smoothed_ = false;
      history_count_ = history_write_ = 0;
      change_frames_ = 0;
      last_stable_f0_hz_ = 0.0f;
      last_stable_period_ = 0.0f;
    }
    if (reliable_measurement) {
      float cents = hz_to_cents(measurement.frequency_hz);
      if (have_smoothed_) {
        const float historical = median_history();
        for (float shift : {-1200.0f, 1200.0f})
          if (std::fabs(cents + shift - historical) + 80 <
                  std::fabs(cents - historical) &&
              measurement.confidence < .98f)
            cents += shift;
        const float hop_ms =
            1000.0f * config_.hop_size / config_.analysis_sample_rate;
        const float alpha =
            config_.smoothing_ms <= 0
                ? 1.0f
                : 1.0f - std::exp(-hop_ms / config_.smoothing_ms);
        const float delta = cents - smoothed_cents_;
        if (voiced_ && std::fabs(delta) > config_.pitch_change_cents) {
          if (++change_frames_ >= 2) {
            result.pitch_changed = true;
            change_frames_ = 0;
          }
        } else
          change_frames_ = 0;
        smoothed_cents_ += alpha * delta;
      } else {
        smoothed_cents_ = cents;
        have_smoothed_ = true;
      }
      if (voiced_) {
        pitch_history_[history_write_] = cents;
        history_write_ = (history_write_ + 1) % pitch_history_.size();
        history_count_ = std::min(history_count_ + 1, pitch_history_.size());
      }
    }
    if (have_smoothed_) {
      result.frequency_hz = cents_to_hz(smoothed_cents_);
      result.period_samples = config_.input_sample_rate / result.frequency_hz;
    } else if (voiced_ && last_stable_f0_hz_ > 0.0f) {
      result.frequency_hz = last_stable_f0_hz_;
      result.period_samples = config_.input_sample_rate / result.frequency_hz;
    }
    const double center_back = decimator_.group_delay_input_samples() +
                               .5 * config_.window_size *
                                   config_.input_sample_rate /
                                   config_.analysis_sample_rate;
    result.analysis_timestamp_samples =
        latest_analysis_position_ > center_back
            ? latest_analysis_position_ -
                  static_cast<uint64_t>(std::llround(center_back))
            : 0;
    result.timestamp_samples = result.analysis_timestamp_samples;
    VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::PitchSmoother),
                   0);
    update_marks(result);
    result.coherent_marks = coherent_marks_;
    const uint8_t track_st = mark_state_.load(std::memory_order_relaxed);
    result.pitch_track_state = track_st;
    const float eff_coast_ms = config_.coast_ms > 0.0f ? config_.coast_ms : 15.0f;
    const uint32_t max_c_hops = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::lround(
               eff_coast_ms * config_.analysis_sample_rate /
               (config_.hop_size * 1000.0f))));
    result.coast_remaining = (track_st == static_cast<uint8_t>(PitchTrackState::Coasting))
                                 ? (max_c_hops > coasting_hops_ ? max_c_hops - coasting_hops_ : 0)
                                 : (track_st == static_cast<uint8_t>(PitchTrackState::Locked) ? max_c_hops : 0);
    publish(result);
    VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::Total),
                   static_cast<uint64_t>(1000000.0 * config_.hop_size /
                                         config_.analysis_sample_rate));
    ++completed;
  }
  return completed;
}
void PitchAnalysis::publish(const PitchResult &r) {
  state_seq_.fetch_add(1, std::memory_order_acq_rel);
  published_ = r;
  state_seq_.fetch_add(1, std::memory_order_release);
}
PitchResult PitchAnalysis::latest() const {
  PitchResult r;
  uint32_t a, b;
  do {
    a = state_seq_.load(std::memory_order_acquire);
    r = published_;
    b = state_seq_.load(std::memory_order_acquire);
  } while (a != b || (a & 1));
  return r;
}
float PitchAnalysis::audio_at(uint64_t p) const {
  return audio_[p % kAudioHistory];
}
void PitchAnalysis::publish_audio_end(uint64_t position) {
  audio_end_sequence_.fetch_add(1, std::memory_order_acq_rel);
  audio_end_low_.store(static_cast<uint32_t>(position),
                       std::memory_order_relaxed);
  audio_end_high_.store(static_cast<uint32_t>(position >> 32U),
                        std::memory_order_relaxed);
  audio_end_sequence_.fetch_add(1, std::memory_order_release);
}
uint64_t PitchAnalysis::audio_end() const {
  uint32_t before = 0, after = 0;
  uint64_t result = 0;
  do {
    before = audio_end_sequence_.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    const uint64_t low = audio_end_low_.load(std::memory_order_relaxed);
    const uint64_t high = audio_end_high_.load(std::memory_order_relaxed);
    result = low | (high << 32U);
    after = audio_end_sequence_.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}
void PitchAnalysis::add_mark(PitchMark mark) {
  mark_generation_.fetch_add(1, std::memory_order_acq_rel);
  const uint32_t metadata = mark_metadata_.load(std::memory_order_relaxed);
  const size_t write = metadata & 0xffffU;
  const size_t count = metadata >> 16U;
  auto &destination = mark_ring_[write];
  destination.sequence.fetch_add(1, std::memory_order_acq_rel);
  destination.position_low.store(static_cast<uint32_t>(mark.sample_position),
                                 std::memory_order_relaxed);
  destination.position_high.store(
      static_cast<uint32_t>(mark.sample_position >> 32U),
      std::memory_order_relaxed);
  destination.confidence.store(mark.confidence, std::memory_order_relaxed);
  destination.sequence.fetch_add(1, std::memory_order_release);
  const uint32_t next_write = (write + 1) % kMarkCapacity;
  const uint32_t next_count = std::min(count + 1, kMarkCapacity);
  mark_metadata_.store((next_count << 16U) | next_write,
                       std::memory_order_release);
  mark_generation_.fetch_add(1, std::memory_order_release);
  extern void vocal_fx_funnel_inc_pitch_marks_generated(uint64_t count);
  vocal_fx_funnel_inc_pitch_marks_generated(1);
}
void PitchAnalysis::update_marks(const PitchResult &p) {
  VF_PROFILE_BEGIN(profiler_, ps(PitchAnalysisProfileSection::PitchMarkSearch));
  const uint8_t cur_state = mark_state_.load(std::memory_order_relaxed);
  const bool can_coast =
      (config_.continuity_policy == PsolaContinuityPolicy::Coasting ||
       config_.continuity_policy == PsolaContinuityPolicy::OnsetContinuityCoasting) &&
      (cur_state == static_cast<uint8_t>(PitchTrackState::Locked) ||
       cur_state == static_cast<uint8_t>(PitchTrackState::Coasting));
  const float effective_coast_ms = config_.coast_ms > 0.0f ? config_.coast_ms : 15.0f;
  const uint32_t max_coast_hops = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::lround(
             effective_coast_ms * config_.analysis_sample_rate /
             (config_.hop_size * 1000.0f))));

  const bool should_unvoice = config_.stateful_voicing_enabled
                                  ? (!p.voiced)
                                  : (!p.voiced || p.confidence < config_.voiced_exit_confidence);
  if (should_unvoice) {
    if (can_coast && coasting_hops_ < max_coast_hops) {
      ++coasting_hops_;
      mark_state_.store(static_cast<uint8_t>(PitchTrackState::Coasting),
                        std::memory_order_release);
      const uint64_t boundary = p.analysis_timestamp_samples;
      const size_t period = static_cast<size_t>(std::lround(p.period_samples));
      if (previous_mark_ > 0 && period >= 24 && period <= 800) {
        while (previous_mark_ + period <= boundary) {
          previous_mark_ += period;
          add_mark({previous_mark_, 0.5f, true /* predicted */});
        }
      }
    } else {
      previous_mark_ = 0;
      coherent_marks_ = 0;
      coasting_hops_ = 0;
      mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked),
                        std::memory_order_release);
    }
    VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::PitchMarkSearch),
                   0);
    return;
  }
  coasting_hops_ = 0;
  const uint64_t available = audio_end();
  const size_t period = static_cast<size_t>(std::lround(p.period_samples));
  if (period < 8 || period * 3 >= kAudioHistory) {
    VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::PitchMarkSearch),
                   0);
    return;
  }
  const uint64_t boundary = p.analysis_timestamp_samples;
  if (!previous_mark_) {
    previous_mark_ = boundary;
    add_mark({previous_mark_, p.confidence * .5f});
    coherent_marks_ = 1;
    mark_state_.store(static_cast<uint8_t>(PitchTrackState::Acquiring),
                      std::memory_order_release);
  } else {
    if (previous_mark_ + period + kAudioHistory <= available) {
      previous_mark_ = boundary;
      coherent_marks_ = 1;
      mark_failures_ = 0;
      add_mark({previous_mark_, p.confidence * .5f});
      mark_state_.store(static_cast<uint8_t>(PitchTrackState::Acquiring),
                        std::memory_order_release);
    }
  }
  const int radius = std::max<int>(2, period / 5),
            window = std::max<int>(4, period / 2);
  while (previous_mark_ + period <= boundary) {
      const uint64_t predicted = previous_mark_ + period;
      if (predicted + period / 2 >= available)
        break;
      float best = -2;
      int best_offset = 0;
      // Hotspot candidate for ESP32-P4 Xai/SIMD.
      for (int offset = -radius; offset <= radius; ++offset) {
        const int64_t candidate = static_cast<int64_t>(predicted) + offset;
        if (candidate < window ||
            previous_mark_ < static_cast<uint64_t>(window))
          continue;
        float dot = 0, aa = 0, bb = 0;
        for (int i = -window; i < 0; ++i) {
          const float a = audio_at(previous_mark_ + i),
                      b = audio_at(candidate + i);
          dot += a * b;
          aa += a * a;
          bb += b * b;
        }
        const float score = dot / std::sqrt(std::max(aa * bb, 1e-20f));
        if (score > best) {
          best = score;
          best_offset = offset;
        }
      }
      if (best > .35f) {
        previous_mark_ = static_cast<uint64_t>(static_cast<int64_t>(predicted) +
                                               best_offset);
        const float distance =
            1.0f - std::fabs(static_cast<float>(best_offset)) / (radius + 1);
        add_mark({previous_mark_,
                  std::clamp(.5f * p.confidence + .35f * std::max(best, 0.0f) +
                                 .15f * distance,
                             0.0f, 1.0f)});
        coherent_marks_ =
            static_cast<uint8_t>(std::min<int>(255, coherent_marks_ + 1));
        mark_failures_ = 0;
        if (coherent_marks_ >= 3)
          mark_state_.store(static_cast<uint8_t>(PitchTrackState::Locked),
                            std::memory_order_release);
      } else {
        if (++mark_failures_ >= 3) {
          if (can_coast && coasting_hops_ < max_coast_hops) {
            ++coasting_hops_;
            mark_state_.store(static_cast<uint8_t>(PitchTrackState::Coasting),
                              std::memory_order_release);
          } else {
            previous_mark_ = 0;
            coherent_marks_ = mark_failures_ = 0;
            coasting_hops_ = 0;
            mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked),
                              std::memory_order_release);
          }
        }
        break;
      }
    }
  VF_PROFILE_END(profiler_, ps(PitchAnalysisProfileSection::PitchMarkSearch),
                 0);
}
PitchMark PitchAnalysis::read_mark(size_t index) const {
  const auto &source = mark_ring_[index];
  PitchMark result;
  uint32_t before, after;
  do {
    before = source.sequence.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    const uint64_t low = source.position_low.load(std::memory_order_relaxed);
    const uint64_t high = source.position_high.load(std::memory_order_relaxed);
    result.sample_position = low | (high << 32U);
    result.confidence = source.confidence.load(std::memory_order_relaxed);
    after = source.sequence.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}
bool PitchAnalysis::try_read_mark(size_t index, PitchMark *out) const {
  if (!out)
    return false;
  const auto &source = mark_ring_[index];
  const uint32_t before = source.sequence.load(std::memory_order_acquire);
  if (before & 1U)
    return false;
  const uint64_t low = source.position_low.load(std::memory_order_relaxed);
  const uint64_t high = source.position_high.load(std::memory_order_relaxed);
  PitchMark result;
  result.sample_position = low | (high << 32U);
  result.confidence = source.confidence.load(std::memory_order_relaxed);
  const uint32_t after = source.sequence.load(std::memory_order_acquire);
  if (before != after || (after & 1U))
    return false;
  *out = result;
  return true;
}
bool PitchAnalysis::latest_mark(PitchMark *out) const {
  if (!out)
    return false;
  uint32_t before, after;
  size_t count = 0;
  do {
    before = mark_generation_.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    const uint32_t metadata = mark_metadata_.load(std::memory_order_acquire);
    count = metadata >> 16U;
    const size_t write = metadata & 0xffffU;
    if (count)
      *out = read_mark((write + kMarkCapacity - 1) % kMarkCapacity);
    after = mark_generation_.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return count != 0;
}
size_t PitchAnalysis::marks(uint64_t start, uint64_t end, PitchMark *out,
                            size_t cap) const {
  if (!out || !cap || start > end)
    return 0;
  uint32_t before, after;
  size_t n = 0;
  do {
    before = mark_generation_.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    const uint32_t metadata = mark_metadata_.load(std::memory_order_acquire);
    const size_t count = metadata >> 16U;
    const size_t write = metadata & 0xffffU;
    const size_t first = (write + kMarkCapacity - count) % kMarkCapacity;
    n = 0;
    for (size_t i = 0; i < count && n < cap; ++i) {
      const auto mark = read_mark((first + i) % kMarkCapacity);
      if (mark.sample_position >= start && mark.sample_position <= end)
        out[n++] = mark;
    }
    after = mark_generation_.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return n;
}
bool PitchAnalysis::try_marks(uint64_t start, uint64_t end, PitchMark *out,
                              size_t cap, size_t *written) const {
  if (written)
    *written = 0;
  if (!written || !out || !cap || start > end)
    return false;
  const uint32_t before = mark_generation_.load(std::memory_order_acquire);
  if (before & 1U)
    return false;
  const uint32_t metadata = mark_metadata_.load(std::memory_order_acquire);
  const size_t count = metadata >> 16U;
  const size_t write = metadata & 0xffffU;
  const size_t first = (write + kMarkCapacity - count) % kMarkCapacity;
  size_t n = 0;
  for (size_t i = 0; i < count && n < cap; ++i) {
    PitchMark mark;
    if (!try_read_mark((first + i) % kMarkCapacity, &mark))
      return false;
    if (mark.sample_position >= start && mark.sample_position <= end)
      out[n++] = mark;
  }
  const uint32_t after = mark_generation_.load(std::memory_order_acquire);
  if (before != after || (after & 1U))
    return false;
  *written = n;
  return true;
}
PitchTrackState PitchAnalysis::track_state() const {
  return static_cast<PitchTrackState>(
      mark_state_.load(std::memory_order_acquire));
}
ProfileStats PitchAnalysis::profile(PitchAnalysisProfileSection s) const {
  if (s >= PitchAnalysisProfileSection::Count)
    return {};
  if (s >= PitchAnalysisProfileSection::YinDifference &&
      s <= PitchAnalysisProfileSection::YinInterpolation)
    return yin_.profile(s);
  return profiler_.stats(ps(s));
}
