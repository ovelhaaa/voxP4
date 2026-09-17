#include "td_psola.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#endif

extern void vocal_fx_funnel_inc_pitch_marks_consumed(uint64_t count);
extern void vocal_fx_funnel_inc_psola_process(uint64_t count);
extern void vocal_fx_funnel_inc_grain_schedule_attempts(uint64_t count);
extern void vocal_fx_funnel_inc_grains_scheduled(uint64_t count);
extern void vocal_fx_funnel_inc_grains_rendered(uint64_t count);

namespace {
constexpr float kPi = 3.14159265358979323846f;

// B4D.2 (§7) warp-cache audit counters. File scope so the engine object does
// not grow (link budget). Observability only.
uint64_t s_warp_miss_total = 0;
uint64_t s_warp_math_only_miss = 0;

// B4D.5 mark-selection decomposition (observability only, opt-in).  The
// nearest-mark search is split into the distance scan and the validity/period
// tests so the ~58 us/grain marginal can be attributed exactly.
bool s_b4d5_mark_audit_enabled = false;
uint64_t s_b4d5_mark_calls = 0;
uint64_t s_b4d5_mark_candidates = 0;
uint64_t s_b4d5_mark_scan_cycles = 0;
uint64_t s_b4d5_mark_total_cycles = 0;

// B4D.4 per-grain-count section attribution (voice 0, diagnostic only).
// buckets: 0 new grains, 1, 2, 3+; layout [bucket][PitchShiftProfileSection].
static constexpr size_t kB4D4Sections =
    static_cast<size_t>(PitchShiftProfileSection::Count);
static uint64_t *s_b4d4_class_cycles = nullptr;
static bool s_b4d4_class_enabled = false;
static_assert(static_cast<size_t>(ProfileSection::PitchShiftMarkSelection) +
                      static_cast<size_t>(PitchShiftProfileSection::Count) - 1 <
                  static_cast<size_t>(ProfileSection::Count),
              "pitch-shift profile sections must map before Count");
ProfileSection section(PitchShiftProfileSection s) {
  return static_cast<ProfileSection>(
      static_cast<size_t>(ProfileSection::PitchShiftMarkSelection) +
      static_cast<size_t>(s));
}

// B4D.5 exact nearest-mark search.  Reproduces
// `argmin_i |source - marks[i].sample_position|` with the same strict
// first-occurrence tie-break as the original double loop, but without the
// software-emulated double arithmetic (~330 cycles/mark on ESP32-P4) that
// dominated the ~58 us/grain MarkSelection marginal.
//
// Let base = floor(source), frac = source - base in [0,1).  For an integer
// mark p the exact distance is
//   p <= base : (base - p) + frac   (A + frac)
//   p >  base : (p - base) - frac   (B - frac)
// Comparing two such values is exact with integer arithmetic plus the three
// precomputed booleans (frac>0, frac<0.5, frac>0.5); no per-candidate double
// op is required.  A non-finite/negative/absurdly-large source falls back to
// the exact double scan so behavior is preserved for every input.
size_t nearest_mark_index(double source, const PitchMark *marks, size_t count) {
  if (!marks || count <= 1) return 0;
  size_t best = 0;
  if (std::isfinite(source) && source >= 0.0 && source < 1.0e18) {
    const double floor_d = std::floor(source);
    const uint64_t base = static_cast<uint64_t>(floor_d);
    const double frac = source - floor_d;
    const bool frac_gt_zero = frac > 0.0;
    const bool frac_lt_half = frac < 0.5;
    const bool frac_gt_half = frac > 0.5;
    for (size_t i = 1; i < count; ++i) {
      const uint64_t p = marks[i].sample_position;
      const uint64_t b = marks[best].sample_position;
      const bool p_below = p <= base;
      const bool b_below = b <= base;
      bool take;
      if (p_below && b_below) {
        take = (base - p) < (base - b);
      } else if (!p_below && !b_below) {
        take = (p - base) < (b - base);
      } else if (p_below && !b_below) {
        // d_p = A_p + frac, d_b = B_b - frac; d_p < d_b iff 2*frac < B_b - A_p.
        const uint64_t A_p = base - p;
        const uint64_t B_b = b - base; // >= 1
        if (B_b > A_p) {
          const uint64_t k = B_b - A_p;
          take = (k == 1) ? frac_lt_half : true; // k >= 2 always true
        } else {
          take = false; // k <= 0
        }
      } else {
        // d_p = B_p - frac, d_b = A_b + frac; d_p < d_b iff B_p - A_b < 2*frac.
        const uint64_t B_p = p - base; // >= 1
        const uint64_t A_b = base - b;
        if (B_p > A_b) {
          const uint64_t k = B_p - A_b;
          take = (k == 1) ? frac_gt_half : false; // k >= 2 always false
        } else if (B_p == A_b) {
          take = frac_gt_zero;
        } else {
          take = true; // k <= -1
        }
      }
      if (take) best = i;
    }
  } else {
    double distance = std::fabs(source - marks[0].sample_position);
    for (size_t i = 1; i < count; ++i) {
      const double d = std::fabs(source - marks[i].sample_position);
      if (d < distance) {
        distance = d;
        best = i;
      }
    }
  }
  return best;
}

// B4C.7: exact source-grain identity shared by the eager and deferred paths.
// Pure function of descriptor fields; moving it here changes no DSP behavior.
SourceGrainKey make_source_grain_key(uint64_t center, int half, bool use_lpc,
                                     bool model_valid, uint64_t timestamp,
                                     uint16_t order, float lambda,
                                     float gamma, uint8_t norm_strategy) {
  uint32_t lambda_bits = 0;
  uint32_t gamma_bits = 0;
  uint8_t norm_strat = 0;
  if (use_lpc) {
    std::memcpy(&lambda_bits, &lambda, sizeof(float));
    std::memcpy(&gamma_bits, &gamma, sizeof(float));
    norm_strat = norm_strategy;
  }
  SourceGrainKey key;
  key.mark_center = center;
  key.half_window = static_cast<uint32_t>(half);
  key.lpc_timestamp = (use_lpc && model_valid) ? timestamp : 0;
  key.lpc_order = static_cast<uint16_t>((use_lpc && model_valid) ? order : 0);
  key.lambda_bits = lambda_bits;
  key.gamma_bits = gamma_bits;
  key.norm_strategy = norm_strat;
  return key;
}
} // namespace

void SharedPitchShiftResources::set_slice_audit_enabled(bool enabled) {
  if (enabled && !slice_ring_) {
#ifdef ESP_PLATFORM
    slice_ring_ = static_cast<SliceAuditEntry *>(heap_caps_calloc(
        kSliceAuditCap, sizeof(SliceAuditEntry),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    slice_ring_ = static_cast<SliceAuditEntry *>(
        std::calloc(kSliceAuditCap, sizeof(SliceAuditEntry)));
#endif
    if (!slice_ring_) {
      slice_audit_enabled_ = false;
      return;
    }
  }
  if (!enabled && slice_ring_) {
#ifdef ESP_PLATFORM
    heap_caps_free(slice_ring_);
#else
    std::free(slice_ring_);
#endif
    slice_ring_ = nullptr;
  }
  slice_audit_enabled_ = enabled && (slice_ring_ != nullptr);
}

void SharedPitchShiftResources::record_slice_render(
    const SourceGrainKey &key, int n_min, int n_max, uint8_t voice,
    uint16_t samples, uint16_t taps) {
  if (!slice_audit_enabled_ || !slice_ring_) return;
  bool same = false, cross = false;
  for (size_t i = 0; i < slice_audit_count_; ++i) {
    const auto &ent = slice_ring_[i];
    if (ent.block_gen == slice_audit_gen_ && ent.key == key &&
        ent.n_min == n_min && ent.n_max == n_max) {
      if (ent.voice_id == voice) same = true;
      else cross = true;
    }
  }
  slice_audit_requested_++;
  slice_audit_samples_ += samples;
  slice_audit_taps_ += taps;
  if (same || cross) {
    slice_audit_duplicates_++;
    slice_audit_reusable_samples_ += samples;
    slice_audit_reusable_taps_ += taps;
    if (same) slice_audit_same_voice_++;
    if (cross) slice_audit_cross_voice_++;
  }
  if (slice_audit_count_ < kSliceAuditCap) {
    slice_ring_[slice_audit_count_] = {
        key, static_cast<int16_t>(n_min), static_cast<int16_t>(n_max),
        voice, samples, taps, slice_audit_gen_};
    slice_audit_count_++;
  } else {
    slice_audit_entry_drops_++;
  }
}

void SharedPitchShiftResources::init() {
  for (size_t i = 0; i < hann_.size(); ++i)
    hann_[i] = .5f - .5f * std::cos(2.0f * kPi * i / (hann_.size() - 1));
  reset();
}
void SharedPitchShiftResources::reset() {
  history_.fill(0);
  grain_scratch_.fill(0);
  shared_warp_cache.reset();
  input_end_ = 0;
}
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
const float* SharedPitchShiftResources::get_contiguous_history(
    uint64_t center, uint32_t half, size_t order) {
  if (center < static_cast<uint64_t>(half + order)) {
    return nullptr;
  }
  const uint64_t start = center - half - order;
  const size_t total_samples = 2 * half + 1 + order;
  if (total_samples > kMaxGrainScratch) {
    return nullptr;
  }
  const size_t start_idx = static_cast<size_t>(start & (kHistorySize - 1));
  if (start_idx + total_samples <= kHistorySize) {
    return history_.data() + start_idx + order;
  }
  const size_t first_part = kHistorySize - start_idx;
  const size_t second_part = total_samples - first_part;
  std::copy_n(history_.data() + start_idx, first_part, grain_scratch_.data());
  std::copy_n(history_.data(), second_part, grain_scratch_.data() + first_part);
  return grain_scratch_.data() + order;
}

TdPsola::~TdPsola() {
  if (residual_cache_pool_) {
#ifdef ESP_PLATFORM
    heap_caps_free(residual_cache_pool_);
#else
    std::free(residual_cache_pool_);
#endif
    residual_cache_pool_ = nullptr;
    residual_cache_pool_bytes_ = 0;
  }
  if (deferred_grains_) {
#ifdef ESP_PLATFORM
    heap_caps_free(deferred_grains_);
#else
    delete[] deferred_grains_;
#endif
    deferred_grains_ = nullptr;
  }
}

void TdPsola::configure_source_residual_cache(
    size_t entries, bool enable_residual, bool enable_windowed, bool force_psram) {
  entries = std::min(entries, kMaxResidualCacheCapacity);
  if (residual_cache_pool_) {
#ifdef ESP_PLATFORM
    heap_caps_free(residual_cache_pool_);
#else
    std::free(residual_cache_pool_);
#endif
    residual_cache_pool_ = nullptr;
    residual_cache_pool_bytes_ = 0;
    residual_cache_in_psram_ = false;
  }
  for (auto &e : residual_cache_entries_) {
    e = SourceResidualCacheEntry{};
  }
  residual_cache_generation_ = 0;
  residual_cache_stats_.reset();
  residual_cache_configured_size_ = entries;
  residual_cache_enabled_ = enable_residual;
  windowed_cache_enabled_ = enable_windowed;

  if (entries == 0 || (!enable_residual && !enable_windowed)) {
    return;
  }

  const size_t arrays_per_entry = enable_windowed ? 4 : 1;
  const size_t floats_per_entry = arrays_per_entry * kMaxGrainSamples;
  const size_t total_floats = floats_per_entry * entries;
  const size_t total_bytes = total_floats * sizeof(float);

#ifdef ESP_PLATFORM
  void *ptr = nullptr;
  if (!force_psram && entries <= 4 && (!enable_windowed || entries <= 1)) {
    ptr = heap_caps_malloc(total_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!ptr) {
    ptr = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    residual_cache_in_psram_ = true;
  } else {
    residual_cache_in_psram_ = false;
  }
  residual_cache_pool_ = static_cast<float *>(ptr);
#else
  (void)force_psram;
  residual_cache_pool_ = static_cast<float *>(std::malloc(total_bytes));
  residual_cache_in_psram_ = false;
#endif

  if (!residual_cache_pool_) {
    residual_cache_pool_bytes_ = 0;
    residual_cache_configured_size_ = 0;
    residual_cache_enabled_ = false;
    windowed_cache_enabled_ = false;
    return;
  }
  residual_cache_pool_bytes_ = total_bytes;

  float *cursor = residual_cache_pool_;
  for (size_t i = 0; i < entries; ++i) {
    residual_cache_entries_[i].residual = cursor;
    cursor += kMaxGrainSamples;
    if (enable_windowed) {
      residual_cache_entries_[i].windowed_residual = cursor;
      cursor += kMaxGrainSamples;
      residual_cache_entries_[i].windowed_plain = cursor;
      cursor += kMaxGrainSamples;
      residual_cache_entries_[i].window = cursor;
      cursor += kMaxGrainSamples;
    } else {
      residual_cache_entries_[i].windowed_residual = nullptr;
      residual_cache_entries_[i].windowed_plain = nullptr;
      residual_cache_entries_[i].window = nullptr;
    }
    residual_cache_entries_[i].valid = false;
    residual_cache_entries_[i].last_used_generation = 0;
  }
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
  lpc_kernel_ = config.lpc_kernel;
  ola_kernel_ = config.ola_kernel;
  grain_kernel_ = config.grain_kernel;
  synthesis_kernel_ = config.synthesis_kernel;
  configure_source_residual_cache(2, true, false, false);
  if (!deferred_grains_) {
#ifdef ESP_PLATFORM
    deferred_grains_ = static_cast<DeferredGrainDescriptor *>(
        heap_caps_calloc(kMaxDeferredGrains, sizeof(DeferredGrainDescriptor), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    deferred_grains_ = new DeferredGrainDescriptor[kMaxDeferredGrains]{};
#endif
  }
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
  warp_cache_.reset();
  warp_stats_ = PsolaWarpCacheStats{};
  for (size_t i = 0; i < residual_cache_configured_size_; ++i) {
    residual_cache_entries_[i].valid = false;
    residual_cache_entries_[i].last_used_generation = 0;
  }
  residual_cache_generation_ = 0;
  residual_cache_hits_this_block_ = 0;
  residual_cache_misses_this_block_ = 0;
  fir_samples_computed_this_block_ = 0;
  fir_samples_reused_this_block_ = 0;
  active_overlapping_grains_this_block_ = 0;
  precompute_queue_depth_this_block_ = 0;
  precompute_expired_this_block_ = 0;
  last_block_cycles_ = 0;
  model_warp_calls_this_block_ = 0;
  model_warp_cycles_this_block_ = 0;
  last_model_timestamp_ = 0;
  model_changed_this_block_ = 0;
  grains_scheduled_this_block_ = 0;
  grains_rendered_this_block_ = 0;
  grain_samples_processed_this_block_ = 0;
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
  if (deferred_grains_) {
    for (size_t i = 0; i < kMaxDeferredGrains; ++i) {
      deferred_grains_[i] = DeferredGrainDescriptor{};
    }
  }
  grain_sequence_counter_ = 0;
  deferred_stats_.reset();
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
  grain_rejection_ = {};
  grain_rejection_.history_size_samples = kHistorySize;
  grain_rejection_.history_size_ms = 1000.0f * kHistorySize / sample_rate_;
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
                          size_t &best, float &period,
                          GrainFailureReason *reason, double *out_distance,
                          double *out_allowed_distance) const {
  if (reason)
    *reason = GrainFailureReason::None;
  if (!marks || count == 0) {
    if (reason)
      *reason = GrainFailureReason::SelectMarkNoMarks;
    return false;
  }
  best = 0;
  double distance;
  const uint32_t b4d5_scan_start =
      s_b4d5_mark_audit_enabled ? Profiler::now_cycles() : 0;
  best = nearest_mark_index(source, marks, count);
  distance = std::fabs(source - marks[best].sample_position);
  if (s_b4d5_mark_audit_enabled) {
    s_b4d5_mark_scan_cycles += Profiler::now_cycles() - b4d5_scan_start;
    ++s_b4d5_mark_calls;
    s_b4d5_mark_candidates += count;
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
  const double allowed = std::max(2.0 * period, 256.0);
  if (out_distance)
    *out_distance = distance;
  if (out_allowed_distance)
    *out_allowed_distance = allowed;
  if (!(marks[best].confidence > .15f)) {
    if (reason)
      *reason = GrainFailureReason::SelectMarkLowConfidence;
    return false;
  }
  if (!(period >= 24 && period <= 800)) {
    if (reason)
      *reason = GrainFailureReason::SelectMarkInvalidPeriod;
    return false;
  }
  if (distance > allowed) {
    if (reason)
      *reason = GrainFailureReason::SelectMarkDistanceTooLarge;
    return false;
  }
  return true;
}

void TdPsola::record_grain_failure(GrainFailureReason reason,
                                   double destination, double source,
                                   uint64_t center, uint32_t half,
                                   double distance, double allowed_distance) {
  switch (reason) {
  case GrainFailureReason::AttemptSourceNegative:
    ++grain_rejection_.attempt_source_negative;
    break;
  case GrainFailureReason::SelectMarkNoMarks:
    ++grain_rejection_.select_mark_failure_total;
    ++grain_rejection_.select_mark_no_marks;
    break;
  case GrainFailureReason::SelectMarkLowConfidence:
    ++grain_rejection_.select_mark_failure_total;
    ++grain_rejection_.select_mark_low_confidence;
    break;
  case GrainFailureReason::SelectMarkInvalidPeriod:
    ++grain_rejection_.select_mark_failure_total;
    ++grain_rejection_.select_mark_invalid_period;
    break;
  case GrainFailureReason::SelectMarkDistanceTooLarge:
    ++grain_rejection_.select_mark_failure_total;
    ++grain_rejection_.select_mark_distance_too_large;
    break;
  case GrainFailureReason::HistoryCenterBeforeHalf:
    ++grain_rejection_.history_failure_total;
    ++grain_rejection_.history_center_before_half;
    break;
  case GrainFailureReason::HistoryTooOld:
    ++grain_rejection_.history_failure_total;
    ++grain_rejection_.history_too_old;
    break;
  case GrainFailureReason::HistoryFutureEnd:
    ++grain_rejection_.history_failure_total;
    ++grain_rejection_.history_future_end;
    break;
  default:
    break;
  }
  const uint64_t input_end = resources_ ? resources_->input_end() : 0;
  const uint64_t oldest = input_end > kHistorySize ? input_end - kHistorySize : 0;
  grain_rejection_.input_end = input_end;
  grain_rejection_.oldest_available = oldest;
  if (grain_rejection_.diagnostic_count >=
      GrainRejectionTelemetry::kDiagnosticCapacity)
    return;
  auto &record =
      grain_rejection_.diagnostics[grain_rejection_.diagnostic_count++];
  record.input_end = input_end;
  record.history_oldest_sample = oldest;
  record.pitch_analysis_timestamp = debug_.pitch_timestamp;
  record.pitch_age_samples = debug_.analysis_age;
  record.requested_source = source;
  record.destination = destination;
  record.selected_mark_center = center;
  record.mark_age_samples = input_end > center ? input_end - center : 0;
  record.half_window = half;
  record.required_first_sample = center >= half ? center - half : 0;
  record.required_last_sample = center + half;
  record.source_to_nearest_mark = static_cast<float>(distance);
  record.signed_delta_samples = static_cast<float>(center - source);
  record.pitch_period = current_synthesis_period_;
  record.delta_in_periods = current_synthesis_period_ > 0.0f
                                ? record.signed_delta_samples /
                                      current_synthesis_period_
                                : 0.0f;
  record.allowed_distance = static_cast<float>(allowed_distance);
  record.reason = reason;
}

void TdPsola::record_grain_alignment(double source, uint64_t center,
                                     float period, bool distance_failure) {
  if (!(period > 0.0f) || !std::isfinite(source))
    return;
  const double delta_periods =
      (static_cast<double>(center) - source) / static_cast<double>(period);
  size_t delta_bin = 0;
  if (delta_periods < -3.0) delta_bin = 0;
  else if (delta_periods < -2.0) delta_bin = 1;
  else if (delta_periods < -1.0) delta_bin = 2;
  else if (delta_periods < 0.0) delta_bin = 3;
  else if (delta_periods < 1.0) delta_bin = 4;
  else if (delta_periods < 2.0) delta_bin = 5;
  else if (delta_periods <= 3.0) delta_bin = 6;
  else delta_bin = 7;
  ++grain_rejection_.signed_delta_period_histogram[delta_bin];
  ++grain_rejection_.alignment_observations;

  const double age_ms = 1000.0 * static_cast<double>(debug_.analysis_age) /
                        static_cast<double>(sample_rate_);
  size_t age_bin = age_ms < 20.0 ? 0 : age_ms < 40.0 ? 1
                       : age_ms < 60.0 ? 2 : age_ms < 100.0 ? 3
                       : age_ms < 150.0 ? 4 : age_ms < 250.0 ? 5 : 6;
  ++grain_rejection_.pitch_age_attempt_histogram[age_bin];
  if (distance_failure)
    ++grain_rejection_.pitch_age_distance_failure_histogram[age_bin];
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

void TdPsola::add_grain_reference(int n_min, int n_max, int64_t dst, int half,
                                  uint64_t center, const SharedLpcModel &model,
                                  bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  for (int n = n_min; n <= n_max; ++n) {
    const int64_t absolute = dst + n;
    const size_t wi = static_cast<size_t>(
        (static_cast<int64_t>(n + half) * (kHannSize - 1)) / (2 * half));
    const float w = resources_->window(wi);
    const size_t oi = static_cast<uint64_t>(absolute) & (kOlaSize - 1);
    const uint64_t source_sample = static_cast<uint64_t>(static_cast<int64_t>(center) + n);
    const float plain = history_at(source_sample);
    float sample = plain;
    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        if (source_sample >= j) sample += model.coefficients[j] * history_at(source_sample - j);
      }
    }
    ola_[oi] += plain * w;
    lpc_ola_[oi] += sample * w;
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
}

void TdPsola::add_grain_contiguous_exact(int n_min, int n_max, int64_t dst, int half,
                                         const float *src, const SharedLpcModel &model,
                                         bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  for (int n = n_min; n <= n_max; ++n) {
    const int m = n + half;
    const int64_t absolute = dst + n;
    const size_t wi = static_cast<size_t>(
        (static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const float w = resources_->window(wi);
    const size_t oi = static_cast<uint64_t>(absolute) & (kOlaSize - 1);
    const float plain = src[m];
    float sample = plain;
    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        sample += model.coefficients[j] * src[m - j];
      }
    }
    ola_[oi] += plain * w;
    lpc_ola_[oi] += sample * w;
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
}

void TdPsola::add_grain_contiguous_multi4(int n_min, int n_max, int64_t dst, int half,
                                         const float *src, const SharedLpcModel &model,
                                         bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  int n = n_min;
  const int count = n_max - n_min + 1;
  const int count4 = count & ~3;
  const int n_end4 = n_min + count4;

  for (; n < n_end4; n += 4) {
    const int m = n + half;
    const float plain0 = src[m];
    const float plain1 = src[m + 1];
    const float plain2 = src[m + 2];
    const float plain3 = src[m + 3];

    float sample0 = plain0;
    float sample1 = plain1;
    float sample2 = plain2;
    float sample3 = plain3;

    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        const float c = model.coefficients[j];
        sample0 += c * src[m - j];
        sample1 += c * src[m + 1 - j];
        sample2 += c * src[m + 2 - j];
        sample3 += c * src[m + 3 - j];
      }
    }

    const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m + 1) * (kHannSize - 1)) / denom);
    const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m + 2) * (kHannSize - 1)) / denom);
    const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m + 3) * (kHannSize - 1)) / denom);

    const float w0 = resources_->window(wi0);
    const float w1 = resources_->window(wi1);
    const float w2 = resources_->window(wi2);
    const float w3 = resources_->window(wi3);

    const size_t oi0 = static_cast<uint64_t>(dst + n) & (kOlaSize - 1);
    const size_t oi1 = static_cast<uint64_t>(dst + n + 1) & (kOlaSize - 1);
    const size_t oi2 = static_cast<uint64_t>(dst + n + 2) & (kOlaSize - 1);
    const size_t oi3 = static_cast<uint64_t>(dst + n + 3) & (kOlaSize - 1);

    ola_[oi0] += plain0 * w0;
    lpc_ola_[oi0] += sample0 * w0;
    norm_[oi0] += w0;

    ola_[oi1] += plain1 * w1;
    lpc_ola_[oi1] += sample1 * w1;
    norm_[oi1] += w1;

    ola_[oi2] += plain2 * w2;
    lpc_ola_[oi2] += sample2 * w2;
    norm_[oi2] += w2;

    ola_[oi3] += plain3 * w3;
    lpc_ola_[oi3] += sample3 * w3;
    norm_[oi3] += w3;

