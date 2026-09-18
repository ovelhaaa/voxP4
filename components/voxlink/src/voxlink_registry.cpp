#include "voxlink_registry.h"
#include <cmath>
#include <cstring>

namespace voxlink {
namespace {

constexpr uint32_t kPersistRt = kParamPersistent | kParamRealtimeSafe;
constexpr uint32_t kPersistRtSmoothed =
    kParamPersistent | kParamRealtimeSafe | kParamSmoothed;
constexpr uint32_t kPersistRtDiscrete =
    kParamPersistent | kParamRealtimeSafe | kParamDiscrete;

#define PBOOL(id, key, name, def, flags, group, binding)                       \
  { id, key, name, "", ValueTag::Bool, 0.0f, 1.0f, def, 1.0f, flags, 0.0f,    \
    group, binding }
#define PINT(id, key, name, unit, lo, hi, def, flags, group, binding)          \
  { id, key, name, unit, ValueTag::Int32, lo, hi, def, 1.0f, flags, 0.0f,     \
    group, binding }
#define PENUM(id, key, name, lo, hi, def, group, binding)                      \
  { id, key, name, "", ValueTag::Enum16, lo, hi, def, 1.0f,                    \
    kPersistRtDiscrete, 0.0f, group, binding }
#define PFLOAT(id, key, name, unit, lo, hi, def, step, flags, smooth, group,    \
               binding)                                                        \
  { id, key, name, unit, ValueTag::Float32, lo, hi, def, step, flags, smooth,  \
    group, binding }

// ---------------------------------------------------------------------------
// Canonical parameter inventory. IDs are protocol ABI: once shipped they must
// not change. Ranges mirror the engine's own clamps.
// ---------------------------------------------------------------------------
const ParamDescriptor kParams[] = {
    // Harmony ---------------------------------------------------------------
    PBOOL(0x0100, "harmony.enable", "Harmony Enable", 0.0f, kPersistRt,
          "harmony", "PitchShiftEnabled"),
    PINT(0x0101, "harmony.interval", "Harmony Interval", "semitones", -12.0f,
         12.0f, 0.0f, kPersistRtDiscrete, "harmony", "PitchShiftSemitones"),
    PFLOAT(0x0102, "harmony.level", "Harmony Level", "", 0.0f, 1.0f, 1.0f,
           0.001f, kPersistRtSmoothed, 30.0f, "harmony", "PitchShiftWet"),
    PENUM(0x0103, "harmony.mode", "Harmony Mode", 0.0f, 2.0f, 0.0f, "harmony",
          "HarmonyMode"),
    PENUM(0x0104, "harmony.key", "Harmony Key", 0.0f, 11.0f, 0.0f, "harmony",
          "HarmonyKey"),
    PENUM(0x0105, "harmony.scale", "Harmony Scale", 0.0f, 1.0f, 0.0f, "harmony",
          "HarmonyScale"),
    PFLOAT(0x0107, "harmony.voice1.pan", "Harmony Voice Pan", "", -1.0f, 1.0f,
           0.0f, 0.01f, kPersistRtSmoothed, 30.0f, "harmony",
           "HarmonyVoice1Pan"),
    PINT(0x0108, "harmony.voice1.degree", "Harmony Voice Degree", "degrees",
         -7.0f, 7.0f, 0.0f, kPersistRtDiscrete, "harmony",
         "HarmonyVoice1Degree"),
    PFLOAT(0x0109, "harmony.voice1.smoothing_ms", "Harmony Smoothing", "ms",
           1.0f, 500.0f, 30.0f, 1.0f, kPersistRtSmoothed, 0.0f, "harmony",
           "HarmonyVoice1Smoothing"),
    PBOOL(0x010A, "harmony.formant.enable", "Formant Preservation", 0.0f,
          kPersistRt, "harmony", "FormantVoice1Mode"),
    PFLOAT(0x010B, "harmony.formant.amount", "Formant Amount", "", 0.0f, 1.0f,
           1.0f, 0.01f, kPersistRtSmoothed, 20.0f, "harmony",
           "FormantVoice1Amount"),
    PFLOAT(0x010C, "harmony.attack_ms", "Harmony Attack", "ms", 0.1f, 100.0f,
           4.0f, 0.1f, kPersistRtSmoothed, 0.0f, "harmony", "HarmonyAttackMs"),
    PFLOAT(0x010D, "harmony.release_ms", "Harmony Release", "ms", 1.0f, 500.0f,
           20.0f, 1.0f, kPersistRtSmoothed, 0.0f, "harmony",
           "HarmonyReleaseMs"),
    PBOOL(0x010E, "harmony.limiter.enable", "Harmony Limiter", 1.0f, kPersistRt,
          "harmony", "HarmonyLimiterEnabled"),
    PFLOAT(0x010F, "harmony.limiter.threshold_db", "Harmony Limiter Threshold",
           "dB", -24.0f, 0.0f, -3.0f, 0.5f, kPersistRtSmoothed, 0.0f, "harmony",
           "HarmonyLimiterThresholdDb"),
    PBOOL(0x0110, "harmony.dry_alignment.enable", "Dry Alignment", 0.0f,
          kPersistRt, "harmony", "DryAlignmentEnabled"),
    PFLOAT(0x0111, "harmony.dry_alignment.ms", "Dry Alignment Delay", "ms",
           0.0f, 120.0f, 32.0f, 1.0f, kPersistRtSmoothed, 0.0f, "harmony",
           "DryAlignmentMs"),

    // Dynamics (gate + compressor) ----------------------------------------
    PBOOL(0x0200, "compressor.enable", "Compressor Enable", 1.0f, kPersistRt,
          "dynamics", "EnableCompressor"),
    PFLOAT(0x0201, "compressor.threshold_db", "Compressor Threshold", "dB",
           -60.0f, 0.0f, -18.0f, 0.5f, kPersistRtSmoothed, 0.0f, "dynamics",
           "CompressorThresholdDb"),
    PFLOAT(0x0202, "compressor.ratio", "Compressor Ratio", ":1", 1.0f, 20.0f,
           3.0f, 0.1f, kPersistRtSmoothed, 0.0f, "dynamics",
           "CompressorRatio"),
    PFLOAT(0x0203, "compressor.attack_ms", "Compressor Attack", "ms", 0.1f,
           200.0f, 10.0f, 0.1f, kPersistRtSmoothed, 0.0f, "dynamics",
           "CompressorAttackMs"),
    PFLOAT(0x0204, "compressor.release_ms", "Compressor Release", "ms", 1.0f,
           1000.0f, 100.0f, 1.0f, kPersistRtSmoothed, 0.0f, "dynamics",
           "CompressorReleaseMs"),
    PFLOAT(0x0205, "compressor.makeup_db", "Compressor Makeup", "dB", 0.0f,
           24.0f, 3.0f, 0.5f, kPersistRtSmoothed, 0.0f, "dynamics",
           "CompressorMakeupDb"),
    PFLOAT(0x0206, "compressor.knee_db", "Compressor Knee", "dB", 0.0f, 24.0f,
           6.0f, 0.5f, kPersistRtSmoothed, 0.0f, "dynamics", "CompressorKneeDb"),
    PBOOL(0x0207, "gate.enable", "Gate Enable", 1.0f, kPersistRt, "dynamics",
          "EnableGate"),
    PFLOAT(0x0208, "gate.threshold_db", "Gate Threshold", "dB", -80.0f, 0.0f,
           -55.0f, 1.0f, kPersistRtSmoothed, 0.0f, "dynamics",
           "GateThresholdDb"),
    PFLOAT(0x0209, "gate.attack_ms", "Gate Attack", "ms", 0.1f, 100.0f, 5.0f,
           0.1f, kPersistRtSmoothed, 0.0f, "dynamics", "GateAttackMs"),
    PFLOAT(0x020A, "gate.hold_ms", "Gate Hold", "ms", 0.0f, 500.0f, 40.0f, 1.0f,
           kPersistRtSmoothed, 0.0f, "dynamics", "GateHoldMs"),
    PFLOAT(0x020B, "gate.release_ms", "Gate Release", "ms", 1.0f, 2000.0f,
           120.0f, 1.0f, kPersistRtSmoothed, 0.0f, "dynamics", "GateReleaseMs"),
    PFLOAT(0x020C, "gate.range_db", "Gate Range", "dB", -80.0f, 0.0f, -60.0f,
           1.0f, kPersistRtSmoothed, 0.0f, "dynamics", "GateRangeDb"),

    // Delay -----------------------------------------------------------------
    PBOOL(0x0300, "delay.enable", "Delay Enable", 1.0f, kPersistRt, "delay",
          "EnableDelay"),
    PFLOAT(0x0301, "delay.left_ms", "Delay Left", "ms", 0.0f, 2000.0f, 250.0f,
           1.0f, kPersistRtSmoothed, 0.0f, "delay", "DelayLeftMs"),
    PFLOAT(0x0302, "delay.right_ms", "Delay Right", "ms", 0.0f, 2000.0f, 375.0f,
           1.0f, kPersistRtSmoothed, 0.0f, "delay", "DelayRightMs"),
    PFLOAT(0x0303, "delay.feedback", "Delay Feedback", "", 0.0f, 0.95f, 0.25f,
           0.01f, kPersistRtSmoothed, 0.0f, "delay", "DelayFeedback"),
    PFLOAT(0x0304, "delay.wet", "Delay Wet", "", 0.0f, 1.0f, 0.2f, 0.001f,
           kPersistRtSmoothed, 20.0f, "delay", "DelayWet"),
    PFLOAT(0x0305, "delay.dry", "Delay Dry", "", 0.0f, 1.0f, 1.0f, 0.001f,
           kPersistRtSmoothed, 20.0f, "delay", "DelayDry"),
    PFLOAT(0x0306, "delay.feedback_lowpass_hz", "Delay Feedback Lowpass", "Hz",
           200.0f, 20000.0f, 6000.0f, 1.0f, kPersistRtSmoothed, 0.0f, "delay",
           "DelayFeedbackLowpassHz"),

    // Reverb ----------------------------------------------------------------
    PBOOL(0x0400, "reverb.enable", "Reverb Enable", 1.0f, kPersistRt, "reverb",
          "EnableReverb"),
    PFLOAT(0x0401, "reverb.wet", "Reverb Wet", "", 0.0f, 1.0f, 0.18f, 0.001f,
           kPersistRtSmoothed, 30.0f, "reverb", "ReverbWet"),
    PFLOAT(0x0402, "reverb.decay_s", "Reverb Decay", "s", 0.15f, 20.0f, 2.0f,
           0.01f, kPersistRtSmoothed, 0.0f, "reverb", "ReverbDecaySeconds"),
    PFLOAT(0x0403, "reverb.damping", "Reverb Damping", "", 0.0f, 1.0f, 0.45f,
           0.01f, kPersistRtSmoothed, 0.0f, "reverb", "ReverbDamping"),

    // Output / master -------------------------------------------------------
    PFLOAT(0x0500, "limiter.ceiling", "Limiter Ceiling", "", 0.1f, 1.0f, 0.95f,
           0.01f, kPersistRtSmoothed, 0.0f, "output", "LimiterCeiling"),
    PBOOL(0x0501, "output.mute_dry", "Mute Dry", 0.0f, kPersistRt, "output",
          "MuteDry"),
    PENUM(0x0502, "output.spatial_routing", "Spatial Routing", 0.0f, 1.0f, 1.0f,
          "output", "SpatialRouting"),
    PENUM(0x0503, "output.spatial_source", "Spatial Source", 0.0f, 2.0f, 0.0f,
          "output", "SpatialSource"),
};

#undef PBOOL
#undef PINT
#undef PENUM
#undef PFLOAT

constexpr size_t kParamCount = sizeof(kParams) / sizeof(kParams[0]);

} // namespace

