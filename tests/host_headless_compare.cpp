#include "vocal_fx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kBlockSize = 64;
constexpr float kSampleRate = 48000.0f;
constexpr size_t kCompareBlocks = 500; // 32,000 samples per module test

uint32_t calc_crc32(const void *data, size_t length, uint32_t crc = 0xFFFFFFFF) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  while (length--) {
    crc ^= *p++;
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
  }
  return ~crc;
}

uint32_t prng_state = 0x12345678;
float prng_next_float() {
  prng_state = prng_state * 1664525U + 1013904223U;
  return static_cast<float>(static_cast<int32_t>(prng_state)) / 2147483648.0f;
}

struct ModuleResult {
  const char *name;
  uint32_t final_crc;
  float peak_l;
  float peak_r;
  float rms_l;
  float rms_r;
  uint32_t block_crcs[kCompareBlocks];
  float snap_l0[kCompareBlocks];
};

ModuleResult s_module_results[7];

void configure_module_test(int mod) {
  VocalFxConfig cfg{};
  cfg.block_size = kBlockSize;
  cfg.sample_rate = kSampleRate;

  switch (mod) {
    case 0: // Test 1: Dry puro
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.align_dry_to_harmony = false;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 1: // Test 2: Input conditioning (HPF + Gate + Comp)
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.align_dry_to_harmony = false;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 2: // Test 3: Delay isolado
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.3f);
      vocal_fx_set_parameter(VocalFxParameter::DelayWet, 1.0f);
      vocal_fx_set_parameter(VocalFxParameter::DelayDry, 0.0f);
      break;

    case 3: // Test 4: Reverb isolado
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = false;
      cfg.spatial_source = SpatialFxSource::DryOnly;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      vocal_fx_set_parameter(VocalFxParameter::ReverbWet, 1.0f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 1.5f);
      vocal_fx_set_parameter(VocalFxParameter::ReverbDamping, 0.5f);
      break;

    case 4: // Test 5: Harmony isolada (pitch shift puro com razão fixa)
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 1.0f;
      cfg.mute_dry = true;
      vocal_fx_init(cfg);
      break;

    case 5: // Test 6: Dry + Harmony
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;

    case 6: // Test 7: Full chain
    default:
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
      cfg.mute_dry = false;
      vocal_fx_init(cfg);
      break;
  }
}

void run_module_test(int mod, const char *name) {
  configure_module_test(mod);
  prng_state = 0xABCDEF01; // Deterministic reset per module

  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  float peak_l = 0.0f, peak_r = 0.0f;
  double energy_l = 0.0, energy_r = 0.0;
  uint32_t cumulative_crc = 0xFFFFFFFF;

  s_module_results[mod].name = name;

  for (size_t b = 0; b < kCompareBlocks; ++b) {
    float t_base = static_cast<float>(b * kBlockSize) / kSampleRate;
    for (size_t i = 0; i < kBlockSize; ++i) {
      float t = t_base + static_cast<float>(i) / kSampleRate;
      in[i] = 0.35f * std::sin(2.0f * M_PI * 220.0f * t) +
              0.15f * std::sin(2.0f * M_PI * 440.0f * t) +
              0.05f * prng_next_float();
    }

    if (mod >= 4) {
      // Synchronous pitch analysis on host for harmony/full chain
      vocal_fx_run_pitch_analysis(4);
    }

    vocal_fx_process(in, out_l, out_r, kBlockSize);

    // Calculate block-level CRC
    uint32_t b_crc = 0xFFFFFFFF;
    b_crc = calc_crc32(out_l, kBlockSize * sizeof(float), b_crc);
    b_crc = calc_crc32(out_r, kBlockSize * sizeof(float), b_crc);
    s_module_results[mod].block_crcs[b] = ~b_crc;
    s_module_results[mod].snap_l0[b] = out_l[0];

    // Cumulative CRC
    cumulative_crc = calc_crc32(out_l, kBlockSize * sizeof(float), cumulative_crc);
    cumulative_crc = calc_crc32(out_r, kBlockSize * sizeof(float), cumulative_crc);

    for (size_t i = 0; i < kBlockSize; ++i) {
      float al = std::fabs(out_l[i]);
      float ar = std::fabs(out_r[i]);
      if (al > peak_l) peak_l = al;
      if (ar > peak_r) peak_r = ar;
      energy_l += out_l[i] * out_l[i];
      energy_r += out_r[i] * out_r[i];
    }
  }

  size_t total_samples = kCompareBlocks * kBlockSize;
  s_module_results[mod].final_crc = ~cumulative_crc;
  s_module_results[mod].peak_l = peak_l;
  s_module_results[mod].peak_r = peak_r;
  s_module_results[mod].rms_l = std::sqrt(static_cast<float>(energy_l / total_samples));
  s_module_results[mod].rms_r = std::sqrt(static_cast<float>(energy_r / total_samples));

  printf("[HOST_MODULE_RESULT] mod=%s,crc=0x%08lX,peak_l=%.5f,peak_r=%.5f,rms_l=%.5f,rms_r=%.5f,snap0=%.5f,snap1=%.5f,snap10=%.5f,snap100=%.5f\n",
         name, static_cast<unsigned long>(s_module_results[mod].final_crc), peak_l, peak_r,
         s_module_results[mod].rms_l, s_module_results[mod].rms_r,
         s_module_results[mod].snap_l0[0], s_module_results[mod].snap_l0[1],
         s_module_results[mod].snap_l0[10], s_module_results[mod].snap_l0[100]);
}