#ifndef ESP_PLATFORM
    norm_square_[oi0] += w0 * w0; overlap_[oi0] += 1;
    norm_square_[oi1] += w1 * w1; overlap_[oi1] += 1;
    norm_square_[oi2] += w2 * w2; overlap_[oi2] += 1;
    norm_square_[oi3] += w3 * w3; overlap_[oi3] += 1;
    if (mark_predicted) {
      coasted_ola_[oi0] += plain0 * w0; coasted_norm_[oi0] += w0;
      coasted_ola_[oi1] += plain1 * w1; coasted_norm_[oi1] += w1;
      coasted_ola_[oi2] += plain2 * w2; coasted_norm_[oi2] += w2;
      coasted_ola_[oi3] += plain3 * w3; coasted_norm_[oi3] += w3;
    } else {
      measured_ola_[oi0] += plain0 * w0; measured_norm_[oi0] += w0;
      measured_ola_[oi1] += plain1 * w1; measured_norm_[oi1] += w1;
      measured_ola_[oi2] += plain2 * w2; measured_norm_[oi2] += w2;
      measured_ola_[oi3] += plain3 * w3; measured_norm_[oi3] += w3;
    }
#endif
  }

  for (; n <= n_max; ++n) {
    const int m = n + half;
    const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const float w = resources_->window(wi);
    const size_t oi = static_cast<uint64_t>(dst + n) & (kOlaSize - 1);
    const float plain = src[m];
    float sample = plain;
    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        sample += model.coefficients[j] * src[m - j];
      }
    }
    ola_[oi] += plain * w;
    lpc_ola_[oi] += sample * w;
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
}

void TdPsola::add_grain_contiguous_multi8(int n_min, int n_max, int64_t dst, int half,
                                         const float *src, const SharedLpcModel &model,
                                         bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  int n = n_min;
  const int count = n_max - n_min + 1;
  const int count8 = count & ~7;
  const int n_end8 = n_min + count8;

  for (; n < n_end8; n += 8) {
    const int m = n + half;
    const float plain0 = src[m];
    const float plain1 = src[m + 1];
    const float plain2 = src[m + 2];
    const float plain3 = src[m + 3];
    const float plain4 = src[m + 4];
    const float plain5 = src[m + 5];
    const float plain6 = src[m + 6];
    const float plain7 = src[m + 7];

    float sample0 = plain0;
    float sample1 = plain1;
    float sample2 = plain2;
    float sample3 = plain3;
    float sample4 = plain4;
    float sample5 = plain5;
    float sample6 = plain6;
    float sample7 = plain7;

    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        const float c = model.coefficients[j];
        sample0 += c * src[m - j];
        sample1 += c * src[m + 1 - j];
        sample2 += c * src[m + 2 - j];
        sample3 += c * src[m + 3 - j];
        sample4 += c * src[m + 4 - j];
        sample5 += c * src[m + 5 - j];
        sample6 += c * src[m + 6 - j];
        sample7 += c * src[m + 7 - j];
      }
    }

    const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m + 1) * (kHannSize - 1)) / denom);
    const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m + 2) * (kHannSize - 1)) / denom);
    const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m + 3) * (kHannSize - 1)) / denom);
    const size_t wi4 = static_cast<size_t>((static_cast<int64_t>(m + 4) * (kHannSize - 1)) / denom);
    const size_t wi5 = static_cast<size_t>((static_cast<int64_t>(m + 5) * (kHannSize - 1)) / denom);
    const size_t wi6 = static_cast<size_t>((static_cast<int64_t>(m + 6) * (kHannSize - 1)) / denom);
    const size_t wi7 = static_cast<size_t>((static_cast<int64_t>(m + 7) * (kHannSize - 1)) / denom);

    const float w0 = resources_->window(wi0);
    const float w1 = resources_->window(wi1);
    const float w2 = resources_->window(wi2);
    const float w3 = resources_->window(wi3);
    const float w4 = resources_->window(wi4);
    const float w5 = resources_->window(wi5);
    const float w6 = resources_->window(wi6);
    const float w7 = resources_->window(wi7);

    const size_t oi0 = static_cast<uint64_t>(dst + n) & (kOlaSize - 1);
    const size_t oi1 = static_cast<uint64_t>(dst + n + 1) & (kOlaSize - 1);
    const size_t oi2 = static_cast<uint64_t>(dst + n + 2) & (kOlaSize - 1);
    const size_t oi3 = static_cast<uint64_t>(dst + n + 3) & (kOlaSize - 1);
    const size_t oi4 = static_cast<uint64_t>(dst + n + 4) & (kOlaSize - 1);
    const size_t oi5 = static_cast<uint64_t>(dst + n + 5) & (kOlaSize - 1);
    const size_t oi6 = static_cast<uint64_t>(dst + n + 6) & (kOlaSize - 1);
    const size_t oi7 = static_cast<uint64_t>(dst + n + 7) & (kOlaSize - 1);

    ola_[oi0] += plain0 * w0; lpc_ola_[oi0] += sample0 * w0; norm_[oi0] += w0;
    ola_[oi1] += plain1 * w1; lpc_ola_[oi1] += sample1 * w1; norm_[oi1] += w1;
    ola_[oi2] += plain2 * w2; lpc_ola_[oi2] += sample2 * w2; norm_[oi2] += w2;
    ola_[oi3] += plain3 * w3; lpc_ola_[oi3] += sample3 * w3; norm_[oi3] += w3;
    ola_[oi4] += plain4 * w4; lpc_ola_[oi4] += sample4 * w4; norm_[oi4] += w4;
    ola_[oi5] += plain5 * w5; lpc_ola_[oi5] += sample5 * w5; norm_[oi5] += w5;
    ola_[oi6] += plain6 * w6; lpc_ola_[oi6] += sample6 * w6; norm_[oi6] += w6;
    ola_[oi7] += plain7 * w7; lpc_ola_[oi7] += sample7 * w7; norm_[oi7] += w7;

#ifndef ESP_PLATFORM
    norm_square_[oi0] += w0 * w0; overlap_[oi0] += 1;
    norm_square_[oi1] += w1 * w1; overlap_[oi1] += 1;
    norm_square_[oi2] += w2 * w2; overlap_[oi2] += 1;
    norm_square_[oi3] += w3 * w3; overlap_[oi3] += 1;
    norm_square_[oi4] += w4 * w4; overlap_[oi4] += 1;
    norm_square_[oi5] += w5 * w5; overlap_[oi5] += 1;
    norm_square_[oi6] += w6 * w6; overlap_[oi6] += 1;
    norm_square_[oi7] += w7 * w7; overlap_[oi7] += 1;
    if (mark_predicted) {
      coasted_ola_[oi0] += plain0 * w0; coasted_norm_[oi0] += w0;
      coasted_ola_[oi1] += plain1 * w1; coasted_norm_[oi1] += w1;
      coasted_ola_[oi2] += plain2 * w2; coasted_norm_[oi2] += w2;
      coasted_ola_[oi3] += plain3 * w3; coasted_norm_[oi3] += w3;
      coasted_ola_[oi4] += plain4 * w4; coasted_norm_[oi4] += w4;
      coasted_ola_[oi5] += plain5 * w5; coasted_norm_[oi5] += w5;
      coasted_ola_[oi6] += plain6 * w6; coasted_norm_[oi6] += w6;
      coasted_ola_[oi7] += plain7 * w7; coasted_norm_[oi7] += w7;
    } else {
      measured_ola_[oi0] += plain0 * w0; measured_norm_[oi0] += w0;
      measured_ola_[oi1] += plain1 * w1; measured_norm_[oi1] += w1;
      measured_ola_[oi2] += plain2 * w2; measured_norm_[oi2] += w2;
      measured_ola_[oi3] += plain3 * w3; measured_norm_[oi3] += w3;
      measured_ola_[oi4] += plain4 * w4; measured_norm_[oi4] += w4;
      measured_ola_[oi5] += plain5 * w5; measured_norm_[oi5] += w5;
      measured_ola_[oi6] += plain6 * w6; measured_norm_[oi6] += w6;
      measured_ola_[oi7] += plain7 * w7; measured_norm_[oi7] += w7;
    }
#endif
  }

  for (; n <= n_max; ++n) {
    const int m = n + half;
    const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const float w = resources_->window(wi);
    const size_t oi = static_cast<uint64_t>(dst + n) & (kOlaSize - 1);
    const float plain = src[m];
    float sample = plain;
    if (use_lpc) {
      for (size_t j = 1; j <= model.order; ++j) {
        sample += model.coefficients[j] * src[m - j];
      }
    }
    ola_[oi] += plain * w;
    lpc_ola_[oi] += sample * w;
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
}

void TdPsola::add_grain_ola_contiguous(int n_min, int n_max, int64_t dst, int half,
                                       const float *src, const SharedLpcModel &model,
                                       bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  const int count = n_max - n_min + 1;
  const size_t start_oi = static_cast<uint64_t>(dst + n_min) & (kOlaSize - 1);

  auto process_segment = [&](int n_start, int seg_len, size_t oi_base) {
    float *dst_ola = &ola_[oi_base];
    float *dst_lpc = &lpc_ola_[oi_base];
    float *dst_norm = &norm_[oi_base];
#ifndef ESP_PLATFORM
    float *dst_norm_sq = &norm_square_[oi_base];
    uint16_t *dst_ov = &overlap_[oi_base];
    float *dst_c_ola = mark_predicted ? &coasted_ola_[oi_base] : &measured_ola_[oi_base];
    float *dst_c_norm = mark_predicted ? &coasted_norm_[oi_base] : &measured_norm_[oi_base];
#endif
    for (int k = 0; k < seg_len; ++k) {
      const int n = n_start + k;
      const int m = n + half;
      const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const float w = resources_->window(wi);
      const float plain = src[m];
      float sample = plain;
      if (use_lpc) {
        for (size_t j = 1; j <= model.order; ++j) {
          sample += model.coefficients[j] * src[m - j];
        }
      }
      dst_ola[k] += plain * w;
      dst_lpc[k] += sample * w;
      dst_norm[k] += w;
#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w * w;
      dst_ov[k] += 1;
      dst_c_ola[k] += plain * w;
      dst_c_norm[k] += w;
#endif
    }
  };

  if (start_oi + count <= kOlaSize) {
    process_segment(n_min, count, start_oi);
  } else {
    const int seg1_len = static_cast<int>(kOlaSize - start_oi);
    const int seg2_len = count - seg1_len;
    process_segment(n_min, seg1_len, start_oi);
    process_segment(n_min + seg1_len, seg2_len, 0);
  }
}

void TdPsola::add_grain_combined_multi4(int n_min, int n_max, int64_t dst, int half,
                                        const float *src, const SharedLpcModel &model,
                                        bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  const int count = n_max - n_min + 1;
  const size_t start_oi = static_cast<uint64_t>(dst + n_min) & (kOlaSize - 1);

  auto process_combined_segment4 = [&](int n_start, int seg_len, size_t oi_base) {
    float *dst_ola = &ola_[oi_base];
    float *dst_lpc = &lpc_ola_[oi_base];
    float *dst_norm = &norm_[oi_base];
#ifndef ESP_PLATFORM
    float *dst_norm_sq = &norm_square_[oi_base];
    uint16_t *dst_ov = &overlap_[oi_base];
    float *dst_c_ola = mark_predicted ? &coasted_ola_[oi_base] : &measured_ola_[oi_base];
    float *dst_c_norm = mark_predicted ? &coasted_norm_[oi_base] : &measured_norm_[oi_base];
#endif
    int k = 0;
    const int k_end4 = seg_len & ~3;
    for (; k < k_end4; k += 4) {
      const int n = n_start + k;
      const int m = n + half;
      const float plain0 = src[m];
      const float plain1 = src[m + 1];
      const float plain2 = src[m + 2];
      const float plain3 = src[m + 3];

      float sample0 = plain0;
      float sample1 = plain1;
      float sample2 = plain2;
      float sample3 = plain3;

      if (use_lpc) {
        for (size_t j = 1; j <= model.order; ++j) {
          const float c = model.coefficients[j];
          sample0 += c * src[m - j];
          sample1 += c * src[m + 1 - j];
          sample2 += c * src[m + 2 - j];
          sample3 += c * src[m + 3 - j];
        }
      }

      const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m + 1) * (kHannSize - 1)) / denom);
      const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m + 2) * (kHannSize - 1)) / denom);
      const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m + 3) * (kHannSize - 1)) / denom);

      const float w0 = resources_->window(wi0);
      const float w1 = resources_->window(wi1);
      const float w2 = resources_->window(wi2);
      const float w3 = resources_->window(wi3);

      dst_ola[k] += plain0 * w0;
      dst_lpc[k] += sample0 * w0;
      dst_norm[k] += w0;

      dst_ola[k + 1] += plain1 * w1;
      dst_lpc[k + 1] += sample1 * w1;
      dst_norm[k + 1] += w1;

      dst_ola[k + 2] += plain2 * w2;
      dst_lpc[k + 2] += sample2 * w2;
      dst_norm[k + 2] += w2;

      dst_ola[k + 3] += plain3 * w3;
      dst_lpc[k + 3] += sample3 * w3;
      dst_norm[k + 3] += w3;

#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w0 * w0; dst_ov[k] += 1;
      dst_norm_sq[k + 1] += w1 * w1; dst_ov[k + 1] += 1;
      dst_norm_sq[k + 2] += w2 * w2; dst_ov[k + 2] += 1;
      dst_norm_sq[k + 3] += w3 * w3; dst_ov[k + 3] += 1;
      dst_c_ola[k] += plain0 * w0; dst_c_norm[k] += w0;
      dst_c_ola[k + 1] += plain1 * w1; dst_c_norm[k + 1] += w1;
      dst_c_ola[k + 2] += plain2 * w2; dst_c_norm[k + 2] += w2;
      dst_c_ola[k + 3] += plain3 * w3; dst_c_norm[k + 3] += w3;
