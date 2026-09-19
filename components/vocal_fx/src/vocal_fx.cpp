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
#include "esp_attr.h"
#include "esp_heap_caps.h"
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
  TdPsola pitch_shift[MAX_HARMONY_VOICES];
  HarmonyEngine harmony;
  MidiChordState midi;
  bool ready = false;
  float work[VOCAL_FX_MAX_BLOCK_SIZE], left[VOCAL_FX_MAX_BLOCK_SIZE],
      right[VOCAL_FX_MAX_BLOCK_SIZE];
  float shifted[MAX_HARMONY_VOICES][VOCAL_FX_MAX_BLOCK_SIZE];
  float delay_wet_l[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float delay_wet_r[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float rev_wet_l[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float rev_wet_r[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float dry_bus[VOCAL_FX_MAX_BLOCK_SIZE]{};
  float harm_bus_mono[VOCAL_FX_MAX_BLOCK_SIZE]{};
  SmoothedValue delay_to_reverb_send;
  float harmony_mix[MAX_HARMONY_VOICES]{};
  float last_wanted_mix[MAX_HARMONY_VOICES]{};
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
// B4D.6: the product has a single harmony voice, so any legacy voice index
// resolves to voice 0 (the second TD-PSOLA instance no longer exists).
inline TdPsola &vf_voice(size_t) { return e.pitch_shift[0]; }
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

VocalFxLimiterDiagnostics s_limiter_diag{};

// Control-plane observability only: the exact float value handed to the engine
// for each parameter at the last block-boundary drain. No DSP behavior change
// and no effect on any audio computation.
constexpr size_t kAppliedTraceSize = 128;
float s_last_applied[kAppliedTraceSize] = {};
bool s_applied_valid[kAppliedTraceSize] = {};
uint64_t s_applied_count = 0;

constexpr size_t kHarmonizerTraceCapacity = 512;
static HarmonizerBlockTraceRecord *s_harmonizer_trace = nullptr;
std::atomic<uint32_t> s_harmonizer_trace_head{0};
std::atomic<uint32_t> s_harmonizer_trace_count{0};
std::atomic<uint32_t> s_harmonizer_block_index{0};

void record_harmonizer_trace(const HarmonizerBlockTraceRecord &rec) {
  if (!s_harmonizer_trace) return;
  uint32_t head = s_harmonizer_trace_head.load(std::memory_order_relaxed);
  s_harmonizer_trace[head] = rec;
  s_harmonizer_trace_head.store((head + 1) % kHarmonizerTraceCapacity, std::memory_order_relaxed);
  uint32_t c = s_harmonizer_trace_count.load(std::memory_order_relaxed);
  if (c < kHarmonizerTraceCapacity) {
    s_harmonizer_trace_count.store(c + 1, std::memory_order_relaxed);
  }
}
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

PitchAnalysisConfig
vocal_fx_p4_pitch_analysis_defaults(float input_sample_rate) {
  PitchAnalysisConfig config{};
  config.input_sample_rate = input_sample_rate;
  config.yin_difference = YinDifferenceVariant::IncrementalF32;
  config.yin_incremental_rebase_hops = 64;
  config.yin_cmnd = YinCmndVariant::F32Compensated;
  config.yin_energy = YinEnergyVariant::F32Compensated;
  config.pitch_mark_ncc = PitchMarkNccVariant::ContiguousMulti8;
  return config;
}

VocalFxEffectiveDspConfig vocal_fx_effective_dsp_config() {
  const auto &pitch = e.pitch_analysis.effective_config();
  const auto &lpc = e.lpc_analysis.effective_config();
  return {pitch.yin_difference,
          pitch.yin_incremental_rebase_hops,
          pitch.yin_energy,
          pitch.yin_cmnd,
          pitch.pitch_mark_ncc,
          lpc.windowing,
          lpc.autocorrelation};
}

bool vocal_fx_init(const VocalFxConfig &c) {
  e.ready = false;
  if (!s_harmonizer_trace) {
#ifdef ESP_PLATFORM
    s_harmonizer_trace = static_cast<HarmonizerBlockTraceRecord *>(
        heap_caps_calloc(kHarmonizerTraceCapacity, sizeof(HarmonizerBlockTraceRecord),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_harmonizer_trace) {
      s_harmonizer_trace = static_cast<HarmonizerBlockTraceRecord *>(
          heap_caps_calloc(kHarmonizerTraceCapacity, sizeof(HarmonizerBlockTraceRecord),
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
#else
    s_harmonizer_trace = static_cast<HarmonizerBlockTraceRecord *>(
        std::calloc(kHarmonizerTraceCapacity, sizeof(HarmonizerBlockTraceRecord)));
#endif
  }
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
  e.harmony_mix[0] = 0.0f;
  e.last_wanted_mix[0] = 0.0f;
  const float send_target = (c.spatial_routing == SpatialFxRouting::DelayIntoReverb) ? 1.0f : 0.0f;
  e.delay_to_reverb_send.init(send_target, c.sample_rate, 20.0f);
  if (c.enable_pitch_analysis) {
    PitchAnalysisConfig pitch_config =
#ifdef ESP_PLATFORM
        vocal_fx_p4_pitch_analysis_defaults(c.sample_rate);
#else
        PitchAnalysisConfig{};
#endif
    pitch_config.input_sample_rate = c.sample_rate;
    // B4D.3S: keep an exact 4:1 decimation at every input rate so the pitch
    // analysis runs at the same relative rate (12000 Hz at 48 kHz, unchanged).
    pitch_config.analysis_sample_rate = c.sample_rate * 0.25f;
#ifdef ESP_PLATFORM
    // P4 production default selected by the B4B.6A optimization.
    pitch_config.pitch_mark_ncc = PitchMarkNccVariant::ContiguousMulti8;
#endif
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
  for (size_t i = 0; i < kAppliedTraceSize; ++i)
    s_applied_valid[i] = false;
  s_applied_count = 0;
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
  s_limiter_diag = {};
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
  e.harmony_mix[0] = 0.0f;
  e.last_wanted_mix[0] = 0.0f;
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

// B4D.5 engine-level global-section attribution by renderer class.  Buckets
// match B4D.4: 0=NMC+0, 1=NMC+1, 2=MC+1, 3=MC+2+.  Groups: 0=global pre,
// 1=compressor, 2=delay, 3=reverb, 4=global post, 5=harmony/voice.
namespace {
constexpr size_t kB4D5GlobalGroups = 7;
constexpr size_t kB4D5GlobalBuckets = 4;
bool s_b4d5_global_enabled = false;
uint64_t s_b4d5_global_cycles[kB4D5GlobalBuckets][kB4D5GlobalGroups] = {};
uint64_t s_b4d5_global_blocks[kB4D5GlobalBuckets] = {};
uint64_t s_b4d5_global_snapshot[kB4D5GlobalGroups] = {};
const char *const kB4D5GlobalNames[kB4D5GlobalGroups] = {
    "global_pre", "compressor", "delay", "reverb", "global_post",
    "harmony_voice", "pipeline_residual"};

uint64_t b4d5_group_cycles(size_t group) {
  switch (group) {
  case 0: // global pre
    return e.profiler.raw_total_cycles(ProfileSection::PitchLpcTap) +
           e.profiler.raw_total_cycles(ProfileSection::ParameterQueue) +
           e.profiler.raw_total_cycles(ProfileSection::InputHpf) +
           e.profiler.raw_total_cycles(ProfileSection::InputGate);
  case 1:
    return e.profiler.raw_total_cycles(ProfileSection::Compressor);
  case 2:
    return e.profiler.raw_total_cycles(ProfileSection::DelayPrep) +
           e.profiler.raw_total_cycles(ProfileSection::Delay);
  case 3:
    return e.profiler.raw_total_cycles(ProfileSection::ReverbPrep) +
           e.profiler.raw_total_cycles(ProfileSection::Reverb);
  case 4: // global post
    return e.profiler.raw_total_cycles(ProfileSection::BusMixing) +
           e.profiler.raw_total_cycles(ProfileSection::MasterMix) +
           e.profiler.raw_total_cycles(ProfileSection::MasterLimiter);
  case 5: // harmony / voice
    return e.profiler.raw_total_cycles(ProfileSection::Harmony) +
           e.profiler.raw_total_cycles(ProfileSection::DryAlignment) +
           e.profiler.raw_total_cycles(ProfileSection::HarmonySlewPan) +
           e.profiler.raw_total_cycles(ProfileSection::HarmonyLimiter) +
           e.profiler.raw_total_cycles(ProfileSection::PitchMarkSync);
  default: { // 6: everything the Pipeline section measured but no group owns
    const uint64_t total =
        e.profiler.raw_total_cycles(ProfileSection::Pipeline);
    uint64_t owned = 0;
    for (size_t g = 0; g < 6; ++g) owned += b4d5_group_cycles(g);
    return total > owned ? total - owned : 0;
  }
  }
}
} // namespace

void vocal_fx_process(const float *in, float *ol, float *orr, size_t frames) {
  if (!e.ready || !in || !ol || !orr)
    return;
  while (frames) {
    size_t n = std::min<size_t>(frames, VOCAL_FX_MAX_BLOCK_SIZE);
    if (s_b4d5_global_enabled)
      for (size_t g = 0; g < kB4D5GlobalGroups; ++g)
        s_b4d5_global_snapshot[g] = b4d5_group_cycles(g);
    [[maybe_unused]] uint64_t deadline =
        (uint64_t)(1000000.0 * n / e.cfg.sample_rate);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Pipeline);
    const uint32_t c_pipe_start = Profiler::now_cycles();

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
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::InputHpf);
    for (size_t i = 0; i < n; i++) {
      float x = e.cfg.isolate_pitch_shift_output ? in[i] : e.hpf.process(in[i]);
      e.work[i] = x;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::InputHpf, 0);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::InputGate);
    if (e.cfg.enable_gate) {
      e.gate.process_block(e.work, n);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::InputGate, 0);
    VF_PROFILE_END(e.profiler, ProfileSection::Input, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Compressor);
    if (e.cfg.enable_compressor)
      e.compressor.process_block(e.work, n);
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
    if (e.cfg.enable_pitch_analysis && e.pitch_shift[0].enabled()) {
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
    const uint32_t c_harm_block_start = Profiler::now_cycles();
    e.pitch_resources.push(e.work,n);
    const HarmonyVoiceTarget target=e.harmony.update(current_pitch.frequency_hz,
                                         current_pitch.voiced,e.midi);
#ifndef ESP_PLATFORM
    SampleTelemetryRecord telem_records[64];
    const size_t iso_v = 0;
    if (s_sample_telemetry_cb) {
      e.pitch_shift[0].set_sample_telemetry_buffer(telem_records);
    }
#endif
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::HarmonyVoice0);
    e.pitch_shift[0].set_enabled(e.harmony.voice().enabled);
    if(target.valid) {
      g_funnel.harmony_target_activations.fetch_add(1, std::memory_order_relaxed);
      e.pitch_shift[0].set_ratio(target.target_frequency_hz/current_pitch.frequency_hz);
    }
    e.pitch_shift[0].process_shared(e.work,e.shifted[0],n,current_pitch,
        vocal_fx_pitch_track_state(),e.pitch_shift_marks,e.pitch_shift_mark_count);
    VF_PROFILE_END(e.profiler, ProfileSection::HarmonyVoice0, 0);
    // B4C.8B: record per-block data after the voice is processed.
    {
      const uint32_t blk_id = s_harmonizer_block_index.load(std::memory_order_relaxed);
      const uint8_t voice_class = target.valid ? 1 : 0;
      const float pitch_f0 = current_pitch.frequency_hz;
      e.pitch_shift[0].b4c8b_record_block(blk_id, 0, voice_class, pitch_f0);
    }
    const uint32_t c_harm_block_end = Profiler::now_cycles();
    VF_PROFILE_END(e.profiler, ProfileSection::Harmony, 0);

    const float cycles_to_us = 1.0f / static_cast<float>(Profiler::cycles_per_us());
    HarmonizerBlockTraceRecord trace_rec{};
    trace_rec.block_index = s_harmonizer_block_index.fetch_add(1, std::memory_order_relaxed);
    trace_rec.block_runtime_us = static_cast<float>(c_harm_block_end - c_harm_block_start) * cycles_to_us;
    trace_rec.pipeline_base_us = static_cast<float>(c_harm_block_start > c_pipe_start ? (c_harm_block_start - c_pipe_start) : 0) * cycles_to_us;
    trace_rec.voice0_runtime_us = static_cast<float>(e.pitch_shift[0].last_block_cycles()) * cycles_to_us;
    trace_rec.new_grains_scheduled_v0 = e.pitch_shift[0].last_grains_scheduled();
    trace_rec.active_grains_rendered_v0 = e.pitch_shift[0].last_grains_rendered();
    trace_rec.source_grains_built_v0 = e.pitch_shift[0].last_source_grains_built();
    trace_rec.grain_samples_processed = e.pitch_shift[0].last_grain_samples_processed();
    trace_rec.unique_grain_samples = e.pitch_shift[0].last_unique_grain_samples();
    trace_rec.unique_source_grain_keys = e.pitch_shift[0].last_unique_source_grains();
    trace_rec.duplicate_source_grain_keys = e.pitch_shift[0].last_duplicate_source_grains();
    trace_rec.model_warp_lookups = e.pitch_shift[0].last_model_warp_lookups();
    trace_rec.model_warp_expensive_calls = e.pitch_shift[0].last_model_warp_expensive_calls();
    trace_rec.model_warp_us = static_cast<float>(e.pitch_shift[0].last_model_warp_cycles()) * cycles_to_us;
    trace_rec.lpc_model_timestamp = e.pitch_shift[0].last_model_timestamp();
    trace_rec.formant_model_changed = e.pitch_shift[0].last_model_changed();
    trace_rec.pitch_changed = current_pitch.pitch_changed ? 1 : 0;
    trace_rec.track_state = static_cast<uint8_t>(vocal_fx_pitch_track_state());
    trace_rec.fallback_active = e.pitch_shift[0].fallback_active() ? 1 : 0;
    trace_rec.articulation_active = e.pitch_shift[0].articulation_active() ? 1 : 0;
    trace_rec.plosive_active = e.pitch_shift[0].plosive_bridge_active() ? 1 : 0;
    trace_rec.recovery_active = (e.pitch_shift[0].debug().recovery_class != PsolaRecoveryClass::None) ? 1 : 0;
    trace_rec.render_slack_blocks_min = e.pitch_shift[0].last_render_slack_blocks_min();
    trace_rec.residual_cache_hits_v0 = e.pitch_shift[0].last_residual_cache_hits();
    trace_rec.residual_cache_misses_v0 = e.pitch_shift[0].last_residual_cache_misses();
    trace_rec.fir_samples_computed = e.pitch_shift[0].last_fir_samples_computed();
    trace_rec.fir_samples_reused = e.pitch_shift[0].last_fir_samples_reused();
    trace_rec.active_overlapping_grains_v0 = e.pitch_shift[0].last_active_overlapping_grains();
    trace_rec.precompute_queue_depth = e.pitch_shift[0].last_precompute_queue_depth();
    trace_rec.precompute_expired_count = e.pitch_shift[0].last_precompute_expired_count();
    trace_rec.deferred_active_grains_v0 = e.pitch_shift[0].last_deferred_active_grains();
    trace_rec.deferred_slices_rendered_v0 = e.pitch_shift[0].last_deferred_slices_rendered();
    trace_rec.deferred_fir_samples_v0 = e.pitch_shift[0].last_deferred_fir_samples();
    record_harmonizer_trace(trace_rec);

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
    // B4C.7 exact sqrt hoist: pan, release/usable flags and wanted mix are
    // block-constant (set before this loop; only harmony_mix ramps below),
    // and IEEE sqrt is deterministic, so hoisting preserves bit-identity.
    // B4D.6: single voice, so these are scalars (no per-voice arrays/loop).
    const auto &harmony_voice = e.harmony.voice();
    const float p = std::clamp(harmony_voice.pan, -1.0f, 1.0f);
    const float pan_l = std::sqrt(0.5f * (1.0f - p));
    const float pan_r = std::sqrt(0.5f * (1.0f + p));
    const float wanted = (target.valid || e.pitch_shift[0].is_releasing()) &&
                         e.pitch_shift[0].has_usable_output()
                             ? 1.0f
                             : 0.0f;
    float local_pre_peak = s_limiter_diag.harmony_pre_peak;
    for(size_t i=0;i<n;++i){
      e.last_wanted_mix[0] = wanted;
      const float diff = wanted - e.harmony_mix[0];
      if (diff > 0.0f) {
        e.harmony_mix[0] += std::min(diff, attack_step);
      } else {
        e.harmony_mix[0] += std::max(diff, -release_step);
      }
      const float voice_sig = e.shifted[0][i] * harmony_voice.gain * e.harmony_mix[0];
      const float hl = voice_sig * pan_l;
      const float hr = voice_sig * pan_r;
      harm_bus_l[i] = hl;
      harm_bus_r[i] = hr;
      const float hl_abs = std::fabs(hl);
      const float hr_abs = std::fabs(hr);
      if (hl_abs > local_pre_peak) local_pre_peak = hl_abs;
      if (hr_abs > local_pre_peak) local_pre_peak = hr_abs;
    }
    s_limiter_diag.harmony_pre_peak = local_pre_peak;
    VF_PROFILE_END(e.profiler, ProfileSection::HarmonySlewPan, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::HarmonyLimiter);
    if (e.cfg.enable_harmony_limiter) {
      float local_post_peak = s_limiter_diag.harmony_post_peak;
      for(size_t i=0;i<n;++i){
        e.harmony_limiter.process(harm_bus_l[i], harm_bus_r[i]);
        const float hl_post = std::fabs(harm_bus_l[i]);
        const float hr_post = std::fabs(harm_bus_r[i]);
        if (hl_post > local_post_peak) local_post_peak = hl_post;
        if (hr_post > local_post_peak) local_post_peak = hr_post;
      }
      s_limiter_diag.harmony_post_peak = local_post_peak;
      const float red_db = e.harmony_limiter.reduction_db();
      if (red_db > s_limiter_diag.max_reduction_db) s_limiter_diag.max_reduction_db = red_db;
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
          const size_t iso_v_out = 0;
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

    float source_mono[VOCAL_FX_MAX_BLOCK_SIZE];
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::DelayPrep);
    switch (e.cfg.spatial_source) {
    case SpatialFxSource::DryOnly:
      std::copy_n(e.dry_bus, n, source_mono);
      break;
    case SpatialFxSource::HarmonyOnly:
      std::copy_n(e.harm_bus_mono, n, source_mono);
      break;
    default:
      for (size_t i = 0; i < n; ++i)
        source_mono[i] = (e.left[i] + e.right[i]) * 0.5f;
      break;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::DelayPrep, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Delay);
    if (e.cfg.enable_delay) {
      e.delay.process_wet_block(source_mono, e.delay_wet_l, e.delay_wet_r, n);
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
      // B4D.2 exact block path (bit-identical to the per-sample reference).
      e.reverb.process_block(rev_in, e.rev_wet_l, e.rev_wet_r, n);
    } else {
      std::fill_n(e.rev_wet_l, n, 0.0f);
      std::fill_n(e.rev_wet_r, n, 0.0f);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Reverb, 0);

    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Master);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::MasterMix);
    for (size_t i = 0; i < n; i++) {
      e.left[i] += e.delay_wet_l[i] + e.rev_wet_l[i];
      e.right[i] += e.delay_wet_r[i] + e.rev_wet_r[i];
    }
    VF_PROFILE_END(e.profiler, ProfileSection::MasterMix, 0);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::MasterLimiter);
    for (size_t i = 0; i < n; i++) {
      e.limiter.process(e.left[i], e.right[i]);
      ol[i] = e.left[i];
      orr[i] = e.right[i];
      const float ml_abs = std::fabs(ol[i]);
      const float mr_abs = std::fabs(orr[i]);
      if (ml_abs > s_limiter_diag.master_peak) s_limiter_diag.master_peak = ml_abs;
      if (mr_abs > s_limiter_diag.master_peak) s_limiter_diag.master_peak = mr_abs;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::MasterLimiter, 0);
    VF_PROFILE_END(e.profiler, ProfileSection::Master, 0);
    VF_PROFILE_END(e.profiler, ProfileSection::Pipeline, deadline);
    if (s_b4d5_global_enabled) {
      const uint8_t ng = e.pitch_shift[0].last_grains_scheduled();
      const uint8_t mc = e.pitch_shift[0].last_model_changed();
      size_t bucket;
      if (mc)
        bucket = ng <= 1 ? 2 : 3;
      else
        bucket = ng == 0 ? 0 : 1;
      ++s_b4d5_global_blocks[bucket];
      for (size_t g = 0; g < kB4D5GlobalGroups; ++g) {
        const uint64_t now = b4d5_group_cycles(g);
        if (now >= s_b4d5_global_snapshot[g])
          s_b4d5_global_cycles[bucket][g] += now - s_b4d5_global_snapshot[g];
      }
    }
    in += n;
    ol += n;
    orr += n;
    frames -= n;
  }
}
namespace {
void apply_parameter(VocalFxParameter p, float v) {
  const size_t trace_index = static_cast<size_t>(p);
  if (trace_index < kAppliedTraceSize) {
    s_last_applied[trace_index] = v;
    s_applied_valid[trace_index] = true;
    ++s_applied_count;
  }
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
  case VocalFxParameter::HarmonyScale:if(std::isfinite(v))e.harmony.set_scale_type(static_cast<ScaleType>(std::clamp(static_cast<int>(v),0,static_cast<int>(ScaleType::Count)-1)));break;
  case VocalFxParameter::HarmonyKey:if(std::isfinite(v))e.harmony.set_root(static_cast<uint8_t>(std::clamp(static_cast<int>(v),0,11)));break;
  case VocalFxParameter::HarmonyMode:if(std::isfinite(v))e.harmony.set_mode(static_cast<HarmonyMode>(std::clamp(static_cast<int>(v),0,2)));break;
  case VocalFxParameter::FormantVoice1Mode:
  case VocalFxParameter::FormantVoice2Mode: {
    const size_t voice=static_cast<size_t>(p)-static_cast<size_t>(VocalFxParameter::FormantVoice1Mode);
    vf_voice(voice).set_formants(v>=.5f?FormantMode::Lpc:FormantMode::Off,vf_voice(voice).formant_amount());
    break;
  }
  case VocalFxParameter::FormantVoice1Amount:
  case VocalFxParameter::FormantVoice2Amount: {
    const size_t voice=static_cast<size_t>(p)-static_cast<size_t>(VocalFxParameter::FormantVoice1Amount);
    vf_voice(voice).set_formants(vf_voice(voice).formant_mode(),v);
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
    if(x>=0 && std::isfinite(v)){size_t voice=static_cast<size_t>(x%2);int field=x/2;auto c=e.harmony.voice(voice);
      if(field==0)c.enabled=v>=.5f;else if(field==1)c.interval=v;else if(field==2)c.degree=static_cast<int>(v);else if(field==3)c.gain=std::clamp(v,0.0f,1.0f);else if(field==4)c.pan=std::clamp(v,-1.0f,1.0f);else if(field==5)c.smoothing_ms=std::clamp(v,1.0f,500.0f);
      else if(field==6)c.non_scale_policy=static_cast<NonScaleNotePolicy>(std::clamp(static_cast<int>(v),0,2));
      else if(field==7)c.voice_leading_enabled=v>=.5f;
      else if(field==8)c.min_midi=static_cast<int>(std::clamp(v,0.0f,127.0f));
      else if(field==9)c.max_midi=static_cast<int>(std::clamp(v,0.0f,127.0f));
      e.harmony.set_voice(voice,c);vf_voice(voice).set_smoothing(c.smoothing_ms);}
    break; }
  }
}
} // namespace
void vocal_fx_set_parameter(VocalFxParameter p, float v) {
  // Deliberately non-blocking. If the SPSC queue is saturated, retaining the
  // last complete audio-thread state is safer than a partial cross-core update.
  (void)parameter_queue.push({p, v});
}
bool vocal_fx_try_set_parameter(VocalFxParameter p, float v) {
  return parameter_queue.push({p, v});
}
bool vocal_fx_is_ready() { return e.ready; }
bool vocal_fx_last_applied_parameter(VocalFxParameter p, float *value) {
  const size_t index = static_cast<size_t>(p);
  if (value == nullptr || index >= kAppliedTraceSize || !s_applied_valid[index])
    return false;
  *value = s_last_applied[index];
  return true;
}
uint64_t vocal_fx_applied_parameter_count() { return s_applied_count; }
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
void vocal_fx_set_harmony_enabled(size_t v,bool x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Enabled),x?1:0);}
void vocal_fx_set_harmony_interval(size_t v,float x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Interval),x);}
void vocal_fx_set_harmony_degree(size_t v,int x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Degree),static_cast<float>(x));}
void vocal_fx_set_harmony_gain(size_t v,float x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Gain),x);}
void vocal_fx_set_harmony_pan(size_t v,float x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Pan),x);}
void vocal_fx_set_harmony_smoothing(size_t v,float x){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1Smoothing),x);}
void vocal_fx_set_non_scale_policy(size_t v,NonScaleNotePolicy p){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1NonScalePolicy),static_cast<float>(p));}
void vocal_fx_set_voice_leading(size_t v,bool enabled){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1VoiceLeadingEnabled),enabled?1.0f:0.0f);}
void vocal_fx_set_harmony_range(size_t v,int min_midi,int max_midi){
  if(v<MAX_HARMONY_VOICES){
    vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1MinMidi),static_cast<float>(min_midi));
    vocal_fx_set_parameter(voice_param(v,VocalFxParameter::HarmonyVoice1MaxMidi),static_cast<float>(max_midi));
  }
}
void vocal_fx_set_formant_mode(size_t v,FormantMode mode){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::FormantVoice1Mode),mode==FormantMode::Lpc?1.0f:0.0f);}
void vocal_fx_set_formant_amount(size_t v,float amount){if(v<MAX_HARMONY_VOICES)vocal_fx_set_parameter(voice_param(v,VocalFxParameter::FormantVoice1Amount),amount);}
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
void vocal_fx_set_psola_kernels(PsolaLpcKernel lpc, PsolaOlaKernel ola,
                                PsolaGrainKernel grain, PsolaSynthesisKernel synth) {
  for (auto &voice : e.pitch_shift) {
    voice.set_kernels(lpc, ola, grain, synth);
  }
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
PitchShiftTelemetry vocal_fx_harmony_telemetry(size_t v){return v<MAX_HARMONY_VOICES?e.pitch_shift[v].telemetry():PitchShiftTelemetry{};}
uint32_t vocal_fx_pitch_shift_latency_samples() {
  return e.pitch_shift[0].latency_samples();
}
PitchShiftTelemetry vocal_fx_pitch_shift_telemetry() {
  return e.pitch_shift[0].telemetry();
}
PitchShiftDebug vocal_fx_pitch_shift_debug() { return e.pitch_shift[0].debug(); }
VocalFxProfileStats
vocal_fx_harmony_voice_profile_stats(size_t voice, PitchShiftProfileSection s) {
  if (voice >= 2)
    return {};
  const auto stats = vf_voice(voice).profile(s);
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses,
          stats.total_cycles, stats.max_cycles};
}