void run_host_test_j() {
  configure_module_test(6); // Full Chain
  float in[kBlockSize], out_l[kBlockSize], out_r[kBlockSize];
  prng_state = 0xABCDEF01;

  float peak_l = 0.0f, peak_r = 0.0f;
  double energy_l = 0.0, energy_r = 0.0;
  uint32_t crc = 0xFFFFFFFF;
  float snapshots[5] = {0.0f};

  constexpr size_t kLegacyBlocks = 3750;
  for (size_t b = 0; b < kLegacyBlocks; ++b) {
    float t_base = static_cast<float>(b * kBlockSize) / kSampleRate;
    for (size_t i = 0; i < kBlockSize; ++i) {
      float t = t_base + static_cast<float>(i) / kSampleRate;
      in[i] = 0.35f * std::sin(2.0f * M_PI * 220.0f * t) +
              0.15f * std::sin(2.0f * M_PI * 440.0f * t) +
              0.05f * prng_next_float();
    }
    vocal_fx_run_pitch_analysis(4);
    vocal_fx_process(in, out_l, out_r, kBlockSize);

    for (size_t i = 0; i < kBlockSize; ++i) {
      float al = std::fabs(out_l[i]);
      float ar = std::fabs(out_r[i]);
      if (al > peak_l) peak_l = al;
      if (ar > peak_r) peak_r = ar;
      energy_l += out_l[i] * out_l[i];
      energy_r += out_r[i] * out_r[i];
    }
    crc = calc_crc32(out_l, kBlockSize * sizeof(float), crc);
    crc = calc_crc32(out_r, kBlockSize * sizeof(float), crc);

    if (b == 750) snapshots[0] = out_l[0];
    if (b == 1500) snapshots[1] = out_l[0];
    if (b == 2250) snapshots[2] = out_l[0];
    if (b == 3000) snapshots[3] = out_l[0];
    if (b == 3749) snapshots[4] = out_l[0];
  }
  crc = ~crc;
  size_t total_samples = kLegacyBlocks * kBlockSize;
  float rms_l = std::sqrt(static_cast<float>(energy_l / total_samples));
  float rms_r = std::sqrt(static_cast<float>(energy_r / total_samples));

  printf("[HOST_TEST_J_RESULT] blocks=%zu,crc=0x%08lX,peak_l=%.5f,peak_r=%.5f,rms_l=%.5f,rms_r=%.5f,snap0=%.5f,snap1=%.5f,snap2=%.5f,snap3=%.5f,snap4=%.5f\n",
         kLegacyBlocks, static_cast<unsigned long>(crc), peak_l, peak_r, rms_l, rms_r,
         snapshots[0], snapshots[1], snapshots[2], snapshots[3], snapshots[4]);
}

