// Registry <-> engine binding completeness: every advertised parameter must
// map to exactly one engine setter, and no two public IDs may alias the same
// engine parameter (which would create two competing authorities).
#include "vocal_fx_param_binding.h"
#include "voxlink_registry.h"
#include <cstdint>
#include <cstdio>
#include <set>

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
  std::set<int> engine_params;
  for (size_t i = 0; i < registry_count(); ++i) {
    const ParamDescriptor &d = registry_params()[i];
    VocalFxParameter engine{};
    const bool mapped = voxp4::voxlink_to_engine_param(d.id, &engine);
    if (!mapped) {
      std::fprintf(stderr, "FAIL: public id 0x%04X (%s) has no engine binding\n",
                   d.id, d.key);
      ++g_failures;
      continue;
    }
    const int key = static_cast<int>(engine);
    if (engine_params.count(key) != 0) {
      std::fprintf(stderr, "FAIL: engine parameter aliased by 0x%04X (%s)\n",
                   d.id, d.key);
      ++g_failures;
    }
    engine_params.insert(key);
  }

  // Unknown IDs must not bind.
  VocalFxParameter unused{};
  check(!voxp4::voxlink_to_engine_param(0x9999, &unused), "unknown id rejected");
  check(!voxp4::voxlink_to_engine_param(0x0106, &unused),
        "removed duplicate gain id rejected");

  if (g_failures == 0)
    std::puts("voxlink_binding_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
