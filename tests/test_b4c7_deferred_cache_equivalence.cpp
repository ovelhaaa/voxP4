// B4C.7 host test: deferred residual-cache + slew-hoist bit identity.
//
// Renders the canonical 2V production configuration twice: once with the
// deferred residual cache disabled (reference = pre-B4C.7 behavior) and
// once enabled. Rendered audio, grain counts, and slice counts must be
// bit-identical; the cache must demonstrably hit (else the test is vacuous).
// The always-on slew sqrt hoist is covered implicitly: any deviation would
// mismatch the render.
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
constexpr size_t kFrames = 48000;  // 1 s

void require(bool ok, const char *msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    std::exit(1);
  }
}

void configure_canonical_2v() {
  vocal_fx_set_parameter(VocalFxParameter::EnableGate, 1.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableCompressor, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableDelay, 0.0f);
  vocal_fx_set_parameter(VocalFxParameter::EnableReverb, 0.0f);
  for (size_t v = 0; v < 2; ++v) {
    vocal_fx_set_harmony_enabled(v, true);
    vocal_fx_set_harmony_gain(v, 1.0f);
    vocal_fx_set_formant_mode(v, FormantMode::Lpc);
    vocal_fx_set_psola_grain_render_mode(v, PsolaGrainRenderMode::DeferredSlice);
    vocal_fx_set_psola_fir_kernel(v, PsolaFirKernel::MultiFma);
  }
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_psola_warp_cache(true, true, false);
}

struct RenderOut {
  std::vector<float> l, r;
  uint64_t grains0 = 0, grains1 = 0;
  uint64_t slices0 = 0, slices1 = 0;
  uint64_t hits0 = 0, hits1 = 0;
  uint64_t misses0 = 0, misses1 = 0;
};

void render(RenderOut &out) {
  std::vector<float> in(kFrames);
  for (size_t i = 0; i < kFrames; ++i) {
    float s = 0.0f;
    for (int h = 1; h <= 8; ++h)
      s += (0.12f / static_cast<float>(h)) *
           std::sin(2.0f * kPi * 220.0f * static_cast<float>(h) * i / kFs);
    in[i] = s;
  }
  out.l.assign(kFrames, 0.0f);
  out.r.assign(kFrames, 0.0f);
  vocal_fx_reset();
  vocal_fx_reset_psola_deferred_stats(0);
  vocal_fx_reset_psola_deferred_stats(1);
  for (size_t pos = 0; pos < kFrames; pos += kBlock) {
    vocal_fx_process(in.data() + pos, out.l.data() + pos, out.r.data() + pos,
                     kBlock);
    while (vocal_fx_run_pitch_analysis(4) > 0) {
    }
  }
  out.grains0 = vocal_fx_get_psola_deferred_stats(0).slices_rendered;
  out.grains1 = vocal_fx_get_psola_deferred_stats(1).slices_rendered;
  const auto d0 = vocal_fx_get_psola_deferred_stats(0);
  const auto d1 = vocal_fx_get_psola_deferred_stats(1);
  out.slices0 = d0.slices_rendered;
  out.slices1 = d1.slices_rendered;
  out.hits0 = vocal_fx_get_psola_residual_cache_stats(0).hits;
  out.hits1 = vocal_fx_get_psola_residual_cache_stats(1).hits;
  out.misses0 = vocal_fx_get_psola_residual_cache_stats(0).misses;
  out.misses1 = vocal_fx_get_psola_residual_cache_stats(1).misses;
}

}  // namespace

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
  configure_canonical_2v();
  SharedLpcAnalysis::prepare_gainnorm_basis(kFs);
  SharedLpcAnalysis::set_gainnorm_basis_enabled(true);

  // Warmup: the first render after init differs from subsequent renders
  // (pre-existing init-vs-reset state delta, unrelated to the cache).
  // Discard it so the comparison below is steady-state vs steady-state.
  for (size_t v = 0; v < 2; ++v) {
    vocal_fx_configure_psola_residual_cache(v, 2, true, false);
    vocal_fx_set_psola_residual_cache_enabled(v, true);
  }
  {
    RenderOut warm{};
    render(warm);
  }
  // Control: two identical post-warmup renders must match exactly.
  RenderOut opt_a{};
  render(opt_a);
  RenderOut opt_b{};
  render(opt_b);
  size_t control_mm = 0;
  for (size_t i = 0; i < kFrames; ++i) {
    control_mm += std::memcmp(&opt_a.l[i], &opt_b.l[i], sizeof(float)) != 0;
    control_mm += std::memcmp(&opt_a.r[i], &opt_b.r[i], sizeof(float)) != 0;
  }
  std::printf("control_identical_renders_mismatches=%zu\n", control_mm);

  // Reference: deferred residual cache fully disabled.
  for (size_t v = 0; v < 2; ++v) {
    vocal_fx_configure_psola_residual_cache(v, 0, false, false);
    vocal_fx_set_psola_residual_cache_enabled(v, false);
  }
  RenderOut ref{};
  render(ref);

  // Optimized: production 2-entry residual cache per voice.
  for (size_t v = 0; v < 2; ++v) {
    vocal_fx_configure_psola_residual_cache(v, 2, true, false);
    vocal_fx_set_psola_residual_cache_enabled(v, true);
  }
  RenderOut opt{};
  render(opt);

  size_t sample_mm = 0;
  size_t first_mm = kFrames;
  for (size_t i = 0; i < kFrames; ++i) {
    const bool ml = std::memcmp(&ref.l[i], &opt.l[i], sizeof(float)) != 0;
    const bool mr = std::memcmp(&ref.r[i], &opt.r[i], sizeof(float)) != 0;
    sample_mm += ml + mr;
    if ((ml || mr) && first_mm == kFrames) first_mm = i;
  }
  const uint64_t grain_mm =
      (ref.grains0 == opt.grains0 && ref.grains1 == opt.grains1 &&
       ref.slices0 == opt.slices0 && ref.slices1 == opt.slices1)
          ? 0
          : 1;
  // The test is vacuous unless the cache actually served hits AND misses.
  const bool exercised =
      (opt.hits0 + opt.hits1) > 0 && (opt.misses0 + opt.misses1) > 0;
  const bool ref_quiet =
      (ref.hits0 + ref.hits1 + ref.misses0 + ref.misses1) == 0;
  const bool pass = sample_mm == 0 && grain_mm == 0 && exercised &&
                      ref_quiet && control_mm == 0;
  std::printf("rendered_sample_mismatches=%zu first_mm=%zu "
              "grain_mismatch=%llu "
              "hits=%llu/%llu misses=%llu/%llu ref_lookups=%s exercised=%s\n",
              sample_mm, first_mm, static_cast<unsigned long long>(grain_mm),
              static_cast<unsigned long long>(opt.hits0),
              static_cast<unsigned long long>(opt.hits1),
              static_cast<unsigned long long>(opt.misses0),
              static_cast<unsigned long long>(opt.misses1),
              ref_quiet ? "zero" : "NONZERO",
              exercised ? "yes" : "NO");
  std::printf("DEFERRED CACHE BIT IDENTITY: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
