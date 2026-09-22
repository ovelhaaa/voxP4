#pragma once
#include <cstdint>
#include <cstddef>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define VOXP4_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define VOXP4_WASM_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the VoxP4 DSP preview engine with the P4Production profile.
// sample_rate: audio sample rate in Hz (typically 48000.0f)
// block_size: processing block size (must be 64, 128, or 256; typically 64)
VOXP4_WASM_EXPORT bool voxp4_preview_init(float sample_rate, uint32_t block_size);

// Sets a parameter using its canonical semantic key (e.g. "delay.wet", "reverb.decay_s", "harmony.interval").
// Returns true if the key is recognized and accepted.
VOXP4_WASM_EXPORT bool voxp4_preview_set_parameter(const char *semantic_key, float value);

// Resets all parameters to their canonical firmware/registry defaults.
VOXP4_WASM_EXPORT bool voxp4_preview_reset_parameters();

// Resets internal DSP state (delay lines, reverb tank, pitch analysis history) without reallocating.
VOXP4_WASM_EXPORT void voxp4_preview_reset();

// Renders an entire buffer of mono audio to stereo left and right outputs.
// The internal processing loop runs in blocks of 64 frames with pitch tracking hops drained internally.
// input: array of mono float samples [frames]
// output_l: array of left float samples [frames]
// output_r: array of right float samples [frames]
VOXP4_WASM_EXPORT bool voxp4_preview_render(const float *input, size_t frames, float *output_l, float *output_r);

// Returns the engine version identifier string.
VOXP4_WASM_EXPORT const char *voxp4_preview_version();

// Returns the embedded provenance manifest JSON (engine, dspCommit, version).
// The full compatibility manifest is generated at build time by generate-manifest.mjs.
VOXP4_WASM_EXPORT const char *voxp4_preview_get_manifest_json();

#ifdef __cplusplus
}
#endif
