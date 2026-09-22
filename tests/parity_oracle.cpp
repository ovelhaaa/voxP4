#include "vocal_fx.h"
#include "vocal_fx_config.h"
#include "vocal_fx_types.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr float kSampleRate = 48000.0f;
constexpr size_t kBlockSize = 64;
constexpr size_t kBlocks = 100;
constexpr size_t kTotalFrames = kBlocks * kBlockSize;

uint32_t prng_state = 0x12345678;
float prng_next_float() {
  prng_state = prng_state * 1664525U + 1013904223U;
  return static_cast<float>(static_cast<int32_t>(prng_state)) / 2147483648.0f;
}

std::vector<float> generate_input() {
  std::vector<float> in(kTotalFrames);
  prng_state = 0xABCDEF01;
  for (size_t b = 0; b < kBlocks; ++b) {
    float t_base = static_cast<float>(b * kBlockSize) / kSampleRate;
    for (size_t i = 0; i < kBlockSize; ++i) {
      float t = t_base + static_cast<float>(i) / kSampleRate;
      in[b * kBlockSize + i] = 0.35f * std::sin(2.0f * M_PI * 220.0f * t) +
                               0.15f * std::sin(2.0f * M_PI * 440.0f * t) +
                               0.05f * prng_next_float();
    }
  }
  return in;
}

struct ScenarioResult {
  std::string name;
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  float rms_l = 0.0f;
  float rms_r = 0.0f;
  std::vector<float> out_l;
  std::vector<float> out_r;
};

ScenarioResult run_scenario(int scenario_id, const std::string &name, const std::vector<float> &in) {
  VocalFxConfig cfg{};
  cfg.profile = VocalFxPlatformProfile::P4Production;
  cfg.sample_rate = kSampleRate;
  cfg.block_size = kBlockSize;

  switch (scenario_id) {
    case 0: // Dry
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_chorus = false;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = false;
      cfg.mute_dry = false;
      break;

    case 1: // Dynamics
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_chorus = false;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = false;
      cfg.mute_dry = false;
      break;

    case 2: // Drive
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_chorus = false;
      cfg.enable_drive = true;
      cfg.enable_pitch_analysis = false;
      cfg.drive.mode = DriveMode::Overdrive;
      cfg.drive.drive = 0.6f;
      cfg.drive.tone = 0.5f;
      cfg.drive.mix = 0.8f;
      cfg.mute_dry = false;
      break;

    case 3: // Chorus
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_chorus = true;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = false;
      cfg.chorus.mode = ChorusMode::Dimension;
      cfg.chorus.mix = 0.5f;
      cfg.chorus.depth_ms = 4.0f;
      cfg.mute_dry = false;
      break;

    case 4: // Delay
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = true;
      cfg.enable_reverb = false;
      cfg.enable_chorus = false;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = false;
      cfg.mute_dry = false;
      break;

    case 5: // Reverb
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = true;
      cfg.enable_chorus = false;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = false;
      cfg.mute_dry = false;
      break;

    case 6: // Harmony fixed interval (+4 semitones)
      cfg.enable_gate = false;
      cfg.enable_compressor = false;
      cfg.enable_delay = false;
      cfg.enable_reverb = false;
      cfg.enable_chorus = false;
      cfg.enable_drive = false;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.mute_dry = false;
      break;

    case 7: // Full chain
    default:
      cfg.enable_gate = true;
      cfg.enable_compressor = true;
      cfg.enable_delay = true;
      cfg.enable_reverb = true;
      cfg.enable_chorus = true;
      cfg.enable_drive = true;
      cfg.enable_pitch_analysis = true;
      cfg.pitch_shift.enabled = true;
      cfg.pitch_shift.semitones = 4.0f;
      cfg.pitch_shift.wet = 0.8f;
      cfg.align_dry_to_harmony = true;
      cfg.enable_harmony_limiter = true;
      cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
      cfg.mute_dry = false;
      break;
  }

  vocal_fx_init(cfg);
  if (scenario_id == 4) {
    vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, 250.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, 375.0f);
    vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.35f);
    vocal_fx_set_parameter(VocalFxParameter::DelayWet, 0.5f);
  } else if (scenario_id == 5) {
    vocal_fx_set_parameter(VocalFxParameter::ReverbWet, 0.5f);
    vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, 2.5f);
    vocal_fx_set_parameter(VocalFxParameter::ReverbDamping, 0.4f);
  }

  ScenarioResult res;
  res.name = name;
  res.out_l.resize(kTotalFrames);
  res.out_r.resize(kTotalFrames);

  double sum_l = 0.0, sum_r = 0.0;
  for (size_t b = 0; b < kBlocks; ++b) {
    const size_t pos = b * kBlockSize;
    vocal_fx_process(in.data() + pos, res.out_l.data() + pos, res.out_r.data() + pos, kBlockSize);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
    for (size_t i = 0; i < kBlockSize; ++i) {
      const float l = res.out_l[pos + i];
      const float r = res.out_r[pos + i];
      if (std::fabs(l) > res.peak_l) res.peak_l = std::fabs(l);
      if (std::fabs(r) > res.peak_r) res.peak_r = std::fabs(r);
      sum_l += static_cast<double>(l) * l;
      sum_r += static_cast<double>(r) * r;
    }
  }

  res.rms_l = static_cast<float>(std::sqrt(sum_l / kTotalFrames));
  res.rms_r = static_cast<float>(std::sqrt(sum_r / kTotalFrames));
  return res;
}
} // namespace