#endif
    }

    for (; k < seg_len; ++k) {
      const int n = n_start + k;
      const int m = n + half;
      const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const float w = resources_->window(wi);
      const float plain = src[m];
      float sample = plain;
      if (use_lpc) {
        for (size_t j = 1; j <= model.order; ++j) {
          sample += model.coefficients[j] * src[m - j];
        }
      }
      dst_ola[k] += plain * w;
      dst_lpc[k] += sample * w;
      dst_norm[k] += w;
#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w * w;
      dst_ov[k] += 1;
      dst_c_ola[k] += plain * w;
      dst_c_norm[k] += w;
#endif
    }
  };

  if (start_oi + count <= kOlaSize) {
    process_combined_segment4(n_min, count, start_oi);
  } else {
    const int seg1_len = static_cast<int>(kOlaSize - start_oi);
    const int seg2_len = count - seg1_len;
    process_combined_segment4(n_min, seg1_len, start_oi);
    process_combined_segment4(n_min + seg1_len, seg2_len, 0);
  }
}

void TdPsola::add_grain_combined_multi8(int n_min, int n_max, int64_t dst, int half,
                                        const float *src, const SharedLpcModel &model,
                                        bool use_lpc, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  const int count = n_max - n_min + 1;
  const size_t start_oi = static_cast<uint64_t>(dst + n_min) & (kOlaSize - 1);

  auto process_combined_segment8 = [&](int n_start, int seg_len, size_t oi_base) {
    float *dst_ola = &ola_[oi_base];
    float *dst_lpc = &lpc_ola_[oi_base];
    float *dst_norm = &norm_[oi_base];
#ifndef ESP_PLATFORM
    float *dst_norm_sq = &norm_square_[oi_base];
    uint16_t *dst_ov = &overlap_[oi_base];
    float *dst_c_ola = mark_predicted ? &coasted_ola_[oi_base] : &measured_ola_[oi_base];
    float *dst_c_norm = mark_predicted ? &coasted_norm_[oi_base] : &measured_norm_[oi_base];
#endif
    int k = 0;
    const int k_end8 = seg_len & ~7;
    for (; k < k_end8; k += 8) {
      const int n = n_start + k;
      const int m = n + half;
      const float plain0 = src[m];
      const float plain1 = src[m + 1];
      const float plain2 = src[m + 2];
      const float plain3 = src[m + 3];
      const float plain4 = src[m + 4];
      const float plain5 = src[m + 5];
      const float plain6 = src[m + 6];
      const float plain7 = src[m + 7];

      float sample0 = plain0;
      float sample1 = plain1;
      float sample2 = plain2;
      float sample3 = plain3;
      float sample4 = plain4;
      float sample5 = plain5;
      float sample6 = plain6;
      float sample7 = plain7;

      if (use_lpc) {
        for (size_t j = 1; j <= model.order; ++j) {
          const float c = model.coefficients[j];
          sample0 += c * src[m - j];
          sample1 += c * src[m + 1 - j];
          sample2 += c * src[m + 2 - j];
          sample3 += c * src[m + 3 - j];
          sample4 += c * src[m + 4 - j];
          sample5 += c * src[m + 5 - j];
          sample6 += c * src[m + 6 - j];
          sample7 += c * src[m + 7 - j];
        }
      }

      const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m + 1) * (kHannSize - 1)) / denom);
      const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m + 2) * (kHannSize - 1)) / denom);
      const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m + 3) * (kHannSize - 1)) / denom);
      const size_t wi4 = static_cast<size_t>((static_cast<int64_t>(m + 4) * (kHannSize - 1)) / denom);
      const size_t wi5 = static_cast<size_t>((static_cast<int64_t>(m + 5) * (kHannSize - 1)) / denom);
      const size_t wi6 = static_cast<size_t>((static_cast<int64_t>(m + 6) * (kHannSize - 1)) / denom);
      const size_t wi7 = static_cast<size_t>((static_cast<int64_t>(m + 7) * (kHannSize - 1)) / denom);

      const float w0 = resources_->window(wi0);
      const float w1 = resources_->window(wi1);
      const float w2 = resources_->window(wi2);
      const float w3 = resources_->window(wi3);
      const float w4 = resources_->window(wi4);
      const float w5 = resources_->window(wi5);
      const float w6 = resources_->window(wi6);
      const float w7 = resources_->window(wi7);

      dst_ola[k] += plain0 * w0;
      dst_lpc[k] += sample0 * w0;
      dst_norm[k] += w0;

      dst_ola[k + 1] += plain1 * w1;
      dst_lpc[k + 1] += sample1 * w1;
      dst_norm[k + 1] += w1;

      dst_ola[k + 2] += plain2 * w2;
      dst_lpc[k + 2] += sample2 * w2;
      dst_norm[k + 2] += w2;

      dst_ola[k + 3] += plain3 * w3;
      dst_lpc[k + 3] += sample3 * w3;
      dst_norm[k + 3] += w3;

      dst_ola[k + 4] += plain4 * w4;
      dst_lpc[k + 4] += sample4 * w4;
      dst_norm[k + 4] += w4;

      dst_ola[k + 5] += plain5 * w5;
      dst_lpc[k + 5] += sample5 * w5;
      dst_norm[k + 5] += w5;

      dst_ola[k + 6] += plain6 * w6;
      dst_lpc[k + 6] += sample6 * w6;
      dst_norm[k + 6] += w6;

      dst_ola[k + 7] += plain7 * w7;
      dst_lpc[k + 7] += sample7 * w7;
      dst_norm[k + 7] += w7;

#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w0 * w0; dst_ov[k] += 1;
      dst_norm_sq[k + 1] += w1 * w1; dst_ov[k + 1] += 1;
      dst_norm_sq[k + 2] += w2 * w2; dst_ov[k + 2] += 1;
      dst_norm_sq[k + 3] += w3 * w3; dst_ov[k + 3] += 1;
      dst_norm_sq[k + 4] += w4 * w4; dst_ov[k + 4] += 1;
      dst_norm_sq[k + 5] += w5 * w5; dst_ov[k + 5] += 1;
      dst_norm_sq[k + 6] += w6 * w6; dst_ov[k + 6] += 1;
      dst_norm_sq[k + 7] += w7 * w7; dst_ov[k + 7] += 1;
      dst_c_ola[k] += plain0 * w0; dst_c_norm[k] += w0;
      dst_c_ola[k + 1] += plain1 * w1; dst_c_norm[k + 1] += w1;
      dst_c_ola[k + 2] += plain2 * w2; dst_c_norm[k + 2] += w2;
      dst_c_ola[k + 3] += plain3 * w3; dst_c_norm[k + 3] += w3;
      dst_c_ola[k + 4] += plain4 * w4; dst_c_norm[k + 4] += w4;
      dst_c_ola[k + 5] += plain5 * w5; dst_c_norm[k + 5] += w5;
      dst_c_ola[k + 6] += plain6 * w6; dst_c_norm[k + 6] += w6;
      dst_c_ola[k + 7] += plain7 * w7; dst_c_norm[k + 7] += w7;
#endif
    }

    for (; k < seg_len; ++k) {
      const int n = n_start + k;
      const int m = n + half;
      const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const float w = resources_->window(wi);
      const float plain = src[m];
      float sample = plain;
      if (use_lpc) {
        for (size_t j = 1; j <= model.order; ++j) {
          sample += model.coefficients[j] * src[m - j];
        }
      }
      dst_ola[k] += plain * w;
      dst_lpc[k] += sample * w;
      dst_norm[k] += w;
#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w * w;
      dst_ov[k] += 1;
      dst_c_ola[k] += plain * w;
      dst_c_norm[k] += w;
#endif
    }
  };

  if (start_oi + count <= kOlaSize) {
    process_combined_segment8(n_min, count, start_oi);
  } else {
    const int seg1_len = static_cast<int>(kOlaSize - start_oi);
    const int seg2_len = count - seg1_len;
    process_combined_segment8(n_min, seg1_len, start_oi);
    process_combined_segment8(n_min + seg1_len, seg2_len, 0);
  }
}

void TdPsola::compute_lpc_residual_fir_contiguous_scalar(
    const float *src, size_t count, const SharedLpcModel &model, float *residual_out) {
  const size_t order = model.order;
  for (size_t m = 0; m < count; ++m) {
    float s = src[m];
    for (size_t j = 1; j <= order; ++j) {
      s += model.coefficients[j] * src[m - j];
    }
    residual_out[m] = s;
  }
}

void TdPsola::compute_lpc_residual_fir_multi2(
    const float *src, size_t count, const SharedLpcModel &model, float *residual_out) {
  const size_t order = model.order;
  const size_t count2 = count & ~1;
  size_t m = 0;
  for (; m < count2; m += 2) {
    float s0 = src[m];
    float s1 = src[m + 1];
    for (size_t j = 1; j <= order; ++j) {
      const float c = model.coefficients[j];
      s0 += c * src[m - j];
      s1 += c * src[m + 1 - j];
    }
    residual_out[m] = s0;
    residual_out[m + 1] = s1;
  }
  for (; m < count; ++m) {
    float s = src[m];
    for (size_t j = 1; j <= order; ++j) {
      s += model.coefficients[j] * src[m - j];
    }
    residual_out[m] = s;
  }
}

void TdPsola::compute_lpc_residual_fir_multi4(
    const float *src, size_t count, const SharedLpcModel &model, float *residual_out) {
  const size_t order = model.order;
  const size_t count4 = count & ~3;
  size_t m = 0;
  for (; m < count4; m += 4) {
    float s0 = src[m];
    float s1 = src[m + 1];
    float s2 = src[m + 2];
    float s3 = src[m + 3];
    for (size_t j = 1; j <= order; ++j) {
      const float c = model.coefficients[j];
      s0 += c * src[m - j];
      s1 += c * src[m + 1 - j];
      s2 += c * src[m + 2 - j];
      s3 += c * src[m + 3 - j];
    }
    residual_out[m] = s0;
    residual_out[m + 1] = s1;
    residual_out[m + 2] = s2;
    residual_out[m + 3] = s3;
  }
  for (; m < count; ++m) {
    float s = src[m];
    for (size_t j = 1; j <= order; ++j) {
      s += model.coefficients[j] * src[m - j];
    }
    residual_out[m] = s;
  }
}

void TdPsola::compute_lpc_residual_fir_multi8(
    const float *src, size_t count, const SharedLpcModel &model, float *residual_out) {
  const size_t order = model.order;
  const size_t count8 = count & ~7;
  size_t m = 0;
  for (; m < count8; m += 8) {
    float s0 = src[m];
    float s1 = src[m + 1];
    float s2 = src[m + 2];
    float s3 = src[m + 3];
    float s4 = src[m + 4];
    float s5 = src[m + 5];
    float s6 = src[m + 6];
    float s7 = src[m + 7];
    for (size_t j = 1; j <= order; ++j) {
      const float c = model.coefficients[j];
      s0 += c * src[m - j];
      s1 += c * src[m + 1 - j];
      s2 += c * src[m + 2 - j];
      s3 += c * src[m + 3 - j];
      s4 += c * src[m + 4 - j];
      s5 += c * src[m + 5 - j];
      s6 += c * src[m + 6 - j];
      s7 += c * src[m + 7 - j];
    }
    residual_out[m] = s0;
    residual_out[m + 1] = s1;
    residual_out[m + 2] = s2;
    residual_out[m + 3] = s3;
    residual_out[m + 4] = s4;
    residual_out[m + 5] = s5;
    residual_out[m + 6] = s6;
    residual_out[m + 7] = s7;
  }
  for (; m < count; ++m) {
    float s = src[m];
    for (size_t j = 1; j <= order; ++j) {
      s += model.coefficients[j] * src[m - j];
    }
    residual_out[m] = s;
  }
}

void TdPsola::compute_lpc_residual_fir_multi8(
    const float *src, int half, const SharedLpcModel &model, float *residual_out) {
  compute_lpc_residual_fir_multi8(src, static_cast<size_t>(2 * half + 1), model, residual_out);
}

void TdPsola::compute_lpc_residual_fir_multi_fma(
    const float *src, size_t count, const SharedLpcModel &model, float *residual_out) {
  const size_t order = model.order;
  const size_t count8 = count & ~7;
  size_t m = 0;
  for (; m < count8; m += 8) {
    float s0 = src[m];
    float s1 = src[m + 1];
    float s2 = src[m + 2];
    float s3 = src[m + 3];
    float s4 = src[m + 4];
    float s5 = src[m + 5];
    float s6 = src[m + 6];
    float s7 = src[m + 7];
    for (size_t j = 1; j <= order; ++j) {
      const float c = model.coefficients[j];
      s0 = __builtin_fmaf(c, src[m - j], s0);
      s1 = __builtin_fmaf(c, src[m + 1 - j], s1);
      s2 = __builtin_fmaf(c, src[m + 2 - j], s2);
      s3 = __builtin_fmaf(c, src[m + 3 - j], s3);
      s4 = __builtin_fmaf(c, src[m + 4 - j], s4);
      s5 = __builtin_fmaf(c, src[m + 5 - j], s5);
      s6 = __builtin_fmaf(c, src[m + 6 - j], s6);
      s7 = __builtin_fmaf(c, src[m + 7 - j], s7);
    }
    residual_out[m] = s0;
    residual_out[m + 1] = s1;
    residual_out[m + 2] = s2;
    residual_out[m + 3] = s3;
    residual_out[m + 4] = s4;
    residual_out[m + 5] = s5;
    residual_out[m + 6] = s6;
    residual_out[m + 7] = s7;
  }
  for (; m < count; ++m) {
    float s = src[m];
    for (size_t j = 1; j <= order; ++j) {
      s = __builtin_fmaf(model.coefficients[j], src[m - j], s);
    }
    residual_out[m] = s;
  }
}

void TdPsola::compute_fir_by_kernel(
    PsolaFirKernel kernel, const float *src, size_t count,
    const SharedLpcModel &model, float *residual_out) {
  switch (kernel) {
    case PsolaFirKernel::ContiguousScalar:
      compute_lpc_residual_fir_contiguous_scalar(src, count, model, residual_out);
      break;
    case PsolaFirKernel::Multi2:
      compute_lpc_residual_fir_multi2(src, count, model, residual_out);
      break;
    case PsolaFirKernel::Multi4:
      compute_lpc_residual_fir_multi4(src, count, model, residual_out);
      break;
    case PsolaFirKernel::Multi8:
      compute_lpc_residual_fir_multi8(src, count, model, residual_out);
      break;
    case PsolaFirKernel::MultiFma:
      compute_lpc_residual_fir_multi_fma(src, count, model, residual_out);
      break;
    default:
      compute_lpc_residual_fir_multi8(src, count, model, residual_out);
      break;
  }
}

