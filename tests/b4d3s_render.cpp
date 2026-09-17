// B4D.3S host render: same source and FX parameters at 48k / 44.1k / 40k.
// This is a host-side listening render (not a P4 capture); it uses the same
// engine, FX parameters and 64-frame blocks as the firmware.
#include "vocal_fx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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
  char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
               static_cast<char>((v >> 16) & 0xff),
               static_cast<char>((v >> 24) & 0xff)};
  f.write(b, 4);
}
static bool write_wav(const std::string &path, const float *l, const float *r,
                      size_t frames, uint32_t rate) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  const uint32_t ch = 2, bits = 32;
  const uint32_t byte_rate = rate * ch * (bits / 8);
  const uint32_t block_align = ch * (bits / 8);
  const uint32_t data_bytes = static_cast<uint32_t>(frames * block_align);
  f.write("RIFF", 4);
  put32(f, 36 + data_bytes);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 3);
  put16(f, ch);
  put32(f, rate);
  put32(f, byte_rate);
  put16(f, block_align);
  put16(f, bits);
  f.write("data", 4);
  put32(f, data_bytes);
  for (size_t i = 0; i < frames; ++i) {
    float s[2] = {l[i], r[i]};
    f.write(reinterpret_cast<const char *>(s), sizeof(s));
  }
  return f.good();
}
static bool load_wav_mono(const std::string &path, std::vector<float> &mono,
                          uint32_t &rate) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) ||
      std::memcmp(b.data() + 8, "WAVE", 4))
    return false;
  uint16_t fmt = 0, ch = 0, bits = 0;
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
  if (!data || !ch || !rate || (fmt != 1 && fmt != 3)) return false;
  const size_t stride = ch * bits / 8;
  const size_t frames = bytes / stride;
  mono.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (unsigned k = 0; k < ch; ++k) {
      const unsigned char *p = data + i * stride + k * bits / 8;
      if (fmt == 1) {
        if (bits == 16)
          sum += static_cast<int16_t>(u16(p)) / 32768.0;
        else if (bits == 24) {
          int32_t val = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
          sum += (val >> 8) / 8388608.0;
        }
      } else if (fmt == 3 && bits == 32) {
        float val;
        std::memcpy(&val, p, 4);
        sum += val;
      }
    }
    mono[i] = static_cast<float>(sum / ch);
  }
  return true;
}

static bool render_rate(const std::vector<float> &src, uint32_t src_rate,
                        uint32_t rate, const std::string &out) {
  // Linear resample of the source to the target rate (identical source region).
  const double ratio = static_cast<double>(rate) / static_cast<double>(src_rate);
  const size_t out_frames = static_cast<size_t>(src.size() * ratio);
  std::vector<float> in(out_frames);
  for (size_t i = 0; i < out_frames; ++i) {
    const double sp = static_cast<double>(i) / ratio;
    const size_t a = static_cast<size_t>(sp);
    const size_t b = std::min(a + 1, src.size() - 1);
    const float f = static_cast<float>(sp - a);
    in[i] = src[a] * (1.0f - f) + src[b] * f;
  }

  VocalFxConfig cfg{};
  cfg.sample_rate = static_cast<float>(rate);
  cfg.enable_gate = true;
  cfg.enable_compressor = true;
  cfg.enable_delay = true;
  cfg.enable_reverb = true;
  cfg.enable_pitch_analysis = true;
  cfg.align_dry_to_harmony = true;
  cfg.enable_harmony_limiter = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 4.0f;
  cfg.pitch_shift.wet = 1.0f;
  cfg.pitch_shift.formant_mode = FormantMode::Lpc;
  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "vocal_fx_init failed at %u\n", rate);
    return false;
  }
  vocal_fx_set_harmony_mode(HarmonyMode::FixedInterval);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);

  constexpr size_t kBlock = 64;
  std::vector<float> ol(out_frames, 0.0f), orr(out_frames, 0.0f);
  for (size_t pos = 0; pos < out_frames; pos += kBlock) {
    const size_t n = std::min(kBlock, out_frames - pos);
    vocal_fx_process(in.data() + pos, ol.data() + pos, orr.data() + pos, n);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }
  return write_wav(out, ol.data(), orr.data(), out_frames, rate);
}

int main(int argc, char **argv) {
  const std::string src_path =
      argc > 1 ? argv[1] : "samples/dry-acapella-leave-this-place_95bpm.wav";
  const std::string dir = argc > 2 ? argv[2] : "artifacts/alpha01d";
  std::vector<float> mono;
  uint32_t src_rate = 0;
  if (!load_wav_mono(src_path, mono, src_rate)) {
    std::fprintf(stderr, "failed to load %s\n", src_path.c_str());
    return 1;
  }
  std::printf("source %s rate=%u frames=%zu\n", src_path.c_str(), src_rate,
              mono.size());
  const struct {
    uint32_t rate;
    const char *name;
  } rates[] = {{48000, "b4d3s_render_48k.wav"},
               {44100, "b4d3s_render_44100.wav"},
               {40000, "b4d3s_render_40k.wav"}};
  for (const auto &r : rates) {
    const std::string out = dir + "/" + r.name;
    if (!render_rate(mono, src_rate, r.rate, out)) {
      std::fprintf(stderr, "render failed at %u\n", r.rate);
      return 1;
    }
    std::printf("wrote %s (%u Hz)\n", out.c_str(), r.rate);
  }
  return 0;
}
