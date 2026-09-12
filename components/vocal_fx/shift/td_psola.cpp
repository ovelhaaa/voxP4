#include "td_psola.h"
#include <algorithm>
#include <cmath>

extern void vocal_fx_funnel_inc_pitch_marks_consumed(uint64_t count);
extern void vocal_fx_funnel_inc_psola_process(uint64_t count);
extern void vocal_fx_funnel_inc_grain_schedule_attempts(uint64_t count);
extern void vocal_fx_funnel_inc_grains_scheduled(uint64_t count);
extern void vocal_fx_funnel_inc_grains_rendered(uint64_t count);

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
                   SharedPitchShiftResources *shared, const SharedLpcAnalysis *lpc,
                   uint32_t voice_index) {
  if (!std::isfinite(rate) || rate < 8000 || rate > 192000)
    return false;
  if (!shared) return false;
  voice_index_ = voice_index;
  unvoiced_config_ = config.unvoiced_articulation;
  unvoiced_articulation_.init(rate, voice_index_, unvoiced_config_);
  plosive_config_ = config.plosive_bridge;
  plosive_bridge_.init(rate, voice_index_, plosive_config_);
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
  history_offset_ = config.history_offset_samples == 1536
                        ? static_cast<uint32_t>(std::lround(rate * 0.032))
                        : std::clamp<uint32_t>(config.history_offset_samples,
                                               1152, 3072);
  smoothing_ms_ = std::clamp(smoothing_ms, 1.0f, 500.0f);
  target_enabled_ = config.enabled;
  target_semitones_ = std::clamp(semitones, -12.0f, 12.0f);
  target_wet_ = std::clamp(wet, 0.0f, 1.0f);
  continuity_policy_ = config.continuity_policy;
  recovery_mode_ = config.recovery_mode;
  recovery_crossfade_ms_ = std::clamp(config.recovery_crossfade_ms, 1.0f, 20.0f);
  recovery_small_error_cents_ = std::clamp(config.recovery_small_error_cents, 10.0f, 100.0f);
  recovery_note_change_cents_ = std::clamp(config.recovery_note_change_cents, 50.0f, 300.0f);
  fallback_policy_ = config.fallback_policy;
  ola_normalization_ = config.ola_normalization;
  unvoiced_fallback_gain_ = config.unvoiced_fallback_gain;
  onset_fallback_gain_ = config.onset_fallback_gain;
  energy_alpha_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.020f));
  fallback_hpf_alpha_ = 1.0f / (1.0f + 2.0f * kPi * config.unvoiced_hpf_hz / sample_rate_);
  formant_mode_ = config.formant_mode;
  formant_amount_ = std::clamp(config.formant_amount, 0.0f, 1.0f);
  formant_shift_semitones_ = std::clamp(config.formant_shift_semitones, -12.0f, 12.0f);
  formant_bandwidth_expansion_ = std::clamp(config.formant_bandwidth_expansion, 0.90f, 1.0f);
  formant_normalization_strategy_ = config.formant_normalization_strategy;
  reset();
  return true;
}
void TdPsola::clear_ola() {
  ola_.fill(0);
  lpc_ola_.fill(0);
  norm_.fill(0);
#ifndef ESP_PLATFORM
  norm_square_.fill(0);
  overlap_.fill(0);
  measured_ola_.fill(0);
  coasted_ola_.fill(0);
  measured_norm_.fill(0);
  coasted_norm_.fill(0);
#endif
  crossfade_old_ola_.fill(0);
  crossfade_old_norm_.fill(0);
}
void TdPsola::reset() {
  unvoiced_articulation_.reset();
  clear_ola();
  profiler_.reset();
  output_position_ = 0;
  next_synthesis_mark_ = 0;
  have_cursor_ = false;
  block_has_psola_ = false;
  current_semitones_ = target_semitones_;
  current_wet_ = target_wet_;
  psola_gain_ = 0;
  active_mix_ = (target_enabled_ || target_wet_ >= 0.99f) ? 1.0f : 0.0f;
  onset_hold_ = 0;
  short_pitch_loss_remaining_ = 0;
  short_pitch_loss_holding_ = false;
  release_total_ = 0;
  release_remaining_ = 0;
  source_energy_ = 0.0f;
  ola_energy_ = 0.0f;
  energy_alpha_ = 1.0f - std::exp(-1.0f / (sample_rate_ * 0.020f));
  fallback_hpf_input_ = 0.0f;
  fallback_hpf_output_ = 0.0f;
  previous_track_state_ = PitchTrackState::Unlocked;
  last_coasted_period_ = 0.0f;
  last_reliable_f0_before_coast_ = 0.0f;
  active_recovery_class_ = PsolaRecoveryClass::None;
  active_recovery_type_ = PsolaRecoveryType::None;
  recovery_period_error_cents_ = 0.0f;
  recovery_mark_error_fraction_ = 0.0f;
  recovery_mark_error_samples_ = 0;
  crossfade_total_ = 0;
  crossfade_pos_ = 0;
  crossfade_remaining_ = 0;
  current_synthesis_period_ = 0.0f;
  slew_grains_remaining_ = 0;
  slew_period_step_ = 0.0f;
  pre_recovery_rms_ = 0.0f;
  transition_min_rms_ = 1.0f;
#ifndef ESP_PLATFORM
  source_mark_run_value_ = 0;
  source_mark_run_count_ = 0;
  source_marks_1x_ = 0;
  source_marks_2x_ = 0;
  source_marks_3x_ = 0;
  source_marks_4x_or_more_ = 0;
  source_mark_reuses_total_ = 0;
#endif
  state_ = target_enabled_ ? PitchShiftState::WaitingForAnalysis
                           : PitchShiftState::Bypass;
  telemetry_ = {};
  unvoiced_articulation_.reset();
  plosive_bridge_.reset();
  synthesis_state_.fill(0);
  grain_model_ = {};
  formant_mix_ = 0;
  formant_filter_gain_ = 1.0f;
  fast_lpc_energy_ = 0.0f;
  fast_psola_energy_ = 0.0f;
  slow_gain_target_ = 1.0f;
  smoothed_gain_ = 1.0f;
  softclip_events_ = 0;
  gain_rail_events_ = 0;
  formant_frames_ = 0;
  formant_resets_ = 0;
  max_restored_ = 0.0f;
  max_pre_tanh_ = 0.0f;
  last_pre_tanh_ = 0.0f;
  last_post_tanh_ = 0.0f;
  last_gain_scale_ = 1.0f;
  debug_ = {};
  debug_.history_offset = history_offset_;
  debug_.onset_unvoiced_attenuation_enabled = onset_unvoiced_attenuation_enabled_;
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
  if (!marks || count == 0)
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
  if (count == 1) {
    period = (current_synthesis_period_ >= 24.0f && current_synthesis_period_ <= 800.0f)
                 ? current_synthesis_period_
                 : 100.0f;
  } else {
    const uint64_t p0 =
        best ? marks[best - 1].sample_position : marks[best].sample_position;
    const uint64_t p1 = best + 1 < count ? marks[best + 1].sample_position
                                         : marks[best].sample_position;
    period = best && best + 1 < count
                 ? .5f * ((marks[best].sample_position - p0) +
                          (p1 - marks[best].sample_position))
                 : static_cast<float>(p1 - p0);
    if (period < 24.0f || period > 800.0f) {
      if (current_synthesis_period_ >= 24.0f && current_synthesis_period_ <= 800.0f) {
        period = current_synthesis_period_;
      }
    }
  }
  return marks[best].confidence > .15f && period >= 24 && period <= 800 &&
         distance <= std::max(2.0 * period, 256.0);
}

