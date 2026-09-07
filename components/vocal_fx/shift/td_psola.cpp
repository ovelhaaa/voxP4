#include "td_psola.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;
static_assert(static_cast<size_t>(ProfileSection::PitchShiftLookup) +
                      static_cast<size_t>(PitchShiftProfileSection::Count) - 1 <
                  static_cast<size_t>(ProfileSection::Count),
              "pitch-shift profile sections must map before Count");
ProfileSection section(PitchShiftProfileSection s) {
  return static_cast<ProfileSection>(
      static_cast<size_t>(ProfileSection::PitchShiftLookup) +
      static_cast<size_t>(s));
}
} // namespace

void SharedPitchShiftResources::init() {
  for (size_t i = 0; i < hann_.size(); ++i)
    hann_[i] = .5f - .5f * std::cos(2.0f * kPi * i / (hann_.size() - 1));
  reset();
}
void SharedPitchShiftResources::reset() { history_.fill(0); input_end_ = 0; }
void SharedPitchShiftResources::push(const float *input, size_t frames) {
  for (size_t i = 0; i < frames; ++i)
    history_[(input_end_ + i) & (kHistorySize - 1)] =
        std::isfinite(input[i]) ? input[i] : 0.0f;
  input_end_ += frames;
}
bool SharedPitchShiftResources::available(uint64_t first, uint64_t last) const {
  const uint64_t oldest = input_end_ > kHistorySize ? input_end_ - kHistorySize : 0;
  return first >= oldest && last < input_end_ && first <= last;
}
float SharedPitchShiftResources::sample(uint64_t p) const {
  return history_[p & (kHistorySize - 1)];
}