const ParamDescriptor *registry_params() { return kParams; }
size_t registry_count() { return kParamCount; }

const ParamDescriptor *find_param(uint16_t id) {
  for (size_t i = 0; i < kParamCount; ++i)
    if (kParams[i].id == id)
      return &kParams[i];
  return nullptr;
}

const ParamDescriptor *find_param_by_key(const char *key) {
  if (key == nullptr)
    return nullptr;
  for (size_t i = 0; i < kParamCount; ++i)
    if (std::strcmp(kParams[i].key, key) == 0)
      return &kParams[i];
  return nullptr;
}

const char *normalize_status_name(NormalizeStatus status) {
  switch (status) {
  case NormalizeStatus::Ok: return "OK";
  case NormalizeStatus::Clamped: return "CLAMPED";
  case NormalizeStatus::UnknownParam: return "UNKNOWN_PARAM";
  case NormalizeStatus::ReadOnly: return "READ_ONLY";
  case NormalizeStatus::TypeMismatch: return "TYPE_MISMATCH";
  case NormalizeStatus::NonFinite: return "NON_FINITE";
  case NormalizeStatus::EnumInvalid: return "ENUM_INVALID";
  }
  return "UNKNOWN";
}

NormalizeStatus normalize_param_value(uint16_t id, ValueTag tag, double raw,
                                      float *out) {
  if (out == nullptr)
    return NormalizeStatus::UnknownParam;
  if (!std::isfinite(raw))
    return NormalizeStatus::NonFinite;

  const ParamDescriptor *desc = find_param(id);
  if (desc == nullptr)
    return NormalizeStatus::UnknownParam;
  if ((desc->flags & kParamReadOnly) != 0)
    return NormalizeStatus::ReadOnly;
  if (tag != desc->type)
    return NormalizeStatus::TypeMismatch;

  const double lo = static_cast<double>(desc->min_value);
  const double hi = static_cast<double>(desc->max_value);
  const double step = static_cast<double>(desc->step);

  switch (desc->type) {
  case ValueTag::Bool:
    *out = (raw >= 0.5) ? 1.0f : 0.0f;
    return NormalizeStatus::Ok;

  case ValueTag::Int32: {
    bool clamped = false;
    double v = raw;
    if (v < lo) { v = lo; clamped = true; }
    if (v > hi) { v = hi; clamped = true; }
    if (step > 0.0)
      v = lo + std::round((v - lo) / step) * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *out = static_cast<float>(std::llround(v));
    return clamped ? NormalizeStatus::Clamped : NormalizeStatus::Ok;
  }

  case ValueTag::Float32: {
    bool clamped = false;
    double v = raw;
    if (v < lo) { v = lo; clamped = true; }
    if (v > hi) { v = hi; clamped = true; }
    if (step > 0.0)
      v = lo + std::round((v - lo) / step) * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    *out = static_cast<float>(v);
    return clamped ? NormalizeStatus::Clamped : NormalizeStatus::Ok;
  }

  case ValueTag::Enum16: {
    if (raw < lo || raw > hi)
      return NormalizeStatus::EnumInvalid;
    if (raw != std::floor(raw))
      return NormalizeStatus::EnumInvalid;
    *out = static_cast<float>(static_cast<int>(raw));
    return NormalizeStatus::Ok;
  }
  }
  return NormalizeStatus::TypeMismatch;
}

