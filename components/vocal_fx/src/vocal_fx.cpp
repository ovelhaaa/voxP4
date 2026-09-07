#include "vocal_fx.h"
#include "biquad.h"
#include "compressor.h"
#include "delay.h"
#include "fdn_reverb.h"
#include "gate.h"
#include "limiter.h"
#include "parameter_queue.h"
#include "pitch_analysis.h"
#include "profiling.h"
#include "td_psola.h"
#include "harmony_engine.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
struct Engine {
  VocalFxConfig cfg;
  Biquad hpf;
  Gate gate;
  Compressor compressor;
  StereoDelay delay;
  FdnReverb reverb;
  Limiter limiter;
  Profiler profiler;
  PitchAnalysis pitch_analysis;
  SharedPitchShiftResources pitch_resources;
  TdPsola pitch_shift[2];
  HarmonyEngine harmony;
  MidiChordState midi;
  bool ready = false;
  float work[VOCAL_FX_MAX_BLOCK_SIZE], left[VOCAL_FX_MAX_BLOCK_SIZE],
      right[VOCAL_FX_MAX_BLOCK_SIZE];
  float shifted[2][VOCAL_FX_MAX_BLOCK_SIZE];
  float harmony_mix[2]{};
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
  std::atomic<float> hz{0}, confidence{0};
  std::atomic<float> period{0};
  std::atomic<bool> voiced{false};
  std::atomic<bool> onset{false}, changed{false};
  std::atomic<uint64_t> timestamp{0};
} pitch;
constexpr uint32_t kPitchResetting = 1U << 31U;
std::atomic<uint32_t> pitch_users{0};
bool enter_pitch_path() {
  uint32_t state = pitch_users.load(std::memory_order_acquire);
  if (state & kPitchResetting)
    return false;
  return pitch_users.compare_exchange_strong(
      state, state + 1U, std::memory_order_acq_rel, std::memory_order_relaxed);
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
  if (c.enable_pitch_analysis) {
    PitchAnalysisConfig pitch_config{};
    pitch_config.input_sample_rate = c.sample_rate;
    if (!e.pitch_analysis.init(pitch_config))
      return false;
  }
  e.pitch_resources.init();
  for (auto &voice : e.pitch_shift)
    if (!voice.init(c.sample_rate, c.pitch_shift, &e.pitch_resources)) return false;
  HarmonyVoiceConfig legacy{}; legacy.enabled=c.pitch_shift.enabled;
  legacy.interval=c.pitch_shift.semitones; legacy.gain=c.pitch_shift.wet;
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
  e.pitch_resources.reset();
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
    if (e.cfg.enable_pitch_analysis && enter_pitch_path()) {
      e.pitch_analysis.tap(in, n);
      leave_pitch_path();
    }
    apply_pending_parameters();
    [[maybe_unused]] uint64_t deadline =
        (uint64_t)(1000000.0 * n / e.cfg.sample_rate);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Pipeline);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Input);
    for (size_t i = 0; i < n; i++) {
      float x = e.hpf.process(in[i]);
      e.work[i] = e.cfg.enable_gate ? e.gate.process(x) : x;
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Input, 0);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Compressor);
    if (e.cfg.enable_compressor)
      for (size_t i = 0; i < n; i++)
        e.work[i] = e.compressor.process(e.work[i]);
    VF_PROFILE_END(e.profiler, ProfileSection::Compressor, 0);
    PitchResult pitch_candidate;
    if (vocal_fx_try_latest_pitch(&pitch_candidate))
      e.pitch_shift_pitch = pitch_candidate;
    const PitchResult current_pitch = e.pitch_shift_pitch;
    const uint64_t mark_end = current_pitch.analysis_timestamp_samples;
    const uint64_t mark_start = mark_end > PitchAnalysis::kAudioHistory
                                    ? mark_end - PitchAnalysis::kAudioHistory
                                    : 0;
    if (e.cfg.enable_pitch_analysis &&
        (e.pitch_shift[0].enabled() || e.pitch_shift[1].enabled())) {
      size_t candidate_count = 0;
      if (vocal_fx_try_get_pitch_marks(mark_start, mark_end,
                                       e.pitch_shift_candidate_marks,
                                       TdPsola::kMaxMarks, &candidate_count)) {
        std::copy_n(e.pitch_shift_candidate_marks, candidate_count,
                    e.pitch_shift_marks);
        e.pitch_shift_mark_count = candidate_count;
      }
    } else {
      e.pitch_shift_mark_count = 0;
    }
    e.pitch_resources.push(e.work,n);
    const auto targets=e.harmony.update(current_pitch.frequency_hz,
                                         current_pitch.voiced,e.midi);
    for(size_t v=0;v<2;++v){
      e.pitch_shift[v].set_enabled(targets[v].valid);
      if(targets[v].valid) e.pitch_shift[v].set_ratio(targets[v].target_frequency_hz/current_pitch.frequency_hz);
      e.pitch_shift[v].process_shared(e.work,e.shifted[v],n,current_pitch,
          vocal_fx_pitch_track_state(),e.pitch_shift_marks,e.pitch_shift_mark_count);
    }
    // -6 dB headroom, centred dry, equal-power harmony pan. Unvoiced/onset
    // attenuation is performed by each PSOLA voice's acquisition fade.
    for(size_t i=0;i<n;++i){
      float l=e.work[i],r=e.work[i];
      for(size_t v=0;v<2;++v){const auto &c=e.harmony.voice(v);const float p=std::clamp(c.pan,-1.0f,1.0f);
        const float wanted=targets[v].valid&&current_pitch.voiced&&!current_pitch.onset?1.0f:0.0f;
        const float step=1.0f/std::max(1.0f,e.cfg.sample_rate*.020f);
        e.harmony_mix[v]+=std::clamp(wanted-e.harmony_mix[v],-step,step);
        l+=e.shifted[v][i]*c.gain*e.harmony_mix[v]*std::sqrt(.5f*(1-p));r+=e.shifted[v][i]*c.gain*e.harmony_mix[v]*std::sqrt(.5f*(1+p));}
      e.left[i]=.5f*l;e.right[i]=.5f*r;
    }
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Delay);
    for (size_t i = 0; i < n; i++) {
      if (e.cfg.enable_delay)
        e.delay.process((e.left[i]+e.right[i])*.5f, e.left[i], e.right[i]);
    }
    VF_PROFILE_END(e.profiler, ProfileSection::Delay, 0);
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Reverb);
    if (e.cfg.enable_reverb)
      for (size_t i = 0; i < n; i++) {
        float l, r;
        e.reverb.process((e.left[i] + e.right[i]) * .5f, l, r);
        e.left[i] += l;
        e.right[i] += r;
      }
    VF_PROFILE_END(e.profiler, ProfileSection::Reverb, 0);
    for (size_t i = 0; i < n; i++) {
      e.limiter.process(e.left[i], e.right[i]);
      ol[i] = e.left[i];
      orr[i] = e.right[i];
    }
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
    {auto c=e.harmony.voice(0);c.interval=v;e.harmony.set_voice(0,c);}
    break;
  case VocalFxParameter::PitchShiftWet:
    {auto c=e.harmony.voice(0);c.gain=std::clamp(v,0.0f,1.0f);e.harmony.set_voice(0,c);}
    break;
  case VocalFxParameter::HarmonyMode:e.harmony.set_mode(static_cast<HarmonyMode>(std::clamp(static_cast<int>(v),0,2)));break;
  case VocalFxParameter::HarmonyKey:e.harmony.set_root(static_cast<uint8_t>(std::clamp(static_cast<int>(v),0,11)));break;
  case VocalFxParameter::HarmonyScale:e.harmony.set_scale_type(static_cast<ScaleType>(std::clamp(static_cast<int>(v),0,1)));break;
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
  pitch.confidence.store(r.confidence, std::memory_order_relaxed);
  pitch.period.store(r.period_samples, std::memory_order_relaxed);
  pitch.voiced.store(r.voiced, std::memory_order_relaxed);
  pitch.onset.store(r.onset, std::memory_order_relaxed);
  pitch.changed.store(r.pitch_changed, std::memory_order_relaxed);
  pitch.timestamp.store(r.analysis_timestamp_samples
                            ? r.analysis_timestamp_samples
                            : r.timestamp_samples,
                        std::memory_order_relaxed);
  pitch.seq.fetch_add(1, std::memory_order_release);
  pitch.publisher_lock.clear(std::memory_order_release);
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
  candidate.confidence = pitch.confidence.load(std::memory_order_relaxed);
  candidate.period_samples = pitch.period.load(std::memory_order_relaxed);
  candidate.voiced = pitch.voiced.load(std::memory_order_relaxed);
  candidate.onset = pitch.onset.load(std::memory_order_relaxed);
  candidate.pitch_changed = pitch.changed.load(std::memory_order_relaxed);
  candidate.analysis_timestamp_samples =
      pitch.timestamp.load(std::memory_order_relaxed);
  candidate.timestamp_samples = candidate.analysis_timestamp_samples;

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
         (e.cfg.enable_pitch_analysis ? e.pitch_analysis.memory_bytes() : 0);
}

VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section) {
  const auto stats = e.profiler.stats(static_cast<ProfileSection>(section));
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses};
}
