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
#include <cstring>

struct FormantWarpCache {
  uint64_t model_timestamp = 0;
  uint16_t order = 0;
  uint32_t lambda_bits = 0;
  uint32_t gamma_bits = 0;
  uint32_t sample_rate_bits = 0;
  uint8_t norm_strategy = 0;
  bool valid = false;
  float formant_filter_gain = 1.0f;
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> source_coefficients{};
  std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> warped_coefficients{};

  static uint32_t f2b(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof(b));
    return b;
  }

  bool is_hit(uint64_t ts, uint16_t ord, const float *source, float lambda,
              float gamma, FormantNormalizationStrategy strat,
              float sample_rate) const {
    if (!source || ord > VOCAL_FX_LPC_MAX_ORDER) return false;
    if (!valid || model_timestamp != ts || order != ord ||
        lambda_bits != f2b(lambda) || gamma_bits != f2b(gamma) ||
        sample_rate_bits != f2b(sample_rate) ||
        norm_strategy != static_cast<uint8_t>(strat)) return false;
    for (size_t i = 0; i <= ord; ++i) {
      if (f2b(source_coefficients[i]) != f2b(source[i])) return false;
    }
    return true;
  }

  uint8_t difference(uint64_t ts, uint16_t ord, const float *source,
                     float lambda, float gamma,
                     FormantNormalizationStrategy strat,
                     float sample_rate) const {
    if (!valid) return 1;
    uint8_t bits = 0;
    if (model_timestamp != ts) bits |= 2;
    if (order != ord) bits |= 4;
    if (lambda_bits != f2b(lambda)) bits |= 16;
    if (gamma_bits != f2b(gamma)) bits |= 32;
    if (sample_rate_bits != f2b(sample_rate)) bits |= 64;
    if (norm_strategy != static_cast<uint8_t>(strat)) bits |= 128;
    if (!source || ord > VOCAL_FX_LPC_MAX_ORDER) return bits | 8;
    for (size_t i = 0; i <= ord; ++i)
      if (f2b(source_coefficients[i]) != f2b(source[i])) { bits |= 8; break; }
    return bits;
  }

  void update(uint64_t ts, uint16_t ord, const float *source, float lambda, float gamma,
              FormantNormalizationStrategy strat,
              const std::array<float, VOCAL_FX_LPC_MAX_ORDER + 1> &coeffs,
              float gain, float sample_rate) {
    model_timestamp = ts;
    order = ord;
    lambda_bits = f2b(lambda);
    gamma_bits = f2b(gamma);
    sample_rate_bits = f2b(sample_rate);
    norm_strategy = static_cast<uint8_t>(strat);
    source_coefficients.fill(0.0f);
    if (source) std::memcpy(source_coefficients.data(), source, (ord + 1) * sizeof(float));
    warped_coefficients = coeffs;
    formant_filter_gain = gain;
    valid = true;
  }

  void reset() {
    valid = false;
    model_timestamp = 0;
    sample_rate_bits = 0;
    source_coefficients.fill(0.0f);
  }

  // B4D.2 (§7): true when every mathematical input matches the cached entry,
  // i.e. only model_timestamp differs. Observability only; cache semantics are
  // unchanged.
  bool math_equal(uint16_t ord, const float *source, float lambda, float gamma,
                  FormantNormalizationStrategy strat, float sample_rate) const {
    if (!source || ord > VOCAL_FX_LPC_MAX_ORDER || !valid) return false;
    if (order != ord || lambda_bits != f2b(lambda) ||
        gamma_bits != f2b(gamma) ||
        sample_rate_bits != f2b(sample_rate) ||
        norm_strategy != static_cast<uint8_t>(strat))
      return false;
    for (size_t i = 0; i <= ord; ++i)
      if (f2b(source_coefficients[i]) != f2b(source[i])) return false;
    return true;
  }
};