void TdPsola::render_single_grain_slice(
    DeferredGrainDescriptor &grain, int n_min, int n_max) {
  if (n_min > n_max || !resources_) return;
  const size_t count = static_cast<size_t>(n_max - n_min + 1);
  const size_t order = grain.use_lpc ? grain.grain_model.order : 0;
  // B4C.7 exact slice audit (observability only): identity is the stored
  // descriptor snapshot plus the rendered clip, so a match means the FIR
  // inputs are bit-identical. Voice-specific destination/synthesis state is
  // never recorded here.
  if (resources_->slice_audit_enabled()) {
    const SourceGrainKey skey = make_source_grain_key(
        grain.center, grain.half, grain.use_lpc, grain.grain_model.valid,
        grain.grain_model.timestamp, grain.grain_model.order, grain.lambda,
        grain.gamma, static_cast<uint8_t>(grain.norm_strategy));
    const uint16_t taps =
        (grain.use_lpc && order > 0)
            ? static_cast<uint16_t>(count * order)
            : 0;
    resources_->record_slice_render(skey, n_min, n_max,
                                    static_cast<uint8_t>(voice_index_),
                                    static_cast<uint16_t>(count), taps);
  }

  alignas(16) float slice_src[128];
  alignas(16) float slice_residual[128];

  const uint64_t src_start = grain.center + static_cast<int64_t>(n_min) - static_cast<int64_t>(order);
  const uint32_t c_fetch_start = Profiler::now_cycles();
  for (size_t k = 0; k < count + order; ++k) {
    slice_src[k] = resources_->sample(src_start + k);
  }
  const uint32_t c_fetch = Profiler::now_cycles() - c_fetch_start;
  b4c7_section_cycles_[1] += c_fetch;
  accounted_cycles_this_block_ += c_fetch;

  const float *p_src = slice_src + order;

  if (grain.use_lpc && order > 0) {
    // B4C.7 deferred residual cache (exact, per-voice). Entry holds the
    // FULL-grain FIR (2*half+1); the slice copies its clip subrange.
    // FIR outputs are per-sample independent, so full-then-clip is
    // bit-identical to clip-direct with the same kernel (gated by host +
    // firmware equivalence tests). Falls back to direct clip compute when
    // the cache is disabled or history is unavailable.
    bool fir_cached = false;
    if (residual_cache_enabled_ && residual_cache_configured_size_ > 0 &&
        grain.half <= 800) {
      const SourceGrainKey ckey = make_source_grain_key(
          grain.center, grain.half, true, grain.grain_model.valid,
          grain.grain_model.timestamp, grain.grain_model.order, grain.lambda,
          grain.gamma, static_cast<uint8_t>(grain.norm_strategy));
      const float *gsrc = resources_->get_contiguous_history(
          grain.center, static_cast<uint32_t>(grain.half), order);
      if (gsrc) {
        const uint32_t c_lookup_start = Profiler::now_cycles();
        int hit_index = -1;
        for (size_t i = 0; i < residual_cache_configured_size_; ++i) {
          if (residual_cache_entries_[i].valid &&
              residual_cache_entries_[i].key == ckey) {
            hit_index = static_cast<int>(i);
            break;
          }
        }
        const uint32_t c_lookup = Profiler::now_cycles() - c_lookup_start;
        residual_cache_stats_.total_lookups++;
        residual_cache_stats_.total_lookup_cycles += c_lookup;
        const uint16_t clip_samples = static_cast<uint16_t>(count);
        if (hit_index >= 0) {
          // HIT: copy clip subrange; FIR fully bypassed.
          const auto &hit = residual_cache_entries_[hit_index];
          for (size_t k = 0; k < count; ++k)
            slice_residual[k] = hit.residual[static_cast<size_t>(grain.half) + static_cast<size_t>(n_min) + k];
          residual_cache_stats_.hits++;
          residual_cache_hits_this_block_++;
          fir_samples_reused_this_block_ += clip_samples;
          residual_cache_stats_.fir_samples_reused += clip_samples;
          const uint32_t gen_prev =
              residual_cache_entries_[hit_index].last_used_generation;
          if (gen_prev > 0 && residual_cache_generation_ >= gen_prev) {
            const uint32_t dist = residual_cache_generation_ - gen_prev;
            if (dist >= 1 && dist <= 8) {
              residual_cache_stats_.reuse_distance_hist[dist - 1]++;
            } else if (dist <= 16) {
              residual_cache_stats_.reuse_distance_hist[8]++;
            } else {
              residual_cache_stats_.reuse_distance_hist[9]++;
            }
          }
          residual_cache_entries_[hit_index].last_used_generation =
              ++residual_cache_generation_;
          if (residual_cache_stats_.fir_samples_computed > 0) {
            residual_cache_stats_.fir_cycles_saved +=
                (static_cast<uint64_t>(clip_samples) *
                 residual_cache_stats_.total_fir_cycles) /
                residual_cache_stats_.fir_samples_computed;
          }
          profiler_.record_cycles(
              section(PitchShiftProfileSection::LpcResidualFIR), 0, 1);
          accounted_cycles_this_block_ += c_lookup;
          fir_cached = true;
        } else {
          // MISS: compute FULL grain FIR into LRU victim, then copy clip.
          residual_cache_stats_.misses++;
          residual_cache_misses_this_block_++;
          size_t victim_idx = 0;
          uint32_t oldest_gen = 0xFFFFFFFF;
          for (size_t i = 0; i < residual_cache_configured_size_; ++i) {
            if (!residual_cache_entries_[i].valid) {
              victim_idx = i;
              break;
            }
            if (residual_cache_entries_[i].last_used_generation < oldest_gen) {
              oldest_gen = residual_cache_entries_[i].last_used_generation;
              victim_idx = i;
            }
          }
          if (residual_cache_entries_[victim_idx].valid) {
            residual_cache_stats_.evictions++;
          }
          auto &victim = residual_cache_entries_[victim_idx];
          victim.key = ckey;
          victim.half_window = grain.half;
          victim.sample_count =
              static_cast<uint32_t>(2 * grain.half + 1);
          victim.last_used_generation = ++residual_cache_generation_;
          victim.valid = true;
          const uint32_t c_fir_start = Profiler::now_cycles();
          compute_fir_by_kernel(fir_kernel_, gsrc,
                                static_cast<size_t>(2 * grain.half + 1),
                                grain.grain_model, victim.residual);
          const uint32_t c_fir = Profiler::now_cycles() - c_fir_start;
          residual_cache_stats_.total_fir_cycles += c_fir;
          fir_samples_computed_this_block_ += clip_samples;
          residual_cache_stats_.fir_samples_computed += clip_samples;
          for (size_t k = 0; k < count; ++k)
            slice_residual[k] = victim.residual[static_cast<size_t>(grain.half) + static_cast<size_t>(n_min) + k];
          profiler_.record_cycles(
              section(PitchShiftProfileSection::LpcResidualFIR), c_fir, 1);
          accounted_cycles_this_block_ += c_fir + c_lookup;
          deferred_fir_samples_this_block_ += static_cast<uint16_t>(count);
          deferred_stats_.slice_fir_samples += count;
          fir_cached = true;
        }
      }
    }
    if (!fir_cached) {
      const uint32_t c_fir_start = Profiler::now_cycles();
      compute_fir_by_kernel(fir_kernel_, p_src, count, grain.grain_model, slice_residual);
      const uint32_t c_fir = Profiler::now_cycles() - c_fir_start;
      profiler_.record_cycles(section(PitchShiftProfileSection::LpcResidualFIR), c_fir, 1);
      accounted_cycles_this_block_ += c_fir;
      deferred_fir_samples_this_block_ += static_cast<uint16_t>(count);
      deferred_stats_.slice_fir_samples += count;
    }
  }

  // B4C.7 Hann/OLA split (§35/§37): two passes over the same indices with
  // identical arithmetic and accumulation order (bit-identical output);
  // intermediates spill to small stack temps instead of recomputation.
  alignas(16) float win_plain_tmp[128];
  alignas(16) float win_w_tmp[128];
  const uint32_t c_ola_start = Profiler::now_cycles();
  const int denom = 2 * grain.half;
  const uint32_t c_hann_start = Profiler::now_cycles();
  if (denom > 0) {
    // B4D.4 exact OLA index progression: replaces the per-sample 64-bit
    // division with a quotient/remainder accumulator. wi_k ==
    // floor((n_min+half + k)*(kHannSize-1) / denom) exactly (num0 >= 0).
    const int64_t step = static_cast<int64_t>(kHannSize - 1);
    const int64_t q = step / denom;
    const int64_t r = step % denom;
    int64_t num0 =
        static_cast<int64_t>(n_min + grain.half) * step;
    int64_t wi = num0 / denom;
    int64_t rem = num0 % denom;
    for (size_t k = 0; k < count; ++k) {
      const float w = resources_->window(static_cast<size_t>(wi));
      win_w_tmp[k] = w;
      win_plain_tmp[k] = p_src[k] * w;
      wi += q;
      rem += r;
      if (rem >= denom) {
        ++wi;
        rem -= denom;
      }
    }
  } else {
    for (size_t k = 0; k < count; ++k) {
      const float w = resources_->window(0);
      win_w_tmp[k] = w;
      win_plain_tmp[k] = p_src[k] * w;
    }
  }
  const uint32_t c_hann = Profiler::now_cycles() - c_hann_start;
  b4c7_section_cycles_[2] += c_hann;
  const uint32_t c_olan_start = Profiler::now_cycles();
  for (size_t k = 0; k < count; ++k) {
    const int n = n_min + static_cast<int>(k);
    const float w = win_w_tmp[k];
    const float win_plain = win_plain_tmp[k];
    const float win_res = grain.use_lpc ? (slice_residual[k] * w) : win_plain;

    const size_t oi = static_cast<size_t>(static_cast<uint64_t>(grain.destination + n) & (kOlaSize - 1));
    ola_[oi] += win_plain;
    lpc_ola_[oi] += win_res;
    norm_[oi] += w;
#ifndef ESP_PLATFORM
    norm_square_[oi] += w * w;
    overlap_[oi] += 1;
    measured_ola_[oi] += win_plain;
    measured_norm_[oi] += w;
#endif
  }
  const uint32_t c_olan = Profiler::now_cycles() - c_olan_start;
  b4c7_section_cycles_[3] += c_olan;
  const uint32_t c_ola = Profiler::now_cycles() - c_ola_start;
  profiler_.record_cycles(section(PitchShiftProfileSection::LpcWindowOLA), c_ola, 1);
  accounted_cycles_this_block_ += c_ola;
}

void TdPsola::render_deferred_slices(uint64_t block_start, size_t frames) {
  if (!deferred_grains_) return;
  const int64_t b_start = static_cast<int64_t>(block_start);
  const int64_t b_end = static_cast<int64_t>(block_start + frames);

  std::array<size_t, kMaxDeferredGrains> active_indices{};
  size_t active_count = 0;
  for (size_t i = 0; i < kMaxDeferredGrains; ++i) {
    if (deferred_grains_[i].active) {
      active_indices[active_count++] = i;
    }
  }

  // Sort active grains by sequence_id to preserve EXACT floating-point addition order
  std::sort(active_indices.begin(), active_indices.begin() + active_count,
            [this](size_t a, size_t b) {
              return deferred_grains_[a].sequence_id < deferred_grains_[b].sequence_id;
            });

  deferred_active_grains_this_block_ = static_cast<uint8_t>(active_count);
  deferred_stats_.max_active_descriptors =
      std::max<uint32_t>(deferred_stats_.max_active_descriptors, active_count);

  for (size_t idx = 0; idx < active_count; ++idx) {
    auto &grain = deferred_grains_[active_indices[idx]];
    const int64_t dst = grain.destination;
    const int half = grain.half;

    const int64_t grain_start = dst - half;
    const int64_t grain_end = dst + half;

    if (grain_end < b_start) {
      grain.active = false;
      deferred_stats_.descriptors_retired++;
      continue;
    }
    if (grain_start >= b_end) {
      continue;
    }

    int n_min = static_cast<int>(std::max<int64_t>(-half, b_start - dst));
    int n_max = static_cast<int>(std::min<int64_t>(half, b_end - 1 - dst));

    if (n_min > n_max) {
      continue;
    }

    const size_t slice_len = static_cast<size_t>(n_max - n_min + 1);
    deferred_stats_.slices_rendered++;
    deferred_stats_.slice_samples_rendered += slice_len;
    deferred_slices_rendered_this_block_++;

    render_single_grain_slice(grain, n_min, n_max);

    if (grain_end < b_end) {
      grain.active = false;
      deferred_stats_.descriptors_retired++;
    }
  }
}

SingleGrainBenchmarkResult TdPsola::benchmark_single_grain(
    size_t grain_length, size_t order,
    SharedPitchShiftResources &resources,
    SharedLpcAnalysis &lpc,
    PsolaFirKernel kernel) {
  SingleGrainBenchmarkResult res{};
  res.grain_length = grain_length;
  res.order = order;
  const int half = static_cast<int>(grain_length / 2);

  alignas(16) std::vector<float> audio(grain_length + order + 256, 0.0f);
  for (size_t i = 0; i < audio.size(); ++i) {
    audio[i] = 0.5f * std::sin(2.0f * 3.14159265f * 220.0f * i / 48000.0f);
  }

  // 1. History setup (simulate ring-buffer history extraction to contiguous scratch)
  alignas(16) std::vector<float> scratch(grain_length + order, 0.0f);
  const uint32_t t0 = Profiler::now_cycles();
  for (size_t i = 0; i < grain_length + order; ++i) {
    scratch[i] = audio[i];
  }
  const float *src = scratch.data() + order;
  const uint32_t t1 = Profiler::now_cycles();
  res.history_setup_cycles = t1 - t0;

  // 2. Model lookup
  SharedLpcModel model{};
  const uint32_t t2 = Profiler::now_cycles();
  (void)lpc.model_near(resources.input_end(), &model);
  const uint32_t t3 = Profiler::now_cycles();
  res.model_lookup_cycles = t3 - t2;

  model.order = static_cast<uint8_t>(order);
  model.valid = true;
  model.coefficients[0] = 1.0f;
  for (size_t j = 1; j <= order; ++j) {
    model.coefficients[j] = -0.1f * j;
  }

  // 3. Warp cache lookup
  const float lambda = SharedLpcAnalysis::lambda_from_semitones(4.0f);
  const float gamma = 0.98f;
  const auto strat = FormantNormalizationStrategy::StrategyC_IntegratedSpectral;
  const uint32_t t4 = Profiler::now_cycles();
  bool hit = resources.shared_warp_cache.is_hit(model.timestamp, model.order,
      model.coefficients.data(), lambda, gamma, strat, 48000.0f);
  (void)hit;
  const uint32_t t5 = Profiler::now_cycles();
  res.warp_cache_lookup_cycles = t5 - t4;

  // 4. Warp poly
  SharedLpcModel warped_model = model;
  const uint32_t t6 = Profiler::now_cycles();
  SharedLpcAnalysis::warp_polynomial(model.coefficients.data(), model.order, lambda, gamma, warped_model.coefficients.data());
  const uint32_t t7 = Profiler::now_cycles();
  res.warp_poly_cycles = t7 - t6;

  // 5. Gain normalization
  const uint32_t t8 = Profiler::now_cycles();
  float gain = SharedLpcAnalysis::compute_gain_normalization(model.coefficients.data(), warped_model.coefficients.data(), model.order, strat, 48000.0f);
  (void)gain;
  const uint32_t t9 = Profiler::now_cycles();
  res.gain_norm_cycles = t9 - t8;

  // 6. Residual FIR
  std::vector<float> residual(grain_length, 0.0f);
  const uint32_t t10 = Profiler::now_cycles();
  compute_fir_by_kernel(kernel, src, grain_length, warped_model, residual.data());
  const uint32_t t11 = Profiler::now_cycles();
  res.fir_cycles = t11 - t10;

  // 7. Hann windowing
  std::vector<float> win_res(grain_length, 0.0f);
  std::vector<float> win_plain(grain_length, 0.0f);
  std::vector<float> win(grain_length, 0.0f);
  const uint32_t t12 = Profiler::now_cycles();
  const int denom = 2 * half;
  for (size_t m = 0; m < grain_length; ++m) {
    const size_t wi = (denom > 0) ? ((m * (SharedPitchShiftResources::kHannSize - 1)) / denom) : 0;
    const float w = resources.window(wi);
    win[m] = w;
    win_plain[m] = src[m] * w;
    win_res[m] = residual[m] * w;
  }
  const uint32_t t13 = Profiler::now_cycles();
  res.window_cycles = t13 - t12;

  // 8. OLA writes
  std::vector<float> ola(grain_length, 0.0f);
  const uint32_t t14 = Profiler::now_cycles();
  for (size_t m = 0; m < grain_length; ++m) {
    ola[m] += win_res[m];
  }
  const uint32_t t15 = Profiler::now_cycles();
  res.ola_write_cycles = t15 - t14;

  // 9. Norm writes
  std::vector<float> norm(grain_length, 0.0f);
  const uint32_t t16 = Profiler::now_cycles();
  for (size_t m = 0; m < grain_length; ++m) {
    norm[m] += win[m];
  }
  const uint32_t t17 = Profiler::now_cycles();
  res.norm_write_cycles = t17 - t16;

  const uint32_t measured_sum = res.history_setup_cycles + res.model_lookup_cycles +
                                res.warp_cache_lookup_cycles + res.warp_poly_cycles +
                                res.gain_norm_cycles + res.fir_cycles +
                                res.window_cycles + res.ola_write_cycles +
                                res.norm_write_cycles;
  res.total_add_grain_cycles = measured_sum;

  constexpr double c2us = 1.0 / 360.0;
  res.history_setup_us = res.history_setup_cycles * c2us;
  res.model_lookup_us = res.model_lookup_cycles * c2us;
  res.warp_cache_lookup_us = res.warp_cache_lookup_cycles * c2us;
  res.warp_poly_us = res.warp_poly_cycles * c2us;
  res.gain_norm_us = res.gain_norm_cycles * c2us;
  res.fir_us = res.fir_cycles * c2us;
  res.window_us = res.window_cycles * c2us;
  res.ola_write_us = res.ola_write_cycles * c2us;
  res.norm_write_us = res.norm_write_cycles * c2us;
  res.total_add_grain_us = res.total_add_grain_cycles * c2us;

  res.cycles_per_source_sample = grain_length > 0 ? static_cast<double>(res.fir_cycles) / grain_length : 0.0;
  res.cycles_per_fir_tap = (grain_length > 0 && order > 0) ? static_cast<double>(res.fir_cycles) / (grain_length * order) : 0.0;
  res.reconciliation_pct = 100.0;

  return res;
}

void TdPsola::compute_windowed_grain(
    const float *src, const float *residual, int half,
    const SharedPitchShiftResources *resources,
    float *win_residual_out, float *win_plain_out, float *window_out) {
  const int denom = 2 * half;
  const int count = 2 * half + 1;
  const int count8 = count & ~7;
  int m = 0;
  for (; m < count8; m += 8) {
    const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m + 1) * (kHannSize - 1)) / denom);
    const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m + 2) * (kHannSize - 1)) / denom);
    const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m + 3) * (kHannSize - 1)) / denom);
    const size_t wi4 = static_cast<size_t>((static_cast<int64_t>(m + 4) * (kHannSize - 1)) / denom);
    const size_t wi5 = static_cast<size_t>((static_cast<int64_t>(m + 5) * (kHannSize - 1)) / denom);
    const size_t wi6 = static_cast<size_t>((static_cast<int64_t>(m + 6) * (kHannSize - 1)) / denom);
    const size_t wi7 = static_cast<size_t>((static_cast<int64_t>(m + 7) * (kHannSize - 1)) / denom);

    const float w0 = resources->window(wi0);
    const float w1 = resources->window(wi1);
    const float w2 = resources->window(wi2);
    const float w3 = resources->window(wi3);
    const float w4 = resources->window(wi4);
    const float w5 = resources->window(wi5);
    const float w6 = resources->window(wi6);
    const float w7 = resources->window(wi7);

    window_out[m] = w0;
    window_out[m + 1] = w1;
    window_out[m + 2] = w2;
    window_out[m + 3] = w3;
    window_out[m + 4] = w4;
    window_out[m + 5] = w5;
    window_out[m + 6] = w6;
    window_out[m + 7] = w7;

    win_plain_out[m] = src[m] * w0;
    win_plain_out[m + 1] = src[m + 1] * w1;
    win_plain_out[m + 2] = src[m + 2] * w2;
    win_plain_out[m + 3] = src[m + 3] * w3;
    win_plain_out[m + 4] = src[m + 4] * w4;
    win_plain_out[m + 5] = src[m + 5] * w5;
    win_plain_out[m + 6] = src[m + 6] * w6;
    win_plain_out[m + 7] = src[m + 7] * w7;

    win_residual_out[m] = residual[m] * w0;
    win_residual_out[m + 1] = residual[m + 1] * w1;
    win_residual_out[m + 2] = residual[m + 2] * w2;
    win_residual_out[m + 3] = residual[m + 3] * w3;
    win_residual_out[m + 4] = residual[m + 4] * w4;
    win_residual_out[m + 5] = residual[m + 5] * w5;
    win_residual_out[m + 6] = residual[m + 6] * w6;
    win_residual_out[m + 7] = residual[m + 7] * w7;
  }
  for (; m < count; ++m) {
    const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
    const float w = resources->window(wi);
    window_out[m] = w;
    win_plain_out[m] = src[m] * w;
    win_residual_out[m] = residual[m] * w;
  }
}

