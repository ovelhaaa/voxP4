#pragma once
// Bridges public VoxLink parameter IDs to the frozen engine's existing
// parameter setters. This is the only place that knows both sides; the
// registry itself stays independent of DSP internals.
#include "vocal_fx_types.h"
#include <cstdint>

namespace voxp4 {

// Resolves a public VoxLink parameter ID to its engine parameter. Returns false
// for unknown/unmapped IDs.
bool voxlink_to_engine_param(uint16_t id, VocalFxParameter *out);

// ParamSubmitFn-compatible callback for voxlink::SessionConfig. Returns false
// when the bounded SPSC runtime queue rejected the update.
bool voxlink_submit(uint16_t id, float value, void *user);

// Boot-state coherence: submit every registry default to the engine so that
// registry defaults, ProductState and the effective engine targets agree after
// the queue drains. Must be called after vocal_fx_init(). Returns true if every
// default was accepted by the bounded queue.
bool voxlink_seed_defaults();

} // namespace voxp4