struct SourceResidualCacheEntry {
  SourceGrainKey key{};
  int half_window = 0;
  uint32_t sample_count = 0;
  uint32_t last_used_generation = 0;
  float *residual = nullptr;          // points into buffer: length 2*half + 1
  float *windowed_residual = nullptr; // for Candidate B
  float *windowed_plain = nullptr;    // for Candidate B
  float *window = nullptr;            // for Candidate B
  bool valid = false;
};

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
  static constexpr size_t kMaxGrainScratch = 2048;
  const float* get_contiguous_history(uint64_t center, uint32_t half, size_t order);
  FormantWarpCache shared_warp_cache{};

  struct RecentGrainEntry {
    SourceGrainKey key{};
    uint8_t voice_id = 0;
    uint32_t block_index = 0;
  };
  static constexpr size_t kRecentGrainsCap = 64;
  std::array<RecentGrainEntry, kRecentGrainsCap> recent_grains_{};
  size_t recent_grains_count_ = 0;
  size_t recent_grains_head_ = 0;

  void register_source_grain(const SourceGrainKey &key, uint8_t voice, uint32_t blk,
                             bool *out_same, bool *out_cross) {
    bool same = false;
    bool cross = false;
    for (size_t i = 0; i < recent_grains_count_; ++i) {
      if (recent_grains_[i].key == key) {
        if (recent_grains_[i].voice_id == voice) same = true;
        else cross = true;
      }
    }
    if (out_same) *out_same = same;
    if (out_cross) *out_cross = cross;

    recent_grains_[recent_grains_head_] = {key, voice, blk};
    recent_grains_head_ = (recent_grains_head_ + 1) % kRecentGrainsCap;
    if (recent_grains_count_ < kRecentGrainsCap) {
      recent_grains_count_++;
    }
  }

  void reset_audit_forensics() {
    recent_grains_count_ = 0;
    recent_grains_head_ = 0;
    shared_source_grain_audit.reset();
    shared_scheduling_slack_audit.reset();
  }

  PsolaSourceGrainAudit shared_source_grain_audit{};
  PsolaSchedulingSlackAudit shared_scheduling_slack_audit{};

  // B4C.7 exact slice-level reuse audit (observability only; no DSP change).
  // Deferred slices carry the full descriptor identity plus the rendered clip
  // [n_min, n_max]. Matches require the complete SourceGrainKey AND the clip,
  // so shared FIR work is bit-identical by construction. Fixed capacity, no
  // allocation; disabled by default (zero production impact when off).
  struct SliceAuditEntry {
    SourceGrainKey key{};
    int16_t n_min = 0;
    int16_t n_max = 0;
    uint8_t voice_id = 0;
    uint16_t slice_samples = 0;
    uint16_t fir_taps = 0;  // slice_samples * order (0 when not LPC)
    uint32_t block_gen = 0;
  };
  // Link-budget constrained: the whole vocal_fx Engine `e` is ONE .bss
  // input section that must share sram_high (384 KB); even 1.5 KB extra
  // breaks the link, so ring storage is PSRAM-allocated on enable (cold
  // path) and freed on disable. Hot-path record cost (~1 us/slice) applies
  // ONLY to diagnostic windows; qualification windows keep the recorder
  // disabled (single branch, zero contamination). 32 entries exactly cover
  // the architectural maximum (16 descriptors/voice x 2 voices x 1 slice
  // each = 32/block), so entry_drops must stay 0.
  static constexpr size_t kSliceAuditCap = 32;
  SliceAuditEntry *slice_ring_ = nullptr;
  size_t slice_audit_count_ = 0;
  uint32_t slice_audit_gen_ = 0;
  bool slice_audit_enabled_ = false;
  uint64_t slice_audit_entry_drops_ = 0;
  // Cumulative case totals (audio loop snapshots deltas per block).
  uint64_t slice_audit_requested_ = 0;
  uint64_t slice_audit_duplicates_ = 0;
  uint64_t slice_audit_cross_voice_ = 0;
  uint64_t slice_audit_same_voice_ = 0;
  uint64_t slice_audit_samples_ = 0;
  uint64_t slice_audit_reusable_samples_ = 0;
  uint64_t slice_audit_taps_ = 0;
  uint64_t slice_audit_reusable_taps_ = 0;

  void set_slice_audit_enabled(bool enabled);
  bool slice_audit_enabled() const { return slice_audit_enabled_; }
  void reset_slice_audit() {
    slice_audit_count_ = 0;
    slice_audit_gen_ = 0;
    slice_audit_entry_drops_ = 0;
    slice_audit_requested_ = 0;
    slice_audit_duplicates_ = 0;
    slice_audit_cross_voice_ = 0;
    slice_audit_same_voice_ = 0;
    slice_audit_samples_ = 0;
    slice_audit_reusable_samples_ = 0;
    slice_audit_taps_ = 0;
    slice_audit_reusable_taps_ = 0;
  }
  void next_slice_audit_block() {
    slice_audit_gen_++;
    slice_audit_count_ = 0;
  }
  void record_slice_render(const SourceGrainKey &key, int n_min, int n_max,
                           uint8_t voice, uint16_t samples, uint16_t taps);