void TdPsola::add_grain_ola_from_cached_residual(
    int n_min, int n_max, int64_t dst, int half,
    const float *src, const float *residual, bool mark_predicted) {
  (void)mark_predicted;
  const int denom = 2 * half;
  const int count = n_max - n_min + 1;
  const size_t start_oi = static_cast<uint64_t>(dst + n_min) & (kOlaSize - 1);

  auto process_segment = [&](int n_start, int seg_len, size_t oi_base) {
    float *dst_ola = &ola_[oi_base];
    float *dst_lpc = &lpc_ola_[oi_base];
    float *dst_norm = &norm_[oi_base];
#ifndef ESP_PLATFORM
    float *dst_norm_sq = &norm_square_[oi_base];
    uint16_t *dst_ov = &overlap_[oi_base];
    float *dst_c_ola = mark_predicted ? &coasted_ola_[oi_base] : &measured_ola_[oi_base];
    float *dst_c_norm = mark_predicted ? &coasted_norm_[oi_base] : &measured_norm_[oi_base];
#endif
    int k = 0;
    const int k_end8 = seg_len & ~7;
    for (; k < k_end8; k += 8) {
      const int n0 = n_start + k;
      const int m0 = n0 + half;
      const size_t wi0 = static_cast<size_t>((static_cast<int64_t>(m0) * (kHannSize - 1)) / denom);
      const size_t wi1 = static_cast<size_t>((static_cast<int64_t>(m0 + 1) * (kHannSize - 1)) / denom);
      const size_t wi2 = static_cast<size_t>((static_cast<int64_t>(m0 + 2) * (kHannSize - 1)) / denom);
      const size_t wi3 = static_cast<size_t>((static_cast<int64_t>(m0 + 3) * (kHannSize - 1)) / denom);
      const size_t wi4 = static_cast<size_t>((static_cast<int64_t>(m0 + 4) * (kHannSize - 1)) / denom);
      const size_t wi5 = static_cast<size_t>((static_cast<int64_t>(m0 + 5) * (kHannSize - 1)) / denom);
      const size_t wi6 = static_cast<size_t>((static_cast<int64_t>(m0 + 6) * (kHannSize - 1)) / denom);
      const size_t wi7 = static_cast<size_t>((static_cast<int64_t>(m0 + 7) * (kHannSize - 1)) / denom);

      const float w0 = resources_->window(wi0);
      const float w1 = resources_->window(wi1);
      const float w2 = resources_->window(wi2);
      const float w3 = resources_->window(wi3);
      const float w4 = resources_->window(wi4);
      const float w5 = resources_->window(wi5);
      const float w6 = resources_->window(wi6);
      const float w7 = resources_->window(wi7);

      dst_ola[k] += src[m0] * w0;
      dst_ola[k + 1] += src[m0 + 1] * w1;
      dst_ola[k + 2] += src[m0 + 2] * w2;
      dst_ola[k + 3] += src[m0 + 3] * w3;
      dst_ola[k + 4] += src[m0 + 4] * w4;
      dst_ola[k + 5] += src[m0 + 5] * w5;
      dst_ola[k + 6] += src[m0 + 6] * w6;
      dst_ola[k + 7] += src[m0 + 7] * w7;

      dst_lpc[k] += residual[m0] * w0;
      dst_lpc[k + 1] += residual[m0 + 1] * w1;
      dst_lpc[k + 2] += residual[m0 + 2] * w2;
      dst_lpc[k + 3] += residual[m0 + 3] * w3;
      dst_lpc[k + 4] += residual[m0 + 4] * w4;
      dst_lpc[k + 5] += residual[m0 + 5] * w5;
      dst_lpc[k + 6] += residual[m0 + 6] * w6;
      dst_lpc[k + 7] += residual[m0 + 7] * w7;

      dst_norm[k] += w0;
      dst_norm[k + 1] += w1;
      dst_norm[k + 2] += w2;
      dst_norm[k + 3] += w3;
      dst_norm[k + 4] += w4;
      dst_norm[k + 5] += w5;
      dst_norm[k + 6] += w6;
      dst_norm[k + 7] += w7;

#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w0 * w0; dst_ov[k] += 1;
      dst_norm_sq[k + 1] += w1 * w1; dst_ov[k + 1] += 1;
      dst_norm_sq[k + 2] += w2 * w2; dst_ov[k + 2] += 1;
      dst_norm_sq[k + 3] += w3 * w3; dst_ov[k + 3] += 1;
      dst_norm_sq[k + 4] += w4 * w4; dst_ov[k + 4] += 1;
      dst_norm_sq[k + 5] += w5 * w5; dst_ov[k + 5] += 1;
      dst_norm_sq[k + 6] += w6 * w6; dst_ov[k + 6] += 1;
      dst_norm_sq[k + 7] += w7 * w7; dst_ov[k + 7] += 1;
      dst_c_ola[k] += src[m0] * w0; dst_c_norm[k] += w0;
      dst_c_ola[k + 1] += src[m0 + 1] * w1; dst_c_norm[k + 1] += w1;
      dst_c_ola[k + 2] += src[m0 + 2] * w2; dst_c_norm[k + 2] += w2;
      dst_c_ola[k + 3] += src[m0 + 3] * w3; dst_c_norm[k + 3] += w3;
      dst_c_ola[k + 4] += src[m0 + 4] * w4; dst_c_norm[k + 4] += w4;
      dst_c_ola[k + 5] += src[m0 + 5] * w5; dst_c_norm[k + 5] += w5;
      dst_c_ola[k + 6] += src[m0 + 6] * w6; dst_c_norm[k + 6] += w6;
      dst_c_ola[k + 7] += src[m0 + 7] * w7; dst_c_norm[k + 7] += w7;
#endif
    }
    for (; k < seg_len; ++k) {
      const int n = n_start + k;
      const int m = n + half;
      const size_t wi = static_cast<size_t>((static_cast<int64_t>(m) * (kHannSize - 1)) / denom);
      const float w = resources_->window(wi);
      dst_ola[k] += src[m] * w;
      dst_lpc[k] += residual[m] * w;
      dst_norm[k] += w;
#ifndef ESP_PLATFORM
      dst_norm_sq[k] += w * w;
      dst_ov[k] += 1;
      dst_c_ola[k] += src[m] * w;
      dst_c_norm[k] += w;
#endif
    }
  };

  if (start_oi + count <= kOlaSize) {
    process_segment(n_min, count, start_oi);
  } else {
    const int seg1_len = static_cast<int>(kOlaSize - start_oi);
    const int seg2_len = count - seg1_len;
    process_segment(n_min, seg1_len, start_oi);
    process_segment(n_min + seg1_len, seg2_len, 0);
  }
}

void TdPsola::add_grain_ola_from_cached_windowed(
    int n_min, int n_max, int64_t dst, int half,
    const float *win_plain, const float *win_residual, const float *window,
    bool mark_predicted) {
  (void)mark_predicted;
  const int count = n_max - n_min + 1;
  const size_t start_oi = static_cast<uint64_t>(dst + n_min) & (kOlaSize - 1);

  auto process_segment = [&](int n_start, int seg_len, size_t oi_base) {
    float *dst_ola = &ola_[oi_base];
    float *dst_lpc = &lpc_ola_[oi_base];
    float *dst_norm = &norm_[oi_base];
#ifndef ESP_PLATFORM
    float *dst_norm_sq = &norm_square_[oi_base];
    uint16_t *dst_ov = &overlap_[oi_base];
    float *dst_c_ola = mark_predicted ? &coasted_ola_[oi_base] : &measured_ola_[oi_base];
    float *dst_c_norm = mark_predicted ? &coasted_norm_[oi_base] : &measured_norm_[oi_base];
#endif
    int k = 0;
    const int k_end8 = seg_len & ~7;
    for (; k < k_end8; k += 8) {
      const int m0 = n_start + k + half;
      dst_ola[k] += win_plain[m0];
      dst_ola[k + 1] += win_plain[m0 + 1];
      dst_ola[k + 2] += win_plain[m0 + 2];
      dst_ola[k + 3] += win_plain[m0 + 3];
      dst_ola[k + 4] += win_plain[m0 + 4];
      dst_ola[k + 5] += win_plain[m0 + 5];
      dst_ola[k + 6] += win_plain[m0 + 6];
      dst_ola[k + 7] += win_plain[m0 + 7];

      dst_lpc[k] += win_residual[m0];
      dst_lpc[k + 1] += win_residual[m0 + 1];
      dst_lpc[k + 2] += win_residual[m0 + 2];
      dst_lpc[k + 3] += win_residual[m0 + 3];
      dst_lpc[k + 4] += win_residual[m0 + 4];
      dst_lpc[k + 5] += win_residual[m0 + 5];
      dst_lpc[k + 6] += win_residual[m0 + 6];
      dst_lpc[k + 7] += win_residual[m0 + 7];

      dst_norm[k] += window[m0];
      dst_norm[k + 1] += window[m0 + 1];
      dst_norm[k + 2] += window[m0 + 2];
      dst_norm[k + 3] += window[m0 + 3];
      dst_norm[k + 4] += window[m0 + 4];
      dst_norm[k + 5] += window[m0 + 5];
      dst_norm[k + 6] += window[m0 + 6];
      dst_norm[k + 7] += window[m0 + 7];

#ifndef ESP_PLATFORM
      dst_norm_sq[k] += window[m0] * window[m0]; dst_ov[k] += 1;
      dst_norm_sq[k + 1] += window[m0 + 1] * window[m0 + 1]; dst_ov[k + 1] += 1;
      dst_norm_sq[k + 2] += window[m0 + 2] * window[m0 + 2]; dst_ov[k + 2] += 1;
      dst_norm_sq[k + 3] += window[m0 + 3] * window[m0 + 3]; dst_ov[k + 3] += 1;
      dst_norm_sq[k + 4] += window[m0 + 4] * window[m0 + 4]; dst_ov[k + 4] += 1;
      dst_norm_sq[k + 5] += window[m0 + 5] * window[m0 + 5]; dst_ov[k + 5] += 1;
      dst_norm_sq[k + 6] += window[m0 + 6] * window[m0 + 6]; dst_ov[k + 6] += 1;
      dst_norm_sq[k + 7] += window[m0 + 7] * window[m0 + 7]; dst_ov[k + 7] += 1;
      dst_c_ola[k] += win_plain[m0]; dst_c_norm[k] += window[m0];
      dst_c_ola[k + 1] += win_plain[m0 + 1]; dst_c_norm[k + 1] += window[m0 + 1];
      dst_c_ola[k + 2] += win_plain[m0 + 2]; dst_c_norm[k + 2] += window[m0 + 2];
      dst_c_ola[k + 3] += win_plain[m0 + 3]; dst_c_norm[k + 3] += window[m0 + 3];
      dst_c_ola[k + 4] += win_plain[m0 + 4]; dst_c_norm[k + 4] += window[m0 + 4];
      dst_c_ola[k + 5] += win_plain[m0 + 5]; dst_c_norm[k + 5] += window[m0 + 5];
      dst_c_ola[k + 6] += win_plain[m0 + 6]; dst_c_norm[k + 6] += window[m0 + 6];
      dst_c_ola[k + 7] += win_plain[m0 + 7]; dst_c_norm[k + 7] += window[m0 + 7];
#endif
    }
    for (; k < seg_len; ++k) {
      const int m = n_start + k + half;
      dst_ola[k] += win_plain[m];
      dst_lpc[k] += win_residual[m];
      dst_norm[k] += window[m];
#ifndef ESP_PLATFORM
      dst_norm_sq[k] += window[m] * window[m];
      dst_ov[k] += 1;
      dst_c_ola[k] += win_plain[m];
      dst_c_norm[k] += window[m];
#endif
    }
  };

  if (start_oi + count <= kOlaSize) {
    process_segment(n_min, count, start_oi);
  } else {
    const int seg1_len = static_cast<int>(kOlaSize - start_oi);
    const int seg2_len = count - seg1_len;
    process_segment(n_min, seg1_len, start_oi);
    process_segment(n_min + seg1_len, seg2_len, 0);
  }
}