#ifndef ESP_PLATFORM
void TdPsola::finish_source_mark_run() {
  if (source_mark_run_count_ == 0)
    return;
  if (source_mark_run_count_ == 1)
    ++source_marks_1x_;
  else if (source_mark_run_count_ == 2)
    ++source_marks_2x_;
  else if (source_mark_run_count_ == 3)
    ++source_marks_3x_;
  else
    ++source_marks_4x_or_more_;
  source_mark_run_count_ = 0;
}
#endif

PitchShiftFallbackReason TdPsola::fallback_reason_for_sample(
    const PitchResult &pitch, PitchTrackState track, bool usable,
    bool sample_has_psola) const {
  if (sample_has_psola && !onset_hold_)
    return PitchShiftFallbackReason::None;
  if (release_remaining_ > 0)
    return PitchShiftFallbackReason::None;
  if (!pitch.voiced)
    return PitchShiftFallbackReason::Unvoiced;
  if (track == PitchTrackState::Acquiring)
    return PitchShiftFallbackReason::Acquiring;
  if (pitch.confidence < 0.60f || track == PitchTrackState::Unlocked)
    return PitchShiftFallbackReason::LowConfidence;
  if (pitch.onset || onset_hold_ > 0)
    return PitchShiftFallbackReason::Onset;
  if (!usable)
    return PitchShiftFallbackReason::TargetInvalid;
  return PitchShiftFallbackReason::None;
}

float TdPsola::fallback_policy_gain(PitchShiftFallbackReason reason) const {
  switch (fallback_policy_) {
  case HarmonyFallbackPolicy::CurrentDry:
    return 1.0f;
  case HarmonyFallbackPolicy::Muted:
    return 0.0f;
  case HarmonyFallbackPolicy::UnvoicedOnly:
    return reason == PitchShiftFallbackReason::Unvoiced ? unvoiced_fallback_gain_ : 0.0f;
  case HarmonyFallbackPolicy::OnsetAndUnvoiced:
    if (reason == PitchShiftFallbackReason::Unvoiced)
      return unvoiced_fallback_gain_;
    if (reason == PitchShiftFallbackReason::Onset)
      return onset_fallback_gain_;
    return 0.0f;
  case HarmonyFallbackPolicy::HighpassUnvoiced:
    return reason == PitchShiftFallbackReason::Unvoiced ? unvoiced_fallback_gain_ : 0.0f;
  default:
    return 0.0f;
  }
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
  vocal_fx_funnel_inc_pitch_marks_consumed(1);
  VF_PROFILE_BEGIN(profiler_,
                   section(PitchShiftProfileSection::GrainPreparation));
  const int half = std::clamp(static_cast<int>(std::lround(period)), 24, 800);
  const uint64_t center = marks[index].sample_position;
  debug_.selected_source_mark = center;
  debug_.source_grain_timestamp = static_cast<uint64_t>(std::max(0.0, source));
  debug_.output_synthesis_timestamp =
      static_cast<uint64_t>(std::max(0.0, destination));
  if (center < static_cast<uint64_t>(half) ||
      !history_available(center - half, center + half)) {
    VF_PROFILE_END(profiler_,
                   section(PitchShiftProfileSection::GrainPreparation), 0);
    ++telemetry_.audio_history_underflows;
    return false;
  }
  last_scheduled_mark_ = center;
  new_grain_scheduled_this_block_ = true;
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::GrainPreparation),
                 0);
#ifndef ESP_PLATFORM
  if (source_mark_run_count_ == 0 || center != source_mark_run_value_) {
    finish_source_mark_run();
    source_mark_run_value_ = center;
    source_mark_run_count_ = 1;
  } else {
    ++source_mark_run_count_;
    ++source_mark_reuses_total_;
  }
  const bool mark_predicted = marks[index].predicted;
#endif
  SharedLpcModel model;
  const bool use_lpc = formant_mode_ == FormantMode::Lpc && formant_amount_ > 0 &&
                       lpc_ && lpc_->model_near(center, &model) && model.confidence > .15f;
  if (use_lpc) {
    grain_model_ = model;
    const float lambda = SharedLpcAnalysis::lambda_from_semitones(formant_shift_semitones_);
    SharedLpcAnalysis::warp_polynomial(model.coefficients.data(), model.order,
                                       lambda, formant_bandwidth_expansion_,
                                       grain_model_.coefficients.data());
    formant_filter_gain_ = SharedLpcAnalysis::compute_gain_normalization(
        model.coefficients.data(), grain_model_.coefficients.data(),
        model.order, formant_normalization_strategy_, sample_rate_);
  } else {
    grain_model_ = SharedLpcModel{};
    formant_filter_gain_ = 1.0f;
  }
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
#ifndef ESP_PLATFORM
    norm_square_[oi] += w * w;
    overlap_[oi] += 1;
    if (mark_predicted) {
      coasted_ola_[oi] += plain * w;
      coasted_norm_[oi] += w;
    } else {
      measured_ola_[oi] += plain * w;
      measured_norm_[oi] += w;
    }
#endif
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
                             const PitchMark *marks, size_t mark_count
#ifndef ESP_PLATFORM
                             , float *psola_component,
                             float *fallback_component,
                             float *reference_delayed,
                             uint8_t *fallback_reason,
                             uint8_t *fallback_active,
                             float *measured_component,
                             float *coasted_component,
                             uint8_t *measured_active,
                             uint8_t *coasted_active,
                               float *articulation_component,
                               uint8_t *articulation_active,
                               uint8_t *unvoiced_acoustic_class,
                               float *plosive_bridge_component,
                               uint8_t *plosive_bridge_active,
                               SampleTelemetryRecord *sample_telemetry
#endif
                              ) {
  if (!input || !output || !frames) return;
  vocal_fx_funnel_inc_psola_process(1);
  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Total));
  block_has_psola_ = false;
  new_grain_scheduled_this_block_ = false;
#ifndef ESP_PLATFORM
  SampleTelemetryRecord *telem_dst = sample_telemetry ? sample_telemetry : sample_telemetry_;