VocalFxProfileStats
vocal_fx_pitch_shift_profile_stats(PitchShiftProfileSection s) {
  return vocal_fx_harmony_voice_profile_stats(0, s);
}

VocalFxLimiterDiagnostics vocal_fx_limiter_diagnostics() {
  return s_limiter_diag;
}

void vocal_fx_reset_limiter_diagnostics() {
  s_limiter_diag = {};
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
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses,
          stats.total_cycles, stats.max_cycles};
}
size_t vocal_fx_dsp_memory_bytes() {
  return e.delay.memory_bytes() + e.reverb.memory_bytes() +
         e.pitch_resources.memory_bytes() +
         e.pitch_shift[0].memory_bytes() +
         e.lpc_analysis.memory_bytes() +
         (e.cfg.enable_pitch_analysis ? e.pitch_analysis.memory_bytes() : 0);
}
size_t vocal_fx_delay_memory_bytes() {
  return e.delay.memory_bytes();
}
size_t vocal_fx_reverb_memory_bytes() {
  return e.reverb.memory_bytes();
}
LpcTelemetry vocal_fx_lpc_telemetry(){auto x=e.lpc_analysis.telemetry();x.voice1_formant_frames=e.pitch_shift[0].telemetry().formant_frames;x.voice2_formant_frames=0;return x;}
VocalFxProfileStats vocal_fx_lpc_profile_stats(LpcProfileSection s){const auto x=e.lpc_analysis.profile(s);return{x.calls,x.total_us,x.max_us,x.deadline_misses,x.total_cycles,x.max_cycles};}
LpcFrameCostSummary vocal_fx_lpc_frame_cost_summary() {
  return e.lpc_analysis.frame_cost_summary();
}

