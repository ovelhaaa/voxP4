#include "vocal_fx.h"
#include "biquad.h"
#include "compressor.h"
#include "delay.h"
#include "fdn_reverb.h"
#include "gate.h"
#include "limiter.h"
#include "dry_delay_buffer.h"
#include "harmony_limiter.h"
#include "parameter_queue.h"
#include "pitch_analysis.h"
#include "profiling.h"
#include "td_psola.h"
#include "harmony_engine.h"
#include "lpc.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#ifndef ESP_PLATFORM
#include <chrono>
#endif
#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "esp_timer.h"
#endif

#ifndef ESP_PLATFORM
static VocalFxSampleTelemetryCallback s_sample_telemetry_cb = nullptr;
static void *s_sample_telemetry_user_data = nullptr;
#endif

namespace {
struct Engine {
  VocalFxConfig cfg;
  Biquad hpf;
  Gate gate;
  Compressor compressor;
  StereoDelay delay;
  FdnReverb reverb;
  Limiter limiter;
  DryDelayBuffer dry_delay;
  LightHarmonyLimiter harmony_limiter;
  Profiler profiler;
  PitchAnalysis pitch_analysis;
  SharedLpcAnalysis lpc_analysis;
  SharedPitchShiftResources pitch_resources;
  TdPsola pitch_shift[2];
  HarmonyEngine harmony;
  MidiChordState midi;
  bool ready = false;
  float work[VOCAL_FX_MAX_BLOCK_SIZE], left[VOCAL_FX_MAX_BLOCK_SIZE],
      right[VOCAL_FX_MAX_BLOCK_SIZE];
  float shifted[2][VOCAL_FX_MAX_BLOCK_SIZE];
  float delay_wet_l[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float delay_wet_r[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float rev_wet_l[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float rev_wet_r[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float dry_bus[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float harm_bus_mono[VOCAL_FX_MAX_BLOCK_SIZE]{};
  SmoothedValue delay_to_reverb_send;
  float harmony_mix[2]{};
  float last_wanted_mix[2]{};
  PitchMark pitch_shift_marks[TdPsola::kMaxMarks];
  PitchMark pitch_shift_candidate_marks[TdPsola::kMaxMarks];
  size_t pitch_shift_mark_count = 0;
  PitchResult pitch_shift_pitch{};
  float gate_t = -55, gate_a = 5, gate_h = 40, gate_r = 120, gate_range = -60,
        comp_t = -18, comp_ratio = 3, comp_a = 10, comp_r = 100,
        comp_makeup = 3, comp_knee = 6, delay_l = 250, delay_r = 375,
        delay_dry = 1, delay_wet = .2f;
};
Engine e;
ParameterQueue<64> parameter_queue;
struct PitchMailbox {
  std::atomic_flag publisher_lock = ATOMIC_FLAG_INIT;
  std::atomic<uint32_t> seq{0};
  std::atomic<float> hz{0}, raw_hz{0}, confidence{0};
  std::atomic<float> period{0};
  std::atomic<bool> voiced{false};
  std::atomic<bool> onset{false}, changed{false};
  std::atomic<uint64_t> timestamp{0};
  std::atomic<bool> voiced_raw{false}, voiced_stateful{false};
  std::atomic<float> yin_min{1.0f}, yin_tau{0.0f};
  std::atomic<float> input_rms{0.0f}, input_peak{0.0f};
  std::atomic<float> spectral_centroid{0.0f}, high_frequency_ratio{0.0f}, zero_crossing_rate{0.0f};
  std::atomic<uint8_t> pitch_track_state{0};
  std::atomic<uint8_t> coherent_marks{0};
  std::atomic<uint32_t> coast_remaining{0};
} pitch;

struct AtomicInputIdentity {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> sequence{0};
  std::atomic<uint32_t> dsp_rms_bits{0}, pitch_rms_bits{0};
  std::atomic<uint32_t> dsp_checksum{0}, pitch_checksum{0};
  uint64_t block_counter = 0; // audio-task owned
} input_identity;

} // namespace

struct AtomicFunnelStats {
  std::atomic<uint64_t> synthetic_tone_blocks{0};
  std::atomic<uint64_t> pitch_analysis_blocks{0};
  std::atomic<uint64_t> pitch_results_produced{0};
  std::atomic<uint64_t> voiced_pitch_results{0};
  std::atomic<uint64_t> pitch_marks_generated{0};
  std::atomic<uint64_t> pitch_marks_transferred{0};
  std::atomic<uint64_t> pitch_marks_consumed{0};
  std::atomic<uint64_t> harmony_target_activations{0};
  std::atomic<uint64_t> psola_process_calls{0};
  std::atomic<uint64_t> grain_schedule_attempts{0};
  std::atomic<uint64_t> grains_scheduled{0};
  std::atomic<uint64_t> grains_rendered{0};
};
static AtomicFunnelStats g_funnel;

struct AtomicPitchSync {
  std::atomic<uint64_t> try_pitch_attempts{0};
  std::atomic<uint64_t> try_pitch_successes{0};
  std::atomic<uint64_t> try_marks_attempts{0};
  std::atomic<uint64_t> try_marks_successes{0};
  std::atomic<uint64_t> try_marks_start_gt_end{0};
  std::atomic<uint64_t> last_mark_count{0};
  std::atomic<uint64_t> last_mark_end{0};
  std::atomic<int64_t> last_pitch_published_us{0};
};
static AtomicPitchSync g_sync;

void vocal_fx_funnel_inc_synthetic_tone_blocks(uint64_t count) {
  g_funnel.synthetic_tone_blocks.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_pitch_marks_generated(uint64_t count) {
  g_funnel.pitch_marks_generated.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_pitch_marks_consumed(uint64_t count) {
  g_funnel.pitch_marks_consumed.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_psola_process(uint64_t count) {
  g_funnel.psola_process_calls.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_grain_schedule_attempts(uint64_t count) {
  g_funnel.grain_schedule_attempts.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_grains_scheduled(uint64_t count) {
  g_funnel.grains_scheduled.fetch_add(count, std::memory_order_relaxed);
}
void vocal_fx_funnel_inc_grains_rendered(uint64_t count) {
  g_funnel.grains_rendered.fetch_add(count, std::memory_order_relaxed);
}

namespace {

constexpr uint32_t kPitchResetting = 1U << 31U;
std::atomic<uint32_t> pitch_users{0};
bool enter_pitch_path() {
  uint32_t state = pitch_users.load(std::memory_order_acquire);
  while (!(state & kPitchResetting)) {
    if (pitch_users.compare_exchange_weak(
            state, state + 1U, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}
void leave_pitch_path() {
  pitch_users.fetch_sub(1U, std::memory_order_release);
}
void reset_pitch_analysis() {
  pitch_users.fetch_or(kPitchResetting, std::memory_order_acq_rel);
  while ((pitch_users.load(std::memory_order_acquire) & ~kPitchResetting) !=
         0) {
  }
  e.pitch_analysis.reset();
  pitch_users.store(0, std::memory_order_release);
}
void gate_update() {
  e.gate.set(e.gate_t, e.gate_a, e.gate_h, e.gate_r, e.gate_range);
}
void comp_update() {
  e.compressor.set(e.comp_t, e.comp_ratio, e.comp_a, e.comp_r, e.comp_makeup,
                   e.comp_knee);
}
void apply_parameter(VocalFxParameter p, float v);
void apply_pending_parameters() {
  ParameterChange change;
  while (parameter_queue.pop(change))
    apply_parameter(change.parameter, change.value);
}
} // namespace
bool vocal_fx_init(const VocalFxConfig &c) {
  e.ready = false;
  if (c.sample_rate < 8000 || c.sample_rate > VOCAL_FX_MAX_SAMPLE_RATE ||
      (c.block_size != 64 && c.block_size != 128 && c.block_size != 256))
    return false;
  e.cfg = c;
  e.hpf.configure(BiquadType::HighPass, c.sample_rate, 80);
  e.gate.init(c.sample_rate);
  e.compressor.init(c.sample_rate);
  if (!e.delay.init(c.sample_rate, VOCAL_FX_MAX_DELAY_SECONDS) ||
      !e.reverb.init(c.sample_rate))
    return false;
  e.limiter.init(c.sample_rate);
  e.dry_delay.init(c.sample_rate, c.dry_alignment_ms, c.align_dry_to_harmony);
  e.harmony_limiter.init(c.sample_rate, c.harmony_limiter_threshold_db,
                         c.harmony_limiter_attack_ms, c.harmony_limiter_release_ms,
                         c.harmony_limiter_max_reduction_db);
  e.harmony_mix[0] = e.harmony_mix[1] = 0.0f;
  e.last_wanted_mix[0] = e.last_wanted_mix[1] = 0.0f;
  const float send_target = (c.spatial_routing == SpatialFxRouting::DelayIntoReverb) ? 1.0f : 0.0f;
  e.delay_to_reverb_send.init(send_target, c.sample_rate, 20.0f);
  if (c.enable_pitch_analysis) {
    PitchAnalysisConfig pitch_config{};
    pitch_config.input_sample_rate = c.sample_rate;
    pitch_config.continuity_policy = (c.pitch_shift.continuity_policy != PsolaContinuityPolicy::Baseline)
                                         ? c.pitch_shift.continuity_policy
                                         : c.psola_continuity_policy;
    pitch_config.coast_ms = c.pitch_shift.coast_ms > 0.0f
                                ? c.pitch_shift.coast_ms
                                : (c.psola_coast_ms > 0.0f ? c.psola_coast_ms : 15.0f);
    pitch_config.stateful_voicing_enabled = c.pitch_shift.stateful_voicing_enabled;
    pitch_config.voiced_enter_confidence = c.pitch_shift.voiced_enter_confidence;
    pitch_config.voiced_stay_confidence = c.pitch_shift.voiced_stay_confidence;
    pitch_config.voiced_exit_confidence = c.pitch_shift.voiced_exit_confidence;
    pitch_config.voiced_attack_frames = c.pitch_shift.voiced_attack_frames;
    pitch_config.voiced_release_frames = c.pitch_shift.voiced_release_frames;
    pitch_config.f0_continuity_tolerance_cents = c.pitch_shift.f0_continuity_tolerance_cents;
    pitch_config.max_unvoiced_zcr = c.pitch_shift.max_unvoiced_zcr;
    pitch_config.min_unvoiced_r1 = c.pitch_shift.min_unvoiced_r1;
    if (!e.pitch_analysis.init(pitch_config))
      return false;
  }
  e.pitch_resources.init();
  if (!e.lpc_analysis.init(c.sample_rate, c.lpc)) return false;
  PitchShiftConfig render_config = c.pitch_shift;
  render_config.wet = 1.0f;
  for (auto &voice : e.pitch_shift)
    if (!voice.init(c.sample_rate, render_config, &e.pitch_resources, &e.lpc_analysis))
      return false;
  HarmonyVoiceConfig legacy{}; legacy.enabled=c.pitch_shift.enabled;
  legacy.interval=std::isfinite(c.pitch_shift.semitones)
                      ? std::clamp(c.pitch_shift.semitones, -12.0f, 12.0f)
                      : 0.0f;
  legacy.gain=std::isfinite(c.pitch_shift.wet)
                  ? std::clamp(c.pitch_shift.wet, 0.0f, 1.0f)
                  : 1.0f;
  legacy.pan=0; legacy.smoothing_ms=c.pitch_shift.smoothing_ms;
  e.harmony.set_voice(0,legacy); e.harmony.reset();
  e.pitch_shift_pitch = {};
  e.pitch_shift_mark_count = 0;
  parameter_queue.reset();
  e.ready = true;
  return true;
}
bool vocal_fx_init_pitch_analysis(const PitchAnalysisConfig &config) {
  pitch_users.fetch_or(kPitchResetting, std::memory_order_acq_rel);
  while ((pitch_users.load(std::memory_order_acquire) & ~kPitchResetting) !=
         0) {
  }
  e.cfg.enable_pitch_analysis = false;
  const bool initialized = e.pitch_analysis.init(config);
  if (initialized)
    e.cfg.enable_pitch_analysis = true;
  pitch_users.store(0, std::memory_order_release);
  return initialized;
}
void vocal_fx_reset() {
  if (!e.ready)
    return;
  e.hpf.reset();
  e.gate.reset();
  e.compressor.reset();
  e.delay.reset();
  e.reverb.reset();
  e.limiter.reset();
  e.dry_delay.reset();
  e.harmony_limiter.reset();
  e.harmony_mix[0] = e.harmony_mix[1] = 0.0f;
  e.last_wanted_mix[0] = e.last_wanted_mix[1] = 0.0f;
  const float reset_send = (e.cfg.spatial_routing == SpatialFxRouting::DelayIntoReverb) ? 1.0f : 0.0f;
  e.delay_to_reverb_send.init(reset_send, e.cfg.sample_rate, 20.0f);
  e.pitch_resources.reset();
  e.lpc_analysis.reset();
  for (auto &voice : e.pitch_shift) voice.reset();
  e.harmony.reset();
  e.pitch_shift_mark_count = 0;
  e.pitch_shift_pitch = {};
  if (e.cfg.enable_pitch_analysis)
    reset_pitch_analysis();
}
void vocal_fx_process(const float *in, float *ol, float *orr, size_t frames) {
  if (!e.ready || !in || !ol || !orr)
    return;
  while (frames) {
    size_t n = std::min<size_t>(frames, VOCAL_FX_MAX_BLOCK_SIZE);
    [[maybe_unused]] uint64_t deadline =
        (uint64_t)(1000000.0 * n / e.cfg.sample_rate);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Pipeline);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::PitchLpcTap);
    if (e.cfg.enable_pitch_analysis && enter_pitch_path()) {
      const bool audit_identity = ((++input_identity.block_counter & 63U) == 0);
      float dsp_input_rms = 0.0f;
      uint32_t dsp_checksum = 2166136261U;
      if (audit_identity) {
        double sum_sq = 0.0;
        for (size_t i = 0; i < n; ++i) {
          const float sample = std::isfinite(in[i]) ? in[i] : 0.0f;
          uint32_t bits = 0;
          std::memcpy(&bits, &sample, sizeof(bits));
          dsp_checksum = (dsp_checksum ^ bits) * 16777619U;
          sum_sq += static_cast<double>(sample) * sample;
        }
        dsp_input_rms =
            n ? static_cast<float>(std::sqrt(sum_sq / n)) : 0.0f;
      }
      e.pitch_analysis.tap(in, n, audit_identity);
      if (audit_identity) {
        const VocalFxInputIdentity tap = e.pitch_analysis.tap_identity();
        uint32_t dsp_rms_bits = 0, pitch_rms_bits = 0;
        std::memcpy(&dsp_rms_bits, &dsp_input_rms, sizeof(dsp_rms_bits));
        std::memcpy(&pitch_rms_bits, &tap.pitch_tap_rms,
                    sizeof(pitch_rms_bits));
        input_identity.seq.fetch_add(1, std::memory_order_acq_rel);
        input_identity.sequence.fetch_add(1, std::memory_order_relaxed);
        input_identity.dsp_rms_bits.store(dsp_rms_bits,
                                          std::memory_order_relaxed);
        input_identity.pitch_rms_bits.store(pitch_rms_bits,
                                            std::memory_order_relaxed);
        input_identity.dsp_checksum.store(dsp_checksum,
                                          std::memory_order_relaxed);
        input_identity.pitch_checksum.store(tap.pitch_tap_checksum,
                                            std::memory_order_relaxed);
        input_identity.seq.fetch_add(1, std::memory_order_release);
      }
      e.lpc_analysis.tap(in, n);
      leave_pitch_path();
      g_funnel.pitch_analysis_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::PitchLpcTap, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::ParameterQueue);
    apply_pending_parameters();
    VF_PROFILE_END(e.profiler, ProfileSection::ParameterQueue, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Input);
    for (size_t i = 0; i < n; i++) {
      float x = e.cfg.isolate_pitch_shift_output ? in[i] : e.hpf.process(in[i]);
      e.work[i] = e.cfg.enable_gate ? e.gate.process(x) : x;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Input, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Compressor);
    if (e.cfg.enable_compressor)
      for (size_t i = 0; i < n; i++)
        e.work[i] = e.compressor.process(e.work[i]);
    VF_PROFILE_END(e.profiler, ProfileSection::Compressor, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::PitchMarkSync);
    g_sync.try_pitch_attempts.fetch_add(1, std::memory_order_relaxed);
    PitchResult pitch_candidate;
    if (vocal_fx_try_latest_pitch(&pitch_candidate)) {
      g_sync.try_pitch_successes.fetch_add(1, std::memory_order_relaxed);
      e.pitch_shift_pitch = pitch_candidate;
    }
    const PitchResult current_pitch = e.pitch_shift_pitch;
    const uint64_t mark_end = current_pitch.analysis_timestamp_samples;
    const uint64_t mark_start = mark_end > PitchAnalysis::kAudioHistory
                                    ? mark_end - PitchAnalysis::kAudioHistory
                                    : 0;
    g_sync.last_mark_end.store(mark_end, std::memory_order_relaxed);
    if (mark_start > mark_end) {
      g_sync.try_marks_start_gt_end.fetch_add(1, std::memory_order_relaxed);
    }
    if (e.cfg.enable_pitch_analysis &&
        (e.pitch_shift[0].enabled() || e.pitch_shift[1].enabled())) {
      size_t candidate_count = 0;
      g_sync.try_marks_attempts.fetch_add(1, std::memory_order_relaxed);
      if (vocal_fx_try_get_pitch_marks(mark_start, mark_end,
                                       e.pitch_shift_candidate_marks,
                                       TdPsola::kMaxMarks, &candidate_count)) {
        g_sync.try_marks_successes.fetch_add(1, std::memory_order_relaxed);
        std::copy_n(e.pitch_shift_candidate_marks, candidate_count,
                    e.pitch_shift_marks);
        e.pitch_shift_mark_count = candidate_count;
        g_sync.last_mark_count.store(candidate_count, std::memory_order_relaxed);
        g_funnel.pitch_marks_transferred.fetch_add(candidate_count, std::memory_order_relaxed);
      }
    } else {
      e.pitch_shift_mark_count = 0;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::PitchMarkSync, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Harmony);
    e.pitch_resources.push(e.work,n);
    const auto targets=e.harmony.update(current_pitch.frequency_hz,
                                         current_pitch.voiced,e.midi);
#ifndef ESP_PLATFORM
    SampleTelemetryRecord telem_records[64];
    const size_t iso_v = (e.cfg.isolated_pitch_shift_voice < 2) ? e.cfg.isolated_pitch_shift_voice : 0;
    if (s_sample_telemetry_cb) {
      e.pitch_shift[iso_v].set_sample_telemetry_buffer(telem_records);
    }
#endif
    for(size_t v=0;v<2;++v){
      e.pitch_shift[v].set_enabled(e.harmony.voice(v).enabled);
      if(targets[v].valid) {
        g_funnel.harmony_target_activations.fetch_add(1, std::memory_order_relaxed);
        e.pitch_shift[v].set_ratio(targets[v].target_frequency_hz/current_pitch.frequency_hz);
      }
      e.pitch_shift[v].process_shared(e.work,e.shifted[v],n,current_pitch,
          vocal_fx_pitch_track_state(),e.pitch_shift_marks,e.pitch_shift_mark_count);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Harmony, 0);

    const float attack_samples = std::max(1.0f, e.cfg.sample_rate * e.cfg.harmony_attack_ms * 0.001f);
    const float release_samples = std::max(1.0f, e.cfg.sample_rate * e.cfg.harmony_release_ms * 0.001f);
    const float attack_step = 1.0f / attack_samples;
    const float release_step = 1.0f / release_samples;

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::DryAlignment);
    for(size_t i=0;i<n;++i){
      e.dry_bus[i] = e.dry_delay.process(e.work[i]);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::DryAlignment, 0);

    float harm_bus_l[VOCAL_FX_MAX_BLOCK_SIZE];
    float harm_bus_r[VOCAL_FX_MAX_BLOCK_SIZE];

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::HarmonySlewPan);
    for(size_t i=0;i<n;++i){
      float hl = 0.0f;
      float hr = 0.0f;
      for(size_t v=0;v<2;++v){
        const auto &c = e.harmony.voice(v);
        const float p = std::clamp(c.pan, -1.0f, 1.0f);
        const float wanted = (targets[v].valid || e.pitch_shift[v].is_releasing()) &&
                             e.pitch_shift[v].has_usable_output()
                                 ? 1.0f
                                 : 0.0f;
        e.last_wanted_mix[v] = wanted;
        const float diff = wanted - e.harmony_mix[v];
        if (diff > 0.0f) {
          e.harmony_mix[v] += std::min(diff, attack_step);
        } else {
          e.harmony_mix[v] += std::max(diff, -release_step);
        }
        const float voice_sig = e.shifted[v][i] * c.gain * e.harmony_mix[v];
        hl += voice_sig * std::sqrt(0.5f * (1.0f - p));
        hr += voice_sig * std::sqrt(0.5f * (1.0f + p));
      }
      harm_bus_l[i] = hl;
      harm_bus_r[i] = hr;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::HarmonySlewPan, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::HarmonyLimiter);
    if (e.cfg.enable_harmony_limiter) {
      for(size_t i=0;i<n;++i){
        e.harmony_limiter.process(harm_bus_l[i], harm_bus_r[i]);
      }
    }
    VF_PROFILE_END(e.profiler, ProfileSection::HarmonyLimiter, 0);

#ifndef ESP_PLATFORM
    if (s_sample_telemetry_cb) {
      for(size_t i=0;i<n;++i){
        telem_records[i].limiter_gain = e.harmony_limiter.current_gain();
        telem_records[i].limiter_reduction_db = e.harmony_limiter.reduction_db();
        telem_records[i].limiter_peak = e.harmony_limiter.last_peak();
        telem_records[i].dry_delay_samples = static_cast<uint16_t>(e.dry_delay.delay_samples());
        telem_records[i].dry_alignment_active = e.dry_delay.enabled() ? 1 : 0;
        telem_records[i].wanted_mix = e.last_wanted_mix[iso_v];
      }
    }
#endif

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::BusMixing);
    for(size_t i=0;i<n;++i){
      const float dry_sample = e.dry_bus[i];
      e.harm_bus_mono[i] = 0.5f * (harm_bus_l[i] + harm_bus_r[i]);
      const float dry_mix = e.cfg.mute_dry ? 0.0f : dry_sample;

      if (e.cfg.isolate_pitch_shift_output) {
        if (e.cfg.apply_isolated_voice_envelope) {
          e.left[i] = harm_bus_l[i];
          e.right[i] = harm_bus_r[i];
        } else {
          const size_t iso_v_out = e.cfg.isolated_pitch_shift_voice < 2 ? e.cfg.isolated_pitch_shift_voice : 0;
          e.left[i] = e.right[i] = e.shifted[iso_v_out][i];
        }
      } else {
        e.left[i] = 0.5f * (dry_mix + harm_bus_l[i]);
        e.right[i] = 0.5f * (dry_mix + harm_bus_r[i]);
      }
    }
    VF_PROFILE_END(e.profiler, ProfileSection::BusMixing, 0);

#ifndef ESP_PLATFORM
    if (s_sample_telemetry_cb) {
      e.pitch_shift[iso_v].set_sample_telemetry_buffer(nullptr);
      static float s_in_env = 0.0f;
      const float env_alpha = 1.0f - std::exp(-1.0f / (e.cfg.sample_rate * 0.010f));
      for (size_t i = 0; i < n; ++i) {
        const float in_abs = std::fabs(e.work[i]);
        s_in_env += env_alpha * (in_abs - s_in_env);
        telem_records[i].input_envelope = s_in_env;
        telem_records[i].active_mix = e.harmony_mix[iso_v];
        telem_records[i].final_harmony_rms = std::fabs(e.shifted[iso_v][i]);
        if (s_in_env > 1e-4f) {
          telem_records[i].effective_total_gain = std::fabs(e.shifted[iso_v][i]) / s_in_env;
        } else {
          telem_records[i].effective_total_gain = telem_records[i].psola_gain * e.harmony_mix[iso_v];
        }
      }
      s_sample_telemetry_cb(telem_records, n, s_sample_telemetry_user_data);
    }
#endif

    auto get_source_mono = [&](size_t i) -> float {
      if (e.cfg.spatial_source == SpatialFxSource::DryOnly) {
        return e.dry_bus[i];
      } else if (e.cfg.spatial_source == SpatialFxSource::HarmonyOnly) {
        return e.harm_bus_mono[i];
      } else {
        return (e.left[i] + e.right[i]) * 0.5f;
      }
    };

    float source_mono[VOCAL_FX_MAX_BLOCK_SIZE];
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::DelayPrep);
    for (size_t i = 0; i < n; i++) {
      source_mono[i] = get_source_mono(i);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::DelayPrep, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Delay);
    if (e.cfg.enable_delay) {
      for (size_t i = 0; i < n; i++) {
        e.delay.process_wet(source_mono[i], e.delay_wet_l[i], e.delay_wet_r[i]);
      }
    } else {
      std::fill_n(e.delay_wet_l, n, 0.0f);
      std::fill_n(e.delay_wet_r, n, 0.0f);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Delay, 0);

    float rev_in[VOCAL_FX_MAX_BLOCK_SIZE];
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::ReverbPrep);
    if (e.cfg.enable_reverb) {
      for (size_t i = 0; i < n; i++) {
        const float send = e.delay_to_reverb_send.next();
        const float delay_wet_mono = (e.delay_wet_l[i] + e.delay_wet_r[i]) * 0.5f;
        rev_in[i] = source_mono[i] + send * delay_wet_mono;
      }
    } else {
      for (size_t i = 0; i < n; i++) {
        (void)e.delay_to_reverb_send.next();
      }
      std::fill_n(e.rev_wet_l, n, 0.0f);
      std::fill_n(e.rev_wet_r, n, 0.0f);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::ReverbPrep, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Reverb);
    if (e.cfg.enable_reverb) {
      for (size_t i = 0; i < n; i++) {
        e.reverb.process(rev_in[i], e.rev_wet_l[i], e.rev_wet_r[i]);
      }
    } else {
      std::fill_n(e.rev_wet_l, n, 0.0f);
      std::fill_n(e.rev_wet_r, n, 0.0f);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Reverb, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Master);
    for (size_t i = 0; i < n; i++) {
      e.left[i] += e.delay_wet_l[i] + e.rev_wet_l[i];
      e.right[i] += e.delay_wet_r[i] + e.rev_wet_r[i];
    }
    for (size_t i = 0; i < n; i++) {
      e.limiter.process(e.left[i], e.right[i]);
      ol[i] = e.left[i];
      orr[i] = e.right[i];
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Master, 0);
    VF_PROFILE_END(e.profiler, ProfileSection::Pipeline, deadline);
    in += n;
    ol += n;
    orr += n;
    frames -= n;
  }
}
namespace {
void apply_parameter(VocalFxParameter p, float v) {
  switch (p) {
  case VocalFxParameter::GateThresholdDb:
    e.gate_t = v;
    gate_update();
    break;
  case VocalFxParameter::GateAttackMs:
    e.gate_a = v;
    gate_update();
    break;
  case VocalFxParameter::GateHoldMs:
    e.gate_h = v;
    gate_update();
    break;
  case VocalFxParameter::GateReleaseMs:
    e.gate_r = v;
    gate_update();
    break;
  case VocalFxParameter::GateRangeDb:
    e.gate_range = v;
    gate_update();
    break;
  case VocalFxParameter::CompressorThresholdDb:
    e.comp_t = v;
    comp_update();
    break;
  case VocalFxParameter::CompressorRatio:
    e.comp_ratio = v;
    comp_update();
    break;
  case VocalFxParameter::CompressorAttackMs:
    e.comp_a = v;
    comp_update();
    break;
  case VocalFxParameter::CompressorReleaseMs:
    e.comp_r = v;
    comp_update();
    break;
  case VocalFxParameter::CompressorMakeupDb:
    e.comp_makeup = v;
    comp_update();
    break;
  case VocalFxParameter::CompressorKneeDb:
    e.comp_knee = v;
    comp_update();
    break;
  case VocalFxParameter::DelayLeftMs:
    e.delay_l = v;
    e.delay.set_times(e.delay_l, e.delay_r);
    break;
  case VocalFxParameter::DelayRightMs:
    e.delay_r = v;
    e.delay.set_times(e.delay_l, e.delay_r);
    break;
  case VocalFxParameter::DelayFeedback:
    e.delay.set_feedback(v);
    break;
  case VocalFxParameter::DelayWet:
    e.delay_wet = v;
    e.delay.set_mix(e.delay_dry, e.delay_wet);
    break;
  case VocalFxParameter::DelayDry:
    e.delay_dry = v;
    e.delay.set_mix(e.delay_dry, e.delay_wet);
    break;
  case VocalFxParameter::DelayFeedbackLowpassHz:
    e.delay.set_feedback_lowpass(v);
    break;
  case VocalFxParameter::ReverbWet:
    e.reverb.set_wet(v);
    break;
  case VocalFxParameter::ReverbDecaySeconds:
    e.reverb.set_rt60(v);
    break;
  case VocalFxParameter::ReverbDamping:
    e.reverb.set_damping(v);
    break;
  case VocalFxParameter::LimiterCeiling:
    e.limiter.set_ceiling(v);
    break;
  case VocalFxParameter::EnableGate:
    e.cfg.enable_gate = v >= .5f;
    break;
  case VocalFxParameter::EnableCompressor:
    e.cfg.enable_compressor = v >= .5f;
    break;
  case VocalFxParameter::EnableDelay:
    e.cfg.enable_delay = v >= .5f;
    break;
  case VocalFxParameter::EnableReverb:
    e.cfg.enable_reverb = v >= .5f;
    break;
  case VocalFxParameter::PitchShiftEnabled:
    {auto c=e.harmony.voice(0);c.enabled=v>=.5f;e.harmony.set_voice(0,c);}
    break;
  case VocalFxParameter::PitchShiftSemitones:
    if (std::isfinite(v)) {
      auto c = e.harmony.voice(0);
      c.interval = std::clamp(v, -12.0f, 12.0f);
      e.harmony.set_voice(0, c);
    }
    break;
  case VocalFxParameter::PitchShiftWet:
    if (std::isfinite(v)) {
      auto c = e.harmony.voice(0);
      c.gain = std::clamp(v, 0.0f, 1.0f);
      e.harmony.set_voice(0, c);
    }
    break;
  case VocalFxParameter::HarmonyMode:e.harmony.set_mode(static_cast<HarmonyMode>(std::clamp(static_cast<int>(v),0,2)));break;
  case VocalFxParameter::HarmonyKey:e.harmony.set_root(static_cast<uint8_t>(std::clamp(static_cast<int>(v),0,11)));break;
  case VocalFxParameter::HarmonyScale:e.harmony.set_scale_type(static_cast<ScaleType>(std::clamp(static_cast<int>(v),0,1)));break;
  case VocalFxParameter::FormantVoice1Mode:
  case VocalFxParameter::FormantVoice2Mode: {
    const size_t voice=static_cast<size_t>(p)-static_cast<size_t>(VocalFxParameter::FormantVoice1Mode);
    e.pitch_shift[voice].set_formants(v>=.5f?FormantMode::Lpc:FormantMode::Off,e.pitch_shift[voice].formant_amount());
    break;
  }
  case VocalFxParameter::FormantVoice1Amount:
  case VocalFxParameter::FormantVoice2Amount: {
    const size_t voice=static_cast<size_t>(p)-static_cast<size_t>(VocalFxParameter::FormantVoice1Amount);
    e.pitch_shift[voice].set_formants(e.pitch_shift[voice].formant_mode(),v);
    break;
  }
  case VocalFxParameter::DryAlignmentEnabled:
    e.cfg.align_dry_to_harmony = (v >= 0.5f);
    e.dry_delay.set_enabled(e.cfg.align_dry_to_harmony);
    break;
  case VocalFxParameter::DryAlignmentMs:
    e.cfg.dry_alignment_ms = v;
    e.dry_delay.set_delay_ms(v);
    break;
  case VocalFxParameter::HarmonyAttackMs:
    e.cfg.harmony_attack_ms = std::clamp(v, 0.1f, 100.0f);
    break;
  case VocalFxParameter::HarmonyReleaseMs:
    e.cfg.harmony_release_ms = std::clamp(v, 1.0f, 500.0f);
    break;
  case VocalFxParameter::HarmonyLimiterEnabled:
    e.cfg.enable_harmony_limiter = (v >= 0.5f);
    break;
  case VocalFxParameter::HarmonyLimiterThresholdDb:
    e.cfg.harmony_limiter_threshold_db = v;
    e.harmony_limiter.set_threshold_db(v);
    break;
  case VocalFxParameter::SpatialRouting: {
    const auto r = (v >= 0.5f) ? SpatialFxRouting::DelayIntoReverb : SpatialFxRouting::Parallel;
    e.cfg.spatial_routing = r;
    e.delay_to_reverb_send.set_target(r == SpatialFxRouting::DelayIntoReverb ? 1.0f : 0.0f);
    break;
  }
  case VocalFxParameter::SpatialSource:
    e.cfg.spatial_source = static_cast<SpatialFxSource>(std::clamp(static_cast<int>(v), 0, 2));
    break;
  case VocalFxParameter::MuteDry:
    e.cfg.mute_dry = (v >= 0.5f);
    break;
  default: {
    const int x=static_cast<int>(p)-static_cast<int>(VocalFxParameter::HarmonyVoice1Enabled);
    if(x>=0){size_t voice=static_cast<size_t>(x%2);int field=x/2;auto c=e.harmony.voice(voice);
      if(field==0)c.enabled=v>=.5f;else if(field==1)c.interval=v;else if(field==2)c.degree=static_cast<int>(v);else if(field==3)c.gain=std::clamp(v,0.0f,1.0f);else if(field==4)c.pan=std::clamp(v,-1.0f,1.0f);else if(field==5)c.smoothing_ms=std::clamp(v,1.0f,500.0f);
      e.harmony.set_voice(voice,c);e.pitch_shift[voice].set_smoothing(c.smoothing_ms);}
    break; }
  }
}
} // namespace
void vocal_fx_set_parameter(VocalFxParameter p, float v) {
  // Deliberately non-blocking. If the SPSC queue is saturated, retaining the
  // last complete audio-thread state is safer than a partial cross-core update.
  (void)parameter_queue.push({p, v});
}
void vocal_fx_set_pitch_shift_enabled(bool enabled) {
  vocal_fx_set_parameter(VocalFxParameter::PitchShiftEnabled,
                         enabled ? 1.0f : 0.0f);
}
void vocal_fx_set_pitch_shift_semitones(float semitones) {
  vocal_fx_set_parameter(VocalFxParameter::PitchShiftSemitones, semitones);
}
void vocal_fx_set_pitch_shift_mix(float wet) {
  vocal_fx_set_parameter(VocalFxParameter::PitchShiftWet, wet);
}
void vocal_fx_set_harmony_mode(HarmonyMode m){vocal_fx_set_parameter(VocalFxParameter::HarmonyMode,static_cast<float>(m));}
void vocal_fx_set_key(uint8_t r){vocal_fx_set_parameter(VocalFxParameter::HarmonyKey,static_cast<float>(r%12));}
void vocal_fx_set_scale(ScaleType s){vocal_fx_set_parameter(VocalFxParameter::HarmonyScale,static_cast<float>(s));}
namespace { VocalFxParameter voice_param(size_t v,VocalFxParameter first){return static_cast<VocalFxParameter>(static_cast<int>(first)+static_cast<int>(v));} }
void vocal_fx_set_harmony_enabled(size_t v,bool x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Enabled),x?1:0);}
void vocal_fx_set_harmony_interval(size_t v,float x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Interval),x);}
void vocal_fx_set_harmony_degree(size_t v,int x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Degree),static_cast<float>(x));}
void vocal_fx_set_harmony_gain(size_t v,float x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Gain),x);}
void vocal_fx_set_harmony_pan(size_t v,float x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Pan),x);}
void vocal_fx_set_harmony_smoothing(size_t v,float x){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Smoothing),x);}
void vocal_fx_set_formant_mode(size_t v,FormantMode mode){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::FormantVoice1Mode),mode==FormantMode::Lpc?1.0f:0.0f);}
void vocal_fx_set_formant_amount(size_t v,float amount){if(v<2)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::FormantVoice1Amount),amount);}
void vocal_fx_set_dry_alignment(bool enabled, float delay_ms) {
  vocal_fx_set_parameter(VocalFxParameter::DryAlignmentEnabled, enabled ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::DryAlignmentMs, delay_ms);
}
void vocal_fx_set_harmony_attack_ms(float milliseconds) {
  vocal_fx_set_parameter(VocalFxParameter::HarmonyAttackMs, milliseconds);
}
void vocal_fx_set_harmony_release_ms(float milliseconds) {
  vocal_fx_set_parameter(VocalFxParameter::HarmonyReleaseMs, milliseconds);
}
void vocal_fx_set_harmony_limiter(bool enabled, float threshold_db) {
  vocal_fx_set_parameter(VocalFxParameter::HarmonyLimiterEnabled, enabled ? 1.0f : 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::HarmonyLimiterThresholdDb, threshold_db);
}
void vocal_fx_set_spatial_routing(SpatialFxRouting routing) {
  vocal_fx_set_parameter(VocalFxParameter::SpatialRouting,
                         routing == SpatialFxRouting::DelayIntoReverb ? 1.0f : 0.0f);
}
void vocal_fx_set_spatial_source(SpatialFxSource source) {
  vocal_fx_set_parameter(VocalFxParameter::SpatialSource, static_cast<float>(source));
}
void vocal_fx_set_mute_dry(bool mute) {
  vocal_fx_set_parameter(VocalFxParameter::MuteDry, mute ? 1.0f : 0.0f);
}
void vocal_fx_midi_note_on(uint8_t n,uint8_t velocity){e.midi.note_on(n,velocity);}
void vocal_fx_midi_note_off(uint8_t n){e.midi.note_off(n);}
void vocal_fx_midi_all_notes_off(){e.midi.all_notes_off();}
PitchShiftTelemetry vocal_fx_harmony_telemetry(size_t v){return v<2?e.pitch_shift[v].telemetry():PitchShiftTelemetry{};}
uint32_t vocal_fx_pitch_shift_latency_samples() {
  return e.pitch_shift[0].latency_samples();
}
PitchShiftTelemetry vocal_fx_pitch_shift_telemetry() {
  return e.pitch_shift[0].telemetry();
}
PitchShiftDebug vocal_fx_pitch_shift_debug() { return e.pitch_shift[0].debug(); }
VocalFxProfileStats
vocal_fx_pitch_shift_profile_stats(PitchShiftProfileSection s) {
  const auto stats = e.pitch_shift[0].profile(s);
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses};
}
void vocal_fx_publish_pitch(const PitchResult &r) {
  while (pitch.publisher_lock.test_and_set(std::memory_order_acquire)) {
  }
  pitch.seq.fetch_add(1, std::memory_order_acq_rel);
  pitch.hz.store(r.frequency_hz, std::memory_order_relaxed);
  pitch.raw_hz.store(r.raw_frequency_hz, std::memory_order_relaxed);
  pitch.confidence.store(r.confidence, std::memory_order_relaxed);
  pitch.period.store(r.period_samples, std::memory_order_relaxed);
  pitch.voiced.store(r.voiced, std::memory_order_relaxed);
  pitch.onset.store(r.onset, std::memory_order_relaxed);
  pitch.changed.store(r.pitch_changed, std::memory_order_relaxed);
  pitch.timestamp.store(r.analysis_timestamp_samples
                            ? r.analysis_timestamp_samples
                            : r.timestamp_samples,
                        std::memory_order_relaxed);
  pitch.voiced_raw.store(r.voiced_raw, std::memory_order_relaxed);
  pitch.voiced_stateful.store(r.voiced_stateful, std::memory_order_relaxed);
  pitch.yin_min.store(r.yin_min, std::memory_order_relaxed);
  pitch.yin_tau.store(r.yin_tau, std::memory_order_relaxed);
  pitch.input_rms.store(r.input_rms, std::memory_order_relaxed);
  pitch.input_peak.store(r.input_peak, std::memory_order_relaxed);
  pitch.spectral_centroid.store(r.spectral_centroid, std::memory_order_relaxed);
  pitch.high_frequency_ratio.store(r.high_frequency_ratio, std::memory_order_relaxed);
  pitch.zero_crossing_rate.store(r.zero_crossing_rate, std::memory_order_relaxed);
  pitch.pitch_track_state.store(static_cast<uint8_t>(r.pitch_track_state), std::memory_order_relaxed);
  pitch.coherent_marks.store(r.coherent_marks, std::memory_order_relaxed);
  pitch.coast_remaining.store(r.coast_remaining, std::memory_order_relaxed);
  pitch.seq.fetch_add(1, std::memory_order_release);
  pitch.publisher_lock.clear(std::memory_order_release);

  g_funnel.pitch_results_produced.fetch_add(1, std::memory_order_relaxed);
  if (r.voiced) {
    g_funnel.voiced_pitch_results.fetch_add(1, std::memory_order_relaxed);
  }
#ifdef ESP_PLATFORM
  g_sync.last_pitch_published_us.store(esp_timer_get_time(), std::memory_order_relaxed);
#else
  g_sync.last_pitch_published_us.store(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count(),
      std::memory_order_relaxed);
#endif
}
PitchResult vocal_fx_latest_pitch() {
  PitchResult r;
  while (!vocal_fx_try_latest_pitch(&r)) {
  }
  return r;
}
bool vocal_fx_try_latest_pitch(PitchResult *result) {
  if (!result)
    return false;

  const uint32_t before = pitch.seq.load(std::memory_order_acquire);
  if (before & 1U)
    return false;

  PitchResult candidate;
  candidate.frequency_hz = pitch.hz.load(std::memory_order_relaxed);
  candidate.raw_frequency_hz = pitch.raw_hz.load(std::memory_order_relaxed);
  candidate.confidence = pitch.confidence.load(std::memory_order_relaxed);
  candidate.period_samples = pitch.period.load(std::memory_order_relaxed);
  candidate.voiced = pitch.voiced.load(std::memory_order_relaxed);
  candidate.onset = pitch.onset.load(std::memory_order_relaxed);
  candidate.pitch_changed = pitch.changed.load(std::memory_order_relaxed);
  candidate.analysis_timestamp_samples =
      pitch.timestamp.load(std::memory_order_relaxed);
  candidate.timestamp_samples = candidate.analysis_timestamp_samples;
  candidate.voiced_raw = pitch.voiced_raw.load(std::memory_order_relaxed);
  candidate.voiced_stateful = pitch.voiced_stateful.load(std::memory_order_relaxed);
  candidate.yin_min = pitch.yin_min.load(std::memory_order_relaxed);
  candidate.yin_tau = pitch.yin_tau.load(std::memory_order_relaxed);
  candidate.input_rms = pitch.input_rms.load(std::memory_order_relaxed);
  candidate.input_peak = pitch.input_peak.load(std::memory_order_relaxed);
  candidate.spectral_centroid = pitch.spectral_centroid.load(std::memory_order_relaxed);
  candidate.high_frequency_ratio = pitch.high_frequency_ratio.load(std::memory_order_relaxed);
  candidate.zero_crossing_rate = pitch.zero_crossing_rate.load(std::memory_order_relaxed);
  candidate.pitch_track_state = pitch.pitch_track_state.load(std::memory_order_relaxed);
  candidate.coherent_marks = pitch.coherent_marks.load(std::memory_order_relaxed);
  candidate.coast_remaining = pitch.coast_remaining.load(std::memory_order_relaxed);

  const uint32_t after = pitch.seq.load(std::memory_order_acquire);
  if (before != after || (after & 1U))
    return false;
  *result = candidate;
  return true;
}
size_t vocal_fx_run_pitch_analysis(size_t max_hops) {
  if (!e.cfg.enable_pitch_analysis || !enter_pitch_path())
    return 0;
  const size_t count = e.pitch_analysis.run(max_hops);
  if (count)
    vocal_fx_publish_pitch(e.pitch_analysis.latest());
  (void)e.lpc_analysis.run(max_hops, e.pitch_analysis.latest());
  leave_pitch_path();
  return count;
}
bool vocal_fx_get_latest_pitch_mark(PitchMark *mark) {
  return e.pitch_analysis.latest_mark(mark);
}
size_t vocal_fx_get_pitch_marks(uint64_t start, uint64_t end, PitchMark *out,
                                size_t capacity) {
  return e.pitch_analysis.marks(start, end, out, capacity);
}
bool vocal_fx_try_get_pitch_marks(uint64_t start, uint64_t end, PitchMark *out,
                                  size_t capacity, size_t *written) {
  return e.pitch_analysis.try_marks(start, end, out, capacity, written);
}
PitchTrackState vocal_fx_pitch_track_state() {
  return e.pitch_analysis.track_state();
}
uint64_t vocal_fx_analysis_latency_samples() {
  return e.pitch_analysis.latency_samples();
}
VocalFxProfileStats
vocal_fx_pitch_profile_stats(PitchAnalysisProfileSection section) {
  const auto stats = e.pitch_analysis.profile(section);
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses};
}
size_t vocal_fx_dsp_memory_bytes() {
  return e.delay.memory_bytes() + e.reverb.memory_bytes() +
         e.pitch_resources.memory_bytes() +
         e.pitch_shift[0].memory_bytes() + e.pitch_shift[1].memory_bytes() +
         e.lpc_analysis.memory_bytes() +
         (e.cfg.enable_pitch_analysis ? e.pitch_analysis.memory_bytes() : 0);
}
size_t vocal_fx_delay_memory_bytes() {
  return e.delay.memory_bytes();
}
size_t vocal_fx_reverb_memory_bytes() {
  return e.reverb.memory_bytes();
}
LpcTelemetry vocal_fx_lpc_telemetry(){auto x=e.lpc_analysis.telemetry();x.voice1_formant_frames=e.pitch_shift[0].telemetry().formant_frames;x.voice2_formant_frames=e.pitch_shift[1].telemetry().formant_frames;return x;}
VocalFxProfileStats vocal_fx_lpc_profile_stats(LpcProfileSection s){const auto x=e.lpc_analysis.profile(s);return{x.calls,x.total_us,x.max_us,x.deadline_misses};}

VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section) {
  const auto stats = e.profiler.stats(static_cast<ProfileSection>(section));
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses};
}

void vocal_fx_reset_profiler() {
  e.profiler.reset();
}

size_t vocal_fx_audit_buffers(VocalFxBufferAudit *out, size_t max_count) {
  if (!out || max_count == 0) return 0;
  size_t count = 0;

  auto record = [&](const char *name, const void *ptr, size_t bytes) {
    if (count >= max_count) return;
    std::strncpy(out[count].name, name, sizeof(out[count].name) - 1);
    out[count].name[sizeof(out[count].name) - 1] = '\0';
    out[count].ptr = ptr;
    out[count].size_bytes = bytes;
#ifdef ESP_PLATFORM
    out[count].is_psram = ptr ? esp_ptr_external_ram(ptr) : false;
    out[count].is_sram = ptr ? esp_ptr_internal(ptr) : false;
#else
    out[count].is_psram = false;
    out[count].is_sram = true;
#endif
    count++;
  };

  record("DelayLeft", e.delay.left_ptr(), e.delay.channel_bytes());
  record("DelayRight", e.delay.right_ptr(), e.delay.channel_bytes());
  for (size_t i = 0; i < 8; ++i) {
    char name[32];
    std::snprintf(name, sizeof(name), "ReverbLine%zu", i);
    record(name, e.reverb.line_ptr(i), e.reverb.line_bytes(i));
  }
  for (size_t s = 0; s < 3; ++s) {
    char name[32];
    std::snprintf(name, sizeof(name), "Diffuser_Stage%zu", s);
    record(name, e.reverb.diffuser().stage_ptr(s), e.reverb.diffuser().stage_bytes(s));
  }
  record("DryDelayBuffer", e.dry_delay.buffer_ptr(), e.dry_delay.buffer_bytes());
  record("PitchResourcesHistory", e.pitch_resources.history_ptr(), e.pitch_resources.history_bytes());
  record("PitchResourcesHann", e.pitch_resources.hann_ptr(), e.pitch_resources.hann_bytes());
  record("PitchAudioHistory", e.pitch_analysis.audio_history_ptr(), e.pitch_analysis.audio_history_bytes());
  record("PitchAnalysisFifo", e.pitch_analysis.fifo_ptr(), e.pitch_analysis.fifo_bytes());
  record("LpcFifo", e.lpc_analysis.fifo_ptr(), e.lpc_analysis.fifo_bytes());
  record("LpcFrame", e.lpc_analysis.frame_ptr(), e.lpc_analysis.frame_bytes());
  record("WorkBuffer", e.work, sizeof(e.work));
  record("LeftBuffer", e.left, sizeof(e.left));
  record("RightBuffer", e.right, sizeof(e.right));

  return count;
}