#endif
  float block_input_rms = 0.0f;
  if (input && frames > 0) {
    float sum_sq = 0.0f;
    for (size_t k = 0; k < frames; ++k)
      sum_sq += input[k] * input[k];
    block_input_rms = std::sqrt(sum_sq / frames);
  }
  const uint64_t block_start = output_position_,
                 block_end = block_start + frames;
  debug_.input_absolute_sample = resources_ ? resources_->input_end() : block_end;
  debug_.pitch_timestamp = pitch.analysis_timestamp_samples;
  debug_.analysis_age = block_end > pitch.analysis_timestamp_samples
                            ? block_end - pitch.analysis_timestamp_samples
                            : 0;
  debug_.history_offset = history_offset_;
  debug_.requested_semitones = target_semitones_;
  debug_.target_ratio = std::exp2(target_semitones_ / 12.0f);
  debug_.source_f0 = pitch.frequency_hz;
  debug_.pitch_confidence = pitch.confidence;
  debug_.target_f0 = pitch.frequency_hz * debug_.target_ratio;
  ++telemetry_.blocks;

  if (short_pitch_loss_holding_ && short_pitch_loss_remaining_) {
    const uint32_t step = std::min<uint32_t>(short_pitch_loss_remaining_, frames);
    short_pitch_loss_remaining_ -= step;
    if (short_pitch_loss_remaining_ == 0) {
      target_enabled_ = false;
      short_pitch_loss_holding_ = false;
    }
  }

  if (!target_enabled_ && release_remaining_ == 0) {
    state_ = PitchShiftState::Bypass;
    have_cursor_ = false;
    psola_gain_ = 0.0f;
    for (size_t i = 0; i < frames; ++i) {
      active_mix_ = std::max(0.0f, active_mix_ - 1.0f / (sample_rate_ * .020f));
      const bool history_ready = block_start + i >= history_offset_;
      const uint64_t source =
          history_ready ? block_start + i - history_offset_ : 0;
      const float delayed_lead = history_ready && history_available(source, source)
                                     ? history_at(source)
                                     : 0.0f;
      float fallback = 0.0f;
      if (fallback_policy_ == HarmonyFallbackPolicy::CurrentDry) {
        fallback = delayed_lead;
      }
      const float dry_gain = 1.0f - current_wet_;
      const float effective_wet = current_wet_ * active_mix_;
      output[i] = input[i] * dry_gain + fallback * effective_wet;
      const size_t oi = (block_start + i) & (kOlaSize - 1);
      ola_[oi] = lpc_ola_[oi] = norm_[oi] = 0.0f;
#ifndef ESP_PLATFORM
      if (psola_component) psola_component[i] = 0.0f;
      if (fallback_component) fallback_component[i] = fallback * effective_wet;
      if (reference_delayed) reference_delayed[i] = delayed_lead;
      if (fallback_reason) fallback_reason[i] = static_cast<uint8_t>(PitchShiftFallbackReason::None);
      if (fallback_active) fallback_active[i] = active_mix_ > 0.01f ? 1 : 0;
      if (measured_component) measured_component[i] = 0.0f;
      if (coasted_component) coasted_component[i] = 0.0f;
      if (measured_active) measured_active[i] = 0;
      if (coasted_active) coasted_active[i] = 0;
      if (plosive_bridge_component) plosive_bridge_component[i] = 0.0f;
      if (plosive_bridge_active) plosive_bridge_active[i] = 0;
      if (telem_dst) {
        SampleTelemetryRecord &rec = telem_dst[i];
        rec.sample_index = block_start + i;
        rec.input_rms = block_input_rms;
        rec.input_peak = input ? std::fabs(input[i]) : 0.0f;
        rec.input_envelope = 0.0f;
        rec.pitch_voiced = pitch.voiced ? 1 : 0;
        rec.pitch_confidence = pitch.confidence;
        rec.pitch_period_samples = pitch.period_samples;
        rec.pitch_f0_hz = pitch.frequency_hz;
        rec.pitch_onset = pitch.onset ? 1 : 0;
        rec.pitch_track_state = static_cast<uint8_t>(track);
        rec.coherent_marks = pitch.coherent_marks;
        rec.mark_count = static_cast<uint16_t>(mark_count);
        rec.target_enabled = target_enabled_ ? 1 : 0;
        rec.psola_usable = 0;
        rec.psola_usable_reason = static_cast<uint32_t>(PsolaUsableReason::TargetDisabled);
        rec.continuity_coasting = 0;
        rec.new_grain_scheduled = 0;
        rec.active_grain_count = 0;
        rec.last_grain_age = 0;
        rec.next_synthesis_mark = next_synthesis_mark_;
        rec.ola_weight_sum = 0.0f;
        rec.ola_output_rms = 0.0f;
        rec.release_active = 0;
        rec.release_remaining = 0;
        rec.unvoiced_path_active = 0;
        rec.unvoiced_gain = 0.0f;
        rec.plosive_path_active = 0;
        rec.plosive_gain = 0.0f;
        rec.psola_gain = psola_gain_;
        rec.active_mix = active_mix_;
        rec.final_harmony_rms = std::fabs(output[i]);
        rec.effective_total_gain = 0.0f;
        rec.pitch_voiced_raw = pitch.voiced_raw ? 1 : 0;
        rec.yin_min = pitch.yin_min;
        rec.spectral_centroid = pitch.spectral_centroid;
        rec.high_frequency_ratio = pitch.high_frequency_ratio;
        rec.zero_crossing_rate = pitch.zero_crossing_rate;
      }
#endif
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
  const bool continuity_coasting =
      track == PitchTrackState::Coasting &&
      (continuity_policy_ == PsolaContinuityPolicy::Coasting ||
       continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting);
  const bool acquiring_ready =
      (track == PitchTrackState::Acquiring || track == PitchTrackState::Locked) &&
      mark_count >= 1 &&
      pitch.confidence >= 0.70f && std::isfinite(pitch.period_samples) &&
      pitch.period_samples >= 24.0f && pitch.period_samples <= 800.0f &&
      (continuity_policy_ == PsolaContinuityPolicy::OnsetContinuity ||
       continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting);
  const bool usable = target_enabled_ &&
                      (track == PitchTrackState::Locked || continuity_coasting || acquiring_ready) &&
                      (pitch.voiced || continuity_coasting) &&
                      std::isfinite(pitch.period_samples) &&
                      pitch.period_samples >= 24 &&
                      pitch.period_samples <= 800 && mark_count >= 1;

  uint32_t usable_reason = static_cast<uint32_t>(PsolaUsableReason::Usable);
  if (!usable) {
    if (!target_enabled_)
      usable_reason |= static_cast<uint32_t>(PsolaUsableReason::TargetDisabled);
    if (!pitch.voiced && !continuity_coasting)
      usable_reason |= static_cast<uint32_t>(PsolaUsableReason::NotVoiced);
    if (pitch.confidence < 0.70f)
      usable_reason |= static_cast<uint32_t>(PsolaUsableReason::LowConfidence);
    if (!std::isfinite(pitch.period_samples) || pitch.period_samples < 24.0f ||
        pitch.period_samples > 800.0f)
      usable_reason |= static_cast<uint32_t>(PsolaUsableReason::InvalidPeriod);
    if (mark_count == 0)
      usable_reason |= static_cast<uint32_t>(PsolaUsableReason::NoMarks);
    if (!(track == PitchTrackState::Locked || continuity_coasting || acquiring_ready)) {
      if (track == PitchTrackState::Unlocked)
        usable_reason |= static_cast<uint32_t>(PsolaUsableReason::TrackUnlocked);
      else if (track == PitchTrackState::Acquiring && !acquiring_ready)
        usable_reason |= static_cast<uint32_t>(PsolaUsableReason::AcquiringNotReady);
    }
  }
  if (release_remaining_ > 0) {
    usable_reason |= static_cast<uint32_t>(PsolaUsableReason::ReleaseActive);
  }

  if (pitch.voiced && (!std::isfinite(pitch.period_samples) ||
                       pitch.period_samples < 24 || pitch.period_samples > 800))
    ++telemetry_.invalid_pitch;
  if (pitch.voiced && mark_count == 0)
    ++telemetry_.invalid_mark;

  if (!usable) {
    state_ = pitch.voiced ? PitchShiftState::WaitingForAnalysis
                          : PitchShiftState::Fallback;
  } else if (psola_gain_ < .99f)
    state_ = PitchShiftState::Acquiring;
  else
    state_ = PitchShiftState::Active;

  if (pitch.onset && continuity_policy_ == PsolaContinuityPolicy::Baseline)
    onset_hold_ = static_cast<uint32_t>(sample_rate_ * .012f);
  if (pitch.pitch_changed) {
    if (continuity_policy_ == PsolaContinuityPolicy::Baseline)
      onset_hold_ =
          std::max(onset_hold_, static_cast<uint32_t>(sample_rate_ * .006f));
    ++telemetry_.psola_resyncs;
  }

  const float desired_gain = usable && !onset_hold_ ? 1.0f : 0.0f;
  const float gain_step = (continuity_policy_ == PsolaContinuityPolicy::OnsetContinuity ||
                           continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting)
                              ? 1.0f / std::max(1.0f, sample_rate_ * .002f)
                              : 1.0f / std::max(1.0f, sample_rate_ * .020f);

  if (usable) {
    // Check for recovery: transition from COASTING to LOCKED
    const bool recovering = (previous_track_state_ == PitchTrackState::Coasting &&
                             track == PitchTrackState::Locked);
    if (recovering) {
      float delta_cents = 0.0f;
      if (last_coasted_period_ > 0.0f && pitch.period_samples > 0.0f) {
        delta_cents = 1200.0f * std::log2(pitch.period_samples / last_coasted_period_);
      }
      recovery_period_error_cents_ = delta_cents;

      // Mark phase mismatch against nearest measured mark
      double expected_source = next_synthesis_mark_ - history_offset_;
      double min_dist = 1e9;
      for (size_t k = 0; k < mark_count; ++k) {
        if (!marks[k].predicted) {
          double d = std::abs(static_cast<double>(marks[k].sample_position) - expected_source);
          if (d < min_dist)
            min_dist = d;
        }
      }
      uint32_t mark_err_samples = (min_dist < 1e8) ? static_cast<uint32_t>(std::lround(min_dist)) : 0;
      if (pitch.period_samples > 0) {
        float rem = std::fmod(static_cast<float>(mark_err_samples), pitch.period_samples);
        if (rem > 0.5f * pitch.period_samples)
          rem = pitch.period_samples - rem;
        mark_err_samples = static_cast<uint32_t>(std::lround(rem));
      }
      recovery_mark_error_samples_ = mark_err_samples;
      recovery_mark_error_fraction_ = pitch.period_samples > 0
                                          ? (static_cast<float>(mark_err_samples) / pitch.period_samples)
                                          : 0.0f;

      const float abs_delta_cents = std::fabs(delta_cents);

      // Multi-tier recovery classification
      if (recovery_mode_ == PsolaRecoveryMode::OldHard) {
        current_synthesis_period_ = pitch.period_samples;
        next_synthesis_mark_ = block_start + pitch.period_samples;
        active_recovery_class_ = PsolaRecoveryClass::HardReset;
        active_recovery_type_ = PsolaRecoveryType::None;
      } else if (!std::isfinite(pitch.period_samples) || pitch.period_samples < 24 ||
          pitch.period_samples > 800 || mark_count < 2) {
        active_recovery_class_ = PsolaRecoveryClass::HardReset;
        active_recovery_type_ = PsolaRecoveryType::None;
      } else if (abs_delta_cents <= recovery_small_error_cents_ && recovery_mark_error_fraction_ < 0.25f) {
        active_recovery_class_ = PsolaRecoveryClass::SmallError;
        active_recovery_type_ = PsolaRecoveryType::SameNote;
      } else if (abs_delta_cents <= recovery_note_change_cents_ &&
                 !(pitch.pitch_changed && abs_delta_cents > 75.0f)) {
        active_recovery_class_ = PsolaRecoveryClass::MediumError;
        active_recovery_type_ = (abs_delta_cents > 75.0f) ? PsolaRecoveryType::Glide : PsolaRecoveryType::SameNote;
      } else {
        active_recovery_class_ = PsolaRecoveryClass::NoteChangeCrossfade;
        if (std::fabs(abs_delta_cents - 1200.0f) < 100.0f) {
          active_recovery_type_ = PsolaRecoveryType::OctaveSuspect;
        } else if (abs_delta_cents > recovery_note_change_cents_ || (pitch.pitch_changed && abs_delta_cents > 75.0f)) {
          active_recovery_type_ = PsolaRecoveryType::NoteChange;
        } else {
          active_recovery_type_ = PsolaRecoveryType::Glide;
        }
      }

      // Action based on classification
      if (active_recovery_class_ == PsolaRecoveryClass::SmallError) {
        slew_grains_remaining_ = 4;
        slew_period_step_ = (pitch.period_samples - current_synthesis_period_) / 4.0f;
      } else if (active_recovery_class_ == PsolaRecoveryClass::MediumError) {
        slew_grains_remaining_ = 4;
        float diff = (pitch.period_samples - current_synthesis_period_) / 4.0f;
        float max_step = 0.05f * pitch.period_samples;
        slew_period_step_ = std::clamp(diff, -max_step, max_step);
      } else if (active_recovery_class_ == PsolaRecoveryClass::NoteChangeCrossfade) {
        uint32_t L = static_cast<uint32_t>(std::lround(recovery_crossfade_ms_ * sample_rate_ * 0.001f));
        L = std::clamp<uint32_t>(L, 48, kMaxCrossfadeSamples);
        crossfade_total_ = L;
        crossfade_pos_ = 0;
        crossfade_remaining_ = L;
        for (size_t k = 0; k < L; ++k) {
          const size_t oi = (block_start + k) & (kOlaSize - 1);
          crossfade_old_ola_[k] = ola_[oi];
          crossfade_old_norm_[k] = norm_[oi];
          ola_[oi] = 0.0f;
          lpc_ola_[oi] = 0.0f;
          norm_[oi] = 0.0f;
#ifndef ESP_PLATFORM
          measured_ola_[oi] = 0.0f;
          coasted_ola_[oi] = 0.0f;
          measured_norm_[oi] = 0.0f;
          coasted_norm_[oi] = 0.0f;
          norm_square_[oi] = 0.0f;
          overlap_[oi] = 0;
#endif
        }
        next_synthesis_mark_ = static_cast<double>(block_start);
        current_synthesis_period_ = pitch.period_samples;
        slew_grains_remaining_ = 0;
        slew_period_step_ = 0.0f;
        have_cursor_ = true;
      } else if (active_recovery_class_ == PsolaRecoveryClass::HardReset) {
        have_cursor_ = false;
        clear_ola();
      }
    }

    if (track == PitchTrackState::Coasting) {
      last_coasted_period_ = pitch.period_samples;
    } else if (track == PitchTrackState::Locked && previous_track_state_ != PitchTrackState::Coasting) {
      last_reliable_f0_before_coast_ = pitch.frequency_hz;
      last_coasted_period_ = pitch.period_samples;
    }
    previous_track_state_ = track;

    if (!have_cursor_) {
      // Window-safe onset reach-back:
      // Scan for earliest mark that is window-safe within the history buffer:
      // center >= half and history_available(center - half, center + half)
      double synth_mark = static_cast<double>(block_start);
      debug_.first_detected_mark = (mark_count >= 1 && marks != nullptr) ? marks[0].sample_position : 0;
      debug_.first_window_safe_mark = 0;
      debug_.first_scheduled_mark = 0;

      if (mark_count >= 1 && marks != nullptr) {
        const int half = std::clamp(static_cast<int>(std::lround(pitch.period_samples)), 24, 800);
        size_t safe_idx = mark_count; // sentinel
        const uint64_t max_age = std::max<uint64_t>(1536, 4 * static_cast<uint64_t>(half));
        for (size_t k = 0; k < mark_count; ++k) {
          const uint64_t center = marks[k].sample_position;
          if (pitch.analysis_timestamp_samples > center &&
              pitch.analysis_timestamp_samples - center > max_age) {
            continue;
          }
          if (center >= static_cast<uint64_t>(half) &&
              history_available(center - half, center + half)) {
            safe_idx = k;
            break;
          }
        }

        if (safe_idx < mark_count) {
          debug_.first_window_safe_mark = marks[safe_idx].sample_position;
          const double source_mark = static_cast<double>(marks[safe_idx].sample_position);
          const double aligned_synth = source_mark + history_offset_;
          const float synth_p = pitch.period_samples / std::max(std::exp2(current_semitones_ / 12.0f), 0.5f);
          if (aligned_synth <= static_cast<double>(block_start) && synth_p > 0.0f) {
            double m = aligned_synth;
            if (continuity_policy_ == PsolaContinuityPolicy::Baseline) {
              while (m + synth_p < static_cast<double>(block_start)) {
                m += synth_p;
              }
            } else {
              while (m + half < static_cast<double>(block_start)) {
                m += synth_p;
              }
            }
            synth_mark = m;
          } else {
            synth_mark = aligned_synth;
          }
        }
      }
      next_synthesis_mark_ = synth_mark;
      debug_.first_scheduled_mark = static_cast<uint64_t>(std::max(0.0, synth_mark));
      current_synthesis_period_ = pitch.period_samples;
      slew_grains_remaining_ = 0;
      slew_period_step_ = 0.0f;
      have_cursor_ = true;
      release_remaining_ = 0;
      if (continuity_policy_ == PsolaContinuityPolicy::OnsetContinuity ||
          continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting) {
        psola_gain_ = 1.0f;
        active_mix_ = 1.0f;
      }
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
      debug_.current_smoothed_ratio = ratio;

      if (slew_grains_remaining_ > 0) {
        current_synthesis_period_ += slew_period_step_;
        --slew_grains_remaining_;
      } else {
        current_synthesis_period_ = pitch.period_samples;
      }

      debug_.actual_synthesis_period =
          current_synthesis_period_ / std::max(ratio, .5f);
      const double source = next_synthesis_mark_ - history_offset_;
      vocal_fx_funnel_inc_grain_schedule_attempts(1);
      if (source >= 0 &&
          add_grain(next_synthesis_mark_, source, marks, mark_count)) {
        ++grains;
        vocal_fx_funnel_inc_grains_scheduled(1);
      }
      next_synthesis_mark_ += current_synthesis_period_ / std::max(ratio, .5f);
    }
    vocal_fx_funnel_inc_grains_rendered(grains);
    telemetry_.grains += grains;
    telemetry_.max_grains_per_block =
        std::max<uint32_t>(telemetry_.max_grains_per_block, grains);
    if (next_synthesis_mark_ < static_cast<double>(block_end)) {
      ++telemetry_.max_grains_exceeded;
      ++telemetry_.psola_resyncs;
      next_synthesis_mark_ = block_end + pitch.period_samples;
    }
  } else {
    if (have_cursor_ && release_remaining_ == 0 &&
        (continuity_policy_ == PsolaContinuityPolicy::OnsetContinuity ||
         continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting)) {
      release_total_ = static_cast<uint32_t>(sample_rate_ * 0.010f); // 10 ms release tail
      release_remaining_ = release_total_;
    }
    have_cursor_ = false;
    previous_track_state_ = track;
    crossfade_remaining_ = 0;
    slew_grains_remaining_ = 0;
  }

  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Normalization));