GrainRejectionTelemetry vocal_fx_grain_rejection_telemetry(size_t voice) {
  return voice < MAX_HARMONY_VOICES ? vf_voice(voice).grain_rejection_telemetry()
                   : GrainRejectionTelemetry{};
}

namespace {
ProfileSection to_profile_section(VocalFxProfileSection s) {
  switch (s) {
  case VocalFxProfileSection::Input: return ProfileSection::Input;
  case VocalFxProfileSection::Compressor: return ProfileSection::Compressor;
  case VocalFxProfileSection::Delay: return ProfileSection::Delay;
  case VocalFxProfileSection::Reverb: return ProfileSection::Reverb;
  case VocalFxProfileSection::Pipeline: return ProfileSection::Pipeline;
  case VocalFxProfileSection::Harmony: return ProfileSection::Harmony;
  case VocalFxProfileSection::Master: return ProfileSection::Master;
  case VocalFxProfileSection::ParameterQueue: return ProfileSection::ParameterQueue;
  case VocalFxProfileSection::PitchLpcTap: return ProfileSection::PitchLpcTap;
  case VocalFxProfileSection::PitchMarkSync: return ProfileSection::PitchMarkSync;
  case VocalFxProfileSection::DryAlignment: return ProfileSection::DryAlignment;
  case VocalFxProfileSection::HarmonySlewPan: return ProfileSection::HarmonySlewPan;
  case VocalFxProfileSection::HarmonyLimiter: return ProfileSection::HarmonyLimiter;
  case VocalFxProfileSection::BusMixing: return ProfileSection::BusMixing;
  case VocalFxProfileSection::DelayPrep: return ProfileSection::DelayPrep;
  case VocalFxProfileSection::ReverbPrep: return ProfileSection::ReverbPrep;
  case VocalFxProfileSection::HarmonyVoice0: return ProfileSection::HarmonyVoice0;
  case VocalFxProfileSection::HarmonyVoice1: return ProfileSection::HarmonyVoice1;
  case VocalFxProfileSection::InputHpf: return ProfileSection::InputHpf;
  case VocalFxProfileSection::InputGate: return ProfileSection::InputGate;
  case VocalFxProfileSection::MasterMix: return ProfileSection::MasterMix;
  case VocalFxProfileSection::MasterLimiter: return ProfileSection::MasterLimiter;
  default: return ProfileSection::Count;
  }
}
} // namespace

VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section) {
  const auto ps = to_profile_section(section);
  if (ps >= ProfileSection::Count) return {};
  const auto stats = e.profiler.stats(ps);
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses,
          stats.total_cycles, stats.max_cycles};
}

void vocal_fx_reset_voice_profilers() {
  e.pitch_shift[0].reset_profiler();
}

// B4C.7 observability additions (no DSP behavior change).
bool vocal_fx_latest_harmonizer_trace(HarmonizerBlockTraceRecord *out) {
  if (!out || !s_harmonizer_trace) return false;
  const uint32_t total =
      s_harmonizer_trace_count.load(std::memory_order_relaxed);
  if (total == 0) return false;
  const uint32_t head =
      s_harmonizer_trace_head.load(std::memory_order_relaxed);
  *out = s_harmonizer_trace[(head + kHarmonizerTraceCapacity - 1) %
                            kHarmonizerTraceCapacity];
  return true;
}

VocalFxProfileStats vocal_fx_voice_profile_stats(
    size_t voice, PitchShiftProfileSection section) {
  if (voice >= 2 ||
      static_cast<size_t>(section) >=
          static_cast<size_t>(PitchShiftProfileSection::Count))
    return {};
  const auto stats = vf_voice(voice).profile(section);
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses,
          stats.total_cycles, stats.max_cycles};
}

