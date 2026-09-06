#include "pitch_analysis.h"
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
int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(
        stderr,
        "usage: pitch_analyze input.wav output.csv [pitch_marks.csv]\n");
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
  if (!data || !rate || !ch ||
      !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) {
    std::fprintf(stderr, "supports PCM16 or float32 WAV\n");
    return 2;
  }
  PitchAnalysisConfig c{};
  c.input_sample_rate = (float)rate;
  c.analysis_sample_rate = rate % 12000 == 0 ? 12000.0f : rate / 4.0f;
  PitchAnalysis a;
  if (!a.init(c)) {
    std::fprintf(stderr, "unsupported sample rate/configuration\n");
    return 2;
  }
  std::ofstream csv(argv[2]);
  csv << "time_seconds,frequency_hz,confidence,voiced,onset,pitch_changed\n";
  const size_t stride = ch * (bits / 8), frames = bytes / stride;
  uint64_t last = ~uint64_t(0);
  for (size_t i = 0; i < frames;) {
    float block[64];
    size_t n = std::min<size_t>(64, frames - i);
    for (size_t j = 0; j < n; ++j) {
      double sum = 0;
      for (unsigned k = 0; k < ch; ++k) {
        const unsigned char *p = data + (i + j) * stride + k * bits / 8;
        if (fmt == 1)
          sum += (int16_t)u16(p) / 32768.0;
        else {
          float v;
          std::memcpy(&v, p, 4);
          sum += v;
        }
      }
      block[j] = sum / ch;
    }
    a.tap(block, n);
    i += n;
    while (a.run(1)) {
      auto r = a.latest();
      if (r.analysis_timestamp_samples != last) {
        csv << (double)r.analysis_timestamp_samples / rate << ','
            << r.frequency_hz << ',' << r.confidence << ',' << r.voiced << ','
            << r.onset << ',' << r.pitch_changed << '\n';
        last = r.analysis_timestamp_samples;
      }
    }
  }
  if (argc > 3) {
    std::ofstream marks(argv[3]);
    marks << "sample_position,time_seconds,confidence\n";
    PitchMark m[64];
    size_t n = a.marks(0, frames, m, 64);
    for (size_t i = 0; i < n; ++i)
      marks << m[i].sample_position << ','
            << (double)m[i].sample_position / rate << ',' << m[i].confidence
            << '\n';
  }
}