private:
  std::array<float, kHistorySize> history_{};
  std::array<float, kHannSize> hann_{};
  std::array<float, kMaxGrainScratch> grain_scratch_{};
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
  static constexpr size_t kMaxResidualCacheCapacity = 16;
  static constexpr size_t kMaxGrainSamples = 1601; // half <= 800 (period <= 800 samples)
  static constexpr size_t kPrecomputeQueueCap = 4;

  ~TdPsola();
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
  void set_kernels(PsolaLpcKernel lpc, PsolaOlaKernel ola,
                   PsolaGrainKernel grain, PsolaSynthesisKernel synth) {
    lpc_kernel_ = lpc;
    ola_kernel_ = ola;
    grain_kernel_ = grain;
    synthesis_kernel_ = synth;
  }
  PsolaLpcKernel lpc_kernel() const { return lpc_kernel_; }
  PsolaOlaKernel ola_kernel() const { return ola_kernel_; }
  PsolaGrainKernel grain_kernel() const { return grain_kernel_; }
  PsolaSynthesisKernel synthesis_kernel() const { return synthesis_kernel_; }

  void set_warp_cache_enabled(bool enabled) { warp_cache_enabled_ = enabled; }
  bool warp_cache_enabled() const { return warp_cache_enabled_; }
  void set_shared_warp_cache_enabled(bool enabled) { shared_warp_cache_enabled_ = enabled; }
  bool shared_warp_cache_enabled() const { return shared_warp_cache_enabled_; }
  void set_neutral_warp_fast_path_enabled(bool enabled) { neutral_warp_fast_path_enabled_ = enabled; }
  bool neutral_warp_fast_path_enabled() const { return neutral_warp_fast_path_enabled_; }
  PsolaWarpCacheStats warp_cache_stats() const { return warp_stats_; }
  void reset_warp_cache_stats() { warp_stats_ = PsolaWarpCacheStats{}; }
  void reset_profiler() { profiler_.reset(); }
  void reset_forensic_stats() {
    profiler_.reset();
    warp_stats_ = PsolaWarpCacheStats{};
    warp_audit_.reset();
    residual_cache_stats_.reset();
    precompute_stats_.reset();
  }
  const PsolaModelWarpAudit &model_warp_audit() const { return warp_audit_; }

  void configure_source_residual_cache(size_t entries, bool enable_residual, bool enable_windowed, bool force_psram = false);
  void set_residual_cache_enabled(bool enabled) { residual_cache_enabled_ = enabled; }
  bool residual_cache_enabled() const { return residual_cache_enabled_; }
  void set_windowed_cache_enabled(bool enabled) { windowed_cache_enabled_ = enabled; }
  bool windowed_cache_enabled() const { return windowed_cache_enabled_; }
  size_t residual_cache_configured_size() const { return residual_cache_configured_size_; }
  bool residual_cache_is_psram() const { return residual_cache_in_psram_; }
  size_t residual_cache_bytes() const { return residual_cache_pool_bytes_; }
  const void* residual_cache_pool_ptr() const { return residual_cache_pool_; }
  void set_precompute_enabled(bool enabled, float horizon = 1.0f) { precompute_enabled_ = enabled; precompute_horizon_blocks_ = horizon; }
  bool precompute_enabled() const { return precompute_enabled_; }
  PsolaSourceResidualCacheStats residual_cache_stats() const { return residual_cache_stats_; }
  void reset_residual_cache_stats() { residual_cache_stats_.reset(); }
  PsolaPrecomputeStats precompute_stats() const { return precompute_stats_; }
  void reset_precompute_stats() { precompute_stats_.reset(); }

  uint8_t last_model_warp_calls() const { return model_warp_calls_this_block_; }
  uint8_t last_model_warp_lookups() const { return model_warp_calls_this_block_; }
  uint8_t last_model_warp_expensive_calls() const { return model_warp_expensive_this_block_; }
  uint32_t last_model_warp_cycles() const { return model_warp_cycles_this_block_; }
  uint64_t last_model_timestamp() const { return last_model_timestamp_; }
  uint8_t last_model_changed() const { return model_changed_this_block_; }
  uint8_t last_grains_scheduled() const { return grains_scheduled_this_block_; }
  uint16_t last_schedule_attempts() const { return schedule_attempts_this_block_; }
  uint32_t last_sched_cycles() const { return b4d7_sched_cycles_this_block_; }
  uint32_t last_addgrain_cycles() const { return b4d7_addgrain_cycles_this_block_; }
  uint32_t last_deferred_cycles() const { return b4d7_deferred_cycles_this_block_; }
  uint8_t last_model_new_count() const { return b4d7_model_new_this_block_; }
  uint8_t last_warp_hit_count() const { return b4d7_warp_hit_this_block_; }
  uint8_t last_warp_miss_count() const { return b4d7_warp_miss_this_block_; }
  uint32_t last_mark_cycles() const { return b4d7_mark_cycles_this_block_; }
  uint32_t last_desc_cycles() const { return b4d7_desc_cycles_this_block_; }
  uint32_t last_near_cycles() const { return b4d7_near_cycles_this_block_; }
  uint32_t last_poly_cycles() const { return b4d7_poly_cycles_this_block_; }
  uint32_t last_gn_cycles() const { return b4d7_gn_cycles_this_block_; }
  uint32_t last_cl_cycles() const { return b4d7_cl_cycles_this_block_; }
  uint32_t last_ch_cycles() const { return b4d7_ch_cycles_this_block_; }
  uint32_t ord_mark(size_t i) const { return i < 4 ? b4d8_ord_mark[i] : 0; }
  uint32_t ord_warp(size_t i) const { return i < 4 ? b4d8_ord_warp[i] : 0; }
  uint32_t ord_desc(size_t i) const { return i < 4 ? b4d8_ord_desc[i] : 0; }
  uint32_t ord_near(size_t i) const { return i < 4 ? b4d8_ord_near[i] : 0; }
  uint32_t ord_sel(size_t i) const { return i < 4 ? b4d8_ord_sel[i] : 0; }
  uint32_t ord_align(size_t i) const { return i < 4 ? b4d8_ord_align[i] : 0; }
  const PsolaGrainAuditRecord &grain_audit(size_t i) const {
    return grain_audit_[i < 4 ? i : 0];
  }
  PsolaPredictionCursor prediction_cursor() const;
  uint16_t last_mark_count() const { return b4d8_mark_count_this_block_; }
  uint32_t last_prewarm_cycles() const { return b4d9_prewarm_cycles_this_block_; }
  int32_t last_debt_samples() const { return b4d11_debt_samples_; }
  uint32_t last_output_period_q8() const { return b4d11_output_period_q8_; }
  uint8_t grain_geom_count() const { return b4d11_g_count_; }
  int32_t grain_dest(size_t i) const { return i < 4 ? b4d11_g_dest_[i] : 0; }
  uint16_t grain_half(size_t i) const { return i < 4 ? b4d11_g_half_[i] : 0; }
  uint32_t last_warp_phase_cycles() const { return model_warp_cycles_this_block_; }
  uint8_t last_grains_rendered() const { return grains_rendered_this_block_; }
  uint8_t last_source_grains_built() const { return source_grains_built_this_block_; }
  uint8_t last_unique_source_grains() const { return unique_source_grains_this_block_; }
  uint8_t last_duplicate_source_grains() const { return duplicate_source_grains_this_block_; }
  uint16_t last_grain_samples_processed() const { return grain_samples_processed_this_block_; }
  uint16_t last_unique_grain_samples() const { return unique_grain_samples_this_block_; }
  float last_render_slack_blocks_min() const { return render_slack_blocks_min_this_block_; }
  uint32_t last_block_cycles() const { return last_block_cycles_; }
  // B4C.7 LPC-order forensic gate (observability only): effective runtime
  // synthesis order of this voice (0 when the current model is invalid/LPC off).
  uint16_t synthesis_order() const {
    return grain_model_.valid ? grain_model_.order : 0;
  }
  // B4C.7 Other-bucket decomposition (§35) as plain cumulative cycle
  // counters (NOT profiler sections: 5 more Profiler sections would cost
  // ~2.5 KB engine .bss and break the sram_high link budget). Indices:
  // 0 DescriptorSetup, 1 HistoryFetch, 2 HannWindow, 3 OlaNorm,
  // 4 RecoveryXfade. Observability only; DSP arithmetic untouched.
  static constexpr size_t kB4C7Sections = 5;
  uint64_t b4c7_section_cycles_[kB4C7Sections] = {};
  uint64_t b4c7_section_cycles(size_t idx) const {
    return idx < kB4C7Sections ? b4c7_section_cycles_[idx] : 0;
  }
  // B4C.8 Other decomposition: 18 non-overlapping subcategory cycle counters.
  // Plain uint64_t to avoid Profiler section overhead and .bss pressure.
  static constexpr size_t kB4C8OtherCategories = static_cast<size_t>(B4c8OtherCategory::Count);
  uint64_t b4c8_other_cycles_[kB4C8OtherCategories] = {};
  uint64_t b4c8_other_cycles(size_t idx) const {
    return idx < kB4C8OtherCategories ? b4c8_other_cycles_[idx] : 0;
  }
  uint32_t b4c8_accounted_this_block_ = 0;
  uint32_t b4c8_total_this_block_ = 0;

  // B4C.8A: Deferred traversal zero-vs-active split.
  uint64_t b4c8_deferred_zero_cycles_ = 0;
  uint64_t b4c8_deferred_zero_calls_ = 0;
  uint64_t b4c8_deferred_active_cycles_ = 0;
  uint64_t b4c8_deferred_active_calls_ = 0;

  // B4C.8A: PerSampleLoopControl sub-decomposition.
  uint64_t b4c8_loop_history_fetch_cycles_ = 0;
  uint64_t b4c8_loop_ola_norm_reset_cycles_ = 0;
  uint64_t b4c8_loop_indexing_cycles_ = 0;

  // B4C.8A: Model-change paired comparison.
  uint64_t b4c8_model_change_blocks_ = 0;
  uint64_t b4c8_model_change_cycles_ = 0;
  uint64_t b4c8_no_model_change_blocks_ = 0;
  uint64_t b4c8_no_model_change_cycles_ = 0;
  uint64_t b4c8_model_change_max_cycles_ = 0;
  uint64_t b4c8_no_model_change_max_cycles_ = 0;

  // B4C.8B: MC delta section breakdown — PSRAM-allocated.
  static constexpr size_t kSectionCount = static_cast<size_t>(PitchShiftProfileSection::Count);
  static constexpr size_t kBlockRecordCapacity = 48000;
  // PSRAM layout: [mc_kSectionCount] [nmc_kSectionCount] [mc_count] [nmc_count]
  uint64_t *b4c8b_psram_buf_ = nullptr;
  B4c8bBlockRecord *b4c8b_block_ring_ = nullptr;

  // B4C.8 slow-block recorder: fixed ring, no allocation during timed window.
  static constexpr size_t kSlowBlockCapacity = 128;
  B4c8SlowBlockRecord *slow_block_ring_ = nullptr;
  uint32_t slow_block_head_ = 0;
  uint32_t slow_block_count_ = 0;
  uint32_t slow_block_drops_ = 0;

  // B4C.8 stall detector: blocks >10 ms.
  static constexpr size_t kStallCapacity = 64;
  B4c8StallEvent *stall_ring_ = nullptr;
  uint32_t stall_head_ = 0;
  uint32_t stall_count_ = 0;
  uint32_t stall_drops_ = 0;

  // B4C.8 cumulative totals for end-of-run summary.
  uint64_t b4c8_total_blocks_ = 0;
  uint64_t b4c8_total_cycles_ = 0;
  uint64_t b4c8_max_block_cycles_ = 0;
  uint64_t b4c8_slow_block_count_ = 0;
  uint64_t b4c8_stall_count_ = 0;

  void b4c8_init_recorders();
  void b4c8_free_recorders();
  void b4c8_record_slow_block(uint32_t block_duration_us, uint32_t block_index,
                              uint8_t voice_mask, float pitch_f0, uint8_t mark_count);
  void b4c8_record_stall(uint32_t block_duration_us, uint32_t block_index,
                         uint8_t voice_mask, uint8_t section);
  void b4c8_print_slow_blocks() const;
  void b4c8_print_stall_events() const;
  void b4c8_print_other_breakdown(uint32_t voice_index) const;
  void b4c8_print_summary() const;
  bool b4c8_get_slow_block(size_t index, B4c8SlowBlockRecord *out) const;
  bool b4c8_get_stall_event(size_t index, B4c8StallEvent *out) const;

  // B4C.8B: Per-block recording and MC delta breakdown.
  void b4c8b_init_recorder();
  void b4c8b_free_recorder();
  void b4c8b_record_block(uint32_t block_id, uint8_t voice_index,
                           uint8_t voice_class, float pitch_f0);
  void b4c8b_print_block_records() const;
  void b4c8b_print_mc_delta_breakdown() const;
  size_t b4c8b_block_record_count() const { return 0; }
  bool b4c8b_get_block_record(size_t index, B4c8bBlockRecord *out) const;

  uint8_t last_residual_cache_hits() const { return residual_cache_hits_this_block_; }
  uint8_t last_residual_cache_misses() const { return residual_cache_misses_this_block_; }
  uint16_t last_fir_samples_computed() const { return fir_samples_computed_this_block_; }
  uint16_t last_fir_samples_reused() const { return fir_samples_reused_this_block_; }
  uint8_t last_active_overlapping_grains() const { return active_overlapping_grains_this_block_; }
  uint8_t last_precompute_queue_depth() const { return precompute_queue_depth_this_block_; }
  uint8_t last_precompute_expired_count() const { return precompute_expired_this_block_; }

  // Stage B4C.4: Grain render mode and FIR kernels
  void set_grain_render_mode(PsolaGrainRenderMode mode) { grain_render_mode_ = mode; }
  PsolaGrainRenderMode grain_render_mode() const { return grain_render_mode_; }

  void set_fir_kernel(PsolaFirKernel kernel) { fir_kernel_ = kernel; }
  PsolaFirKernel fir_kernel() const { return fir_kernel_; }

  PsolaDeferredStats deferred_stats() const { return deferred_stats_; }
  void reset_deferred_stats() { deferred_stats_.reset(); }

  uint8_t last_deferred_active_grains() const { return deferred_active_grains_this_block_; }
  uint8_t last_deferred_slices_rendered() const { return deferred_slices_rendered_this_block_; }
  uint16_t last_deferred_fir_samples() const { return deferred_fir_samples_this_block_; }
  const void *deferred_grains_ptr() const { return deferred_grains_; }
  size_t deferred_grains_bytes() const { return kMaxDeferredGrains * sizeof(DeferredGrainDescriptor); }

  static SingleGrainBenchmarkResult benchmark_single_grain(
      size_t grain_length, size_t order,
      SharedPitchShiftResources &resources,
      SharedLpcAnalysis &lpc,
      PsolaFirKernel kernel = PsolaFirKernel::Multi8);
  bool enabled() const { return target_enabled_; }
  bool is_releasing() const { return release_remaining_ > 0; }
  bool fallback_active() const { return state_ == PitchShiftState::Fallback || !target_enabled_ || psola_gain_ < 0.99f; }
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
  void add_grain_reference(int n_min, int n_max, int64_t dst, int half,
                           uint64_t center, const SharedLpcModel &model,
                           bool use_lpc, bool mark_predicted);
  void add_grain_contiguous_exact(int n_min, int n_max, int64_t dst, int half,
                                  const float *src, const SharedLpcModel &model,
                                  bool use_lpc, bool mark_predicted);
  void add_grain_contiguous_multi4(int n_min, int n_max, int64_t dst, int half,
                                   const float *src, const SharedLpcModel &model,
                                   bool use_lpc, bool mark_predicted);
  void add_grain_contiguous_multi8(int n_min, int n_max, int64_t dst, int half,
                                   const float *src, const SharedLpcModel &model,
                                   bool use_lpc, bool mark_predicted);
  void add_grain_ola_contiguous(int n_min, int n_max, int64_t dst, int half,
                                const float *src, const SharedLpcModel &model,
                                bool use_lpc, bool mark_predicted);
  void add_grain_combined_multi4(int n_min, int n_max, int64_t dst, int half,
                                 const float *src, const SharedLpcModel &model,
                                 bool use_lpc, bool mark_predicted);
  void add_grain_combined_multi8(int n_min, int n_max, int64_t dst, int half,
                                 const float *src, const SharedLpcModel &model,
                                 bool use_lpc, bool mark_predicted);
  static void compute_lpc_residual_fir_contiguous_scalar(
      const float *src, size_t count, const SharedLpcModel &model, float *residual_out);
  static void compute_lpc_residual_fir_multi2(
      const float *src, size_t count, const SharedLpcModel &model, float *residual_out);
  static void compute_lpc_residual_fir_multi4(
      const float *src, size_t count, const SharedLpcModel &model, float *residual_out);
  static void compute_lpc_residual_fir_multi8(
      const float *src, size_t count, const SharedLpcModel &model, float *residual_out);
  static void compute_lpc_residual_fir_multi8(
      const float *src, int half, const SharedLpcModel &model, float *residual_out);
  static void compute_lpc_residual_fir_multi_fma(
      const float *src, size_t count, const SharedLpcModel &model, float *residual_out);
  static void compute_fir_by_kernel(
      PsolaFirKernel kernel, const float *src, size_t count,
      const SharedLpcModel &model, float *residual_out);

  struct DeferredGrainDescriptor {
    uint32_t sequence_id = 0;
    int64_t destination = 0;
    uint64_t center = 0;
    int half = 0;
    SharedLpcModel grain_model{};
    float lambda = 0.0f;
    float gamma = 1.0f;
    FormantNormalizationStrategy norm_strategy = FormantNormalizationStrategy::StrategyC_IntegratedSpectral;
    float formant_filter_gain = 1.0f;
    bool use_lpc = false;
    bool mark_predicted = false;
    bool active = false;
  };
  static constexpr size_t kMaxDeferredGrains = 16;
  DeferredGrainDescriptor *deferred_grains_ = nullptr;
  uint32_t grain_sequence_counter_ = 0;

  void render_deferred_slices(uint64_t block_start, size_t frames);
  void render_single_grain_slice(DeferredGrainDescriptor &grain, int n_min, int n_max);
  static void compute_windowed_grain(
      const float *src, const float *residual, int half,
      const SharedPitchShiftResources *resources,
      float *win_residual_out, float *win_plain_out, float *window_out);
  void add_grain_ola_from_cached_residual(
      int n_min, int n_max, int64_t dst, int half,
      const float *src, const float *residual, bool mark_predicted);
  void add_grain_ola_from_cached_windowed(
      int n_min, int n_max, int64_t dst, int half,
      const float *win_plain, const float *win_residual, const float *window,
      bool mark_predicted);
  void record_grain_failure(GrainFailureReason reason, double destination,
                            double source, uint64_t center = 0,
                            uint32_t half = 0, double distance = 0.0,
                            double allowed_distance = 0.0);
  void record_grain_alignment(double source, uint64_t center, float period,
                              bool distance_failure);
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
  PsolaLpcKernel lpc_kernel_ = PsolaLpcKernel::ContiguousMulti8;
  PsolaOlaKernel ola_kernel_ = PsolaOlaKernel::Contiguous;
  PsolaGrainKernel grain_kernel_ = PsolaGrainKernel::ContiguousMulti8;
  PsolaSynthesisKernel synthesis_kernel_ = PsolaSynthesisKernel::UnrolledExact;
  FormantWarpCache warp_cache_{};
  bool warp_cache_enabled_ = true;
  bool shared_warp_cache_enabled_ = true;
  bool neutral_warp_fast_path_enabled_ = true;
  PsolaWarpCacheStats warp_stats_{};
  uint8_t model_warp_calls_this_block_ = 0;
  uint8_t model_warp_expensive_this_block_ = 0;
  uint32_t model_warp_cycles_this_block_ = 0;
  uint64_t last_model_timestamp_ = 0;
  uint8_t model_changed_this_block_ = 0;
  uint8_t grains_scheduled_this_block_ = 0;
  // B4D.7: number of scheduler iterations (add_grain attempts) in the block.
  uint16_t schedule_attempts_this_block_ = 0;
  // B4D.7 tail probe: coarse per-phase cycles for the block (opt-in).
  uint32_t b4d7_sched_cycles_this_block_ = 0;
  uint32_t b4d7_addgrain_cycles_this_block_ = 0;
  uint32_t b4d7_deferred_cycles_this_block_ = 0;
  uint8_t b4d7_model_new_this_block_ = 0;
  uint8_t b4d7_warp_hit_this_block_ = 0;
  uint8_t b4d7_warp_miss_this_block_ = 0;
  uint32_t b4d7_mark_cycles_this_block_ = 0;
  uint32_t b4d7_desc_cycles_this_block_ = 0;
  // B4D.8: per-grain ordinal phase cycles (first 4 grains).
  uint32_t b4d8_ord_mark[4]{};
  uint32_t b4d8_ord_warp[4]{};
  uint32_t b4d8_ord_desc[4]{};
  uint32_t b4d8_ord_near[4]{};
  uint32_t b4d8_ord_sel[4]{};
  uint32_t b4d8_ord_align[4]{};
  PsolaGrainAuditRecord grain_audit_[4]{};
  PsolaGrainAuditRecord last_grain_audit_{};
  uint32_t b4d8_last_sel_cycles_ = 0;
  uint32_t b4d8_last_align_cycles_ = 0;
  // B4D.9 diagnostic prewarm cost (read-only touches, no state change).
  uint32_t b4d9_prewarm_cycles_this_block_ = 0;
  // B4D.11 synthesis-phase debt before the scheduler loop.
  int32_t b4d11_debt_samples_ = 0;
  uint32_t b4d11_output_period_q8_ = 0;  // output period * 256
  // B4D.11.1: per-grain geometry for burst blocks (first 4 grains).
  int32_t b4d11_g_dest_[4]{};
  uint16_t b4d11_g_half_[4]{};
  uint64_t b4d11_g_center_[4]{};
  uint8_t b4d11_g_count_ = 0;
  int32_t b4d11_last_dest_ = 0;
  uint16_t b4d11_last_half_ = 0;
  uint16_t b4d8_mark_count_this_block_ = 0;
  uint32_t b4d7_near_cycles_this_block_ = 0;
  uint32_t b4d7_poly_cycles_this_block_ = 0;
  uint32_t b4d7_gn_cycles_this_block_ = 0;
  uint32_t b4d7_cl_cycles_this_block_ = 0;
  uint32_t b4d7_ch_cycles_this_block_ = 0;
  PsolaModelWarpAudit b4d7_warp_prev_{};
  uint8_t grains_rendered_this_block_ = 0;
  uint8_t source_grains_built_this_block_ = 0;
  uint8_t unique_source_grains_this_block_ = 0;
  uint8_t duplicate_source_grains_this_block_ = 0;
  uint16_t grain_samples_processed_this_block_ = 0;
  uint16_t unique_grain_samples_this_block_ = 0;
  float render_slack_blocks_min_this_block_ = 9999.0f;
  uint32_t current_block_frames_ = 64;
  uint32_t last_block_cycles_ = 0;
  uint32_t accounted_cycles_this_block_ = 0;
  PsolaModelWarpAudit warp_audit_{};

  size_t residual_cache_configured_size_ = 2;
  bool residual_cache_enabled_ = true;
  bool windowed_cache_enabled_ = false;
  bool precompute_enabled_ = false;
  float precompute_horizon_blocks_ = 1.0f;
  float *residual_cache_pool_ = nullptr;
  size_t residual_cache_pool_bytes_ = 0;
  bool residual_cache_in_psram_ = false;
  std::array<SourceResidualCacheEntry, kMaxResidualCacheCapacity> residual_cache_entries_{};
  uint32_t residual_cache_generation_ = 0;
  PsolaSourceResidualCacheStats residual_cache_stats_{};
  PsolaPrecomputeStats precompute_stats_{};

  uint8_t residual_cache_hits_this_block_ = 0;
  uint8_t residual_cache_misses_this_block_ = 0;
  uint16_t fir_samples_computed_this_block_ = 0;
  uint16_t fir_samples_reused_this_block_ = 0;
  uint8_t active_overlapping_grains_this_block_ = 0;
  uint8_t precompute_queue_depth_this_block_ = 0;
  uint8_t precompute_expired_this_block_ = 0;

  PsolaGrainRenderMode grain_render_mode_ = PsolaGrainRenderMode::Eager;
  PsolaFirKernel fir_kernel_ = PsolaFirKernel::Multi8;
  PsolaDeferredStats deferred_stats_{};
  uint8_t deferred_active_grains_this_block_ = 0;
  uint8_t deferred_slices_rendered_this_block_ = 0;
  uint16_t deferred_fir_samples_this_block_ = 0;
};