bool TdPsola::add_grain(double destination, double source,
                        const PitchMark *marks, size_t count) {
  const uint32_t c_ms_start = Profiler::now_cycles();
  size_t index;
  float period;
  GrainFailureReason reason = GrainFailureReason::None;
  double distance = 0.0, allowed_distance = 0.0;
  const bool selected = select_mark(source, marks, count, index, period, &reason,
                                    &distance, &allowed_distance);
  const uint32_t b4d5_select_cycles = Profiler::now_cycles() - c_ms_start;
  if (s_b4d5_mark_audit_enabled)
    s_b4d5_mark_total_cycles += b4d5_select_cycles;
  if (marks && count) {
    const uint64_t nearest = marks[index].sample_position;
    record_grain_alignment(
        source, nearest, period,
        !selected && reason == GrainFailureReason::SelectMarkDistanceTooLarge);
  }
  const uint32_t c_ms = Profiler::now_cycles() - c_ms_start;
  profiler_.record_cycles(section(PitchShiftProfileSection::MarkSelection), c_ms, 1);
  accounted_cycles_this_block_ += c_ms;

  if (!selected) {
    const uint64_t center = marks && count ? marks[index].sample_position : 0;
    record_grain_failure(reason, destination, source, center, 0, distance,
                         allowed_distance);
    ++telemetry_.pitch_mark_underflows;
    return false;
  }
  vocal_fx_funnel_inc_pitch_marks_consumed(1);

  const uint32_t c_hl_start = Profiler::now_cycles();
  const int half = std::clamp(static_cast<int>(std::lround(period)), 24, 800);
  const uint64_t center = marks[index].sample_position;
  debug_.selected_source_mark = center;
  debug_.source_grain_timestamp = static_cast<uint64_t>(std::max(0.0, source));
  debug_.output_synthesis_timestamp =
      static_cast<uint64_t>(std::max(0.0, destination));
  GrainFailureReason history_reason = GrainFailureReason::None;
  const uint64_t input_end = resources_->input_end();
  const uint64_t oldest = input_end > kHistorySize ? input_end - kHistorySize : 0;
  if (center < static_cast<uint64_t>(half + VOCAL_FX_LPC_MAX_ORDER))
    history_reason = GrainFailureReason::HistoryCenterBeforeHalf;
  else if (center - half - VOCAL_FX_LPC_MAX_ORDER < oldest)
    history_reason = GrainFailureReason::HistoryTooOld;
  else if (center + half >= input_end)
    history_reason = GrainFailureReason::HistoryFutureEnd;
  const uint32_t c_hl = Profiler::now_cycles() - c_hl_start;
  profiler_.record_cycles(section(PitchShiftProfileSection::GrainHistoryLookup), c_hl, 1);
  accounted_cycles_this_block_ += c_hl;

  if (history_reason != GrainFailureReason::None) {
    record_grain_failure(history_reason, destination, source, center, half,
                         distance, allowed_distance);
    ++telemetry_.audio_history_underflows;
    return false;
  }
  last_scheduled_mark_ = center;
  new_grain_scheduled_this_block_ = true;
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
#else
  const bool mark_predicted = false;
#endif

  const uint32_t t_warp_start = Profiler::now_cycles();
  SharedLpcModel model;
  const uint32_t c_near_start = Profiler::now_cycles();
  const bool has_near = (lpc_ && lpc_->model_near(center, &model));
  const uint32_t c_near = Profiler::now_cycles() - c_near_start;
  warp_audit_.model_near_calls++;
  warp_audit_.model_near_cycles += c_near;

  const bool use_lpc = formant_mode_ == FormantMode::Lpc && formant_amount_ > 0 &&
                       has_near && model.confidence > .15f;

  uint32_t c_poly = 0;
  uint32_t c_gn = 0;

  if (use_lpc) {
    grain_model_ = model;
    ++model_warp_calls_this_block_;
    ++warp_stats_.total_calls;
    if (last_model_timestamp_ != model.timestamp) {
      model_changed_this_block_ = 1;
      last_model_timestamp_ = model.timestamp;
    }

    const uint32_t c_lam_start = Profiler::now_cycles();
    const float lambda = SharedLpcAnalysis::lambda_from_semitones(formant_shift_semitones_);
    const float gamma = formant_bandwidth_expansion_;
    const auto strat = formant_normalization_strategy_;
    const uint32_t c_lam = Profiler::now_cycles() - c_lam_start;
    warp_audit_.lambda_calc_calls++;
    warp_audit_.lambda_calc_cycles += c_lam;

    bool handled = false;

    const uint32_t c_cache_start = Profiler::now_cycles();
    const bool local_hit = warp_cache_enabled_ && warp_cache_.is_hit(
        model.timestamp, model.order, model.coefficients.data(), lambda, gamma,
        strat, sample_rate_);
    const bool shared_hit = !local_hit && shared_warp_cache_enabled_ && resources_ &&
                            resources_->shared_warp_cache.is_hit(
                                model.timestamp, model.order,
                                model.coefficients.data(), lambda, gamma, strat,
                                sample_rate_);
    const bool neutral_hit = !local_hit && !shared_hit && neutral_warp_fast_path_enabled_ &&
                             std::fabs(lambda) < 1e-5f && (gamma >= 0.9999f || gamma <= 0.0f);
    const uint32_t c_cache_lookup = Profiler::now_cycles() - c_cache_start;
    warp_audit_.cache_lookup_calls++;
    warp_audit_.cache_lookup_cycles += c_cache_lookup;

    if (local_hit) {
      const uint32_t c_hit_start = Profiler::now_cycles();
      grain_model_.coefficients = warp_cache_.warped_coefficients;
      formant_filter_gain_ = warp_cache_.formant_filter_gain;
      const uint32_t c_hit = Profiler::now_cycles() - c_hit_start;
      warp_audit_.cache_hit_calls++;
      warp_audit_.cache_hit_cycles += c_hit;
      warp_audit_.coeff_copy_calls++;
      warp_audit_.coeff_copy_cycles += c_hit;
      warp_audit_.local_hits++;
      ++warp_stats_.local_hits;
      handled = true;
    } else if (shared_hit) {
      const uint32_t c_hit_start = Profiler::now_cycles();
      grain_model_.coefficients = resources_->shared_warp_cache.warped_coefficients;
      formant_filter_gain_ = resources_->shared_warp_cache.formant_filter_gain;
      if (warp_cache_enabled_) {
        warp_cache_.update(model.timestamp, model.order, model.coefficients.data(),
                           lambda, gamma, strat, grain_model_.coefficients,
                           formant_filter_gain_, sample_rate_);
      }
      const uint32_t c_hit = Profiler::now_cycles() - c_hit_start;
      warp_audit_.cache_hit_calls++;
      warp_audit_.cache_hit_cycles += c_hit;
      warp_audit_.coeff_copy_calls++;
      warp_audit_.coeff_copy_cycles += c_hit;
      warp_audit_.shared_hits++;
      ++warp_stats_.shared_hits;
      handled = true;
    } else if (neutral_hit) {
      const uint32_t c_hit_start = Profiler::now_cycles();
      grain_model_.coefficients = model.coefficients;
      formant_filter_gain_ = 1.0f;
      if (warp_cache_enabled_) {
        warp_cache_.update(model.timestamp, model.order, model.coefficients.data(),
                           lambda, gamma, strat, grain_model_.coefficients,
                           1.0f, sample_rate_);
      }
      if (shared_warp_cache_enabled_ && resources_) {
        resources_->shared_warp_cache.update(model.timestamp, model.order,
                                             model.coefficients.data(), lambda,
                                             gamma, strat, grain_model_.coefficients,
                                             1.0f, sample_rate_);
      }
      const uint32_t c_hit = Profiler::now_cycles() - c_hit_start;
      warp_audit_.cache_hit_calls++;
      warp_audit_.cache_hit_cycles += c_hit;
      warp_audit_.neutral_hits++;
      ++warp_stats_.neutral_hits;
      handled = true;
    }

    if (!handled) {
      warp_audit_.cache_miss_calls++;
      ++warp_stats_.misses;
      ++model_warp_expensive_this_block_;
      warp_audit_.expensive_computations++;

      // B4D.2 (§7): was this miss caused only by a timestamp change?
      ++s_warp_miss_total;
      if (warp_cache_.math_equal(model.order, model.coefficients.data(),
                                 lambda, gamma, strat, sample_rate_) ||
          (resources_ &&
           resources_->shared_warp_cache.math_equal(
               model.order, model.coefficients.data(), lambda, gamma, strat,
               sample_rate_))) {
        ++s_warp_math_only_miss;
      }

      const uint32_t c_poly_start = Profiler::now_cycles();
      SharedLpcAnalysis::warp_polynomial(model.coefficients.data(), model.order,
                                         lambda, gamma,
                                         grain_model_.coefficients.data());
      c_poly = Profiler::now_cycles() - c_poly_start;
      warp_audit_.warp_poly_calls++;
      warp_audit_.warp_poly_cycles += c_poly;

      const uint32_t c_gn_start = Profiler::now_cycles();
      formant_filter_gain_ = SharedLpcAnalysis::compute_gain_normalization(
          model.coefficients.data(), grain_model_.coefficients.data(),
          model.order, strat, sample_rate_);
      c_gn = Profiler::now_cycles() - c_gn_start;
      warp_audit_.gain_norm_calls++;
      warp_audit_.gain_norm_cycles += c_gn;

      const uint32_t c_up_start = Profiler::now_cycles();
      if (warp_cache_enabled_) {
        warp_cache_.update(model.timestamp, model.order, model.coefficients.data(),
                           lambda, gamma, strat, grain_model_.coefficients,
                           formant_filter_gain_, sample_rate_);
      }
      if (shared_warp_cache_enabled_ && resources_) {
        resources_->shared_warp_cache.update(model.timestamp, model.order,
                                             model.coefficients.data(), lambda,
                                             gamma, strat, grain_model_.coefficients,
                                             formant_filter_gain_, sample_rate_);
      }
      const uint32_t c_up = Profiler::now_cycles() - c_up_start;
      warp_audit_.coeff_copy_calls++;
      warp_audit_.coeff_copy_cycles += c_up;
    }
  } else {
    grain_model_ = SharedLpcModel{};
    formant_filter_gain_ = 1.0f;
  }
  const uint32_t t_warp_end = Profiler::now_cycles();
  const uint32_t c_warp_total = t_warp_end - t_warp_start;
  model_warp_cycles_this_block_ += c_warp_total;

  const uint32_t c_warp_lookup = (c_warp_total > (c_poly + c_gn)) ? (c_warp_total - (c_poly + c_gn)) : 0;
  profiler_.record_cycles(section(PitchShiftProfileSection::LpcModelLookup), c_warp_lookup, 1);
  if (c_poly > 0) {
    profiler_.record_cycles(section(PitchShiftProfileSection::LpcModelWarpPolynomial), c_poly, 1);
  }
  if (c_gn > 0) {
    profiler_.record_cycles(section(PitchShiftProfileSection::LpcModelWarpGainNorm), c_gn, 1);
  }
  accounted_cycles_this_block_ += c_warp_total;

  const int64_t dst = static_cast<int64_t>(std::llround(destination));
  int n_min = -half;
  int n_max = half;
  while (n_min <= n_max && dst + n_min < static_cast<int64_t>(output_position_)) {
    ++n_min;
  }
  while (n_max >= n_min && dst + n_max >= static_cast<int64_t>(output_position_ + kOlaSize)) {
    --n_max;
  }
  if (n_min > n_max) {
    return true;
  }

  if (grain_render_mode_ == PsolaGrainRenderMode::DeferredSlice && deferred_grains_) {
    const uint32_t c_desc_start = Profiler::now_cycles();
    size_t slot = kMaxDeferredGrains;
    for (size_t i = 0; i < kMaxDeferredGrains; ++i) {
      if (!deferred_grains_[i].active) {
        slot = i;
        break;
      }
    }
    if (slot == kMaxDeferredGrains) {
      uint32_t oldest_seq = 0xFFFFFFFF;
      for (size_t i = 0; i < kMaxDeferredGrains; ++i) {
        if (deferred_grains_[i].sequence_id < oldest_seq) {
          oldest_seq = deferred_grains_[i].sequence_id;
          slot = i;
        }
      }
    }
    auto &g = deferred_grains_[slot];
    g.sequence_id = ++grain_sequence_counter_;
    g.destination = dst;
    g.center = center;
    g.half = half;
    g.grain_model = grain_model_;
    g.lambda = SharedLpcAnalysis::lambda_from_semitones(formant_shift_semitones_);
    g.gamma = formant_bandwidth_expansion_;
    g.norm_strategy = formant_normalization_strategy_;
    g.formant_filter_gain = formant_filter_gain_;
    g.use_lpc = use_lpc;
    g.mark_predicted = mark_predicted;
    g.active = true;
    // B4C.7: register the descriptor's exact source identity so the shared
    // audit covers the production deferred path (audit-only memory writes;
    // scheduling, rendering and synthesis decisions are untouched).
    {
      const SourceGrainKey dkey = make_source_grain_key(
          center, half, use_lpc, grain_model_.valid, grain_model_.timestamp,
          grain_model_.order, g.lambda, g.gamma,
          static_cast<uint8_t>(g.norm_strategy));
      const uint16_t desc_samples = static_cast<uint16_t>(2 * half + 1);
      bool same_v = false, cross_v = false;
      resources_->register_source_grain(
          dkey, static_cast<uint8_t>(voice_index_),
          static_cast<uint32_t>(telemetry_.blocks), &same_v, &cross_v);
      auto &sg = resources_->shared_source_grain_audit;
      sg.source_grains_requested++;
      sg.total_grain_samples += desc_samples;
      if (same_v || cross_v) {
        sg.duplicate_source_grains++;
        sg.reusable_grain_samples += desc_samples;
        if (same_v) sg.same_voice_reuses++;
        if (cross_v) sg.cross_voice_reuses++;
      } else {
        sg.unique_source_grains++;
      }
    }
    deferred_stats_.descriptors_allocated++;
    ++grains_rendered_this_block_;
    active_overlapping_grains_this_block_ = grains_rendered_this_block_;
    grain_samples_processed_this_block_ += static_cast<uint16_t>(2 * half + 1);
    const uint32_t c_desc = Profiler::now_cycles() - c_desc_start;
    b4c7_section_cycles_[0] += c_desc;
    accounted_cycles_this_block_ += c_desc;
    return true;
  }

  const float *src = nullptr;
  if (lpc_kernel_ != PsolaLpcKernel::Reference) {
    src = resources_->get_contiguous_history(center, half, grain_model_.order);
  }

  const SourceGrainKey key = make_source_grain_key(
      center, half, use_lpc, grain_model_.valid, grain_model_.timestamp,
      grain_model_.order,
      SharedLpcAnalysis::lambda_from_semitones(formant_shift_semitones_),
      formant_bandwidth_expansion_,
      static_cast<uint8_t>(formant_normalization_strategy_));

  const uint16_t grain_samples = static_cast<uint16_t>(n_max - n_min + 1);

  const bool can_cache = (residual_cache_enabled_ || windowed_cache_enabled_) &&
                         residual_cache_configured_size_ > 0 &&
                         use_lpc && src && half <= 800 &&
                         lpc_kernel_ != PsolaLpcKernel::Reference;

  if (can_cache) {
    int hit_index = -1;
    const uint32_t c_lookup_start = Profiler::now_cycles();
    for (size_t i = 0; i < residual_cache_configured_size_; ++i) {
      if (residual_cache_entries_[i].valid && residual_cache_entries_[i].key == key) {
        hit_index = static_cast<int>(i);
        break;
      }
    }
    const uint32_t c_lookup = Profiler::now_cycles() - c_lookup_start;
    residual_cache_stats_.total_lookups++;
    residual_cache_stats_.total_lookup_cycles += c_lookup;

    const uint16_t grain_fir_samples = static_cast<uint16_t>(2 * half + 1);

    if (hit_index >= 0) {
      // HIT: FIR is completely bypassed
      residual_cache_stats_.hits++;
      residual_cache_hits_this_block_++;
      fir_samples_reused_this_block_ += grain_fir_samples;
      residual_cache_stats_.fir_samples_reused += grain_fir_samples;

      const uint32_t gen_prev = residual_cache_entries_[hit_index].last_used_generation;
      if (gen_prev > 0 && residual_cache_generation_ >= gen_prev) {
        const uint32_t dist = residual_cache_generation_ - gen_prev;
        if (dist >= 1 && dist <= 8) {
          residual_cache_stats_.reuse_distance_hist[dist - 1]++;
        } else if (dist <= 16) {
          residual_cache_stats_.reuse_distance_hist[8]++;
        } else {
          residual_cache_stats_.reuse_distance_hist[9]++;
        }
      }
      residual_cache_entries_[hit_index].last_used_generation = ++residual_cache_generation_;

      const uint32_t c_ola_start = Profiler::now_cycles();
      if (windowed_cache_enabled_ && residual_cache_entries_[hit_index].windowed_residual) {
        add_grain_ola_from_cached_windowed(
            n_min, n_max, dst, half,
            residual_cache_entries_[hit_index].windowed_plain,
            residual_cache_entries_[hit_index].windowed_residual,
            residual_cache_entries_[hit_index].window,
            mark_predicted);
      } else {
        add_grain_ola_from_cached_residual(
            n_min, n_max, dst, half,
            src,
            residual_cache_entries_[hit_index].residual,
            mark_predicted);
      }
      const uint32_t c_ola = Profiler::now_cycles() - c_ola_start;
      const uint32_t c_grain = c_lookup + c_ola;
      accounted_cycles_this_block_ += c_grain;
      ++grains_rendered_this_block_;
      active_overlapping_grains_this_block_ = grains_rendered_this_block_;
      grain_samples_processed_this_block_ += grain_samples;

      profiler_.record_cycles(section(PitchShiftProfileSection::LpcResidualFIR), 0, 1);
      profiler_.record_cycles(section(PitchShiftProfileSection::LpcWindowOLA), c_grain, 1);

      if (residual_cache_stats_.fir_samples_computed > 0) {
        residual_cache_stats_.fir_cycles_saved +=
            (static_cast<uint64_t>(grain_fir_samples) * residual_cache_stats_.total_fir_cycles) /
            residual_cache_stats_.fir_samples_computed;
      } else {
        residual_cache_stats_.fir_cycles_saved += static_cast<uint64_t>(grain_fir_samples * 2);
      }
    } else {
      // MISS: Compute FIR and populate LRU victim slot
      residual_cache_stats_.misses++;
      residual_cache_misses_this_block_++;
      fir_samples_computed_this_block_ += grain_fir_samples;
      residual_cache_stats_.fir_samples_computed += grain_fir_samples;

      size_t victim_idx = 0;
      uint32_t oldest_gen = 0xFFFFFFFF;
      for (size_t i = 0; i < residual_cache_configured_size_; ++i) {
        if (!residual_cache_entries_[i].valid) {
          victim_idx = i;
          break;
        }
        if (residual_cache_entries_[i].last_used_generation < oldest_gen) {
          oldest_gen = residual_cache_entries_[i].last_used_generation;
          victim_idx = i;
        }
      }
      if (residual_cache_entries_[victim_idx].valid) {
        residual_cache_stats_.evictions++;
      }

      auto &victim = residual_cache_entries_[victim_idx];
      victim.key = key;
      victim.half_window = half;
      victim.sample_count = grain_fir_samples;
      victim.last_used_generation = ++residual_cache_generation_;
      victim.valid = true;

      const uint32_t c_fir_start = Profiler::now_cycles();
      compute_lpc_residual_fir_multi8(src, half, grain_model_, victim.residual);
      const uint32_t c_fir = Profiler::now_cycles() - c_fir_start;
      residual_cache_stats_.total_fir_cycles += c_fir;

      uint32_t c_ola = 0;
      if (windowed_cache_enabled_ && victim.windowed_residual) {
        compute_windowed_grain(src, victim.residual, half, resources_,
                               victim.windowed_residual, victim.windowed_plain, victim.window);
        const uint32_t c_ola_start = Profiler::now_cycles();
        add_grain_ola_from_cached_windowed(
            n_min, n_max, dst, half,
            victim.windowed_plain, victim.windowed_residual, victim.window,
            mark_predicted);
        c_ola = Profiler::now_cycles() - c_ola_start;
      } else {
        const uint32_t c_ola_start = Profiler::now_cycles();
        add_grain_ola_from_cached_residual(
            n_min, n_max, dst, half,
            src, victim.residual, mark_predicted);
        c_ola = Profiler::now_cycles() - c_ola_start;
      }

      const uint32_t c_grain = c_lookup + c_fir + c_ola;
      accounted_cycles_this_block_ += c_grain;
      ++grains_rendered_this_block_;
      active_overlapping_grains_this_block_ = grains_rendered_this_block_;
      grain_samples_processed_this_block_ += grain_samples;

      profiler_.record_cycles(section(PitchShiftProfileSection::LpcResidualFIR), c_fir, 1);
      profiler_.record_cycles(section(PitchShiftProfileSection::LpcWindowOLA), c_ola + c_lookup, 1);
    }
  } else {
    // Cache bypass / fallback
    const uint32_t c_start = Profiler::now_cycles();
    if (lpc_kernel_ == PsolaLpcKernel::Reference || !src) {
      add_grain_reference(n_min, n_max, dst, half, center, grain_model_, use_lpc, mark_predicted);
    } else if (grain_kernel_ == PsolaGrainKernel::ContiguousMulti8) {
      add_grain_combined_multi8(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    } else if (grain_kernel_ == PsolaGrainKernel::ContiguousMulti4) {
      add_grain_combined_multi4(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    } else if (ola_kernel_ == PsolaOlaKernel::Contiguous) {
      add_grain_ola_contiguous(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    } else if (lpc_kernel_ == PsolaLpcKernel::ContiguousMulti8) {
      add_grain_contiguous_multi8(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    } else if (lpc_kernel_ == PsolaLpcKernel::ContiguousMulti4) {
      add_grain_contiguous_multi4(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    } else {
      add_grain_contiguous_exact(n_min, n_max, dst, half, src, grain_model_, use_lpc, mark_predicted);
    }
    const uint32_t c_grain = Profiler::now_cycles() - c_start;
    accounted_cycles_this_block_ += c_grain;
    ++grains_rendered_this_block_;
    active_overlapping_grains_this_block_ = grains_rendered_this_block_;
    grain_samples_processed_this_block_ += grain_samples;
    if (!use_lpc) {
      profiler_.record_cycles(section(PitchShiftProfileSection::PlainWindowOLA), c_grain, 1);
    } else {
      const uint32_t c_fir = (c_grain * 62) / 100;
      profiler_.record_cycles(section(PitchShiftProfileSection::LpcResidualFIR), c_fir, 1);
      profiler_.record_cycles(section(PitchShiftProfileSection::LpcWindowOLA), c_grain - c_fir, 1);
      fir_samples_computed_this_block_ += static_cast<uint16_t>(2 * half + 1);
    }
  }

  if (resources_) {
    bool same_v = false, cross_v = false;
    resources_->register_source_grain(key, static_cast<uint8_t>(voice_index_),
                                      static_cast<uint32_t>(telemetry_.blocks),
                                      &same_v, &cross_v);
    auto &sg = resources_->shared_source_grain_audit;
    sg.source_grains_requested++;
    sg.total_grain_samples += grain_samples;
    if (same_v || cross_v) {
      sg.duplicate_source_grains++;
      sg.reusable_grain_samples += grain_samples;
      if (same_v) sg.same_voice_reuses++;
      if (cross_v) sg.cross_voice_reuses++;
      duplicate_source_grains_this_block_++;
    } else {
      sg.unique_source_grains++;
      unique_source_grains_this_block_++;
      unique_grain_samples_this_block_ += grain_samples;
    }
  }
  source_grains_built_this_block_++;

  const float earliest_dest = static_cast<float>(dst + n_min);
  const float slack_samples = earliest_dest - static_cast<float>(output_position_);
  const float slack_blocks = (current_block_frames_ > 0)
                                 ? (slack_samples / static_cast<float>(current_block_frames_))
                                 : 0.0f;
  if (resources_) {
    resources_->shared_scheduling_slack_audit.record(slack_blocks);
  }
  render_slack_blocks_min_this_block_ = std::min(render_slack_blocks_min_this_block_, slack_blocks);

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
  const uint32_t c_block_total_start = Profiler::now_cycles();
  uint64_t b4d4_snapshot[kB4D4Sections];
  if (s_b4d4_class_enabled && voice_index_ == 0) {
    for (size_t i = 0; i < kB4D4Sections; ++i)
      b4d4_snapshot[i] =
          profiler_.raw_total_cycles(
              section(static_cast<PitchShiftProfileSection>(i)));
  }
  // B4C.8: reset per-block bookkeeping (Other counters accumulate across window).
  b4c8_accounted_this_block_ = 0;
  b4c8_total_this_block_ = 0;
  current_block_frames_ = static_cast<uint32_t>(frames);
  accounted_cycles_this_block_ = 0;
  source_grains_built_this_block_ = 0;
  unique_source_grains_this_block_ = 0;
  duplicate_source_grains_this_block_ = 0;
  unique_grain_samples_this_block_ = 0;
  model_warp_expensive_this_block_ = 0;
  render_slack_blocks_min_this_block_ = 9999.0f;
  model_warp_calls_this_block_ = 0;
  model_warp_cycles_this_block_ = 0;
  model_changed_this_block_ = 0;
  grains_scheduled_this_block_ = 0;
  grains_rendered_this_block_ = 0;
  grain_samples_processed_this_block_ = 0;
  residual_cache_hits_this_block_ = 0;
  residual_cache_misses_this_block_ = 0;
  fir_samples_computed_this_block_ = 0;
  fir_samples_reused_this_block_ = 0;
  active_overlapping_grains_this_block_ = 0;
  precompute_queue_depth_this_block_ = 0;
  precompute_expired_this_block_ = 0;
  deferred_active_grains_this_block_ = 0;
  deferred_slices_rendered_this_block_ = 0;
  deferred_fir_samples_this_block_ = 0;

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
    const uint32_t c_telem_start = Profiler::now_cycles();
    publish_telemetry();
    const uint32_t c_telem = Profiler::now_cycles() - c_telem_start;
    profiler_.record_cycles(section(PitchShiftProfileSection::Telemetry), c_telem, 1);
    accounted_cycles_this_block_ += c_telem;

    const uint32_t c_bypass_total = Profiler::now_cycles() - c_block_total_start;
    last_block_cycles_ = c_bypass_total;
    const uint32_t other_cycles = (c_bypass_total > accounted_cycles_this_block_)
                                      ? (c_bypass_total - accounted_cycles_this_block_)
                                      : 0;
    b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::OtherUnattributed)] += other_cycles;
    profiler_.record_cycles(section(PitchShiftProfileSection::Other), other_cycles, 1);

    // B4C.8: accumulate bypass-block totals.
    b4c8_total_blocks_++;
    b4c8_total_cycles_ += last_block_cycles_;
    if (last_block_cycles_ > b4c8_max_block_cycles_)
      b4c8_max_block_cycles_ = last_block_cycles_;
    b4c8_total_this_block_ = last_block_cycles_;

    VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Total),
                   static_cast<uint64_t>(1000000.0 * frames / sample_rate_));
    return;
  }

  // B4C.8: measure usability-checks region (§8Q + §8L branch tests).
  const uint32_t c_usability_start = Profiler::now_cycles();

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
    // B4C.7 RecoveryXfade timing (observability only).
    const uint32_t c_recov_start = Profiler::now_cycles();
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
      const uint32_t c_recov = Profiler::now_cycles() - c_recov_start;
      b4c7_section_cycles_[4] += c_recov;
      accounted_cycles_this_block_ += c_recov;
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

    // B4C.8: end usability-checks measurement (§8L+§8Q).
    const uint32_t c_usability_end = Profiler::now_cycles();
    const uint32_t usability_cycles = c_usability_end - c_usability_start;
    b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::RecoveryFallbackBranchTests)] += usability_cycles;
    accounted_cycles_this_block_ += usability_cycles;

    size_t grains = 0;
    while (next_synthesis_mark_ <
               static_cast<double>(block_end + pitch.period_samples) &&
           grains < kMaxGrainsPerBlock) {
      uint32_t c_sched_start = Profiler::now_cycles();
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
      const uint32_t c_sched_mid = Profiler::now_cycles();
      const uint32_t c_sched1 = c_sched_mid - c_sched_start;
      profiler_.record_cycles(section(PitchShiftProfileSection::GrainScheduling),
                              c_sched1, 1);
      accounted_cycles_this_block_ += c_sched1;

      if (source < 0) {
        record_grain_failure(GrainFailureReason::AttemptSourceNegative,
                             next_synthesis_mark_, source);
      } else if (add_grain(next_synthesis_mark_, source, marks, mark_count)) {
        ++grains;
        ++grains_scheduled_this_block_;
        vocal_fx_funnel_inc_grains_scheduled(1);
      }

      const uint32_t c_sched2_start = Profiler::now_cycles();
      next_synthesis_mark_ += current_synthesis_period_ / std::max(ratio, .5f);
      const uint32_t c_sched2_end = Profiler::now_cycles();
      const uint32_t c_sched2 = c_sched2_end - c_sched2_start;
      profiler_.record_cycles(section(PitchShiftProfileSection::GrainScheduling),
                              c_sched2, 1);
      accounted_cycles_this_block_ += c_sched2;
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

#ifndef ESP_PLATFORM
  finish_source_mark_run();
  float norm_sum = 0.0f, norm_square_sum = 0.0f, overlap_sum = 0.0f;
  float norm_min = 1e9f, norm_max = 0.0f;
  float norm_sq_min = 1e9f, norm_sq_max = 0.0f;
  uint32_t ov_min = 10000, ov_max = 0;
  uint32_t norm_below_count = 0;
#endif

  uint32_t total_fallback_cycles = 0;
  uint32_t total_articulation_cycles = 0;
  uint32_t total_plosive_cycles = 0;
  uint32_t total_synthesis_cycles = 0;
  uint32_t total_state_shift_cycles = 0;
  uint32_t total_softclip_cycles = 0;
  uint32_t total_gain_matcher_cycles = 0;
  uint32_t total_blend_cycles = 0;
  uint32_t total_history_fetch_cycles = 0;
  uint32_t total_ola_norm_reset_cycles = 0;

  // B4C.8: measure deferred rendering overhead (§8A+§8B+§8C).
  const uint32_t c_deferred_start = Profiler::now_cycles();

  if (grain_render_mode_ == PsolaGrainRenderMode::DeferredSlice) {
    render_deferred_slices(block_start, frames);
  }

  const uint32_t c_deferred_end = Profiler::now_cycles();
  const uint32_t deferred_overhead = c_deferred_end - c_deferred_start;
  b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::DeferredDescriptorTraversal)] += deferred_overhead;
  accounted_cycles_this_block_ += deferred_overhead;

  // B4C.8A: split deferred traversal into zero-grain vs active-grain calls.
  if (deferred_active_grains_this_block_ == 0) {
    b4c8_deferred_zero_cycles_ += deferred_overhead;
    b4c8_deferred_zero_calls_++;
  } else {
    b4c8_deferred_active_cycles_ += deferred_overhead;
    b4c8_deferred_active_calls_++;
  }

  // B4C.8: measure per-sample loop total (§8D+§8E+§8F+§8G+§8H+§8M+§8Q).
  const uint32_t c_loop_start = Profiler::now_cycles();

  for (size_t i = 0; i < frames; ++i) {
    uint32_t c_hf_start = Profiler::now_cycles();
    const size_t oi = (block_start + i) & (kOlaSize - 1);
    const bool history_ready = block_start + i >= history_offset_;
    const uint64_t source =
        history_ready ? block_start + i - history_offset_ : 0;
    const float delayed_lead = history_ready && history_available(source, source)
                                   ? history_at(source)
                                   : 0.0f;
    uint32_t c_hf_end = Profiler::now_cycles();
    total_history_fetch_cycles += (c_hf_end - c_hf_start);
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

    uint32_t c_fb_start = Profiler::now_cycles();
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
    uint32_t c_fb_end = Profiler::now_cycles();
    total_fallback_cycles += (c_fb_end - c_fb_start);

    uint32_t c_art_start = Profiler::now_cycles();
    const float art_source =
        (unvoiced_config_.timing_reference ==
         UnvoicedTimingReference::CurrentInput && input && i < frames)
            ? input[i]
            : delayed_lead;
    float art_sample = 0.0f;
    (void)unvoiced_articulation_.process_sample(
        art_source, pitch, track, block_input_rms, &art_sample);
    uint32_t c_art_end = Profiler::now_cycles();
    total_articulation_cycles += (c_art_end - c_art_start);

    uint32_t c_pb_start = Profiler::now_cycles();
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
    uint32_t c_pb_end = Profiler::now_cycles();
    total_plosive_cycles += (c_pb_end - c_pb_start);

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
        uint32_t c_syn_start = Profiler::now_cycles();
        if (synthesis_kernel_ == PsolaSynthesisKernel::UnrolledExact && grain_model_.order == 10) {
          restored -= grain_model_.coefficients[1] * synthesis_state_[0];
          restored -= grain_model_.coefficients[2] * synthesis_state_[1];
          restored -= grain_model_.coefficients[3] * synthesis_state_[2];
          restored -= grain_model_.coefficients[4] * synthesis_state_[3];
          restored -= grain_model_.coefficients[5] * synthesis_state_[4];
          restored -= grain_model_.coefficients[6] * synthesis_state_[5];
          restored -= grain_model_.coefficients[7] * synthesis_state_[6];
          restored -= grain_model_.coefficients[8] * synthesis_state_[7];
          restored -= grain_model_.coefficients[9] * synthesis_state_[8];
          restored -= grain_model_.coefficients[10] * synthesis_state_[9];
        } else {
          for (size_t j = 1; j <= grain_model_.order; ++j)
            restored -= grain_model_.coefficients[j] * synthesis_state_[j - 1];
        }
        uint32_t c_syn_end = Profiler::now_cycles();
        total_synthesis_cycles += (c_syn_end - c_syn_start);

        if (std::isfinite(restored)) {
          const float pre_softclip = restored;
          max_pre_tanh_ = std::max(max_pre_tanh_, std::fabs(pre_softclip));
          last_pre_tanh_ = pre_softclip;

          uint32_t c_sc_start = Profiler::now_cycles();
          if (std::fabs(restored) > 1.0f) {
            const float ax = std::fabs(restored);
            const float excess = ax - 1.0f;
            restored = std::copysign(1.0f + excess / (1.0f + 0.5f * excess), restored);
            ++softclip_events_;
          }
          last_post_tanh_ = restored;
          uint32_t c_sc_end = Profiler::now_cycles();
          total_softclip_cycles += (c_sc_end - c_sc_start);

          uint32_t c_sh_start = Profiler::now_cycles();
          if (synthesis_kernel_ == PsolaSynthesisKernel::UnrolledExact && grain_model_.order == 10) {
            constexpr float kLeak = 0.999f;
            synthesis_state_[9] = synthesis_state_[8] * kLeak;
            synthesis_state_[8] = synthesis_state_[7] * kLeak;
            synthesis_state_[7] = synthesis_state_[6] * kLeak;
            synthesis_state_[6] = synthesis_state_[5] * kLeak;
            synthesis_state_[5] = synthesis_state_[4] * kLeak;
            synthesis_state_[4] = synthesis_state_[3] * kLeak;
            synthesis_state_[3] = synthesis_state_[2] * kLeak;
            synthesis_state_[2] = synthesis_state_[1] * kLeak;
            synthesis_state_[1] = synthesis_state_[0] * kLeak;
            synthesis_state_[0] = restored;
          } else {
            if (grain_model_.order > 1) {
              for (size_t j = grain_model_.order - 1; j > 0; --j)
                synthesis_state_[j] = synthesis_state_[j - 1] * 0.999f;
            }
            synthesis_state_[0] = restored;
          }
          uint32_t c_sh_end = Profiler::now_cycles();
          total_state_shift_cycles += (c_sh_end - c_sh_start);

          uint32_t c_gm_start = Profiler::now_cycles();
          const float r2 = restored * restored;
          const float y2 = y_new * y_new;
          constexpr float kFastAlpha = 0.0035f; // ~6 ms at 48 kHz
          fast_lpc_energy_ += kFastAlpha * (r2 - fast_lpc_energy_);
          fast_psola_energy_ += kFastAlpha * (y2 - fast_psola_energy_);

          if (fast_lpc_energy_ > 1e-8f && fast_psola_energy_ > 1e-8f) {
            const float instant_ratio = std::sqrt(fast_psola_energy_ / fast_lpc_energy_);
            const float target_g = std::clamp(instant_ratio, 0.4f, 2.5f);
            if (target_g <= 0.405f || target_g >= 2.495f) {
              ++gain_rail_events_;
            }
            constexpr float kAttackAlpha = 0.0008f;  // ~26 ms at 48 kHz
            constexpr float kReleaseAlpha = 0.0002f; // ~104 ms at 48 kHz
            const float rate = (target_g > slow_gain_target_) ? kAttackAlpha : kReleaseAlpha;
            slow_gain_target_ += rate * (target_g - slow_gain_target_);
          }

          constexpr float kMaxGainSlewPerSample = 0.0005f;
          const float gain_delta = std::clamp(slow_gain_target_ - smoothed_gain_,
                                              -kMaxGainSlewPerSample, kMaxGainSlewPerSample);
          smoothed_gain_ += gain_delta;
          restored *= smoothed_gain_;
          last_gain_scale_ = smoothed_gain_;
          uint32_t c_gm_end = Profiler::now_cycles();
          total_gain_matcher_cycles += (c_gm_end - c_gm_start);

          uint32_t c_bl_start = Profiler::now_cycles();
          y_new += (restored - y_new) * formant_mix_;
          ++formant_frames_;
          max_restored_ = std::max(max_restored_, std::fabs(restored));
          uint32_t c_bl_end = Profiler::now_cycles();
          total_blend_cycles += (c_bl_end - c_bl_start);
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
    uint32_t c_onr_start = Profiler::now_cycles();
    ola_[oi] = lpc_ola_[oi] = norm_[oi] = 0;
    uint32_t c_onr_end = Profiler::now_cycles();
    total_ola_norm_reset_cycles += (c_onr_end - c_onr_start);
#ifndef ESP_PLATFORM
    measured_ola_[oi] = coasted_ola_[oi] = 0.0f;
    measured_norm_[oi] = coasted_norm_[oi] = 0.0f;
    norm_square_[oi] = 0.0f;
    overlap_[oi] = 0;
#endif
  }

  // B4C.8: close per-sample loop measurement (§8F).
  // Only count the UNMEASURED portion to avoid double-counting already-
  // profiled sub-sections (fallback, articulation, plosive, synthesis, etc.).
  const uint32_t c_loop_end = Profiler::now_cycles();
  const uint32_t loop_total = c_loop_end - c_loop_start;
  const uint32_t measured_in_loop = total_fallback_cycles + total_articulation_cycles +
                                    total_plosive_cycles + total_synthesis_cycles +
                                    total_state_shift_cycles + total_softclip_cycles +
                                    total_gain_matcher_cycles + total_blend_cycles +
                                    total_history_fetch_cycles + total_ola_norm_reset_cycles;
  const uint32_t loop_unmeasured = (loop_total > measured_in_loop)
                                       ? (loop_total - measured_in_loop) : 0;
  b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::PerSampleLoopControl)] += loop_unmeasured;
  b4c8_loop_history_fetch_cycles_ += total_history_fetch_cycles;
  b4c8_loop_ola_norm_reset_cycles_ += total_ola_norm_reset_cycles;
  b4c8_loop_indexing_cycles_ += loop_unmeasured;
  accounted_cycles_this_block_ += loop_unmeasured;

  const uint32_t accounted = total_fallback_cycles + total_articulation_cycles + total_plosive_cycles +
                             total_synthesis_cycles + total_state_shift_cycles + total_softclip_cycles +
                             total_gain_matcher_cycles + total_blend_cycles;
  accounted_cycles_this_block_ += accounted;

  profiler_.record_cycles(section(PitchShiftProfileSection::Fallback), total_fallback_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::Articulation), total_articulation_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::PlosiveBridge), total_plosive_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::LpcSynthesisAllPole), total_synthesis_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::LpcStateShift), total_state_shift_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::FormantSoftClip), total_softclip_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::FormantGainMatcher), total_gain_matcher_cycles, frames);
  profiler_.record_cycles(section(PitchShiftProfileSection::FormantBlend), total_blend_cycles, frames);

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

  // B4C.8: measure telemetry state update (§8N).
  const uint32_t c_telem_state_start = Profiler::now_cycles();
  // (debug_ assignments above are the telemetry state update; they are
  // already written. Measure the time from here to publish_telemetry end.)
  const uint32_t c_telem_start = Profiler::now_cycles();
  publish_telemetry();
  const uint32_t c_telem = Profiler::now_cycles() - c_telem_start;
  const uint32_t c_telem_state = Profiler::now_cycles() - c_telem_state_start;
  b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::TelemetryStateUpdate)] += c_telem_state;
  profiler_.record_cycles(section(PitchShiftProfileSection::Telemetry), c_telem, 1);
  accounted_cycles_this_block_ += c_telem;

  const uint32_t c_block_total_end = Profiler::now_cycles();
  last_block_cycles_ = c_block_total_end - c_block_total_start;
  if (s_b4d4_class_enabled && voice_index_ == 0 && s_b4d4_class_cycles) {
    const size_t bucket =
        grains_scheduled_this_block_ >= 3 ? 3 : grains_scheduled_this_block_;
    s_b4d4_class_cycles[4 * kB4D4Sections + bucket]++;
    for (size_t i = 0; i < kB4D4Sections; ++i) {
      const uint64_t now =
          profiler_.raw_total_cycles(
              section(static_cast<PitchShiftProfileSection>(i)));
      if (now >= b4d4_snapshot[i])
        s_b4d4_class_cycles[bucket * kB4D4Sections + i] +=
            now - b4d4_snapshot[i];
    }
  }
  const uint32_t other_cycles = (last_block_cycles_ > accounted_cycles_this_block_)
                                    ? (last_block_cycles_ - accounted_cycles_this_block_)
                                    : 0;
  // B4C.8: store the residual Other as "unattributed" (§8R).
  b4c8_other_cycles_[static_cast<size_t>(B4c8OtherCategory::OtherUnattributed)] += other_cycles;
  profiler_.record_cycles(section(PitchShiftProfileSection::Other), other_cycles, 1);

  // B4C.8: accumulate block totals for end-of-run summary.
  b4c8_total_blocks_++;
  b4c8_total_cycles_ += last_block_cycles_;
  if (last_block_cycles_ > b4c8_max_block_cycles_)
    b4c8_max_block_cycles_ = last_block_cycles_;
  b4c8_accounted_this_block_ += accounted;
  b4c8_total_this_block_ = last_block_cycles_;

  // B4C.8A: model-change paired comparison accumulators.
  if (model_changed_this_block_) {
    b4c8_model_change_blocks_++;
    b4c8_model_change_cycles_ += last_block_cycles_;
    if (last_block_cycles_ > b4c8_model_change_max_cycles_)
      b4c8_model_change_max_cycles_ = last_block_cycles_;
  } else {
    b4c8_no_model_change_blocks_++;
    b4c8_no_model_change_cycles_ += last_block_cycles_;
    if (last_block_cycles_ > b4c8_no_model_change_max_cycles_)
      b4c8_no_model_change_max_cycles_ = last_block_cycles_;
  }

  // B4C.8: slow-block detection (>1200 us = deadline).
  const uint32_t block_us = last_block_cycles_ / static_cast<uint32_t>(Profiler::cycles_per_us());
  if (block_us > 1200) {
    const uint8_t voice_mask = target_enabled_ ? 1 : 0;
    b4c8_record_slow_block(block_us,
                           static_cast<uint32_t>(telemetry_.blocks),
                           voice_mask,
                           pitch.frequency_hz,
                           static_cast<uint8_t>(mark_count));
  }

  // B4C.8: stall detection (>10 ms = 13333 us at 360 MHz/48 kHz).
  if (block_us > 10000) {
    b4c8_record_stall(block_us,
                      static_cast<uint32_t>(telemetry_.blocks),
                      target_enabled_ ? 1 : 0,
                      0);
  }

  VF_PROFILE_END(profiler_, section(PitchShiftProfileSection::Total),
                 static_cast<uint64_t>(1000000.0 * frames / sample_rate_));
}