#ifndef ESP_PLATFORM
  finish_source_mark_run();
  float norm_sum = 0.0f, norm_square_sum = 0.0f, overlap_sum = 0.0f;
  float norm_min = 1e9f, norm_max = 0.0f;
  float norm_sq_min = 1e9f, norm_sq_max = 0.0f;
  uint32_t ov_min = 10000, ov_max = 0;
  uint32_t norm_below_count = 0;
#endif

  for (size_t i = 0; i < frames; ++i) {
    const size_t oi = (block_start + i) & (kOlaSize - 1);
    const bool history_ready = block_start + i >= history_offset_;
    const uint64_t source =
        history_ready ? block_start + i - history_offset_ : 0;
    const float delayed_lead = history_ready && history_available(source, source)
                                   ? history_at(source)
                                   : 0.0f;
#ifndef ESP_PLATFORM
    const float cur_norm = norm_[oi];
    const float cur_sq = norm_square_[oi];
    const uint32_t cur_ov = overlap_[oi];
    norm_sum += cur_norm;
    norm_square_sum += cur_sq;
    overlap_sum += static_cast<float>(cur_ov);
    norm_min = std::min(norm_min, cur_norm);
    norm_max = std::max(norm_max, cur_norm);
    norm_sq_min = std::min(norm_sq_min, cur_sq);
    norm_sq_max = std::max(norm_sq_max, cur_sq);
    ov_min = std::min(ov_min, cur_ov);
    ov_max = std::max(ov_max, cur_ov);
    if (cur_norm < 1e-5f)
      ++norm_below_count;
#endif

    const PitchShiftFallbackReason sample_reason =
        fallback_reason_for_sample(pitch, track, usable, block_has_psola_);
    float fallback = delayed_lead * fallback_policy_gain(sample_reason);
    if (fallback_policy_ == HarmonyFallbackPolicy::HighpassUnvoiced &&
        sample_reason == PitchShiftFallbackReason::Unvoiced) {
      fallback_hpf_output_ = fallback_hpf_alpha_ *
                             (fallback_hpf_output_ + delayed_lead - fallback_hpf_input_);
      fallback_hpf_input_ = delayed_lead;
      fallback = fallback_hpf_output_ * unvoiced_fallback_gain_;
    }

    const float art_source =
        (unvoiced_config_.timing_reference ==
         UnvoicedTimingReference::CurrentInput && input && i < frames)
            ? input[i]
            : delayed_lead;
    float art_sample = 0.0f;
    (void)unvoiced_articulation_.process_sample(
        art_source, pitch, track, block_input_rms, &art_sample);

    float plosive_source = delayed_lead;
    if (plosive_config_.timing_offset_ms > 0.0f) {
      const uint32_t offset_samples = static_cast<uint32_t>(
          plosive_config_.timing_offset_ms * 0.001f * sample_rate_);
      const uint64_t adv_source = source + std::min<uint64_t>(offset_samples, history_offset_);
      if (history_available(adv_source, adv_source)) {
        plosive_source = history_at(adv_source);
      }
    }
    float bridge_sample = 0.0f;
    (void)plosive_bridge_.process_sample(
        plosive_source, pitch, track, block_input_rms, &bridge_sample);

    float y_new = 0.0f;
    const bool has_new_grain = norm_[oi] > 1e-5f;
    if (has_new_grain) {
      y_new = ola_[oi] / norm_[oi];
      if (release_remaining_ > 0 && release_total_ > 0) {
        const float rel_gain = static_cast<float>(release_remaining_) / static_cast<float>(release_total_);
        y_new *= rel_gain;
        --release_remaining_;
      }
      const float target = (formant_mode_ == FormantMode::Lpc && grain_model_.valid && grain_model_.order > 0)
                               ? formant_amount_ * grain_model_.confidence
                               : 0.0f;
      formant_mix_ += .002f * (target - formant_mix_);
      if (formant_mix_ > 1e-4f && grain_model_.valid && grain_model_.order > 0 &&
          grain_model_.order <= VOCAL_FX_LPC_MAX_ORDER) {
        float restored = (lpc_ola_[oi] / norm_[oi]) * formant_filter_gain_;
        for (size_t j = 1; j <= grain_model_.order; ++j)
          restored -= grain_model_.coefficients[j] * synthesis_state_[j - 1];

        if (std::isfinite(restored)) {
          const float pre_softclip = restored;
          max_pre_tanh_ = std::max(max_pre_tanh_, std::fabs(pre_softclip));
          last_pre_tanh_ = pre_softclip;

          // Rational Soft Clipper:
          // Strict identity in nominal musical range |x| <= 1.0 (0% THD).
          // Continuous derivative C^1 at |x| = 1.0. Asymptotic ceiling at 3.0.
          if (std::fabs(restored) > 1.0f) {
            const float ax = std::fabs(restored);
            const float excess = ax - 1.0f;
            restored = std::copysign(1.0f + excess / (1.0f + 0.5f * excess), restored);
            ++softclip_events_;
          }
          last_post_tanh_ = restored;

          if (grain_model_.order > 1) {
            for (size_t j = grain_model_.order - 1; j > 0; --j)
              synthesis_state_[j] = synthesis_state_[j - 1] * 0.999f;
          }
          synthesis_state_[0] = restored;

          // Multi-stage musical RMS Gain Matcher:
          // 1. Fast peak/energy detector (tau ~ 6 ms)
          const float r2 = restored * restored;
          const float y2 = y_new * y_new;
          constexpr float kFastAlpha = 0.0035f; // ~6 ms at 48 kHz
          fast_lpc_energy_ += kFastAlpha * (r2 - fast_lpc_energy_);
          fast_psola_energy_ += kFastAlpha * (y2 - fast_psola_energy_);

          // 2. Slow gain target estimator with asymmetric attack/release
          if (fast_lpc_energy_ > 1e-8f && fast_psola_energy_ > 1e-8f) {
            const float instant_ratio = std::sqrt(fast_psola_energy_ / fast_lpc_energy_);
            // Soft limits: nominal [0.7, 1.4], wide safety rail [0.4, 2.5]
            const float target_g = std::clamp(instant_ratio, 0.4f, 2.5f);
            if (target_g <= 0.405f || target_g >= 2.495f) {
              ++gain_rail_events_;
            }
            // Asymmetric attack (fast rise 25 ms) vs release (slow decay 100 ms)
            constexpr float kAttackAlpha = 0.0008f;  // ~26 ms at 48 kHz
            constexpr float kReleaseAlpha = 0.0002f; // ~104 ms at 48 kHz
            const float rate = (target_g > slow_gain_target_) ? kAttackAlpha : kReleaseAlpha;
            slow_gain_target_ += rate * (target_g - slow_gain_target_);
          }

          // 3. Slew-rate-limited application (max 0.0005 per sample = 24.0 / ms)
          constexpr float kMaxGainSlewPerSample = 0.0005f;
          const float gain_delta = std::clamp(slow_gain_target_ - smoothed_gain_,
                                              -kMaxGainSlewPerSample, kMaxGainSlewPerSample);
          smoothed_gain_ += gain_delta;
          restored *= smoothed_gain_;
          last_gain_scale_ = smoothed_gain_;

          y_new += (restored - y_new) * formant_mix_;
          ++formant_frames_;
          max_restored_ = std::max(max_restored_, std::fabs(restored));
        } else {
          synthesis_state_.fill(0);
          formant_mix_ = 0.0f;
          ++formant_resets_;
        }
      }
      float g = 1.0f;
      if (ola_normalization_ == OlaNormalizationMode::ColaEnergyHybrid) {
        const float x2 = delayed_lead * delayed_lead;
        const float y2 = y_new * y_new;
        source_energy_ += energy_alpha_ * (x2 - source_energy_);
        ola_energy_ += energy_alpha_ * (y2 - ola_energy_);
        if (source_energy_ > 1e-10f && ola_energy_ > 1e-10f) {
          g = std::clamp(std::sqrt(source_energy_ / ola_energy_), 0.25f, 4.0f);
          y_new *= g;
        }
      }
      block_has_psola_ = true;

      if (crossfade_remaining_ > 0 && crossfade_total_ > 0) {
        const uint32_t k = crossfade_pos_;
        const float theta = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(crossfade_total_);
        const float w_old = std::cos(theta);
        const float w_new = std::sin(theta);
        float y_old = 0.0f;
        if (crossfade_old_norm_[k] > 1e-5f) {
          y_old = crossfade_old_ola_[k] / crossfade_old_norm_[k];
          if (ola_normalization_ == OlaNormalizationMode::ColaEnergyHybrid && g != 1.0f) {
            y_old *= g;
          }
        }
        output[i] = w_old * y_old + w_new * y_new + art_sample + bridge_sample;
#ifndef ESP_PLATFORM
        if (measured_component)
          measured_component[i] = w_new * y_new;
        if (coasted_component)
          coasted_component[i] = w_old * y_old;
        if (measured_active)
          measured_active[i] = (has_new_grain && w_new > 0.01f) ? 1 : 0;
        if (coasted_active)
          coasted_active[i] = (crossfade_old_norm_[k] > 1e-5f && w_old > 0.01f) ? 1 : 0;
#endif
        ++crossfade_pos_;
        --crossfade_remaining_;
        if (crossfade_remaining_ == 0) {
          active_recovery_class_ = PsolaRecoveryClass::None;
        }
      } else {
        output[i] = y_new + art_sample + bridge_sample;
#ifndef ESP_PLATFORM
        float m_norm = (norm_[oi] > 1e-5f) ? (measured_ola_[oi] / norm_[oi]) : 0.0f;
        float c_norm = (norm_[oi] > 1e-5f) ? (coasted_ola_[oi] / norm_[oi]) : 0.0f;
        if (ola_normalization_ == OlaNormalizationMode::ColaEnergyHybrid && g != 1.0f) {
          m_norm *= g;
          c_norm *= g;
        }
        if (measured_component)
          measured_component[i] = m_norm;
        if (coasted_component)
          coasted_component[i] = c_norm;
        if (measured_active)
          measured_active[i] = (measured_norm_[oi] > 1e-5f) ? 1 : 0;
        if (coasted_active)
          coasted_active[i] = (coasted_norm_[oi] > 1e-5f) ? 1 : 0;
#endif
      }
    } else if (crossfade_remaining_ > 0 && crossfade_total_ > 0) {
      const uint32_t k = crossfade_pos_;
      const float theta = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(crossfade_total_);
      const float w_old = std::cos(theta);
      float y_old = 0.0f;
      if (crossfade_old_norm_[k] > 1e-5f) {
        y_old = crossfade_old_ola_[k] / crossfade_old_norm_[k];
      }
      output[i] = w_old * y_old + art_sample + bridge_sample;
      block_has_psola_ = (w_old * y_old != 0.0f);
#ifndef ESP_PLATFORM
      if (measured_component)
        measured_component[i] = 0.0f;
      if (coasted_component)
        coasted_component[i] = w_old * y_old;
      if (measured_active)
        measured_active[i] = 0;
      if (coasted_active)
        coasted_active[i] = (crossfade_old_norm_[k] > 1e-5f && w_old > 0.01f) ? 1 : 0;
#endif
      ++crossfade_pos_;
      --crossfade_remaining_;
      if (crossfade_remaining_ == 0) {
        active_recovery_class_ = PsolaRecoveryClass::None;
      }
    } else {
      if (release_remaining_ > 0)
        --release_remaining_;
      output[i] = fallback + art_sample + bridge_sample;
#ifndef ESP_PLATFORM
      if (measured_component) measured_component[i] = 0.0f;
      if (coasted_component) coasted_component[i] = 0.0f;
      if (measured_active) measured_active[i] = 0;
      if (coasted_active) coasted_active[i] = 0;
#endif
    }
#ifndef ESP_PLATFORM
    if (articulation_component)
      articulation_component[i] = art_sample;
    if (articulation_active)
      articulation_active[i] = (std::fabs(art_sample) > 1e-4f) ? 1 : 0;
    if (unvoiced_acoustic_class)
      unvoiced_acoustic_class[i] =
          static_cast<uint8_t>(unvoiced_articulation_.acoustic_class());
    if (plosive_bridge_component)
      plosive_bridge_component[i] = bridge_sample;
    if (plosive_bridge_active)
      plosive_bridge_active[i] = plosive_bridge_.is_active() ? 1 : 0;
    if (telem_dst) {
      SampleTelemetryRecord &rec = telem_dst[i];
      rec.sample_index = block_start + i;
      rec.input_rms = block_input_rms;
      rec.input_peak = input ? std::fabs(input[i]) : 0.0f;
      rec.input_envelope = 0.0f;
      rec.pitch_voiced = pitch.voiced ? 1 : 0;
      rec.pitch_confidence = pitch.confidence;
      rec.pitch_period_samples = pitch.period_samples;
      rec.pitch_f0_hz = pitch.frequency_hz;
      rec.pitch_onset = pitch.onset ? 1 : 0;
      rec.pitch_track_state = static_cast<uint8_t>(track);
      rec.coherent_marks = pitch.coherent_marks;
      rec.mark_count = static_cast<uint16_t>(mark_count);
      rec.target_enabled = target_enabled_ ? 1 : 0;
      rec.psola_usable = usable ? 1 : 0;
      rec.psola_usable_reason = usable_reason;
      rec.continuity_coasting = continuity_coasting ? 1 : 0;
      rec.new_grain_scheduled = (i == 0 && new_grain_scheduled_this_block_) ? 1 : 0;
      rec.active_grain_count = static_cast<uint16_t>(cur_ov);
      rec.last_grain_age = (block_start + i >= last_scheduled_mark_)
                               ? static_cast<uint32_t>(block_start + i - last_scheduled_mark_)
                               : 0;
      rec.next_synthesis_mark = next_synthesis_mark_;
      rec.ola_weight_sum = cur_norm;
      rec.ola_output_rms = std::fabs(y_new);
      rec.release_active = release_remaining_ > 0 ? 1 : 0;
      rec.release_remaining = release_remaining_;
      rec.unvoiced_path_active = unvoiced_articulation_.is_active() ? 1 : 0;
      rec.unvoiced_gain = unvoiced_config_.feed_gain;
      rec.plosive_path_active = plosive_bridge_.is_active() ? 1 : 0;
      rec.plosive_gain = plosive_config_.transient_gain;
      rec.psola_gain = psola_gain_;
      rec.active_mix = active_mix_;
      rec.final_harmony_rms = std::fabs(output[i]);
      rec.effective_total_gain = psola_gain_ * active_mix_;
      rec.pitch_voiced_raw = pitch.voiced_raw ? 1 : 0;
      rec.yin_min = pitch.yin_min;
      rec.spectral_centroid = pitch.spectral_centroid;
      rec.high_frequency_ratio = pitch.high_frequency_ratio;
      rec.zero_crossing_rate = pitch.zero_crossing_rate;
    }
#endif
    ola_[oi] = lpc_ola_[oi] = norm_[oi] = 0;
#ifndef ESP_PLATFORM
    measured_ola_[oi] = coasted_ola_[oi] = 0.0f;
    measured_norm_[oi] = coasted_norm_[oi] = 0.0f;
    norm_square_[oi] = 0.0f;
    overlap_[oi] = 0;
#endif
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Normalization),
                 0);

