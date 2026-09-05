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
  mark_ring_.fill({});
  rolling_write_ = rolling_count_ = since_hop_ = 0;
  input_position_ = latest_analysis_position_ = 0;
  audio_end_.store(0);
  voiced_ = have_smoothed_ = false;
  good_frames_ = bad_frames_ = change_frames_ = 0;
  smoothed_cents_ = previous_energy_ = 0;
  history_count_ = history_write_ = 0;
  mark_write_ = mark_count_ = 0;
  previous_mark_ = 0;
  coherent_marks_ = mark_failures_ = 0;
  mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked));
  publish({});
}
void PitchAnalysis::tap(const float *samples, size_t n) {
  if (!samples)
    return;
  profiler_.begin(ps(PitchAnalysisProfileSection::Decimator));
  for (size_t i = 0; i < n; ++i) {
    const float x = std::isfinite(samples[i]) ? samples[i] : 0;
    audio_[input_position_ % kAudioHistory] = x;
    float y;
    if (decimator_.process(x, y))
      (void)fifo_.push({y, input_position_}); // drop newest on overload
    ++input_position_;
  }
  audio_end_.store(input_position_, std::memory_order_release);
  profiler_.end(ps(PitchAnalysisProfileSection::Decimator));
}
float PitchAnalysis::median_history() const {
  if (!history_count_)
    return smoothed_cents_;
  std::array<float, 8> v = pitch_history_;
  std::sort(v.begin(), v.begin() + history_count_);
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
    profiler_.begin(ps(PitchAnalysisProfileSection::Total));
    auto measurement = yin_.analyze(linear_.data(), config_.window_size, 0);
    profiler_.begin(ps(PitchAnalysisProfileSection::VoicedClassifier));
    const bool level_ok = measurement.rms_db > config_.min_input_db;
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
    const float energy = std::pow(10.0f, measurement.rms_db / 10.0f);
    const bool onset = energy > 1e-12f && previous_energy_ > 1e-12f &&
                       energy > previous_energy_ * config_.onset_ratio;
    previous_energy_ = .8f * previous_energy_ + .2f * energy;
    profiler_.end(ps(PitchAnalysisProfileSection::VoicedClassifier));
    profiler_.begin(ps(PitchAnalysisProfileSection::PitchSmoother));
    PitchResult result{};
    result.confidence = measurement.confidence;
    result.voiced = voiced_;
    result.onset = onset;
    if (measurement.frequency_hz > 0 &&
        std::isfinite(measurement.frequency_hz)) {
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
      result.frequency_hz = cents_to_hz(smoothed_cents_);
      result.period_samples = config_.input_sample_rate / result.frequency_hz;
    }
    const double center_back = .5 * config_.window_size *
                               config_.input_sample_rate /
                               config_.analysis_sample_rate;
    result.analysis_timestamp_samples =
        latest_analysis_position_ > center_back
            ? latest_analysis_position_ -
                  static_cast<uint64_t>(std::llround(center_back))
            : 0;
    result.timestamp_samples = result.analysis_timestamp_samples;
    profiler_.end(ps(PitchAnalysisProfileSection::PitchSmoother));
    update_marks(result);
    publish(result);
    profiler_.end(ps(PitchAnalysisProfileSection::Total),
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
void PitchAnalysis::add_mark(PitchMark mark) {
  mark_seq_.fetch_add(1, std::memory_order_acq_rel);
  mark_ring_[mark_write_] = mark;
  mark_write_ = (mark_write_ + 1) % kMarkCapacity;
  mark_count_ = std::min(mark_count_ + 1, kMarkCapacity);
  mark_seq_.fetch_add(1, std::memory_order_release);
}
void PitchAnalysis::update_marks(const PitchResult &p) {
  profiler_.begin(ps(PitchAnalysisProfileSection::PitchMarkSearch));
  if (!p.voiced || p.confidence < config_.voiced_exit_confidence || p.onset) {
    previous_mark_ = 0;
    coherent_marks_ = 0;
    mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked),
                      std::memory_order_release);
    profiler_.end(ps(PitchAnalysisProfileSection::PitchMarkSearch));
    return;
  }
  const uint64_t available = audio_end_.load(std::memory_order_acquire);
  const size_t period = static_cast<size_t>(std::lround(p.period_samples));
  if (period < 8 || period * 3 >= kAudioHistory) {
    profiler_.end(ps(PitchAnalysisProfileSection::PitchMarkSearch));
    return;
  }
  if (!previous_mark_) {
    previous_mark_ = p.analysis_timestamp_samples;
    add_mark({previous_mark_, p.confidence * .5f});
    coherent_marks_ = 1;
    mark_state_.store(static_cast<uint8_t>(PitchTrackState::Acquiring),
                      std::memory_order_release);
  } else {
    const uint64_t predicted = previous_mark_ + period;
    if (predicted + period / 2 >= available) {
      profiler_.end(ps(PitchAnalysisProfileSection::PitchMarkSearch));
      return;
    }
    const int radius = std::max<int>(2, period / 5),
              window = std::max<int>(4, period / 2);
    float best = -2;
    int best_offset = 0;
    // Hotspot candidate for ESP32-P4 Xai/SIMD.
    for (int offset = -radius; offset <= radius; ++offset) {
      const int64_t candidate = static_cast<int64_t>(predicted) + offset;
      if (candidate < window || previous_mark_ < static_cast<uint64_t>(window))
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
      previous_mark_ =
          static_cast<uint64_t>(static_cast<int64_t>(predicted) + best_offset);
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
    } else if (++mark_failures_ >= 3) {
      previous_mark_ = 0;
      coherent_marks_ = mark_failures_ = 0;
      mark_state_.store(static_cast<uint8_t>(PitchTrackState::Unlocked),
                        std::memory_order_release);
    }
  }
  profiler_.end(ps(PitchAnalysisProfileSection::PitchMarkSearch));
}
bool PitchAnalysis::latest_mark(PitchMark *out) const {
  if (!out)
    return false;
  uint32_t a, b;
  size_t count, write;
  PitchMark m;
  do {
    a = mark_seq_.load(std::memory_order_acquire);
    count = mark_count_;
    write = mark_write_;
    if (count)
      m = mark_ring_[(write + kMarkCapacity - 1) % kMarkCapacity];
    b = mark_seq_.load(std::memory_order_acquire);
  } while (a != b || (a & 1));
  if (!count)
    return false;
  *out = m;
  return true;
}
size_t PitchAnalysis::marks(uint64_t start, uint64_t end, PitchMark *out,
                            size_t cap) const {
  if (!out || !cap || start > end)
    return 0;
  uint32_t a, b;
  size_t n;
  do {
    a = mark_seq_.load(std::memory_order_acquire);
    n = 0;
    const size_t first =
        (mark_write_ + kMarkCapacity - mark_count_) % kMarkCapacity;
    for (size_t i = 0; i < mark_count_ && n < cap; ++i) {
      const auto m = mark_ring_[(first + i) % kMarkCapacity];
      if (m.sample_position >= start && m.sample_position <= end)
        out[n++] = m;
    }
    b = mark_seq_.load(std::memory_order_acquire);
  } while (a != b || (a & 1));
  return n;
}
PitchTrackState PitchAnalysis::track_state() const {
  return static_cast<PitchTrackState>(
      mark_state_.load(std::memory_order_acquire));
}
ProfileStats PitchAnalysis::profile(PitchAnalysisProfileSection s) const {
  if (s >= PitchAnalysisProfileSection::YinDifference &&
      s <= PitchAnalysisProfileSection::YinInterpolation)
    return yin_.profile(s);
  return profiler_.stats(ps(s));
}