uint16_t vocal_fx_lpc_config_order() {
  return e.lpc_analysis.effective_config().order;
}

uint16_t vocal_fx_synthesis_order(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).synthesis_order();
}

uint64_t vocal_fx_voice_b4c7_cycles(size_t voice, size_t idx) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c7_section_cycles(idx);
}

uint64_t vocal_fx_voice_other_cycles(size_t voice, size_t cat_index) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_other_cycles(cat_index);
}

void vocal_fx_init_b4c8_recorders(bool slow_blocks, bool stalls) {
  for (auto &ps : e.pitch_shift)
    ps.b4c8_init_recorders();
  (void)slow_blocks;
  (void)stalls;
}

void vocal_fx_free_b4c8_recorders() {
  for (auto &ps : e.pitch_shift)
    ps.b4c8_free_recorders();
}

size_t vocal_fx_slow_block_count(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).slow_block_count_;
}

bool vocal_fx_get_slow_block(size_t voice, size_t index, B4c8SlowBlockRecord *out) {
  if (voice >= 2 || !out) return false;
  return vf_voice(voice).b4c8_get_slow_block(index, out);
}

size_t vocal_fx_stall_event_count() {
  // Stall events are shared across voices; report from voice 0's recorder.
  return e.pitch_shift[0].stall_count_;
}