// B4D.2 warp-cache audit accessors (file scope).
uint64_t td_psola_warp_miss_total();
uint64_t td_psola_warp_math_only_miss();
void td_psola_reset_warp_audit();

// B4D.4 per-grain-count section attribution (voice 0, diagnostic).
bool td_psola_b4d4_class_init();
void td_psola_b4d4_class_free();
uint64_t td_psola_b4d4_class_cycles(size_t bucket, size_t sec);
uint64_t td_psola_b4d4_class_blocks(size_t bucket);

// B4D.5 mark-selection decomposition (diagnostic).
void td_psola_b4d5_mark_audit_reset();
void td_psola_b4d5_mark_audit_enable(bool on);
bool td_psola_b4d5_mark_audit_enabled();
void td_psola_b4d5_mark_audit(uint64_t *calls, uint64_t *candidates,
                              uint64_t *scan_cycles, uint64_t *total_cycles);
// B4D.5 equivalence-gate hook: exact production nearest-mark search.
size_t td_psola_nearest_mark_index(double source, const PitchMark *marks,
                                   size_t count);
// B4D.9 diagnostic prewarm variant.
void td_psola_b4d9_set_prewarm_variant(int v);
// B4D.10 scheduler geometry variant (0=S0, 1=S1).
void td_psola_b4d10_set_sched_variant(int v);