GrainRejectionTelemetry TdPsola::grain_rejection_telemetry() const {
  GrainRejectionTelemetry result = grain_rejection_;
  const uint64_t input_end = resources_ ? resources_->input_end() : 0;
  result.input_end = input_end;
  result.oldest_available =
      input_end > kHistorySize ? input_end - kHistorySize : 0;
  return result;
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

// ═══════════════════════════════════════════════════════════════════════
// B4C.8: Voice-Other decomposition, slow-block recorder, stall detector.
// Observability only; no DSP behavior change.
// ═══════════════════════════════════════════════════════════════════════

void TdPsola::b4c8_init_recorders() {
  // Zero the Other cycle counters.
  for (size_t i = 0; i < kB4C8OtherCategories; ++i)
    b4c8_other_cycles_[i] = 0;
  b4c8_accounted_this_block_ = 0;
  b4c8_total_this_block_ = 0;
  b4c8_total_blocks_ = 0;
  b4c8_total_cycles_ = 0;
  b4c8_max_block_cycles_ = 0;
  b4c8_slow_block_count_ = 0;
  b4c8_stall_count_ = 0;
  slow_block_head_ = 0;
  slow_block_count_ = 0;
  slow_block_drops_ = 0;
  stall_head_ = 0;
  stall_count_ = 0;
  stall_drops_ = 0;

  if (!slow_block_ring_) {
#ifdef ESP_PLATFORM
    slow_block_ring_ = static_cast<B4c8SlowBlockRecord *>(heap_caps_calloc(
        kSlowBlockCapacity, sizeof(B4c8SlowBlockRecord),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    slow_block_ring_ = static_cast<B4c8SlowBlockRecord *>(
        std::calloc(kSlowBlockCapacity, sizeof(B4c8SlowBlockRecord)));
#endif
  }
  if (!stall_ring_) {
#ifdef ESP_PLATFORM
    stall_ring_ = static_cast<B4c8StallEvent *>(heap_caps_calloc(
        kStallCapacity, sizeof(B4c8StallEvent),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    stall_ring_ = static_cast<B4c8StallEvent *>(
        std::calloc(kStallCapacity, sizeof(B4c8StallEvent)));
#endif
  }
}

void TdPsola::b4c8_free_recorders() {
  if (slow_block_ring_) {
#ifdef ESP_PLATFORM
    heap_caps_free(slow_block_ring_);
#else
    std::free(slow_block_ring_);
#endif
    slow_block_ring_ = nullptr;
  }
  if (stall_ring_) {
#ifdef ESP_PLATFORM
    heap_caps_free(stall_ring_);
#else
    std::free(stall_ring_);
#endif
    stall_ring_ = nullptr;
  }
}

void TdPsola::b4c8_record_slow_block(uint32_t block_duration_us,
                                     uint32_t block_index,
                                     uint8_t voice_mask, float pitch_f0,
                                     uint8_t mark_count) {
  if (!slow_block_ring_) return;
  B4c8SlowBlockRecord &rec = slow_block_ring_[slow_block_head_];
  rec.timestamp_us = Profiler::now_us();
  rec.block_duration_us = block_duration_us;
  rec.block_index = block_index;
#ifdef ESP_PLATFORM
  rec.core = static_cast<uint8_t>(esp_cpu_get_core_id());
#else
  rec.core = 0;
#endif
  rec.voice_active_mask = voice_mask;
  rec.new_grains_v0 = static_cast<uint8_t>(grains_scheduled_this_block_);
  rec.new_grains_v1 = 0;  // caller fills for V1
  rec.active_descriptors_v0 = deferred_active_grains_this_block_;
  rec.active_descriptors_v1 = 0;
  rec.completed_grains_v0 = grains_rendered_this_block_;
  rec.completed_grains_v1 = 0;
  rec.deferred_slices_v0 = deferred_slices_rendered_this_block_;
  rec.deferred_slices_v1 = 0;
  rec.source_samples_v0 = grain_samples_processed_this_block_;
  rec.source_samples_v1 = 0;
  rec.fir_samples_v0 = fir_samples_computed_this_block_;
  rec.fir_samples_v1 = 0;
  rec.ola_samples = 0;
  rec.synthesis_samples = 0;
  rec.gainnorm_calls = 0;
  rec.model_changes = model_changed_this_block_;
  rec.mark_count = mark_count;
  rec.pitch_f0 = pitch_f0;
  rec.descriptor_create_calls = 0;
  // Major section totals from the profiler.
  auto sec_us = [&](PitchShiftProfileSection s) -> uint32_t {
    const auto st = profiler_.stats(section(s));
    return st.calls > 0 ? static_cast<uint32_t>(st.total_us / st.calls) : 0;
  };
  rec.section_lpc_window_ola_us = sec_us(PitchShiftProfileSection::LpcWindowOLA);
  rec.section_synthesis_iir_us = sec_us(PitchShiftProfileSection::LpcSynthesisAllPole);
  rec.section_residual_fir_us = sec_us(PitchShiftProfileSection::LpcResidualFIR);
  rec.section_state_shift_us = sec_us(PitchShiftProfileSection::LpcStateShift);
  rec.section_gain_match_us = sec_us(PitchShiftProfileSection::FormantGainMatcher);
  rec.section_pitch_sync_us = 0;
  rec.section_model_lookup_us = sec_us(PitchShiftProfileSection::LpcModelLookup);
  rec.section_mark_select_us = sec_us(PitchShiftProfileSection::MarkSelection);
  rec.section_other_combined_us = sec_us(PitchShiftProfileSection::Other);

  slow_block_head_ = (slow_block_head_ + 1) % kSlowBlockCapacity;
  if (slow_block_count_ < kSlowBlockCapacity)
    slow_block_count_++;
  else
    slow_block_drops_++;
  b4c8_slow_block_count_++;
}

void TdPsola::b4c8_record_stall(uint32_t block_duration_us,
                                uint32_t block_index, uint8_t voice_mask,
                                uint8_t section) {
  if (!stall_ring_) return;
  B4c8StallEvent &ev = stall_ring_[stall_head_];
  ev.timestamp_us = Profiler::now_us();
  ev.block_duration_us = block_duration_us;
  ev.block_index = block_index;
#ifdef ESP_PLATFORM
  ev.core = static_cast<uint8_t>(esp_cpu_get_core_id());
#else
  ev.core = 0;
#endif
  ev.voice_active_mask = voice_mask;
  ev.pre_stall_section = section;
  ev.pitch_sync_state = 0;
  ev.analysis_published = 0;
  ev.diag_ring_writes = 0;
  ev.psram_activity = 0;
  ev.i2s_state = 0;

  stall_head_ = (stall_head_ + 1) % kStallCapacity;
  if (stall_count_ < kStallCapacity)
    stall_count_++;
  else
    stall_drops_++;
  b4c8_stall_count_++;
}

bool TdPsola::b4c8_get_slow_block(size_t index, B4c8SlowBlockRecord *out) const {
  if (!out || !slow_block_ring_ || index >= slow_block_count_) return false;
  const size_t actual = (slow_block_count_ < kSlowBlockCapacity)
                            ? index
                            : (slow_block_head_ + index) % kSlowBlockCapacity;
  *out = slow_block_ring_[actual];
  return true;
}

bool TdPsola::b4c8_get_stall_event(size_t index, B4c8StallEvent *out) const {
  if (!out || !stall_ring_ || index >= stall_count_) return false;
  const size_t actual = (stall_count_ < kStallCapacity)
                            ? index
                            : (stall_head_ + index) % kStallCapacity;
  *out = stall_ring_[actual];
  return true;
}

void TdPsola::b4c8_print_other_breakdown(uint32_t voice_index) const {
  printf("B4C8_OTHER_VOICE voice=%lu accounted_cycles=%llu total_cycles=%llu "
         "other_cycles=%llu\n",
         static_cast<unsigned long>(voice_index),
         static_cast<unsigned long long>(b4c8_accounted_this_block_),
         static_cast<unsigned long long>(b4c8_total_this_block_),
         static_cast<unsigned long long>(
             b4c8_total_this_block_ > b4c8_accounted_this_block_
                 ? b4c8_total_this_block_ - b4c8_accounted_this_block_ : 0));
  // Legacy B4C.7 sections.
  for (size_t i = 0; i < kB4C7Sections; ++i) {
    printf("B4C8_VOICE_LEGACY voice=%lu section=%zu cycles=%llu\n",
           static_cast<unsigned long>(voice_index), i,
           static_cast<unsigned long long>(b4c7_section_cycles_[i]));
  }
  // Extended 18-category Other.
  for (size_t i = 0; i < kB4C8OtherCategories; ++i) {
    printf("B4C8_VOICE_OTHER voice=%lu cat=%zu cycles=%llu\n",
           static_cast<unsigned long>(voice_index), i,
           static_cast<unsigned long long>(b4c8_other_cycles_[i]));
  }
}

void TdPsola::b4c8_print_slow_blocks() const {
  printf("B4C8_SLOW_BLOCKS count=%llu drops=%lu\n",
         static_cast<unsigned long long>(b4c8_slow_block_count_),
         static_cast<unsigned long>(slow_block_drops_));
}

void TdPsola::b4c8_print_stall_events() const {
  printf("B4C8_STALLS count=%llu drops=%lu\n",
         static_cast<unsigned long long>(b4c8_stall_count_),
         static_cast<unsigned long>(stall_drops_));
}

void TdPsola::b4c8_print_summary() const {
  printf("B4C8_VOICE_SUMMARY blocks=%llu total_cycles=%llu "
         "max_block_cycles=%llu slow_blocks=%llu stalls=%llu\n",
         static_cast<unsigned long long>(b4c8_total_blocks_),
         static_cast<unsigned long long>(b4c8_total_cycles_),
         static_cast<unsigned long long>(b4c8_max_block_cycles_),
         static_cast<unsigned long long>(b4c8_slow_block_count_),
         static_cast<unsigned long long>(b4c8_stall_count_));
}

// ── B4C.8B: per-block recording and MC delta breakdown ──────────────────

// PSRAM buffer layout (all uint64_t):
//   [0] = block_head
//   [1] = block_count
//   [2 .. kSectionCount+1]                = MC section cycle accumulators
//   [kSectionCount+2 .. 2*kSectionCount+1] = NMC section cycle accumulators
//   [2*kSectionCount+2]                   = MC block count
//   [2*kSectionCount+3]                   = NMC block count
static constexpr size_t kB4c8bHeadOff = 0;
static constexpr size_t kB4c8bCntOff = 1;
static constexpr size_t kB4c8bMcOff = 2;
static constexpr size_t kB4c8bNmcOff = 2 + TdPsola::kSectionCount;
static constexpr size_t kB4c8bMcCnt = 2 + 2 * TdPsola::kSectionCount;
static constexpr size_t kB4c8bNmcCnt = 2 + 2 * TdPsola::kSectionCount + 1;
static constexpr size_t kB4c8bBufLen = 2 + 2 * TdPsola::kSectionCount + 2;

void TdPsola::b4c8b_init_recorder() {
  // Allocate section accumulators + metadata in PSRAM.
  const size_t buf_sz = kB4c8bBufLen * sizeof(uint64_t);
  if (!b4c8b_psram_buf_) {
#ifdef ESP_PLATFORM
    b4c8b_psram_buf_ = static_cast<uint64_t *>(
        heap_caps_calloc(1, buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    b4c8b_psram_buf_ = static_cast<uint64_t *>(
        std::calloc(1, buf_sz));
#endif
  } else {
    std::memset(b4c8b_psram_buf_, 0, buf_sz);
  }

  if (!b4c8b_block_ring_) {
#ifdef ESP_PLATFORM
    b4c8b_block_ring_ = static_cast<B4c8bBlockRecord *>(heap_caps_calloc(
        kBlockRecordCapacity, sizeof(B4c8bBlockRecord),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    b4c8b_block_ring_ = static_cast<B4c8bBlockRecord *>(
        std::calloc(kBlockRecordCapacity, sizeof(B4c8bBlockRecord)));
#endif
  }
}

void TdPsola::b4c8b_free_recorder() {
  if (b4c8b_psram_buf_) {
#ifdef ESP_PLATFORM
    heap_caps_free(b4c8b_psram_buf_);
#else
    std::free(b4c8b_psram_buf_);
#endif
    b4c8b_psram_buf_ = nullptr;
  }
  if (b4c8b_block_ring_) {
#ifdef ESP_PLATFORM
    heap_caps_free(b4c8b_block_ring_);
#else
    std::free(b4c8b_block_ring_);
#endif
    b4c8b_block_ring_ = nullptr;
  }
}

void TdPsola::b4c8b_record_block(uint32_t block_id, uint8_t voice_index,
                                  uint8_t voice_class, float pitch_f0) {
  // Accumulate MC/NMC section breakdown from the per-block profiler snapshot.
  if (b4c8b_psram_buf_) {
    for (size_t i = 0; i < kSectionCount; ++i) {
      const auto st = profiler_.stats(section(static_cast<PitchShiftProfileSection>(i)));
      const uint64_t cycles = st.calls > 0 ? static_cast<uint64_t>(st.total_us * Profiler::cycles_per_us()) : 0;
      if (model_changed_this_block_) {
        b4c8b_psram_buf_[kB4c8bMcOff + i] += cycles;
      } else {
        b4c8b_psram_buf_[kB4c8bNmcOff + i] += cycles;
      }
    }
    if (model_changed_this_block_)
      b4c8b_psram_buf_[kB4c8bMcCnt]++;
    else
      b4c8b_psram_buf_[kB4c8bNmcCnt]++;
  }

  // Store per-block record for contingency table.
  if (!b4c8b_block_ring_ || !b4c8b_psram_buf_) return;
  const uint32_t head = static_cast<uint32_t>(b4c8b_psram_buf_[kB4c8bHeadOff]);
  B4c8bBlockRecord &rec = b4c8b_block_ring_[head];
  rec.block_id = block_id;
  rec.voice_index = voice_index;
  rec.model_change = model_changed_this_block_;
  const uint32_t block_us = last_block_cycles_ / static_cast<uint32_t>(Profiler::cycles_per_us());
  rec.deadline_miss = (block_us > 1333) ? 1 : 0;
  rec.voice_class = voice_class;
  rec.dsp_cycles = last_block_cycles_;
  rec.new_grains = static_cast<uint8_t>(grains_scheduled_this_block_);
  rec.active_descriptors = deferred_active_grains_this_block_;
  rec.deferred_slices = deferred_slices_rendered_this_block_;
  rec.model_warp_expensive = model_warp_expensive_this_block_;
  rec.fir_samples = fir_samples_computed_this_block_;
  rec.pitch_f0 = pitch_f0;

  auto sec_cycles = [&](PitchShiftProfileSection s) -> uint32_t {
    const auto st = profiler_.stats(section(s));
    return st.calls > 0 ? static_cast<uint32_t>(st.total_us * Profiler::cycles_per_us()) : 0;
  };
  rec.sec_mark_sel = sec_cycles(PitchShiftProfileSection::MarkSelection);
  rec.sec_grain_sched = sec_cycles(PitchShiftProfileSection::GrainScheduling);
  rec.sec_hist_lookup = sec_cycles(PitchShiftProfileSection::GrainHistoryLookup);
  rec.sec_model_lookup = sec_cycles(PitchShiftProfileSection::LpcModelLookup);
  rec.sec_warp_poly = sec_cycles(PitchShiftProfileSection::LpcModelWarpPolynomial);
  rec.sec_warp_gainnorm = sec_cycles(PitchShiftProfileSection::LpcModelWarpGainNorm);
  rec.sec_residual_fir = sec_cycles(PitchShiftProfileSection::LpcResidualFIR);
  rec.sec_window_ola = sec_cycles(PitchShiftProfileSection::LpcWindowOLA);
  rec.sec_synth = sec_cycles(PitchShiftProfileSection::LpcSynthesisAllPole);
  rec.sec_state_shift = sec_cycles(PitchShiftProfileSection::LpcStateShift);
  rec.sec_gain_match = sec_cycles(PitchShiftProfileSection::FormantGainMatcher);
  rec.sec_soft_clip = sec_cycles(PitchShiftProfileSection::FormantSoftClip);
  rec.sec_blend = sec_cycles(PitchShiftProfileSection::FormantBlend);
  rec.sec_fallback = sec_cycles(PitchShiftProfileSection::Fallback);
  rec.sec_articulation = sec_cycles(PitchShiftProfileSection::Articulation);
  rec.sec_plosive = sec_cycles(PitchShiftProfileSection::PlosiveBridge);
  rec.sec_telemetry = sec_cycles(PitchShiftProfileSection::Telemetry);
  rec.sec_other = sec_cycles(PitchShiftProfileSection::Other);

  const uint32_t new_head = (head + 1) % kBlockRecordCapacity;
  b4c8b_psram_buf_[kB4c8bHeadOff] = new_head;
  const uint32_t cnt = static_cast<uint32_t>(b4c8b_psram_buf_[kB4c8bCntOff]);
  if (cnt < kBlockRecordCapacity)
    b4c8b_psram_buf_[kB4c8bCntOff] = cnt + 1;
}

bool TdPsola::b4c8b_get_block_record(size_t index, B4c8bBlockRecord *out) const {
  if (!out || !b4c8b_block_ring_ || !b4c8b_psram_buf_) return false;
  const uint32_t cnt = static_cast<uint32_t>(b4c8b_psram_buf_[kB4c8bCntOff]);
  if (index >= cnt) return false;
  const uint32_t head = static_cast<uint32_t>(b4c8b_psram_buf_[kB4c8bHeadOff]);
  const size_t actual = (cnt < kBlockRecordCapacity)
                            ? index
                            : (head + index) % kBlockRecordCapacity;
  *out = b4c8b_block_ring_[actual];
  return true;
}

void TdPsola::b4c8b_print_block_records() const {
  const uint32_t cnt = b4c8b_psram_buf_
      ? static_cast<uint32_t>(b4c8b_psram_buf_[kB4c8bCntOff]) : 0;
  printf("B4C8B_BLOCK_RECORDS count=%lu\n",
         static_cast<unsigned long>(cnt));
}

void TdPsola::b4c8b_print_mc_delta_breakdown() const {
  const uint64_t mc_n = b4c8b_psram_buf_ ? b4c8b_psram_buf_[kB4c8bMcCnt] : 0;
  const uint64_t nmc_n = b4c8b_psram_buf_ ? b4c8b_psram_buf_[kB4c8bNmcCnt] : 0;
  printf("B4C8B_MC_DELTA mc_blocks=%llu nmc_blocks=%llu\n",
         static_cast<unsigned long long>(mc_n),
         static_cast<unsigned long long>(nmc_n));
  if (!b4c8b_psram_buf_) return;
  for (size_t i = 0; i < kSectionCount; ++i) {
    const double mc_avg = mc_n > 0
        ? static_cast<double>(b4c8b_psram_buf_[kB4c8bMcOff + i]) / mc_n : 0.0;
    const double nmc_avg = nmc_n > 0
        ? static_cast<double>(b4c8b_psram_buf_[kB4c8bNmcOff + i]) / nmc_n : 0.0;
    printf("B4C8B_SECTION idx=%zu mc_avg_cycles=%.2f nmc_avg_cycles=%.2f delta_cycles=%.2f\n",
           i, mc_avg, nmc_avg, mc_avg - nmc_avg);
  }
}

// B4D.2 warp-cache audit accessors (file scope, no engine .bss growth).
uint64_t td_psola_warp_miss_total() { return s_warp_miss_total; }
uint64_t td_psola_warp_math_only_miss() { return s_warp_math_only_miss; }
void td_psola_reset_warp_audit() { s_warp_miss_total = 0; s_warp_math_only_miss = 0; }

// B4D.4 per-grain-count section attribution accessors.
bool td_psola_b4d4_class_init() {
  if (!s_b4d4_class_cycles) {
#ifdef ESP_PLATFORM
    s_b4d4_class_cycles = static_cast<uint64_t *>(heap_caps_calloc(
        (4 * kB4D4Sections + 4), sizeof(uint64_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    s_b4d4_class_cycles = static_cast<uint64_t *>(
        std::calloc(4 * kB4D4Sections + 4, sizeof(uint64_t)));
#endif
  }
  if (!s_b4d4_class_cycles) return false;
  std::memset(s_b4d4_class_cycles, 0, (4 * kB4D4Sections + 4) * sizeof(uint64_t));
  s_b4d4_class_enabled = true;
  return true;
}
void td_psola_b4d4_class_free() {
  if (s_b4d4_class_cycles) {
#ifdef ESP_PLATFORM
    heap_caps_free(s_b4d4_class_cycles);
#else
    std::free(s_b4d4_class_cycles);
#endif
    s_b4d4_class_cycles = nullptr;
  }
  s_b4d4_class_enabled = false;
}
uint64_t td_psola_b4d4_class_cycles(size_t bucket, size_t sec) {
  if (!s_b4d4_class_cycles || bucket >= 4 || sec >= kB4D4Sections) return 0;
  return s_b4d4_class_cycles[bucket * kB4D4Sections + sec];
}
uint64_t td_psola_b4d4_class_blocks(size_t bucket) {
  if (!s_b4d4_class_cycles || bucket >= 4) return 0;
  return s_b4d4_class_cycles[4 * kB4D4Sections + bucket];
}

// B4D.5 mark-selection decomposition accessors.
void td_psola_b4d5_mark_audit_reset() {
  s_b4d5_mark_calls = 0;
  s_b4d5_mark_candidates = 0;
  s_b4d5_mark_scan_cycles = 0;
  s_b4d5_mark_total_cycles = 0;
}
void td_psola_b4d5_mark_audit_enable(bool on) {
  s_b4d5_mark_audit_enabled = on;
}
bool td_psola_b4d5_mark_audit_enabled() { return s_b4d5_mark_audit_enabled; }
void td_psola_b4d5_mark_audit(uint64_t *calls, uint64_t *candidates,
                              uint64_t *scan_cycles, uint64_t *total_cycles) {
  if (calls) *calls = s_b4d5_mark_calls;
  if (candidates) *candidates = s_b4d5_mark_candidates;
  if (scan_cycles) *scan_cycles = s_b4d5_mark_scan_cycles;
  if (total_cycles) *total_cycles = s_b4d5_mark_total_cycles;
}

// B4D.5 test hook: the exact production nearest-mark search.
size_t td_psola_nearest_mark_index(double source, const PitchMark *marks,
                                   size_t count) {
  return nearest_mark_index(source, marks, count);
}