RegistryValidation registry_validate() {
  RegistryValidation r;
  for (size_t i = 0; i < kParamCount; ++i) {
    const ParamDescriptor &a = kParams[i];
    if (a.key == nullptr || a.key[0] == '\0' || a.display_name == nullptr ||
        a.group == nullptr) {
      r.ok = false;
      r.error = "null or empty string field";
      r.offending_id = a.id;
      return r;
    }
    if (!std::isfinite(a.min_value) || !std::isfinite(a.max_value) ||
        !std::isfinite(a.default_value) || !std::isfinite(a.step)) {
      r.ok = false;
      r.error = "non-finite numeric metadata";
      r.offending_id = a.id;
      return r;
    }
    if (a.step < 0.0f) {
      r.ok = false;
      r.error = "negative step";
      r.offending_id = a.id;
      return r;
    }
    if (a.min_value > a.default_value || a.default_value > a.max_value) {
      r.ok = false;
      r.error = "default outside [min,max]";
      r.offending_id = a.id;
      return r;
    }
    if (a.type == ValueTag::Bool &&
        (a.min_value != 0.0f || a.max_value != 1.0f)) {
      r.ok = false;
      r.error = "bool range must be [0,1]";
      r.offending_id = a.id;
      return r;
    }
    if (a.type == ValueTag::Enum16 &&
        (a.min_value < 0.0f || std::floor(a.min_value) != a.min_value ||
         std::floor(a.max_value) != a.max_value)) {
      r.ok = false;
      r.error = "enum bounds must be non-negative integers";
      r.offending_id = a.id;
      return r;
    }
    for (size_t j = i + 1; j < kParamCount; ++j) {
      if (kParams[j].id == a.id) {
        r.ok = false;
        r.error = "duplicate parameter id";
        r.offending_id = a.id;
        return r;
      }
      if (std::strcmp(kParams[j].key, a.key) == 0) {
        r.ok = false;
        r.error = "duplicate parameter key";
        r.offending_id = a.id;
        return r;
      }
    }
  }
  return r;
}

} // namespace voxlink
