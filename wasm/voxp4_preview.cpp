#include "voxp4_preview.h"
#include "vocal_fx.h"
#include "vocal_fx_config.h"
#include "vocal_fx_param_binding.h"
#include "voxlink_registry.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {
constexpr float kDefaultSampleRate = 48000.0f;
constexpr uint32_t kDefaultBlockSize = 64;

static const char kManifestJson[] =
    "{\n"
    "  \"engine\": \"voxP4\",\n"
    "  \"dspCommit\": \"e25136226b76e2a349de0ae8b2bc518cf9477c8b\",\n"
    "  \"contractVersion\": 1,\n"
    "  \"parameterCount\": 71,\n"
    "  \"sampleRate\": 48000,\n"
    "  \"blockSize\": 64,\n"
    "  \"profile\": \"P4Production\"\n"
    "}";
} // namespace

extern "C" {

VOXP4_WASM_EXPORT bool voxp4_preview_init(float sample_rate, uint32_t block_size) {
  if (sample_rate <= 0.0f) sample_rate = kDefaultSampleRate;
  if (block_size != 64 && block_size != 128 && block_size != 256) block_size = kDefaultBlockSize;

  VocalFxConfig cfg{};
  cfg.profile = VocalFxPlatformProfile::P4Production;
  cfg.sample_rate = sample_rate;
  cfg.block_size = block_size;
  cfg.enable_gate = true;
  cfg.enable_compressor = true;
  cfg.enable_delay = true;
  cfg.enable_reverb = true;
  cfg.enable_chorus = true;
  cfg.enable_drive = true;
  cfg.enable_pitch_analysis = true;

  if (!vocal_fx_init(cfg)) {
    return false;
  }

  // Seed canonical defaults from VoxLink registry so engine targets match exactly.
  if (!voxp4::voxlink_seed_defaults()) {
    // Non-fatal, but engine is initialized
  }

  // Drain one small block of silence to immediately apply queued parameter defaults.
  float dummy_in[64] = {0.0f};
  float dummy_l[64] = {0.0f};
  float dummy_r[64] = {0.0f};
  vocal_fx_process(dummy_in, dummy_l, dummy_r, 64);
  vocal_fx_reset();

  return true;
}

VOXP4_WASM_EXPORT bool voxp4_preview_set_parameter(const char *semantic_key, float value) {
  if (semantic_key == nullptr) return false;

  // 1. First attempt exact canonical key lookup (e.g. "delay.wet", "reverb.decay_s")
  const auto *desc = voxlink::find_param_by_key(semantic_key);

  // 2. If not found by key, search by DSP binding name (e.g. "DelayWet", "ReverbDecaySeconds")
  if (!desc) {
    const size_t count = voxlink::registry_count();
    const auto *params = voxlink::registry_params();
    for (size_t i = 0; i < count; ++i) {
      if (params[i].dsp_binding && std::strcmp(params[i].dsp_binding, semantic_key) == 0) {
        desc = &params[i];
        break;
      }
    }
  }

  if (!desc) return false;

  VocalFxParameter param;
  if (!voxp4::voxlink_to_engine_param(desc->id, &param)) {
    return false;
  }

  return vocal_fx_try_set_parameter(param, value);
}

VOXP4_WASM_EXPORT bool voxp4_preview_reset_parameters() {
  return voxp4::voxlink_seed_defaults();
}

VOXP4_WASM_EXPORT void voxp4_preview_reset() {
  vocal_fx_reset();
}

VOXP4_WASM_EXPORT bool voxp4_preview_render(const float *input, size_t frames, float *output_l, float *output_r) {
  if (!vocal_fx_is_ready() || !input || !output_l || !output_r) {
    return false;
  }

  constexpr size_t kBlock = 64;
  for (size_t pos = 0; pos < frames; pos += kBlock) {
    const size_t n = std::min(kBlock, frames - pos);
    vocal_fx_process(input + pos, output_l + pos, output_r + pos, n);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }

  return true;
}

VOXP4_WASM_EXPORT const char *voxp4_preview_version() {
  return "voxP4-preview-1.0.0";
}

VOXP4_WASM_EXPORT const char *voxp4_preview_get_manifest_json() {
  return kManifestJson;
}

} // extern "C"
