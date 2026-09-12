#include "vocal_fx.h"
#include "fdn_reverb.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static uint32_t u32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}
static uint16_t u16(const unsigned char *p) {
  return p[0] | (p[1] << 8);
}
static void put16(std::ofstream &f, uint16_t v) {
  char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
  f.write(b, 2);
}
static void put32(std::ofstream &f, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xff),
               static_cast<char>((v >> 8) & 0xff),
               static_cast<char>((v >> 16) & 0xff),
               static_cast<char>((v >> 24) & 0xff)};
  f.write(b, 4);
}

bool write_wav_float32(const std::string &path, const float *left, const float *right, size_t frames, uint32_t rate) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  const uint32_t channels = 2;
  const uint32_t bits_per_sample = 32;
  const uint32_t byte_rate = rate * channels * (bits_per_sample / 8);
  const uint32_t block_align = channels * (bits_per_sample / 8);
  const uint32_t data_bytes = static_cast<uint32_t>(frames * block_align);

  f.write("RIFF", 4);
  put32(f, 36 + data_bytes);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 3); // IEEE float
  put16(f, channels);
  put32(f, rate);
  put32(f, byte_rate);
  put16(f, block_align);
  put16(f, bits_per_sample);
  f.write("data", 4);
  put32(f, data_bytes);

  std::vector<float> interleaved(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    interleaved[2 * i] = left[i];
    interleaved[2 * i + 1] = right[i];
  }
  f.write(reinterpret_cast<const char *>(interleaved.data()), data_bytes);
  return f.good();
}