void write_reference_header() {
  const char *paths[] = {"main/host_reference_data.h", "../main/host_reference_data.h", "../../main/host_reference_data.h"};
  FILE *fp = nullptr;
  for (const char *p : paths) {
    fp = fopen(p, "w");
    if (fp) break;
  }
  if (!fp) {
    printf("[HOST_WARN] Could not open host_reference_data.h for writing\n");
    return;
  }

  fprintf(fp, "// Auto-generated by host_headless_compare - DO NOT EDIT\n");
  fprintf(fp, "#pragma once\n");
  fprintf(fp, "#include <cstdint>\n\n");
  fprintf(fp, "struct HostModuleRef {\n");
  fprintf(fp, "  const char *name;\n");
  fprintf(fp, "  uint32_t final_crc;\n");
  fprintf(fp, "  float peak_l;\n");
  fprintf(fp, "  float peak_r;\n");
  fprintf(fp, "  float rms_l;\n");
  fprintf(fp, "  float rms_r;\n");
  fprintf(fp, "  uint32_t block_crcs[%zu];\n", kCompareBlocks);
  fprintf(fp, "  float snap_l0[%zu];\n", kCompareBlocks);
  fprintf(fp, "};\n\n");

  fprintf(fp, "inline const HostModuleRef g_host_module_refs[7] = {\n");
  for (size_t m = 0; m < 7; ++m) {
    const auto &r = s_module_results[m];
    fprintf(fp, "  {\n");
    fprintf(fp, "    \"%s\",\n", r.name);
    fprintf(fp, "    0x%08lX,\n", static_cast<unsigned long>(r.final_crc));
    fprintf(fp, "    %.7ff,\n", r.peak_l);
    fprintf(fp, "    %.7ff,\n", r.peak_r);
    fprintf(fp, "    %.7ff,\n", r.rms_l);
    fprintf(fp, "    %.7ff,\n", r.rms_r);
    fprintf(fp, "    {\n      ");
    for (size_t b = 0; b < kCompareBlocks; ++b) {
      fprintf(fp, "0x%08lX%s", static_cast<unsigned long>(r.block_crcs[b]), (b + 1 == kCompareBlocks) ? "" : ", ");
      if ((b + 1) % 8 == 0 && b + 1 < kCompareBlocks) fprintf(fp, "\n      ");
    }
    fprintf(fp, "\n    },\n");
    fprintf(fp, "    {\n      ");
    for (size_t b = 0; b < kCompareBlocks; ++b) {
      fprintf(fp, "%.7ff%s", r.snap_l0[b], (b + 1 == kCompareBlocks) ? "" : ", ");
      if ((b + 1) % 8 == 0 && b + 1 < kCompareBlocks) fprintf(fp, "\n      ");
    }
    fprintf(fp, "\n    }\n");
    fprintf(fp, "  }%s\n", (m + 1 == 7) ? "" : ",");
  }
  fprintf(fp, "};\n");
  fclose(fp);
  printf("[HOST] Successfully generated host_reference_data.h\n");
}

} // namespace

int main() {
  printf("=== VOXP4 HOST HEADLESS MODULE COMPARISON ===\n");
  const char *names[] = {
    "DryPure",
    "InputConditioning",
    "DelayIsolated",
    "ReverbIsolated",
    "HarmonyIsolated",
    "DryHarmony",
    "FullChain"
  };

  for (int m = 0; m < 7; ++m) {
    run_module_test(m, names[m]);
  }

  run_host_test_j();
  write_reference_header();
  return 0;
}
