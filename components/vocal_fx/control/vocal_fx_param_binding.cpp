#include "vocal_fx_param_binding.h"
#include "vocal_fx.h"
#include "voxlink_registry.h"

namespace voxp4 {

bool voxlink_to_engine_param(uint16_t id, VocalFxParameter *out) {
  if (out == nullptr)
    return false;
  VocalFxParameter mapped;
  switch (id) {
  // Global / Tempo
  case 0x0010: mapped = VocalFxParameter::TempoBpm; break;
  // Harmony
  case 0x0100: mapped = VocalFxParameter::PitchShiftEnabled; break;
  case 0x0101: mapped = VocalFxParameter::PitchShiftSemitones; break;
  case 0x0102: mapped = VocalFxParameter::PitchShiftWet; break;
  case 0x0103: mapped = VocalFxParameter::HarmonyMode; break;
  case 0x0104: mapped = VocalFxParameter::HarmonyKey; break;
  case 0x0105: mapped = VocalFxParameter::HarmonyScale; break;
  case 0x0107: mapped = VocalFxParameter::HarmonyVoice1Pan; break;
  case 0x0108: mapped = VocalFxParameter::HarmonyVoice1Degree; break;
  case 0x0109: mapped = VocalFxParameter::HarmonyVoice1Smoothing; break;
  case 0x010A: mapped = VocalFxParameter::FormantVoice1Mode; break;
  case 0x010B: mapped = VocalFxParameter::FormantVoice1Amount; break;
  case 0x010C: mapped = VocalFxParameter::HarmonyAttackMs; break;
  case 0x010D: mapped = VocalFxParameter::HarmonyReleaseMs; break;
  case 0x010E: mapped = VocalFxParameter::HarmonyLimiterEnabled; break;
  case 0x010F: mapped = VocalFxParameter::HarmonyLimiterThresholdDb; break;
  case 0x0110: mapped = VocalFxParameter::DryAlignmentEnabled; break;
  case 0x0111: mapped = VocalFxParameter::DryAlignmentMs; break;
  case 0x0112: mapped = VocalFxParameter::HarmonyVoice1NonScalePolicy; break;
  case 0x0113: mapped = VocalFxParameter::HarmonyVoice1VoiceLeadingEnabled; break;
  case 0x0114: mapped = VocalFxParameter::HarmonyVoice1MinMidi; break;
  case 0x0115: mapped = VocalFxParameter::HarmonyVoice1MaxMidi; break;
  // Dynamics
  case 0x0200: mapped = VocalFxParameter::EnableCompressor; break;
  case 0x0201: mapped = VocalFxParameter::CompressorThresholdDb; break;
  case 0x0202: mapped = VocalFxParameter::CompressorRatio; break;
  case 0x0203: mapped = VocalFxParameter::CompressorAttackMs; break;
  case 0x0204: mapped = VocalFxParameter::CompressorReleaseMs; break;
  case 0x0205: mapped = VocalFxParameter::CompressorMakeupDb; break;
  case 0x0206: mapped = VocalFxParameter::CompressorKneeDb; break;
  case 0x0207: mapped = VocalFxParameter::EnableGate; break;
  case 0x0208: mapped = VocalFxParameter::GateThresholdDb; break;
  case 0x0209: mapped = VocalFxParameter::GateAttackMs; break;
  case 0x020A: mapped = VocalFxParameter::GateHoldMs; break;
  case 0x020B: mapped = VocalFxParameter::GateReleaseMs; break;
  case 0x020C: mapped = VocalFxParameter::GateRangeDb; break;
  // Delay
  case 0x0300: mapped = VocalFxParameter::EnableDelay; break;
  case 0x0301: mapped = VocalFxParameter::DelayLeftMs; break;
  case 0x0302: mapped = VocalFxParameter::DelayRightMs; break;
  case 0x0303: mapped = VocalFxParameter::DelayFeedback; break;
  case 0x0304: mapped = VocalFxParameter::DelayWet; break;
  case 0x0305: mapped = VocalFxParameter::DelayDry; break;
  case 0x0306: mapped = VocalFxParameter::DelayFeedbackLowpassHz; break;
  case 0x0307: mapped = VocalFxParameter::DelaySyncEnabled; break;
  case 0x0308: mapped = VocalFxParameter::DelayLeftSubdivision; break;
  case 0x0309: mapped = VocalFxParameter::DelayRightSubdivision; break;
  // Reverb
  case 0x0400: mapped = VocalFxParameter::EnableReverb; break;
  case 0x0401: mapped = VocalFxParameter::ReverbWet; break;
  case 0x0402: mapped = VocalFxParameter::ReverbDecaySeconds; break;
  case 0x0403: mapped = VocalFxParameter::ReverbDamping; break;
  // Output / master
  case 0x0500: mapped = VocalFxParameter::LimiterCeiling; break;
  case 0x0501: mapped = VocalFxParameter::MuteDry; break;
  case 0x0502: mapped = VocalFxParameter::SpatialRouting; break;
  case 0x0503: mapped = VocalFxParameter::SpatialSource; break;
  // Chorus
  case 0x0600: mapped = VocalFxParameter::EnableChorus; break;
  case 0x0601: mapped = VocalFxParameter::ChorusMode; break;
  case 0x0602: mapped = VocalFxParameter::ChorusMix; break;
  case 0x0603: mapped = VocalFxParameter::ChorusSyncEnabled; break;
  case 0x0604: mapped = VocalFxParameter::ChorusRateHz; break;
  case 0x0605: mapped = VocalFxParameter::ChorusSubdivision; break;
  case 0x0606: mapped = VocalFxParameter::ChorusDepthMs; break;
  case 0x0607: mapped = VocalFxParameter::ChorusBaseDelayMs; break;
  case 0x0608: mapped = VocalFxParameter::ChorusWidth; break;
  case 0x0609: mapped = VocalFxParameter::MicroshiftLeftCents; break;
  case 0x060A: mapped = VocalFxParameter::MicroshiftRightCents; break;
  case 0x060B: mapped = VocalFxParameter::MicroshiftWindowMs; break;
  // Drive
  case 0x0700: mapped = VocalFxParameter::EnableDrive; break;
  case 0x0701: mapped = VocalFxParameter::DriveMode; break;
  case 0x0702: mapped = VocalFxParameter::DriveDrive; break;
  case 0x0703: mapped = VocalFxParameter::DriveTone; break;
  case 0x0704: mapped = VocalFxParameter::DriveMix; break;
  case 0x0705: mapped = VocalFxParameter::DriveOutputLevel; break;
  default:
    return false;
  }
  *out = mapped;
  return true;
}

bool voxlink_submit(uint16_t id, float value, void *user) {
  (void)user;
  VocalFxParameter parameter;
  if (!voxlink_to_engine_param(id, &parameter))
    return false;
  return vocal_fx_try_set_parameter(parameter, value);
}

bool voxlink_seed_defaults() {
  const voxlink::ParamDescriptor *params = voxlink::registry_params();
  const size_t count = voxlink::registry_count();
  bool all_accepted = true;
  for (size_t i = 0; i < count; ++i) {
    VocalFxParameter parameter;
    if (!voxlink_to_engine_param(params[i].id, &parameter)) {
      all_accepted = false;
      continue;
    }
    if (!vocal_fx_try_set_parameter(parameter, params[i].default_value))
      all_accepted = false;
  }
  return all_accepted;
}

} // namespace voxp4