bool vocal_fx_get_stall_event(size_t index, B4c8StallEvent *out) {
  if (!out) return false;
  return e.pitch_shift[0].b4c8_get_stall_event(index, out);
}

uint64_t vocal_fx_deferred_zero_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_deferred_zero_cycles_;
}
uint64_t vocal_fx_deferred_zero_calls(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_deferred_zero_calls_;
}
uint64_t vocal_fx_deferred_active_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_deferred_active_cycles_;
}
uint64_t vocal_fx_deferred_active_calls(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_deferred_active_calls_;
}
uint64_t vocal_fx_loop_history_fetch_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_loop_history_fetch_cycles_;
}
uint64_t vocal_fx_loop_ola_norm_reset_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_loop_ola_norm_reset_cycles_;
}
uint64_t vocal_fx_loop_indexing_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_loop_indexing_cycles_;
}
uint64_t vocal_fx_model_change_blocks(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_model_change_blocks_;
}
uint64_t vocal_fx_model_change_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_model_change_cycles_;
}
uint64_t vocal_fx_no_model_change_blocks(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_no_model_change_blocks_;
}
uint64_t vocal_fx_no_model_change_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_no_model_change_cycles_;
}
uint64_t vocal_fx_model_change_max_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_model_change_max_cycles_;
}
uint64_t vocal_fx_no_model_change_max_cycles(size_t voice) {
  if (voice >= 2) return 0;
  return vf_voice(voice).b4c8_no_model_change_max_cycles_;
}

void vocal_fx_set_slice_audit_enabled(bool enabled) {
  e.pitch_resources.set_slice_audit_enabled(enabled);
}

void vocal_fx_reset_slice_audit() {
  e.pitch_resources.reset_slice_audit();
}

void vocal_fx_next_slice_audit_block() {
  e.pitch_resources.next_slice_audit_block();
}

B4C7SliceAuditSnapshot vocal_fx_slice_audit_snapshot() {
  const auto &r = e.pitch_resources;
  return {r.slice_audit_requested_, r.slice_audit_duplicates_,
          r.slice_audit_cross_voice_, r.slice_audit_same_voice_,
          r.slice_audit_samples_, r.slice_audit_reusable_samples_,
          r.slice_audit_taps_, r.slice_audit_reusable_taps_,
          r.slice_audit_entry_drops_};
}

void vocal_fx_reset_profiler() {
  e.profiler.reset();
  vocal_fx_reset_voice_profilers();
}

void vocal_fx_reset_profiling_epoch() {
  e.profiler.reset();
  e.pitch_shift[0].reset_forensic_stats();
  e.pitch_resources.reset_audit_forensics();
  vocal_fx_reset_harmonizer_trace();
  vocal_fx_reset_limiter_diagnostics();
  vocal_fx_reset_measurement_telemetry();
}

VocalFxProfileDistribution
vocal_fx_pitch_profile_distribution(PitchAnalysisProfileSection section) {
  const auto d = e.pitch_analysis.profile_distribution(section);
  return {d.p50_us, d.p95_us, d.p99_us, d.samples};
}

void vocal_fx_reset_measurement_telemetry() {
  e.pitch_analysis.reset_measurement_telemetry();
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
#if SOC_MEM_TCM_SUPPORTED
    out[count].is_sram = out[count].is_sram || (ptr && esp_ptr_in_tcm(ptr));
#endif
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
  record("PitchRollingWindow", e.pitch_analysis.rolling_window_ptr(), e.pitch_analysis.rolling_window_bytes());
  record("PitchLinearWindow", e.pitch_analysis.linear_window_ptr(), e.pitch_analysis.linear_window_bytes());
  record("YinDifferenceArray", e.pitch_analysis.yin_difference_ptr(), e.pitch_analysis.yin_difference_bytes());
  record("YinCmndArray", e.pitch_analysis.yin_cmnd_ptr(), e.pitch_analysis.yin_cmnd_bytes());
  record("LpcFifo", e.lpc_analysis.fifo_ptr(), e.lpc_analysis.fifo_bytes());
  record("LpcCircularFrame", e.lpc_analysis.frame_ptr(), e.lpc_analysis.frame_bytes());
  record("LpcLinearFrame", e.lpc_analysis.linear_frame_ptr(), e.lpc_analysis.linear_frame_bytes());
  record("LpcHann", e.lpc_analysis.hann_ptr(), e.lpc_analysis.hann_bytes());
  record("WorkBuffer", e.work, sizeof(e.work));
  record("LeftBuffer", e.left, sizeof(e.left));
  record("RightBuffer", e.right, sizeof(e.right));
  if (e.pitch_shift[0].residual_cache_pool_ptr()) {
    record("ResidualCacheV0", e.pitch_shift[0].residual_cache_pool_ptr(), e.pitch_shift[0].residual_cache_bytes());
  }
  if (e.pitch_shift[0].deferred_grains_ptr()) {
    record("DeferredGrainsV0", e.pitch_shift[0].deferred_grains_ptr(), e.pitch_shift[0].deferred_grains_bytes());
  }

  return count;
}

PitchShiftDebug vocal_fx_harmony_debug(size_t voice) {
  if (voice < MAX_HARMONY_VOICES)
    return vf_voice(voice).debug();
  return {};
}

PitchAnalysisDebug vocal_fx_pitch_analysis_debug() {
  return {};
}

PitchAnalysisAuditTelemetry vocal_fx_pitch_analysis_audit_telemetry() {
  return e.pitch_analysis.audit_telemetry();
}

float vocal_fx_pitch_backlog_ms() {
  return e.pitch_analysis.current_backlog_ms();
}

YinForensicTelemetry vocal_fx_yin_forensic_telemetry() {
  return e.pitch_analysis.yin_forensic_telemetry();
}

