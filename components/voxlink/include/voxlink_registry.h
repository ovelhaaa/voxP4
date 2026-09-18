#pragma once
// The single authoritative VoxLink parameter registry. Every externally
// controllable product parameter is described here exactly once; capability
// reporting, validation, state snapshots and (future) presets all derive from
// this table. No UI or DSP module may duplicate this metadata.
#include "voxlink_protocol.h"
#include <cstddef>
#include <cstdint>

namespace voxlink {

struct ParamDescriptor {
  uint16_t id;
  const char *key;
  const char *display_name;
  const char *unit;
  ValueTag type;
  float min_value;
  float max_value;
  float default_value;
  float step;
  uint32_t flags;
  float smoothing_ms;
  const char *group;
  // Documentation/report only. Runtime DSP binding is installed by the
  // application via a ParamId -> engine setter map, keeping this registry
  // independent of DSP internals.
  const char *dsp_binding;
};

const ParamDescriptor *registry_params();
size_t registry_count();
const ParamDescriptor *find_param(uint16_t id);
const ParamDescriptor *find_param_by_key(const char *key);

enum class NormalizeStatus {
  Ok,
  Clamped,
  UnknownParam,
  ReadOnly,
  TypeMismatch,
  NonFinite,
  EnumInvalid,
};

const char *normalize_status_name(NormalizeStatus status);

// Validates and normalizes a decoded SET_PARAM value. `tag` is the wire tag the
// controller supplied and must match the descriptor type. Numeric values are
// clamped to [min,max] and quantized to `step`; invalid enums are rejected.
NormalizeStatus normalize_param_value(uint16_t id, ValueTag tag, double raw,
                                      float *out);

struct RegistryValidation {
  bool ok = true;
  const char *error = nullptr;
  uint16_t offending_id = 0;
};

// Compile-time-independent runtime invariant check used by host tests:
// unique IDs, unique keys, non-null strings, min <= default <= max, step >= 0,
// finite numeric fields.
RegistryValidation registry_validate();

} // namespace voxlink