PitchShiftDebug vocal_fx_harmony_debug(size_t voice) {
  if (voice < 2)
    return e.pitch_shift[voice].debug();
  return {};
}

PitchAnalysisDebug vocal_fx_pitch_analysis_debug() {
  return {};
}

PitchAnalysisAuditTelemetry vocal_fx_pitch_analysis_audit_telemetry() {
  return e.pitch_analysis.audit_telemetry();
}

size_t vocal_fx_read_pitch_audit_events(PitchAuditEvent *events,
                                        size_t capacity) {
  return e.pitch_analysis.read_audit_events(events, capacity);
}

VocalFxInputIdentity vocal_fx_input_identity() {
  VocalFxInputIdentity result{};
  uint32_t before = 0, after = 0;
  do {
    before = input_identity.seq.load(std::memory_order_acquire);
    if (before & 1U) {
      after = before;
      continue;
    }
    result.sequence =
        input_identity.sequence.load(std::memory_order_relaxed);
    uint32_t dsp_rms_bits =
        input_identity.dsp_rms_bits.load(std::memory_order_relaxed);
    uint32_t pitch_rms_bits =
        input_identity.pitch_rms_bits.load(std::memory_order_relaxed);
    std::memcpy(&result.dsp_input_rms, &dsp_rms_bits, sizeof(dsp_rms_bits));
    std::memcpy(&result.pitch_tap_rms, &pitch_rms_bits,
                sizeof(pitch_rms_bits));
    result.dsp_input_checksum =
        input_identity.dsp_checksum.load(std::memory_order_relaxed);
    result.pitch_tap_checksum =
        input_identity.pitch_checksum.load(std::memory_order_relaxed);
    after = input_identity.seq.load(std::memory_order_acquire);
  } while (before != after || (after & 1U));
  return result;
}