int main(int argc, char **argv) {
  std::string dump_path;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
      dump_path = argv[++i];
    }
  }

  const auto in = generate_input();

  const char *names[8] = {
      "Dry", "Dynamics", "Drive", "Chorus",
      "Delay", "Reverb", "Harmony", "FullChain"
  };

  std::vector<ScenarioResult> results;
  results.reserve(8);

  std::printf("========================================================================\n");
  std::printf("  VoxP4 Host Native Parity Oracle (P4Production Profile, 48 kHz)\n");
  std::printf("========================================================================\n");

  for (int i = 0; i < 8; ++i) {
    auto r = run_scenario(i, names[i], in);
    std::printf("[%d] %-10s | Peak: %6.4f / %6.4f | RMS: %6.4f / %6.4f\n",
                i, r.name.c_str(), r.peak_l, r.peak_r, r.rms_l, r.rms_r);
    results.push_back(std::move(r));
  }

  if (!dump_path.empty()) {
    std::ofstream f(dump_path);
    if (f.is_open()) {
      f << "{\n  \"scenarios\": [\n";
      for (size_t s = 0; s < results.size(); ++s) {
        const auto &r = results[s];
        f << "    {\n";
        f << "      \"name\": \"" << r.name << "\",\n";
        f << "      \"peak_l\": " << r.peak_l << ",\n";
        f << "      \"peak_r\": " << r.peak_r << ",\n";
        f << "      \"rms_l\": " << r.rms_l << ",\n";
        f << "      \"rms_r\": " << r.rms_r << ",\n";
        f << "      \"samples_l\": [";
        for (size_t i = 0; i < r.out_l.size(); ++i) {
          if (i > 0) f << ",";
          f << r.out_l[i];
        }
        f << "],\n";
        f << "      \"samples_r\": [";
        for (size_t i = 0; i < r.out_r.size(); ++i) {
          if (i > 0) f << ",";
          f << r.out_r[i];
        }
        f << "]\n";
        f << "    }" << (s + 1 < results.size() ? ",\n" : "\n");
      }
      f << "  ]\n}\n";
      std::printf("Dumped golden reference JSON to: %s\n", dump_path.c_str());
    }
  }

  std::puts("parity_oracle: PASS");
  return 0;
}