PitchMarkForensicTelemetry vocal_fx_pitch_mark_forensic_telemetry() {
  return e.pitch_analysis.mark_forensic_telemetry();
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
  if (voice < MAX_HARMONY_VOICES) return e.harmony_mix[voice];
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

void vocal_fx_record_harmonizer_trace(const HarmonizerBlockTraceRecord &rec) {
  record_harmonizer_trace(rec);
}

size_t vocal_fx_get_harmonizer_trace(HarmonizerBlockTraceRecord *dst, size_t max_count) {
  if (!dst || max_count == 0 || !s_harmonizer_trace) return 0;
  uint32_t total = s_harmonizer_trace_count.load(std::memory_order_relaxed);
  uint32_t head = s_harmonizer_trace_head.load(std::memory_order_relaxed);
  size_t n = std::min<size_t>(total, max_count);
  uint32_t start = (head + kHarmonizerTraceCapacity - total) % kHarmonizerTraceCapacity;
  for (size_t i = 0; i < n; ++i) {
    dst[i] = s_harmonizer_trace[(start + i) % kHarmonizerTraceCapacity];
  }
  return n;
}

void vocal_fx_reset_harmonizer_trace() {
  s_harmonizer_trace_head.store(0, std::memory_order_relaxed);
  s_harmonizer_trace_count.store(0, std::memory_order_relaxed);
  s_harmonizer_block_index.store(0, std::memory_order_relaxed);
}

void vocal_fx_set_psola_warp_cache(bool enable_local, bool enable_shared, bool enable_neutral_fast_path) {
  e.pitch_shift[0].set_warp_cache_enabled(enable_local);
  e.pitch_shift[0].set_shared_warp_cache_enabled(enable_shared);
  e.pitch_shift[0].set_neutral_warp_fast_path_enabled(enable_neutral_fast_path);
}

PsolaWarpCacheStats vocal_fx_get_psola_warp_cache_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    return vf_voice(voice).warp_cache_stats();
  }
  return PsolaWarpCacheStats{};
}

void vocal_fx_reset_psola_warp_cache_stats() {
  e.pitch_shift[0].reset_warp_cache_stats();
}

PsolaModelWarpAudit vocal_fx_get_psola_model_warp_audit(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    return vf_voice(voice).model_warp_audit();
  }
  return PsolaModelWarpAudit{};
}

PsolaSourceGrainAudit vocal_fx_get_psola_source_grain_audit() {
  return e.pitch_resources.shared_source_grain_audit;
}

PsolaSchedulingSlackAudit vocal_fx_get_psola_scheduling_slack_audit() {
  return e.pitch_resources.shared_scheduling_slack_audit;
}

void vocal_fx_configure_psola_residual_cache(size_t voice, size_t entries, bool enable_residual, bool enable_windowed, bool force_psram) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).configure_source_residual_cache(entries, enable_residual, enable_windowed, force_psram);
  }
}

void vocal_fx_set_psola_residual_cache_enabled(size_t voice, bool enabled) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).set_residual_cache_enabled(enabled);
  }
}

void vocal_fx_set_psola_windowed_cache_enabled(size_t voice, bool enabled) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).set_windowed_cache_enabled(enabled);
  }
}

PsolaSourceResidualCacheStats vocal_fx_get_psola_residual_cache_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    return vf_voice(voice).residual_cache_stats();
  }
  return PsolaSourceResidualCacheStats{};
}

void vocal_fx_reset_psola_residual_cache_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).reset_residual_cache_stats();
  }
}

void vocal_fx_set_psola_precompute_enabled(size_t voice, bool enabled, float horizon) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).set_precompute_enabled(enabled, horizon);
  }
}

PsolaPrecomputeStats vocal_fx_get_psola_precompute_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    return vf_voice(voice).precompute_stats();
  }
  return PsolaPrecomputeStats{};
}

void vocal_fx_reset_psola_precompute_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).reset_precompute_stats();
  }
}

void vocal_fx_set_psola_grain_render_mode(size_t voice, PsolaGrainRenderMode mode) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).set_grain_render_mode(mode);
  }
}

void vocal_fx_set_psola_fir_kernel(size_t voice, PsolaFirKernel kernel) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).set_fir_kernel(kernel);
  }
}

PsolaDeferredStats vocal_fx_get_psola_deferred_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    return vf_voice(voice).deferred_stats();
  }
  return PsolaDeferredStats{};
}

void vocal_fx_reset_psola_deferred_stats(size_t voice) {
  if (voice < MAX_HARMONY_VOICES) {
    vf_voice(voice).reset_deferred_stats();
  }
}

SingleGrainBenchmarkResult vocal_fx_benchmark_single_grain(
    size_t grain_length, size_t order, PsolaFirKernel kernel) {
  return TdPsola::benchmark_single_grain(
      grain_length, order, e.pitch_resources, e.lpc_analysis, kernel);
}

// ── B4C.8B: per-block recording and MC delta breakdown ──────────────────

void vocal_fx_init_b4c8b_recorder() {
  e.pitch_shift[0].b4c8b_init_recorder();
}

void vocal_fx_free_b4c8b_recorder() {
  e.pitch_shift[0].b4c8b_free_recorder();
}

void vocal_fx_print_b4c8b_block_records() {
  e.pitch_shift[0].b4c8b_print_block_records();
}

void vocal_fx_print_b4c8b_mc_delta_breakdown() {
  e.pitch_shift[0].b4c8b_print_mc_delta_breakdown();
}

uint32_t vocal_fx_b4c8b_block_count() {
  return s_harmonizer_block_index.load(std::memory_order_relaxed);
}

bool vocal_fx_get_b4c8b_block_record(size_t voice, size_t index,
                                     B4c8bBlockRecord *out) {
  if (voice >= 2 || !out) return false;
  return vf_voice(voice).b4c8b_get_block_record(index, out);
}

uint8_t vocal_fx_last_block_model_changed() {
  return static_cast<uint8_t>(e.pitch_shift[0].last_model_changed());
}

