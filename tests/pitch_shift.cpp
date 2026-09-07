#include "vocal_fx.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static uint32_t u32(const unsigned char *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}
static uint16_t u16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put16(std::ofstream &f, uint16_t v) {
  char b[2] = {(char)v, (char)(v >> 8)};
  f.write(b, 2);
}
static void put32(std::ofstream &f, uint32_t v) {
  char b[4] = {(char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24)};
  f.write(b, 4);
}
int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: pitch_shift input.wav output.wav semitones\n");
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) ||
      std::memcmp(b.data() + 8, "WAVE", 4)) {
    std::fprintf(stderr, "invalid WAV\n");
    return 2;
  }
  uint16_t fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  const unsigned char *data = nullptr;
  size_t bytes = 0;
  for (size_t p = 12; p + 8 <= b.size();) {
    uint32_t n = u32(&b[p + 4]);
    if (p + 8 + n > b.size())
      break;
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
  if (!data || rate != 48000 || !ch ||
      !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
    std::fprintf(stderr, "requires 48 kHz PCM16 or float32 WAV\n");
    return 2;
  }
  VocalFxConfig c{};
  c.enable_gate = c.enable_compressor = c.enable_delay = c.enable_reverb =
      false;
  c.pitch_shift.enabled = true;
  c.pitch_shift.semitones = std::stof(argv[3]);
  c.pitch_shift.wet = 1;
  if (!vocal_fx_init(c)) {
    std::fprintf(stderr, "engine init failed\n");
    return 2;
  }
  const size_t stride = ch * bits / 8, frames = bytes / stride;
  std::vector<float> mono(frames), out(frames), right(64);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (unsigned k = 0; k < ch; ++k) {
      const unsigned char *p = data + i * stride + k * bits / 8;
      if (fmt == 1)
        sum += (int16_t)u16(p) / 32768.0;
      else {
        float v;
        std::memcpy(&v, p, 4);
        sum += v;
      }
    }
    mono[i] = sum / ch;
  }
  for (size_t i = 0; i < frames; i += 64) {
    size_t n = std::min<size_t>(64, frames - i);
    vocal_fx_process(mono.data() + i, out.data() + i, right.data(), n);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }
  std::ofstream w(argv[2], std::ios::binary);
  const uint32_t data_bytes = frames * 4;
  w.write("RIFF", 4);
  put32(w, 36 + data_bytes);
  w.write("WAVEfmt ", 8);
  put32(w, 16);
  put16(w, 3);
  put16(w, 1);
  put32(w, rate);
  put32(w, rate * 4);
  put16(w, 4);
  put16(w, 32);
  w.write("data", 4);
  put32(w, data_bytes);
  w.write(reinterpret_cast<const char *>(out.data()), data_bytes);
  const auto t = vocal_fx_pitch_shift_telemetry();
  std::fprintf(stderr,
               "latency=%u samples grains=%llu max/block=%u fallback=%llu "
               "mark_underflow=%llu history_underflow=%llu\n",
               vocal_fx_pitch_shift_latency_samples(),
               (unsigned long long)t.grains, t.max_grains_per_block,
               (unsigned long long)t.fallback_frames,
               (unsigned long long)t.pitch_mark_underflows,
               (unsigned long long)t.audio_history_underflows);
}