bool TdPsola::init(float rate, const PitchShiftConfig &config,
                   SharedPitchShiftResources *shared, const SharedLpcAnalysis *lpc) {
  if (!std::isfinite(rate) || rate < 8000 || rate > 192000)
    return false;
  if (!shared) return false;
  const PitchShiftConfig defaults{};
  const float smoothing_ms = std::isfinite(config.smoothing_ms)
                                 ? config.smoothing_ms
                                 : defaults.smoothing_ms;
  const float semitones =
      std::isfinite(config.semitones) ? config.semitones : defaults.semitones;
  const float wet = std::isfinite(config.wet) ? config.wet : defaults.wet;
  sample_rate_ = rate;
  resources_ = shared;
  lpc_ = lpc;
  history_offset_ = static_cast<uint32_t>(std::lround(rate * 0.032));
  smoothing_ms_ = std::clamp(smoothing_ms, 1.0f, 500.0f);
  target_enabled_ = config.enabled;
  target_semitones_ = std::clamp(semitones, -12.0f, 12.0f);
  target_wet_ = std::clamp(wet, 0.0f, 1.0f);
  reset();
  return true;
}
void TdPsola::clear_ola() {
  ola_.fill(0);
  lpc_ola_.fill(0);
  norm_.fill(0);
}
void TdPsola::reset() {
  clear_ola();
  profiler_.reset();
  output_position_ = 0;
  next_synthesis_mark_ = 0;
  have_cursor_ = false;
  block_has_psola_ = false;
  current_semitones_ = target_semitones_;
  current_wet_ = target_wet_;
  psola_gain_ = 0;
  active_mix_ = target_enabled_ ? 1.0f : 0.0f;
  onset_hold_ = 0;
  state_ = target_enabled_ ? PitchShiftState::WaitingForAnalysis
                           : PitchShiftState::Bypass;
  telemetry_ = {};
  synthesis_state_.fill(0); grain_model_={}; formant_mix_=0; formant_frames_=0;
  telemetry_.state = state_;
  publish_telemetry();
}
void TdPsola::set_semitones(float value) {
  if (std::isfinite(value))
    target_semitones_ = std::clamp(value, -12.0f, 12.0f);
}
void TdPsola::set_ratio(float ratio) {
  if (std::isfinite(ratio) && ratio > 0.0f)
    set_semitones(12.0f * std::log2(ratio));
}
void TdPsola::set_smoothing(float value) {
  if (std::isfinite(value)) smoothing_ms_ = std::clamp(value, 1.0f, 500.0f);
}
void TdPsola::set_wet(float value) {
  if (std::isfinite(value))
    target_wet_ = std::clamp(value, 0.0f, 1.0f);
}
float TdPsola::history_at(uint64_t p) const {
  return resources_->sample(p);
}
bool TdPsola::history_available(uint64_t first, uint64_t last) const {
  const uint64_t oldest =
      resources_->input_end() > kHistorySize ? resources_->input_end() - kHistorySize : 0;
  return first >= oldest && last < resources_->input_end() && first <= last;
}
bool TdPsola::select_mark(double source, const PitchMark *marks, size_t count,
                          size_t &best, float &period) const {
  if (!marks || count < 2)
    return false;
  best = 0;
  double distance = std::fabs(source - marks[0].sample_position);
  for (size_t i = 1; i < count; ++i) {
    const double d = std::fabs(source - marks[i].sample_position);
    if (d < distance) {
      distance = d;
      best = i;
    }
  }
  const uint64_t p0 =
      best ? marks[best - 1].sample_position : marks[best].sample_position;
  const uint64_t p1 = best + 1 < count ? marks[best + 1].sample_position
                                       : marks[best].sample_position;
  period = best && best + 1 < count
               ? .5f * ((marks[best].sample_position - p0) +
                        (p1 - marks[best].sample_position))
               : static_cast<float>(p1 - p0);
  return marks[best].confidence > .15f && period >= 24 && period <= 800 &&
         distance <= std::max(2.0 * period, 256.0);
}
bool TdPsola::add_grain(double destination, double source,
                        const PitchMark *marks, size_t count) {
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::SourceLookup));
  size_t index;
  float period;
  if (!select_mark(source, marks, count, index, period)) {
    VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::SourceLookup),
                   0);
    ++telemetry_.pitch_mark_underflows;
    return false;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::SourceLookup), 0);
  VF_PROFILE_BEGIN(profiler_,
                   section(PitchShiftProfileSection::GrainPreparation));
  const int half = std::clamp(static_cast<int>(std::lround(period)), 24, 800);
  const uint64_t center = marks[index].sample_position;
  if (center < static_cast<uint64_t>(half) ||
      !history_available(center - half, center + half)) {
    VF_PROFILE_END(profiler_,
                   section(PitchShiftProfileSection::GrainPreparation), 0);
    ++telemetry_.audio_history_underflows;
    return false;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::GrainPreparation),
                 0);
  SharedLpcModel model;
  const bool use_lpc=formant_mode_==FormantMode::Lpc && formant_amount_>0 &&
                     lpc_ && lpc_->model_near(center,&model) && model.confidence>.15f;
  grain_model_=use_lpc ? model : SharedLpcModel{};
  if (!use_lpc)
    synthesis_state_.fill(0);
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::WindowOla));
  const int64_t dst = static_cast<int64_t>(std::llround(destination));
  for (int n = -half; n <= half; ++n) {
    const int64_t absolute = dst + n;
    if (absolute < static_cast<int64_t>(output_position_) ||
        absolute >= static_cast<int64_t>(output_position_ + kOlaSize))
      continue;
    const size_t wi = static_cast<size_t>(
        (static_cast<int64_t>(n + half) * (kHannSize - 1)) / (2 * half));
    const float w = resources_->window(wi);
    const size_t oi = static_cast<uint64_t>(absolute) & (kOlaSize - 1);
    const uint64_t source_sample=static_cast<uint64_t>(static_cast<int64_t>(center)+n);
    const float plain=history_at(source_sample);
    float sample=plain;
    if(use_lpc){
      for(size_t j=1;j<=model.order;++j)
        if(source_sample>=j) sample+=model.coefficients[j]*history_at(source_sample-j);
    }
    ola_[oi] += plain*w;
    lpc_ola_[oi] += sample*w;
    norm_[oi] += w;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::WindowOla), 0);
  return true;
}
void TdPsola::process(const float *input, float *output, size_t frames,
                      const PitchResult &pitch, PitchTrackState track,
                      const PitchMark *marks, size_t mark_count) {
  if (!input || !output || !frames)
    return;
  resources_->push(input, frames);
  process_shared(input, output, frames, pitch, track, marks, mark_count);
}
void TdPsola::process_shared(const float *input, float *output, size_t frames,
                      const PitchResult &pitch, PitchTrackState track,
                      const PitchMark *marks, size_t mark_count) {
  if (!input || !output || !frames) return;
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Total));
  block_has_psola_ = false;
  const uint64_t block_start = output_position_,
                 block_end = block_start + frames;
  ++telemetry_.blocks;
  if (!target_enabled_) {
    state_ = PitchShiftState::Bypass;
    have_cursor_ = false;
    psola_gain_ = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
      active_mix_ = std::max(0.0f, active_mix_ - 1.0f / (sample_rate_ * .020f));
      const bool history_ready = block_start + i >= history_offset_;
      const uint64_t source =
          history_ready ? block_start + i - history_offset_ : 0;
      const float fallback = history_ready && history_available(source, source)
                                 ? history_at(source)
                                 : input[i];
      output[i] = input[i] * (1.0f - active_mix_) + fallback * active_mix_;
      const size_t oi = (block_start + i) & (kOlaSize - 1);
      ola_[oi] = lpc_ola_[oi] = norm_[oi] = 0.0f;
    }
    output_position_ += frames;
    telemetry_.state = state_;
    publish_telemetry();
    VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Total),
                   static_cast<uint64_t>(1000000.0 * frames / sample_rate_));
    return;
  }
  const float alpha =
      1.0f - std::exp(-1.0f / (sample_rate_ * smoothing_ms_ * .001f));
  const bool usable = target_enabled_ && track == PitchTrackState::Locked &&
                      pitch.voiced && std::isfinite(pitch.period_samples) &&
                      pitch.period_samples >= 24 &&
                      pitch.period_samples <= 800 && mark_count >= 2;
  if (pitch.voiced && (!std::isfinite(pitch.period_samples) ||
                       pitch.period_samples < 24 || pitch.period_samples > 800))
    ++telemetry_.invalid_pitch;
  if (pitch.voiced && mark_count && mark_count < 2)
    ++telemetry_.invalid_mark;
  if (!usable) {
    state_ = pitch.voiced ? PitchShiftState::WaitingForAnalysis
                          : PitchShiftState::Fallback;
  } else if (psola_gain_ < .99f)
    state_ = PitchShiftState::Acquiring;
  else
    state_ = PitchShiftState::Active;
  if (pitch.onset)
    onset_hold_ = static_cast<uint32_t>(sample_rate_ * .012f);
  if (pitch.pitch_changed) {
    onset_hold_ =
        std::max(onset_hold_, static_cast<uint32_t>(sample_rate_ * .006f));
    ++telemetry_.psola_resyncs;
  }
  const float desired_gain = usable && !onset_hold_ ? 1.0f : 0.0f;
  const float gain_step = 1.0f / std::max(1.0f, sample_rate_ * .020f);
  if (usable) {
    if (!have_cursor_) {
      next_synthesis_mark_ = block_start + pitch.period_samples;
      have_cursor_ = true;
    }
    size_t grains = 0;
    while (next_synthesis_mark_ <
               static_cast<double>(block_end + pitch.period_samples) &&
           grains < kMaxGrainsPerBlock) {
      const float pitch_alpha =
          1.0f - std::exp(-pitch.period_samples /
                          (sample_rate_ * smoothing_ms_ * .001f));
      current_semitones_ +=
          pitch_alpha * (target_semitones_ - current_semitones_);
      const float ratio = std::exp2(current_semitones_ / 12.0f);
      const double source = next_synthesis_mark_ - history_offset_;
      if (source >= 0 &&
          add_grain(next_synthesis_mark_, source, marks, mark_count))
        ++grains;
      next_synthesis_mark_ += pitch.period_samples / std::max(ratio, .5f);
    }
    telemetry_.grains += grains;
    telemetry_.max_grains_per_block =
        std::max<uint32_t>(telemetry_.max_grains_per_block, grains);
    if (next_synthesis_mark_ < static_cast<double>(block_end)) {
      ++telemetry_.max_grains_exceeded;
      ++telemetry_.psola_resyncs;
      next_synthesis_mark_ = block_end + pitch.period_samples;
    }
  } else {
    have_cursor_ = false;
  }
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Normalization));
  for (size_t i = 0; i < frames; ++i) {
    const size_t oi = (block_start + i) & (kOlaSize - 1);
    const bool history_ready = block_start + i >= history_offset_;
    const uint64_t source =
        history_ready ? block_start + i - history_offset_ : 0;
    const float fallback = history_ready && history_available(source, source)
                               ? history_at(source)
                               : 0.0f;
    if (norm_[oi] > 1e-5f) {
      output[i] = ola_[oi] / norm_[oi];
      const float target=(formant_mode_==FormantMode::Lpc && grain_model_.valid)
                             ? formant_amount_*grain_model_.confidence:0.0f;
      formant_mix_+=.002f*(target-formant_mix_);
      if(formant_mix_>1e-4f){
        float restored=lpc_ola_[oi]/norm_[oi];
        for(size_t j=1;j<=grain_model_.order;++j)restored-=grain_model_.coefficients[j]*synthesis_state_[j-1];
        if(std::isfinite(restored)&&std::fabs(restored)<8.0f){
          for(size_t j=grain_model_.order-1;j>0;--j)synthesis_state_[j]=synthesis_state_[j-1];
          synthesis_state_[0]=restored; output[i]+=(restored-output[i])*formant_mix_; ++formant_frames_;
        } else { synthesis_state_.fill(0); formant_mix_=0; }
      }
      block_has_psola_ = true;
    } else {
      output[i] = fallback;
    }
    ola_[oi] = lpc_ola_[oi] = norm_[oi] = 0;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Normalization),
                 0);
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Unvoiced));
  // The fallback is read from the same source-history timeline as the grains.
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Unvoiced), 0);
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Crossfade));
  for (size_t i = 0; i < frames; ++i) {
    current_wet_ += alpha * (target_wet_ - current_wet_);
    active_mix_ = std::min(1.0f, active_mix_ + 1.0f / (sample_rate_ * .020f));
    psola_gain_ +=
        std::clamp(desired_gain - psola_gain_, -gain_step, gain_step);
    if (onset_hold_)
      --onset_hold_;
    const bool history_ready = block_start + i >= history_offset_;
    const uint64_t source =
        history_ready ? block_start + i - history_offset_ : 0;
    const float fallback = history_ready && history_available(source, source)
                               ? history_at(source)
                               : 0.0f;
    const float shifted = output[i];
    const float wet_signal =
        shifted * psola_gain_ + fallback * (1.0f - psola_gain_);
    const float effective_wet = current_wet_ * active_mix_;
    output[i] = input[i] * (1.0f - effective_wet) + wet_signal * effective_wet;
    if (psola_gain_ < .001f)
      ++telemetry_.fallback_frames;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Crossfade), 0);
  output_position_ += frames;
  telemetry_.state = state_;
  publish_telemetry();
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Total),
                 static_cast<uint64_t>(1000000.0 * frames / sample_rate_));
}
void TdPsola::store_atomic64(Atomic64Parts &destination, uint64_t value) {
  destination.low.store(static_cast<uint32_t>(value),
                        std::memory_order_relaxed);
  destination.high.store(static_cast<uint32_t>(value >> 32U),
                         std::memory_order_relaxed);
}
uint64_t TdPsola::load_atomic64(const Atomic64Parts &source) {
  return source.low.load(std::memory_order_relaxed) |
         (static_cast<uint64_t>(source.high.load(std::memory_order_relaxed))
          << 32U);
}
void TdPsola::publish_telemetry() {
  published_telemetry_.sequence.fetch_add(1, std::memory_order_acq_rel);
  store_atomic64(published_telemetry_.blocks, telemetry_.blocks);
  store_atomic64(published_telemetry_.grains, telemetry_.grains);
  published_telemetry_.max_grains_per_block.store(
      telemetry_.max_grains_per_block, std::memory_order_relaxed);
  store_atomic64(published_telemetry_.pitch_mark_underflows,
                 telemetry_.pitch_mark_underflows);
  store_atomic64(published_telemetry_.audio_history_underflows,
                 telemetry_.audio_history_underflows);
  store_atomic64(published_telemetry_.psola_resyncs, telemetry_.psola_resyncs);
  store_atomic64(published_telemetry_.fallback_frames,
                 telemetry_.fallback_frames);
  store_atomic64(published_telemetry_.max_grains_exceeded,
                 telemetry_.max_grains_exceeded);
  store_atomic64(published_telemetry_.invalid_pitch, telemetry_.invalid_pitch);
  store_atomic64(published_telemetry_.invalid_mark, telemetry_.invalid_mark);
  store_atomic64(published_telemetry_.formant_frames, formant_frames_);
  published_telemetry_.state.store(static_cast<uint32_t>(state_),
                                   std::memory_order_relaxed);
  published_telemetry_.sequence.fetch_add(1, std::memory_order_release);
}
PitchShiftTelemetry TdPsola::telemetry() const {
  PitchShiftTelemetry result;
  uint32_t before, after;
  do {
    before = published_telemetry_.sequence.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    result.blocks = load_atomic64(published_telemetry_.blocks);
    result.grains = load_atomic64(published_telemetry_.grains);
    result.max_grains_per_block =
        published_telemetry_.max_grains_per_block.load(
            std::memory_order_relaxed);
    result.pitch_mark_underflows =
        load_atomic64(published_telemetry_.pitch_mark_underflows);
    result.audio_history_underflows =
        load_atomic64(published_telemetry_.audio_history_underflows);
    result.psola_resyncs = load_atomic64(published_telemetry_.psola_resyncs);
    result.fallback_frames =
        load_atomic64(published_telemetry_.fallback_frames);
    result.max_grains_exceeded =
        load_atomic64(published_telemetry_.max_grains_exceeded);
    result.invalid_pitch = load_atomic64(published_telemetry_.invalid_pitch);
    result.invalid_mark = load_atomic64(published_telemetry_.invalid_mark);
    result.formant_frames = load_atomic64(published_telemetry_.formant_frames);
    result.state = static_cast<PitchShiftState>(
        published_telemetry_.state.load(std::memory_order_relaxed));
    after = published_telemetry_.sequence.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}
ProfileStats TdPsola::profile(PitchShiftProfileSection s) const {
  if (s >= PitchShiftProfileSection::Count)
    return {};
  return profiler_.stats(section(s));
}