VocalFxLastBlockStats vocal_fx_last_block_stats() {
  VocalFxLastBlockStats s;
  s.new_grains = e.pitch_shift[0].last_grains_scheduled();
  s.exp_warp = e.pitch_shift[0].last_model_warp_expensive_calls();
  s.active_desc = e.pitch_shift[0].last_deferred_active_grains();
  s.slices = e.pitch_shift[0].last_deferred_slices_rendered();
  s.f0 = e.pitch_shift[0].debug().source_f0;
  s.source_grains = e.pitch_shift[0].last_source_grains_built();
  s.schedule_attempts = e.pitch_shift[0].last_schedule_attempts();
  s.sched_cycles = e.pitch_shift[0].last_sched_cycles();
  s.addgrain_cycles = e.pitch_shift[0].last_addgrain_cycles();
  s.deferred_cycles = e.pitch_shift[0].last_deferred_cycles();
  s.model_new_count = e.pitch_shift[0].last_model_new_count();
  s.warp_hit_count = e.pitch_shift[0].last_warp_hit_count();
  s.warp_miss_count = e.pitch_shift[0].last_warp_miss_count();
  s.mark_cycles = e.pitch_shift[0].last_mark_cycles();
  s.warp_phase_cycles = e.pitch_shift[0].last_model_warp_cycles();
  s.desc_cycles = e.pitch_shift[0].last_desc_cycles();
  s.near_cycles = e.pitch_shift[0].last_near_cycles();
  s.poly_cycles = e.pitch_shift[0].last_poly_cycles();
  s.gn_cycles = e.pitch_shift[0].last_gn_cycles();
  s.cl_cycles = e.pitch_shift[0].last_cl_cycles();
  s.ch_cycles = e.pitch_shift[0].last_ch_cycles();
  for (size_t i = 0; i < 3; ++i) {
    s.ord_mark[i] = e.pitch_shift[0].ord_mark(i);
    s.ord_warp[i] = e.pitch_shift[0].ord_warp(i);
    s.ord_desc[i] = e.pitch_shift[0].ord_desc(i);
    s.ord_near[i] = e.pitch_shift[0].ord_near(i);
    s.ord_sel[i] = e.pitch_shift[0].ord_sel(i);
    s.ord_align[i] = e.pitch_shift[0].ord_align(i);
  }
  s.mark_count = e.pitch_shift[0].last_mark_count();
  s.prewarm_cycles = e.pitch_shift[0].last_prewarm_cycles();
  s.debt_samples = e.pitch_shift[0].last_debt_samples();
  s.output_period_q8 = e.pitch_shift[0].last_output_period_q8();
  for (size_t i = 0; i < 4; ++i) {
    s.g_dest[i] = e.pitch_shift[0].grain_dest(i);
    s.g_half[i] = e.pitch_shift[0].grain_half(i);
  }
  return s;
}

// ── B4D.2 reverb observability ──────────────────────────────────────────
void vocal_fx_set_reverb_profile_enabled(bool enabled) {
  e.reverb.set_profile_enabled(enabled);
}
void vocal_fx_reset_reverb_profile() { e.reverb.reset_profile(); }
VocalFxReverbProfile vocal_fx_reverb_profile() {
  const FdnReverb::ReverbProfile &p = e.reverb.profile();
  VocalFxReverbProfile out;
  out.read_damping_cycles = p.read_damping_cycles;
  out.mix_cycles = p.mix_cycles;
  out.hadamard_cycles = p.hadamard_cycles;
  out.diffuser_cycles = p.diffuser_cycles;
  out.write_index_cycles = p.write_index_cycles;
  out.wet_mix_cycles = p.wet_mix_cycles;
  out.total_cycles = p.total_cycles;
  out.samples = p.samples;
  out.blocks = p.blocks;
  return out;
}
size_t vocal_fx_reverb_line_bytes(size_t i) { return e.reverb.line_bytes(i); }
bool vocal_fx_reverb_line_in_psram(size_t i) {
  return e.reverb.line_in_psram(i);
}
size_t vocal_fx_reverb_diffuser_bytes(size_t i) {
  return e.reverb.diffuser().stage_bytes(i);
}
bool vocal_fx_reverb_diffuser_in_psram(size_t i) {
  return e.reverb.diffuser_in_psram(i);
}
size_t vocal_fx_reverb_total_bytes() { return e.reverb.memory_bytes(); }

void vocal_fx_warp_cache_audit(uint64_t *miss_total, uint64_t *math_only_miss) {
  if (miss_total) *miss_total = td_psola_warp_miss_total();
  if (math_only_miss) *math_only_miss = td_psola_warp_math_only_miss();
}
void vocal_fx_reset_warp_cache_audit() { td_psola_reset_warp_audit(); }

void vocal_fx_lpc_invalid_reasons(uint64_t *unvoiced, uint64_t *solve_fail) {
  SharedLpcAnalysis::invalid_reason_counts(unvoiced, solve_fail);
}
void vocal_fx_reset_lpc_invalid_reasons() {
  SharedLpcAnalysis::reset_invalid_reason_counts();
}

bool vocal_fx_b4d4_class_init() { return td_psola_b4d4_class_init(); }
void vocal_fx_b4d4_class_free() { td_psola_b4d4_class_free(); }
uint64_t vocal_fx_b4d4_class_cycles(size_t bucket, size_t section) {
  return td_psola_b4d4_class_cycles(bucket, section);
}
uint64_t vocal_fx_b4d4_class_blocks(size_t bucket) {
  return td_psola_b4d4_class_blocks(bucket);
}

// B4D.5 exact mark-selection / model-lookup decomposition.
void vocal_fx_b4d5_audit_reset() {
  td_psola_b4d5_mark_audit_reset();
  shared_lpc_b4d5_model_audit_reset();
}
void vocal_fx_b4d5_audit_enable(bool on) {
  td_psola_b4d5_mark_audit_enable(on);
  shared_lpc_b4d5_model_audit_enable(on);
}
void vocal_fx_b4d5_mark_audit(uint64_t *calls, uint64_t *candidates,
                              uint64_t *scan_cycles, uint64_t *total_cycles) {
  td_psola_b4d5_mark_audit(calls, candidates, scan_cycles, total_cycles);
}
void vocal_fx_b4d5_model_audit(uint64_t *calls, uint64_t *candidates,
                               uint64_t *scan_cycles, uint64_t *copies,
                               uint64_t *repeat) {
  shared_lpc_b4d5_model_audit(calls, candidates, scan_cycles, copies, repeat);
}

// B4D.5 engine-level global-section-by-class attribution.
void vocal_fx_b4d5_global_class_init() {
  for (size_t b = 0; b < kB4D5GlobalBuckets; ++b) {
    s_b4d5_global_blocks[b] = 0;
    for (size_t g = 0; g < kB4D5GlobalGroups; ++g)
      s_b4d5_global_cycles[b][g] = 0;
  }
  s_b4d5_global_enabled = true;
}
void vocal_fx_b4d5_global_class_free() { s_b4d5_global_enabled = false; }
uint64_t vocal_fx_b4d5_global_class_cycles(size_t bucket, size_t group) {
  if (bucket >= kB4D5GlobalBuckets || group >= kB4D5GlobalGroups) return 0;
  return s_b4d5_global_cycles[bucket][group];
}
uint64_t vocal_fx_b4d5_global_class_blocks(size_t bucket) {
  return bucket < kB4D5GlobalBuckets ? s_b4d5_global_blocks[bucket] : 0;
}
const char *vocal_fx_b4d5_global_class_name(size_t group) {
  return group < kB4D5GlobalGroups ? kB4D5GlobalNames[group] : "?";
}

// B4D.9 diagnostic prewarm variant.
void vocal_fx_b4d9_set_prewarm_variant(int v) {
  td_psola_b4d9_set_prewarm_variant(v);
}
void vocal_fx_b4d10_set_sched_variant(int v) {
  td_psola_b4d10_set_sched_variant(v);
}