#ifndef ESP_PLATFORM
void vocal_fx_set_sample_telemetry_callback(VocalFxSampleTelemetryCallback cb, void *user_data) {
  s_sample_telemetry_cb = cb;
  s_sample_telemetry_user_data = user_data;
}
#endif

VocalFxFunnelStats vocal_fx_funnel_stats() {
  VocalFxFunnelStats s{};
  s.synthetic_tone_blocks = g_funnel.synthetic_tone_blocks.load(std::memory_order_relaxed);
  s.pitch_analysis_blocks = g_funnel.pitch_analysis_blocks.load(std::memory_order_relaxed);
  s.pitch_results_produced = g_funnel.pitch_results_produced.load(std::memory_order_relaxed);
  s.voiced_pitch_results = g_funnel.voiced_pitch_results.load(std::memory_order_relaxed);
  s.pitch_marks_generated = g_funnel.pitch_marks_generated.load(std::memory_order_relaxed);
  s.pitch_marks_transferred = g_funnel.pitch_marks_transferred.load(std::memory_order_relaxed);
  s.pitch_marks_consumed = g_funnel.pitch_marks_consumed.load(std::memory_order_relaxed);
  s.harmony_target_activations = g_funnel.harmony_target_activations.load(std::memory_order_relaxed);
  s.psola_process_calls = g_funnel.psola_process_calls.load(std::memory_order_relaxed);
  s.grain_schedule_attempts = g_funnel.grain_schedule_attempts.load(std::memory_order_relaxed);
  s.grains_scheduled = g_funnel.grains_scheduled.load(std::memory_order_relaxed);
  s.grains_rendered = g_funnel.grains_rendered.load(std::memory_order_relaxed);
  return s;
}