#ifndef ESP_PLATFORM
  debug_.ola_norm_mean = norm_sum / frames;
  debug_.ola_norm_min = norm_min < 1e8f ? norm_min : 0.0f;
  debug_.ola_norm_max = norm_max;
  debug_.ola_norm_square_mean = norm_square_sum / frames;
  debug_.ola_norm_square_min = norm_sq_min < 1e8f ? norm_sq_min : 0.0f;
  debug_.ola_norm_square_max = norm_sq_max;
  debug_.ola_overlap_mean = overlap_sum / frames;
  debug_.ola_overlap_min = ov_min < 9999 ? ov_min : 0;
  debug_.ola_overlap_max = ov_max;
  debug_.ola_norm_samples_below_threshold += norm_below_count;
  debug_.source_marks_1x = source_marks_1x_;
  debug_.source_marks_2x = source_marks_2x_;
  debug_.source_marks_3x = source_marks_3x_;
  debug_.source_marks_4x_or_more = source_marks_4x_or_more_;
  debug_.source_mark_reuses_total = source_mark_reuses_total_;
#endif

  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Unvoiced));
  // Handled inline during synthesis/normalization
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Unvoiced), 0);

  VF_PROFILE_BEGIN(profiler_, section(PitchShiftProfileSection::Crossfade));
  for (size_t i = 0; i < frames; ++i) {
    current_wet_ += alpha * (target_wet_ - current_wet_);
    active_mix_ = std::min(1.0f, active_mix_ + 1.0f / (sample_rate_ * .020f));
    psola_gain_ +=
        std::clamp(desired_gain - psola_gain_, -gain_step, gain_step);
    if (!onset_unvoiced_attenuation_enabled_) {
      psola_gain_ = 1.0f;
      active_mix_ = 1.0f;
    } else if (continuity_policy_ == PsolaContinuityPolicy::OnsetContinuity ||
               continuity_policy_ == PsolaContinuityPolicy::OnsetContinuityCoasting) {
      if (usable || release_remaining_ > 0 || block_has_psola_) {
        psola_gain_ = 1.0f;
        active_mix_ = 1.0f;
      }
    }
    if (onset_hold_)
      --onset_hold_;
    const bool history_ready = block_start + i >= history_offset_;
    const uint64_t source =
        history_ready ? block_start + i - history_offset_ : 0;
    const float delayed_lead = history_ready && history_available(source, source)
                                   ? history_at(source)
                                   : 0.0f;
    const PitchShiftFallbackReason reason =
        fallback_reason_for_sample(pitch, track, usable, block_has_psola_);
    float fallback = delayed_lead * fallback_policy_gain(reason);
    if (fallback_policy_ == HarmonyFallbackPolicy::HighpassUnvoiced &&
        reason == PitchShiftFallbackReason::Unvoiced) {
      fallback = fallback_hpf_output_ * unvoiced_fallback_gain_;
    }
    const float shifted = output[i];
    const float wet_signal =
        shifted * psola_gain_ + fallback * (1.0f - psola_gain_);
    const float dry_gain = 1.0f - current_wet_;
    const float effective_wet = current_wet_ * active_mix_;
    output[i] = input[i] * dry_gain + wet_signal * effective_wet;
#ifndef ESP_PLATFORM
    if (psola_component)
      psola_component[i] = shifted * psola_gain_ * effective_wet;
    if (fallback_component)
      fallback_component[i] = fallback * (1.0f - psola_gain_) * effective_wet;
    if (reference_delayed)
      reference_delayed[i] = delayed_lead;
    if (fallback_reason)
      fallback_reason[i] = static_cast<uint8_t>(reason);
    if (fallback_active)
      fallback_active[i] = (psola_gain_ < 0.99f || !usable) ? 1 : 0;
    if (measured_component)
      measured_component[i] *= psola_gain_ * effective_wet;
    if (coasted_component)
      coasted_component[i] *= psola_gain_ * effective_wet;
#endif
    if (psola_gain_ < .001f)
      ++telemetry_.fallback_frames;
  }
  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Crossfade), 0);

  output_position_ += frames;
  telemetry_.state = state_;
  debug_.voice_state = state_;
  debug_.pitch_tracker_state = static_cast<uint8_t>(track);
  debug_.recovery_class = active_recovery_class_;
  debug_.recovery_type = active_recovery_type_;
  debug_.recovery_period_error_cents = recovery_period_error_cents_;
  debug_.recovery_mark_error_fraction = recovery_mark_error_fraction_;
  debug_.recovery_mark_error_samples = recovery_mark_error_samples_;
  debug_.crossfade_active = (crossfade_remaining_ > 0);
  debug_.recovery_excess_discontinuity =
      (active_recovery_class_ == PsolaRecoveryClass::SmallError &&
       std::fabs(recovery_period_error_cents_) > 25.0f)
          ? (std::fabs(recovery_period_error_cents_) - 25.0f)
          : 0.0f;
  debug_.transition_energy_dip_db = 0.0f;
  debug_.pitch_voiced = pitch.voiced;
  debug_.pitch_confidence = pitch.confidence;
  debug_.pitch_onset = pitch.onset;
  debug_.pitch_changed = pitch.pitch_changed;
  debug_.psola_gain = psola_gain_;
  debug_.formant_mix = formant_mix_;
  debug_.active_mix = active_mix_;
  debug_.output_gain = active_mix_ * current_wet_;
  debug_.fallback_gain = fallback_policy_gain(fallback_reason_for_sample(pitch, track, usable, block_has_psola_));
  debug_.mark_valid = (mark_count >= 2);
  debug_.history_valid = history_available(block_start > history_offset_ ? block_start - history_offset_ : 0,
                                           block_end > history_offset_ ? block_end - history_offset_ : 0);
  debug_.usable = usable;
  debug_.onset_unvoiced_attenuation_enabled = onset_unvoiced_attenuation_enabled_;
  debug_.grains_total = telemetry_.grains;
  debug_.resyncs_total = telemetry_.psola_resyncs;
  debug_.fallback_samples_total = telemetry_.fallback_frames;
  debug_.articulation_gain = unvoiced_articulation_.transition_gain();
  debug_.unvoiced_eligible = unvoiced_articulation_.is_eligible() ? 1 : 0;
  debug_.unvoiced_acoustic_class =
      static_cast<uint8_t>(unvoiced_articulation_.acoustic_class());
  debug_.articulation_active = unvoiced_articulation_.is_active() ? 1 : 0;
  debug_.plosive_bridge_active = plosive_bridge_.is_active() ? 1 : 0;
  debug_.plosive_bridge_gain = plosive_bridge_.current_gain();
  debug_.plosive_score = plosive_bridge_.plosive_score();
  debug_.phrase_start_flag = plosive_bridge_.phrase_start_flag() ? 1 : 0;
  debug_.formant_shift_semitones = formant_shift_semitones_;
  debug_.formant_resets = static_cast<uint32_t>(formant_resets_);
  debug_.max_restored = max_restored_;
  debug_.max_pre_tanh = max_pre_tanh_;
  debug_.last_pre_tanh = last_pre_tanh_;
  debug_.last_post_tanh = last_post_tanh_;
  debug_.last_gain_scale = last_gain_scale_;
  debug_.lpc_energy = fast_lpc_energy_;
  debug_.psola_ref_energy = fast_psola_energy_;
  debug_.formant_gain_norm = formant_filter_gain_;
  debug_.softclip_events = softclip_events_;
  debug_.gain_rail_events = gain_rail_events_;
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
  store_atomic64(published_telemetry_.formant_resets, formant_resets_);
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
    result.formant_resets = load_atomic64(published_telemetry_.formant_resets);
    result.max_restored = max_restored_;
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
