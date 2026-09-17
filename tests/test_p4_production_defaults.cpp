#include "vocal_fx.h"
#include <cstdio>

namespace {
bool require(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "P4 production default regression: %s\n", message);
  return condition;
}
} // namespace

int main() {
  const auto pitch = vocal_fx_p4_pitch_analysis_defaults(48000.0f);
  const VocalFxConfig normal{};
  bool pass = true;
  pass &= require(pitch.yin_difference == YinDifferenceVariant::IncrementalF32,
                  "YIN Difference is not INCREMENTAL_F32");
  pass &= require(pitch.yin_incremental_rebase_hops == 64,
                  "YIN incremental rebase is not 64");
  pass &= require(pitch.yin_cmnd == YinCmndVariant::F32Compensated,
                  "YIN CMND is not F32_COMPENSATED");
  pass &= require(pitch.yin_energy == YinEnergyVariant::F32Compensated,
                  "YIN Energy is not F32_COMPENSATED");
  pass &= require(pitch.pitch_mark_ncc == PitchMarkNccVariant::ContiguousMulti8,
                  "PitchMark NCC is not CONTIGUOUS_MULTI8");
  pass &= require(
      normal.lpc.windowing ==
          LpcWindowVariant::PrecomputedHannCompensatedEnergy,
      "LPC Energy is not F32_COMPENSATED");
  pass &= require(
      normal.lpc.autocorrelation ==
          LpcAutocorrelationVariant::AutocorrF32DoubleSingle,
      "LPC autocorrelation is not F32_DOUBLE_SINGLE");

  // The cross-platform pitch defaults remain explicit regression oracles.
  const PitchAnalysisConfig generic{};
  pass &= require(generic.yin_difference == YinDifferenceVariant::Fma8Acc,
                  "generic YIN Difference default changed");
  pass &= require(generic.yin_incremental_rebase_hops == 8,
                  "generic YIN rebase default changed");
  pass &= require(generic.yin_cmnd == YinCmndVariant::ReferenceDouble,
                  "generic CMND oracle changed");
  pass &= require(generic.yin_energy == YinEnergyVariant::ReferenceDouble,
                  "generic Energy oracle changed");
  pass &= require(generic.pitch_mark_ncc == PitchMarkNccVariant::Reference,
                  "generic NCC oracle changed");
  if (pass)
    std::puts("B4B.5 P4 production defaults: PASS");
  return pass ? 0 : 1;
}