int main(int argc, char **argv) {
  std::string input_path;
  std::string output_path = "m512_out.wav";
  std::string stimulus_type = "none";
  std::string param_sweep = "none";
  float duration_sec = 5.0f;
  bool enable_dry = true;
  bool enable_harmony = false;
  float harmony_interval = 7.0f;
  float harmony_gain = 0.707f;
  float harmony_pan = 0.0f;
  bool enable_delay = false;
  float delay_left = 250.0f;
  float delay_right = 375.0f;
  float delay_feedback = 0.30f;
  float delay_wet = 0.25f;
  bool enable_reverb = false;
  float reverb_rt60 = 2.0f;
  float reverb_damping = 0.45f;
  float reverb_wet = 0.25f;
  SpatialFxRouting routing = SpatialFxRouting::DelayIntoReverb;
  SpatialFxSource spatial_source = SpatialFxSource::MainMix;
  bool mute_dry = false;
  bool align_dry = true;
  float dry_alignment_ms = 32.0f;
  bool print_profile = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--input" && i + 1 < argc) input_path = argv[++i];
    else if (arg == "--output" && i + 1 < argc) output_path = argv[++i];
    else if (arg == "--stimulus" && i + 1 < argc) stimulus_type = argv[++i];
    else if (arg == "--duration" && i + 1 < argc) duration_sec = std::stof(argv[++i]);
    else if (arg == "--dry" && i + 1 < argc) enable_dry = (std::stoi(argv[++i]) != 0);
    else if (arg == "--harmony" && i + 1 < argc) enable_harmony = (std::stoi(argv[++i]) != 0);
    else if (arg == "--harmony-interval" && i + 1 < argc) harmony_interval = std::stof(argv[++i]);
    else if (arg == "--harmony-gain" && i + 1 < argc) harmony_gain = std::stof(argv[++i]);
    else if (arg == "--harmony-pan" && i + 1 < argc) harmony_pan = std::stof(argv[++i]);
    else if (arg == "--delay" && i + 1 < argc) enable_delay = (std::stoi(argv[++i]) != 0);
    else if (arg == "--delay-left" && i + 1 < argc) delay_left = std::stof(argv[++i]);
    else if (arg == "--delay-right" && i + 1 < argc) delay_right = std::stof(argv[++i]);
    else if (arg == "--delay-feedback" && i + 1 < argc) delay_feedback = std::stof(argv[++i]);
    else if (arg == "--delay-wet" && i + 1 < argc) delay_wet = std::stof(argv[++i]);
    else if (arg == "--reverb" && i + 1 < argc) enable_reverb = (std::stoi(argv[++i]) != 0);
    else if (arg == "--reverb-rt60" && i + 1 < argc) reverb_rt60 = std::stof(argv[++i]);
    else if (arg == "--reverb-damping" && i + 1 < argc) reverb_damping = std::stof(argv[++i]);
    else if (arg == "--reverb-wet" && i + 1 < argc) reverb_wet = std::stof(argv[++i]);
    else if (arg == "--routing" && i + 1 < argc) {
      std::string r = argv[++i];
      if (r == "parallel") routing = SpatialFxRouting::Parallel;
      else if (r == "delay_into_reverb") routing = SpatialFxRouting::DelayIntoReverb;
    }
    else if (arg == "--align-dry" && i + 1 < argc) align_dry = (std::stoi(argv[++i]) != 0);
    else if (arg == "--mute-dry" && i + 1 < argc) mute_dry = (std::stoi(argv[++i]) != 0);
    else if (arg == "--source" && i + 1 < argc) {
      std::string s = argv[++i];
      if (s == "dry_only") spatial_source = SpatialFxSource::DryOnly;
      else if (s == "harmony_only") spatial_source = SpatialFxSource::HarmonyOnly;
      else spatial_source = SpatialFxSource::MainMix;
    }
    else if (arg == "--param-sweep" && i + 1 < argc) param_sweep = argv[++i];
    else if (arg == "--profile") print_profile = true;
  }

  constexpr uint32_t sample_rate = 48000;

  if (stimulus_type == "fdn_ir") {
    FdnReverb fdn;
    if (!fdn.init(sample_rate)) return 1;
    fdn.set_rt60(reverb_rt60);
    fdn.set_damping(reverb_damping);
    fdn.set_wet(1.0f);
    size_t total_frames = static_cast<size_t>(sample_rate * duration_sec);
    std::vector<float> ir_l(total_frames), ir_r(total_frames);
    for (size_t i = 0; i < total_frames; ++i) {
      fdn.process(i == 0 ? 1.0f : 0.0f, ir_l[i], ir_r[i]);
    }
    if (!write_wav_float32(output_path, ir_l.data(), ir_r.data(), total_frames, sample_rate)) {
      std::fprintf(stderr, "failed to write fdn_ir: %s\n", output_path.c_str());
      return 3;
    }
    return 0;
  }

  std::vector<float> mono_in;

  if (stimulus_type == "impulse") {
    size_t total_frames = static_cast<size_t>(sample_rate * duration_sec);
    mono_in.assign(total_frames, 0.0f);
    if (total_frames > 0) mono_in[0] = 1.0f;
  } else if (stimulus_type == "tone") {
    size_t total_frames = static_cast<size_t>(sample_rate * duration_sec);
    mono_in.resize(total_frames);
    for (size_t i = 0; i < total_frames; ++i) {
      float t = static_cast<float>(i) / sample_rate;
      float env = 1.0f;
      if (t < 0.010f) env = 0.5f * (1.0f - std::cos(3.14159265f * t / 0.010f));
      else if (t > duration_sec - 0.010f) env = 0.5f * (1.0f - std::cos(3.14159265f * (duration_sec - t) / 0.010f));
      mono_in[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * t) * env;
    }
  } else if (stimulus_type == "transient") {
    size_t total_frames = static_cast<size_t>(sample_rate * duration_sec);
    mono_in.assign(total_frames, 0.0f);
    size_t interval = sample_rate / 4;
    for (size_t pos = 100; pos + 200 < total_frames; pos += interval) {
      for (size_t k = 0; k < 64; ++k) {
        float atten = std::exp(-static_cast<float>(k) / 8.0f);
        mono_in[pos + k] = 0.8f * ((k % 2 == 0) ? 1.0f : -1.0f) * atten;
      }
    }
  } else if (!input_path.empty()) {
    std::ifstream f(input_path, std::ios::binary);
    if (!f.is_open()) {
      std::fprintf(stderr, "failed to open input file: %s\n", input_path.c_str());
      return 1;
    }
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
      std::fprintf(stderr, "invalid WAV file\n");
      return 1;
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const unsigned char *data = nullptr;
    size_t bytes = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
      uint32_t n = u32(&b[p + 4]);
      if (p + 8 + n > b.size()) break;
      if (!std::memcmp(&b[p], "fmt ", 4) && n >= 16) {
        fmt = u16(&b[p + 8]);
        ch = u16(&b[p + 10]);
        rate = u32(&b[p + 12]);
        bits = u16(&b[p + 22]);
      } else if (!std::memcmp(&b[p], "data", 4)) {
        data = &b[p + 8];
        bytes = n;
      }
      p += 8 + n + (n & 1);
    }
    if (!data || rate != sample_rate || !ch) {
      std::fprintf(stderr, "requires 48 kHz WAV\n");
      return 1;
    }
    const size_t stride = ch * bits / 8;
    const size_t frames = bytes / stride;
    mono_in.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
      double sum = 0;
      for (unsigned k = 0; k < ch; ++k) {
        const unsigned char *p = data + i * stride + k * bits / 8;
        if (fmt == 1) sum += static_cast<int16_t>(u16(p)) / 32768.0;
        else if (fmt == 3) {
          float val;
          std::memcpy(&val, p, 4);
          sum += val;
        }
      }
      mono_in[i] = static_cast<float>(sum / ch);
    }
  } else {
    std::fprintf(stderr, "no input or stimulus specified\n");
    return 1;
  }

  VocalFxConfig cfg{};
  cfg.sample_rate = 48000.0f;
  cfg.block_size = 64;
  cfg.enable_gate = false;
  cfg.enable_compressor = false;
  cfg.enable_delay = enable_delay;
  cfg.enable_reverb = enable_reverb;
  cfg.enable_pitch_analysis = enable_harmony;
  cfg.align_dry_to_harmony = align_dry;
  cfg.dry_alignment_ms = dry_alignment_ms;
  cfg.spatial_routing = routing;
  cfg.spatial_source = spatial_source;
  cfg.mute_dry = mute_dry;

  cfg.pitch_shift.enabled = enable_harmony;
  cfg.pitch_shift.semitones = harmony_interval;
  cfg.pitch_shift.wet = harmony_gain;
  cfg.harmony_attack_ms = 4.0f;
  cfg.harmony_release_ms = 20.0f;
  cfg.enable_harmony_limiter = true;
  cfg.harmony_limiter_threshold_db = -3.0f;

  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "failed to initialize vocal_fx\n");
    return 2;
  }

  vocal_fx_set_parameter(VocalFxParameter::DelayLeftMs, delay_left);
  vocal_fx_set_parameter(VocalFxParameter::DelayRightMs, delay_right);
  vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, delay_feedback);
  vocal_fx_set_parameter(VocalFxParameter::DelayWet, delay_wet);
  vocal_fx_set_parameter(VocalFxParameter::DelayDry, 1.0f);

  vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, reverb_rt60);
  vocal_fx_set_parameter(VocalFxParameter::ReverbDamping, reverb_damping);
  vocal_fx_set_parameter(VocalFxParameter::ReverbWet, reverb_wet);
  vocal_fx_set_spatial_routing(routing);
  vocal_fx_set_spatial_source(spatial_source);
  vocal_fx_set_mute_dry(mute_dry);

  if (enable_harmony) {
    vocal_fx_set_harmony_enabled(0, true);
    vocal_fx_set_harmony_interval(0, harmony_interval);
    vocal_fx_set_harmony_gain(0, harmony_gain);
    vocal_fx_set_harmony_pan(0, harmony_pan);
  }

  size_t total_frames = mono_in.size();
  std::vector<float> out_l(total_frames, 0.0f);
  std::vector<float> out_r(total_frames, 0.0f);

  float block_in[64];
  float block_out_l[64];
  float block_out_r[64];

  for (size_t pos = 0; pos < total_frames; pos += 64) {
    size_t n = std::min<size_t>(64, total_frames - pos);
    for (size_t i = 0; i < n; ++i) {
      block_in[i] = mono_in[pos + i];
    }
    for (size_t i = n; i < 64; ++i) block_in[i] = 0.0f;

    if (param_sweep == "routing") {
      size_t block_idx = pos / 64;
      if (block_idx > 0 && block_idx % 375 == 0) {
        static bool s_is_parallel = false;
        s_is_parallel = !s_is_parallel;
        vocal_fx_set_spatial_routing(s_is_parallel ? SpatialFxRouting::Parallel : SpatialFxRouting::DelayIntoReverb);
      }
    } else if (param_sweep == "wet") {
      float t = static_cast<float>(pos) / sample_rate;
      float wet = 0.5f * (1.0f + std::sin(2.0f * 3.14159265f * 0.5f * t));
      vocal_fx_set_parameter(VocalFxParameter::ReverbWet, wet);
    } else if (param_sweep == "rt60") {
      float t = static_cast<float>(pos) / sample_rate;
      float rt = 0.4f + 4.6f * (0.5f * (1.0f + std::sin(2.0f * 3.14159265f * 0.2f * t)));
      vocal_fx_set_parameter(VocalFxParameter::ReverbDecaySeconds, rt);
    } else if (param_sweep == "damping") {
      float t = static_cast<float>(pos) / sample_rate;
      float d = 0.5f * (1.0f + std::sin(2.0f * 3.14159265f * 0.5f * t));
      vocal_fx_set_parameter(VocalFxParameter::ReverbDamping, d);
    } else if (param_sweep == "enable_reverb") {
      size_t block_idx = pos / 64;
      if (block_idx > 0 && block_idx % 375 == 0) {
        static bool s_rev_en = true;
        s_rev_en = !s_rev_en;
        vocal_fx_set_parameter(VocalFxParameter::EnableReverb, s_rev_en ? 1.0f : 0.0f);
      }
    } else if (param_sweep == "enable_delay") {
      size_t block_idx = pos / 64;
      if (block_idx > 0 && block_idx % 375 == 0) {
        static bool s_del_en = true;
        s_del_en = !s_del_en;
        vocal_fx_set_parameter(VocalFxParameter::EnableDelay, s_del_en ? 1.0f : 0.0f);
      }
    }

    vocal_fx_process(block_in, block_out_l, block_out_r, n);

    if (enable_harmony) {
      while (vocal_fx_run_pitch_analysis(8)) {
      }
    }

    for (size_t i = 0; i < n; ++i) {
      out_l[pos + i] = block_out_l[i];
      out_r[pos + i] = block_out_r[i];
    }
  }

  if (!write_wav_float32(output_path, out_l.data(), out_r.data(), total_frames, sample_rate)) {
    std::fprintf(stderr, "failed to write output WAV: %s\n", output_path.c_str());
    return 3;
  }

  if (print_profile) {
    auto pipe_stats = vocal_fx_profile_stats(VocalFxProfileSection::Pipeline);
    auto harm_stats = vocal_fx_profile_stats(VocalFxProfileSection::Harmony);
    auto del_stats = vocal_fx_profile_stats(VocalFxProfileSection::Delay);
    auto rev_stats = vocal_fx_profile_stats(VocalFxProfileSection::Reverb);

    std::printf("PROFILING_SUMMARY:\n");
    std::printf("blocks=%llu\n", static_cast<unsigned long long>(pipe_stats.blocks));
    std::printf("pipeline_total_us=%llu pipeline_avg_us=%.2f pipeline_max_us=%llu misses=%llu\n",
                static_cast<unsigned long long>(pipe_stats.total_us),
                pipe_stats.blocks ? (double)pipe_stats.total_us / pipe_stats.blocks : 0.0,
                static_cast<unsigned long long>(pipe_stats.worst_us),
                static_cast<unsigned long long>(pipe_stats.deadline_misses));
    std::printf("harmony_total_us=%llu harmony_avg_us=%.2f harmony_max_us=%llu\n",
                static_cast<unsigned long long>(harm_stats.total_us),
                harm_stats.blocks ? (double)harm_stats.total_us / harm_stats.blocks : 0.0,
                static_cast<unsigned long long>(harm_stats.worst_us));
    std::printf("delay_total_us=%llu delay_avg_us=%.2f delay_max_us=%llu\n",
                static_cast<unsigned long long>(del_stats.total_us),
                del_stats.blocks ? (double)del_stats.total_us / del_stats.blocks : 0.0,
                static_cast<unsigned long long>(del_stats.worst_us));
    std::printf("reverb_total_us=%llu reverb_avg_us=%.2f reverb_max_us=%llu\n",
                static_cast<unsigned long long>(rev_stats.total_us),
                rev_stats.blocks ? (double)rev_stats.total_us / rev_stats.blocks : 0.0,
                static_cast<unsigned long long>(rev_stats.worst_us));
    std::printf("delay_mem_bytes=%zu\n", vocal_fx_delay_memory_bytes());
    std::printf("reverb_mem_bytes=%zu\n", vocal_fx_reverb_memory_bytes());
    std::printf("dsp_total_mem_bytes=%zu\n", vocal_fx_dsp_memory_bytes());
  }

  return 0;
}
