#include "vocal_fx.h"
#include "biquad.h"
#include "compressor.h"
#include "delay.h"
#include "fdn_reverb.h"
#include "gate.h"
#include "limiter.h"
#include "parameter_queue.h"
#include "profiling.h"
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
  bool ready = false;
  float work[VOCAL_FX_MAX_BLOCK_SIZE], left[VOCAL_FX_MAX_BLOCK_SIZE],
      right[VOCAL_FX_MAX_BLOCK_SIZE];
  float gate_t = -55, gate_a = 5, gate_h = 40, gate_r = 120, gate_range = -60,
        comp_t = -18, comp_ratio = 3, comp_a = 10, comp_r = 100,
        comp_makeup = 3, comp_knee = 6, delay_l = 250, delay_r = 375,
        delay_dry = 1, delay_wet = .2f;
};
Engine e;
ParameterQueue<64> parameter_queue;
struct PitchMailbox {
  std::atomic<uint32_t> seq{0};
  std::atomic<float> hz{0}, confidence{0};
  std::atomic<bool> voiced{false};
  std::atomic<uint64_t> timestamp{0};
} pitch;
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
  parameter_queue.reset();
  e.ready = true;
  return true;
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
}
void vocal_fx_process(const float *in, float *ol, float *orr, size_t frames) {
  if (!e.ready || !in || !ol || !orr)
    return;
  while (frames) {
    size_t n = std::min<size_t>(frames, VOCAL_FX_MAX_BLOCK_SIZE);
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
    VF_PROFILE_BEGIN(e.profiler, ProfileSection::Delay);
    for (size_t i = 0; i < n; i++) {
      e.left[i] = e.right[i] = e.work[i];
      if (e.cfg.enable_delay)
        e.delay.process(e.work[i], e.left[i], e.right[i]);
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
  }
}
} // namespace
void vocal_fx_set_parameter(VocalFxParameter p, float v) {
  // Deliberately non-blocking. If the SPSC queue is saturated, retaining the
  // last complete audio-thread state is safer than a partial cross-core update.
  (void)parameter_queue.push({p, v});
}
void vocal_fx_publish_pitch(const PitchResult &r) {
  pitch.seq.fetch_add(1, std::memory_order_acq_rel);
  pitch.hz.store(r.frequency_hz, std::memory_order_relaxed);
  pitch.confidence.store(r.confidence, std::memory_order_relaxed);
  pitch.voiced.store(r.voiced, std::memory_order_relaxed);
  pitch.timestamp.store(r.timestamp_samples, std::memory_order_relaxed);
  pitch.seq.fetch_add(1, std::memory_order_release);
}
PitchResult vocal_fx_latest_pitch() {
  PitchResult r;
  uint32_t a, b;
  do {
    a = pitch.seq.load(std::memory_order_acquire);
    r.frequency_hz = pitch.hz.load(std::memory_order_relaxed);
    r.confidence = pitch.confidence.load(std::memory_order_relaxed);
    r.voiced = pitch.voiced.load(std::memory_order_relaxed);
    r.timestamp_samples = pitch.timestamp.load(std::memory_order_relaxed);
    b = pitch.seq.load(std::memory_order_acquire);
  } while (a != b || (a & 1));
  return r;
}
size_t vocal_fx_dsp_memory_bytes() {
  return e.delay.memory_bytes() + e.reverb.memory_bytes();
}

VocalFxProfileStats vocal_fx_profile_stats(VocalFxProfileSection section) {
  const auto stats = e.profiler.stats(static_cast<ProfileSection>(section));
  return {stats.calls, stats.total_us, stats.max_us, stats.deadline_misses};
}
