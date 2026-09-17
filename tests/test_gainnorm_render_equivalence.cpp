#include "lpc.h"
#include "vocal_fx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kFs = 48000.0f;
constexpr size_t kBlock = 64;
constexpr size_t kFrames = 48000; // 1 s

void require(bool ok, const char *msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
  }
}

void configure_canonical_1v() {
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 0.0f);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_harmony_enabled(1, false);
  vocal_fx_set_psola_grain_render_mode(0, PsolaGrainRenderMode::DeferredSlice);
  vocal_fx_set_psola_fir_kernel(0, PsolaFirKernel::MultiFma);
  vocal_fx_configure_psola_residual_cache(0, 2, true, false);
  vocal_fx_set_psola_residual_cache_enabled(0, true);
}

void render(std::vector<float> &l, std::vector<float> &r, uint64_t &grains,
            PitchShiftDebug &dbg) {
  std::vector<float> in(kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    float s = 0.0f;
    for (int h = 1; h <= 8; ++h)
      s += (0.12f / static_cast<float>(h)) *
           std::sin(2.0f * kPi * 220.0f * static_cast<float>(h) * i / kFs);
    in[i] = s;
  }
  l.assign(kFrames, 0.0f);
  r.assign(kFrames, 0.0f);
  vocal_fx_reset();
  vocal_fx_reset_psola_deferred_stats(0);
  for (size_t pos = 0; pos < kFrames; pos += kBlock) {
    vocal_fx_process(in.data() + pos, l.data() + pos, r.data() + pos, kBlock);
    while (vocal_fx_run_pitch_analysis(4) > 0) {
    }
  }
  grains = vocal_fx_get_psola_deferred_stats(0).slices_rendered;
  dbg = vocal_fx_harmony_debug(0);
}

bool same_debug(const PitchShiftDebug &a, const PitchShiftDebug &b) {
  return a.grains_total == b.grains_total &&
         a.input_absolute_sample == b.input_absolute_sample &&
         a.history_offset == b.history_offset &&
         std::memcmp(&a.formant_gain_norm, &b.formant_gain_norm,
                     sizeof(float)) == 0 &&
         std::memcmp(&a.current_smoothed_ratio, &b.current_smoothed_ratio,
                     sizeof(float)) == 0;
}
} // namespace

int main() {
  VocalFxConfig cfg{};
  cfg.enable_gate = true;
  cfg.enable_compressor = false;
  cfg.enable_delay = false;
  cfg.enable_reverb = false;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 4.0f;
  cfg.pitch_shift.wet = 1.0f;
  require(vocal_fx_init(cfg), "vocal_fx_init");
  configure_canonical_1v();
  SharedLpcAnalysis::prepare_gainnorm_basis(kFs);
  const size_t basis_mm = SharedLpcAnalysis::gainnorm_basis_bit_mismatches(kFs);
  std::printf("gainnorm_basis_bit_mismatches=%zu bytes=%zu\n", basis_mm,
              SharedLpcAnalysis::gainnorm_basis_bytes());

  SharedLpcAnalysis::set_gainnorm_basis_enabled(false);
  vocal_fx_set_psola_warp_cache(false, false, false);
  std::vector<float> l_ref, r_ref, l_opt, r_opt;
  uint64_t grains_ref = 0, grains_opt = 0;
  PitchShiftDebug dbg_ref{}, dbg_opt{};
  render(l_ref, r_ref, grains_ref, dbg_ref);

  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);
  vocal_fx_set_psola_warp_cache(true, true, false);
  render(l_opt, r_opt, grains_opt, dbg_opt);

  size_t sample_mm = 0;
  for (size_t i = 0; i < kFrames; ++i) {
    sample_mm += std::memcmp(&l_ref[i], &l_opt[i], sizeof(float)) != 0;
    sample_mm += std::memcmp(&r_ref[i], &r_opt[i], sizeof(float)) != 0;
  }
  const uint64_t grain_mm =
      grains_ref == grains_opt ? 0 : (grains_ref > grains_opt
                                          ? grains_ref - grains_opt
                                          : grains_opt - grains_ref);
  const size_t state_mm = same_debug(dbg_ref, dbg_opt) ? 0 : 1;
  const bool pass = basis_mm == 0 && sample_mm == 0 && grain_mm == 0 &&
                    state_mm == 0;
  std::printf("rendered_sample_mismatches=%zu grain_mismatch=%llu "
              "state_mismatch=%zu grains_ref=%llu grains_opt=%llu\n",
              sample_mm, static_cast<unsigned long long>(grain_mm), state_mm,
              static_cast<unsigned long long>(grains_ref),
              static_cast<unsigned long long>(grains_opt));
  std::printf("GAINNORM FULL RENDER EQUIVALENCE: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
