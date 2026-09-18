// Parameter registry invariants and normalization semantics.
#include "voxlink_registry.h"
#include <cmath>
#include <cstdio>
#include <limits>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}
} // namespace

int main() {
  // Structural invariants.
  const RegistryValidation v = registry_validate();
  if (!v.ok) {
    std::fprintf(stderr, "FAIL: registry_validate: %s (id=0x%04X)\n",
                 v.error ? v.error : "?", v.offending_id);
    return 1;
  }
  check(registry_count() >= 40, "expected a substantial parameter inventory");
  check(find_param(0x0101) != nullptr, "HarmonyInterval must exist");

  // Explicit, stable protocol IDs (ABI regression guards).
  const ParamDescriptor *p = nullptr;
  p = find_param(0x0100); check(p && p->type == ValueTag::Bool, "HarmonyEnable bool");
  p = find_param(0x0101); check(p && p->type == ValueTag::Int32 && p->min_value == -12.0f &&
                                p->max_value == 12.0f, "HarmonyInterval int [-12,12]");
  p = find_param(0x0300); check(p != nullptr, "DelayEnable 0x0300");
  p = find_param(0x0400); check(p != nullptr, "ReverbEnable 0x0400");
  p = find_param(0x0500); check(p != nullptr, "LimiterCeiling 0x0500");
  p = find_param(0x0F00); check(p == nullptr, "GlobalBypass must not be advertised");

  // Lookup by key.
  p = find_param_by_key("reverb.wet");
  check(p != nullptr && p->id == 0x0401, "key lookup reverb.wet -> 0x0401");
  check(find_param_by_key("no.such.key") == nullptr, "unknown key rejected");

  // Every descriptor has finite, ordered metadata (validate covers this, but
  // also confirm defaults are directly retrievable).
  for (size_t i = 0; i < registry_count(); ++i)
    check(std::isfinite(registry_params()[i].default_value), "finite default");

  // Normalization: non-finite rejection.
  float out = 0.0f;
  check(normalize_param_value(0x0401, ValueTag::Float32,
                              std::nan(""), &out) == NormalizeStatus::NonFinite,
        "NaN rejected");
  const double inf = std::numeric_limits<double>::infinity();
  check(normalize_param_value(0x0401, ValueTag::Float32, inf, &out) ==
            NormalizeStatus::NonFinite,
        "Inf rejected");

  // Unknown id.
  check(normalize_param_value(0x9999, ValueTag::Float32, 0.5, &out) ==
            NormalizeStatus::UnknownParam,
        "unknown id rejected");

  // Type mismatch.
  check(normalize_param_value(0x0101, ValueTag::Float32, 7.0, &out) ==
            NormalizeStatus::TypeMismatch,
        "interval requires Int32");

  // Int clamp + quantize.
  check(normalize_param_value(0x0101, ValueTag::Int32, 99.0, &out) ==
            NormalizeStatus::Clamped && out == 12.0f,
        "interval clamps to +12");
  check(normalize_param_value(0x0101, ValueTag::Int32, -99.0, &out) ==
            NormalizeStatus::Clamped && out == -12.0f,
        "interval clamps to -12");
  check(normalize_param_value(0x0101, ValueTag::Int32, 7.0, &out) ==
            NormalizeStatus::Ok && out == 7.0f,
        "interval accepts +7");

  // Float clamp.
  check(normalize_param_value(0x0401, ValueTag::Float32, 2.0, &out) ==
            NormalizeStatus::Clamped && out == 1.0f,
        "reverb.wet clamps to 1");

  // Bool normalization.
  check(normalize_param_value(0x0400, ValueTag::Bool, 1.0, &out) ==
            NormalizeStatus::Ok && out == 1.0f,
        "bool true");

  // Enum validation.
  check(normalize_param_value(0x0503, ValueTag::Enum16, 5.0, &out) ==
            NormalizeStatus::EnumInvalid,
        "enum out of range rejected");
  check(normalize_param_value(0x0503, ValueTag::Enum16, 2.0, &out) ==
            NormalizeStatus::Ok && out == 2.0f,
        "enum in range accepted");

  if (g_failures == 0)
    std::puts("voxlink_registry_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
