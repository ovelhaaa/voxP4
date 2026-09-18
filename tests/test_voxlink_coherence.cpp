// Boot-state and applied-target coherence.
//
// Proves:
//   * registry defaults == ProductState initial values;
//   * seeding every registry default through the binding reaches the exact
//     engine target after the bounded ParameterQueue is drained at a block
//     boundary (boot-state coherence);
//   * a normalized value accepted via the VoxLink submit path reaches the exact
//     engine target (applied-target coherence), including the clamp path.
#include "vocal_fx.h"
#include "vocal_fx_param_binding.h"
#include "voxlink_registry.h"
#include "voxlink_state.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

void drain_one_block() {
  float in[64] = {0};
  float l[64] = {0};
  float r[64] = {0};
  vocal_fx_process(in, l, r, 64);
}
} // namespace

int main() {
  // ProductState must be seeded from the registry exactly.
  ProductState state;
  state.init();
  for (size_t i = 0; i < registry_count(); ++i) {
    const ParamDescriptor &d = registry_params()[i];
    float value = 0.0f;
    if (!state.get_value(d.id, &value) || value != d.default_value) {
      std::fprintf(stderr, "FAIL: state default mismatch 0x%04X (%s)\n", d.id,
                   d.key);
      ++g_failures;
    }
  }

  // Engine boot-state coherence via explicit seeding.
  VocalFxConfig cfg;
  cfg.sample_rate = 44100.0f;
  cfg.block_size = 64;
  cfg.enable_pitch_analysis = false;
  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "FAIL: vocal_fx_init failed\n");
    return 1;
  }

  check(vocal_fx_is_ready(), "engine reports ready after init");
  check(voxp4::voxlink_seed_defaults(), "all registry defaults accepted");

  drain_one_block();

  for (size_t i = 0; i < registry_count(); ++i) {
    const ParamDescriptor &d = registry_params()[i];
    VocalFxParameter engine{};
    if (!voxp4::voxlink_to_engine_param(d.id, &engine)) {
      std::fprintf(stderr, "FAIL: no engine binding for 0x%04X\n", d.id);
      ++g_failures;
      continue;
    }
    float applied = 0.0f;
    if (!vocal_fx_last_applied_parameter(engine, &applied)) {
      std::fprintf(stderr, "FAIL: default not applied 0x%04X (%s)\n", d.id,
                   d.key);
      ++g_failures;
      continue;
    }
    if (applied != d.default_value) {
      std::fprintf(stderr,
                   "FAIL: boot coherence 0x%04X (%s): engine %.9g != registry "
                   "%.9g\n",
                   d.id, d.key, static_cast<double>(applied),
                   static_cast<double>(d.default_value));
      ++g_failures;
    }
  }

  // Applied-target coherence: normalized accepted value reaches the engine.
  struct Case {
    uint16_t id;
    ValueTag tag;
    double requested;
    float expected;
  };
  const Case cases[] = {
      {0x0101, ValueTag::Int32, 5.0, 5.0f},        // harmony.interval
      {0x0102, ValueTag::Float32, 0.33, 0.33f},    // harmony.level
      {0x0304, ValueTag::Float32, 0.44, 0.44f},    // delay.wet
      {0x0401, ValueTag::Float32, 0.25, 0.25f},    // reverb.wet
      {0x0500, ValueTag::Float32, 0.8, 0.8f},      // limiter.ceiling
      {0x0201, ValueTag::Float32, -12.5, -12.5f},  // compressor.threshold_db
      {0x0101, ValueTag::Int32, 99.0, 12.0f},      // clamp interval to +12
      {0x0401, ValueTag::Float32, 2.0, 1.0f},      // clamp reverb.wet to 1.0
  };

  for (const Case &c : cases) {
    float normalized = 0.0f;
    const NormalizeStatus status =
        normalize_param_value(c.id, c.tag, c.requested, &normalized);
    check(status == NormalizeStatus::Ok || status == NormalizeStatus::Clamped,
          "normalization accepts representative case");
    if (std::fabs(normalized - c.expected) > 1e-4f) {
      std::fprintf(stderr,
                   "FAIL: normalized 0x%04X = %.9g, expected %.9g\n", c.id,
                   static_cast<double>(normalized),
                   static_cast<double>(c.expected));
      ++g_failures;
    }

    // Submit through the exact VoxLink application path and drain immediately.
    check(voxp4::voxlink_submit(c.id, normalized, nullptr),
          "submit accepted representative case");
    drain_one_block();

    VocalFxParameter engine{};
    if (!voxp4::voxlink_to_engine_param(c.id, &engine)) {
      ++g_failures;
      continue;
    }
    float applied = 0.0f;
    if (!vocal_fx_last_applied_parameter(engine, &applied) ||
        applied != normalized) {
      std::fprintf(stderr,
                   "FAIL: applied-target 0x%04X: engine %.9g != accepted %.9g\n",
                   c.id, static_cast<double>(applied),
                   static_cast<double>(normalized));
      ++g_failures;
    }
  }

  if (g_failures == 0)
    std::puts("voxlink_coherence_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