void vocal_fx_reset_funnel_stats() {
  g_funnel.synthetic_tone_blocks.store(0, std::memory_order_relaxed);
  g_funnel.pitch_analysis_blocks.store(0, std::memory_order_relaxed);
  g_funnel.pitch_results_produced.store(0, std::memory_order_relaxed);
  g_funnel.voiced_pitch_results.store(0, std::memory_order_relaxed);
  g_funnel.pitch_marks_generated.store(0, std::memory_order_relaxed);
  g_funnel.pitch_marks_transferred.store(0, std::memory_order_relaxed);
  g_funnel.pitch_marks_consumed.store(0, std::memory_order_relaxed);
  g_funnel.harmony_target_activations.store(0, std::memory_order_relaxed);
  g_funnel.psola_process_calls.store(0, std::memory_order_relaxed);
  g_funnel.grain_schedule_attempts.store(0, std::memory_order_relaxed);
  g_funnel.grains_scheduled.store(0, std::memory_order_relaxed);
  g_funnel.grains_rendered.store(0, std::memory_order_relaxed);
}

float vocal_fx_latest_pitch_age_ms() {
  int64_t last = g_sync.last_pitch_published_us.load(std::memory_order_relaxed);
  if (last <= 0) return 999999.0f;
#ifdef ESP_PLATFORM
  int64_t now = esp_timer_get_time();
#else
  int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  if (now < last) return 0.0f;
  return static_cast<float>(now - last) * 0.001f;
}

float vocal_fx_effective_harmony_mix(size_t voice) {
  if (voice < 2) return e.harmony_mix[voice];
  return 0.0f;
}

PitchSyncDiagnostics vocal_fx_pitch_sync_diagnostics() {
  PitchSyncDiagnostics d{};
  d.try_pitch_attempts = g_sync.try_pitch_attempts.load(std::memory_order_relaxed);
  d.try_pitch_successes = g_sync.try_pitch_successes.load(std::memory_order_relaxed);
  d.try_marks_attempts = g_sync.try_marks_attempts.load(std::memory_order_relaxed);
  d.try_marks_successes = g_sync.try_marks_successes.load(std::memory_order_relaxed);
  d.try_marks_start_gt_end = g_sync.try_marks_start_gt_end.load(std::memory_order_relaxed);
  d.last_mark_count = g_sync.last_mark_count.load(std::memory_order_relaxed);
  d.last_mark_end = g_sync.last_mark_end.load(std::memory_order_relaxed);
  return d;
}

