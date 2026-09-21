#include "chorus.h"
#include "vocal_fx.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

bool read_wav_mono(const std::string &path, std::vector<float> &mono_out, uint32_t &sample_rate) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
  if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
    return false;
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
  if (!data || !ch || !rate || (fmt != 1 && fmt != 3)) {
    return false;
  }
  sample_rate = rate;
  const size_t stride = ch * bits / 8;
  const size_t frames = bytes / stride;
  mono_out.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    double sum = 0;
    for (unsigned k = 0; k < ch; ++k) {
      const unsigned char *p = data + i * stride + k * bits / 8;
      if (fmt == 1) {
        if (bits == 16) {
          sum += static_cast<int16_t>(u16(p)) / 32768.0;
        } else if (bits == 24) {
          int32_t val = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
          sum += (val >> 8) / 8388608.0;
        }
      } else if (fmt == 3 && bits == 32) {
        float val;
        std::memcpy(&val, p, 4);
        sum += val;
      }
    }
    mono_out[i] = static_cast<float>(sum / ch);
  }
  return true;
}

bool write_wav_pcm16(const std::string &path, const float *left, const float *right, size_t frames, uint32_t rate) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  const uint16_t channels = (right != nullptr) ? 2 : 1;
  const uint16_t bits_per_sample = 16;
  const uint32_t byte_rate = rate * channels * (bits_per_sample / 8);
  const uint16_t block_align = channels * (bits_per_sample / 8);
  const uint32_t data_bytes = static_cast<uint32_t>(frames * block_align);

  f.write("RIFF", 4);
  put32(f, 36 + data_bytes);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 1); // PCM
  put16(f, channels);
  put32(f, rate);
  put32(f, byte_rate);
  put16(f, block_align);
  put16(f, bits_per_sample);
  f.write("data", 4);
  put32(f, data_bytes);

  std::vector<int16_t> pcm_data(frames * channels);
  if (channels == 2) {
    for (size_t i = 0; i < frames; ++i) {
      float l = std::clamp(left[i], -1.0f, 1.0f);
      float r = std::clamp(right[i], -1.0f, 1.0f);
      pcm_data[2 * i] = static_cast<int16_t>(std::round(l * 32767.0f));
      pcm_data[2 * i + 1] = static_cast<int16_t>(std::round(r * 32767.0f));
    }
  } else {
    for (size_t i = 0; i < frames; ++i) {
      float s = std::clamp(left[i], -1.0f, 1.0f);
      pcm_data[i] = static_cast<int16_t>(std::round(s * 32767.0f));
    }
  }
  f.write(reinterpret_cast<const char *>(pcm_data.data()), data_bytes);
  return f.good();
}

struct AudioMetrics {
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  float peak_mono = 0.0f;
  float rms_l = 0.0f;
  float rms_r = 0.0f;
  float rms_stereo = 0.0f;
  float rms_mono = 0.0f;
  float crest_l_db = 0.0f;
  float crest_r_db = 0.0f;
  float lr_correlation = 0.0f;
  float mono_diff_db = 0.0f;
  float side_mid_ratio_db = 0.0f;
};

AudioMetrics analyze_stereo(const std::vector<float> &left, const std::vector<float> &right) {
  const size_t n = left.size();
  AudioMetrics m{};
  if (n == 0) return m;

  double sum_sq_l = 0.0, sum_sq_r = 0.0, sum_sq_mono = 0.0;
  double sum_sq_mid = 0.0, sum_sq_side = 0.0;
  double dot_lr = 0.0;

  for (size_t i = 0; i < n; ++i) {
    const float l = left[i];
    const float r = right[i];
    const float mono = 0.5f * (l + r);
    const float mid = mono;
    const float side = 0.5f * (l - r);

    m.peak_l = std::max(m.peak_l, std::fabs(l));
    m.peak_r = std::max(m.peak_r, std::fabs(r));
    m.peak_mono = std::max(m.peak_mono, std::fabs(mono));

    sum_sq_l += static_cast<double>(l) * l;
    sum_sq_r += static_cast<double>(r) * r;
    sum_sq_mono += static_cast<double>(mono) * mono;
    sum_sq_mid += static_cast<double>(mid) * mid;
    sum_sq_side += static_cast<double>(side) * side;
    dot_lr += static_cast<double>(l) * r;
  }

  m.rms_l = static_cast<float>(std::sqrt(sum_sq_l / n));
  m.rms_r = static_cast<float>(std::sqrt(sum_sq_r / n));
  m.rms_stereo = static_cast<float>(std::sqrt(0.5 * (sum_sq_l + sum_sq_r) / n));
  m.rms_mono = static_cast<float>(std::sqrt(sum_sq_mono / n));

  m.crest_l_db = (m.rms_l > 1e-7f) ? 20.0f * std::log10(m.peak_l / m.rms_l) : 0.0f;
  m.crest_r_db = (m.rms_r > 1e-7f) ? 20.0f * std::log10(m.peak_r / m.rms_r) : 0.0f;

  const double denom = std::sqrt(sum_sq_l * sum_sq_r);
  m.lr_correlation = (denom > 1e-9) ? static_cast<float>(dot_lr / denom) : 1.0f;
  m.mono_diff_db = (m.rms_stereo > 1e-7f && m.rms_mono > 1e-7f)
                       ? 20.0f * std::log10(m.rms_mono / m.rms_stereo)
                       : 0.0f;
  m.side_mid_ratio_db = (sum_sq_mid > 1e-12 && sum_sq_side > 1e-12)
                            ? 10.0f * std::log10(sum_sq_side / sum_sq_mid)
                            : -120.0f;

  return m;
}

struct MicroshiftRenderCase {
  const char *id;
  const char *description;
  ChorusMode mode;
  float rate;
  float depth;
  float mix;
  float feedback;
  float left_cents;
  float right_cents;
  float window_ms;
  MicroshiftCrossfade xfade;
};

int main(int argc, char **argv) {
  std::string input_path = "samples/dry-acapella-leave-this-place_95bpm.wav";
  std::string out_dir = "artifacts/microshift";

  if (argc > 1) input_path = argv[1];
  if (argc > 2) out_dir = argv[2];

  fs::create_directories(out_dir);

  std::vector<float> input;
  uint32_t sample_rate = 44100;
  if (!read_wav_mono(input_path, input, sample_rate)) {
    std::fprintf(stderr, "Failed to load input WAV: %s\n", input_path.c_str());
    return 1;
  }
  std::printf("Loaded %s: %zu frames @ %u Hz (%.2f s)\n",
              input_path.c_str(), input.size(), sample_rate,
              static_cast<float>(input.size()) / sample_rate);

  const std::vector<MicroshiftRenderCase> cases = {
      {"01_dry", "Unprocessed Dry Vocal",
       ChorusMode::Chorus, 1.2f, 0.35f, 0.0f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"02_chorus_default", "Standard VoxP4 Chorus Default (1.2 Hz, 0.35 depth, 35% mix)",
       ChorusMode::Chorus, 1.2f, 0.35f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"03_ensemble_default", "Standard VoxP4 Ensemble Default (Multi-tap BBD, 35% mix)",
       ChorusMode::Ensemble, 0.8f, 0.5f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"04_dimension_default", "Standard VoxP4 Dimension Default (Static Dual-LFO cross-mix, 35% mix)",
       ChorusMode::Dimension, 0.5f, 0.4f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"05_microshift_default", "Microshift Default (-7/+9 cents, 25 ms, 35% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"06_microshift_subtle", "Microshift Subtle (-4/+4 cents, 20 ms, 25% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.25f, 0.0f, -4.0f, 4.0f, 20.0f, MicroshiftCrossfade::EqualPower},
      {"07_microshift_wide", "Microshift Wide (-12/+12 cents, 30 ms, 45% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.45f, 0.0f, -12.0f, 12.0f, 30.0f, MicroshiftCrossfade::EqualPower},
      {"08_microshift_symmetric", "Microshift Symmetric (-9/+9 cents, 25 ms, 35% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.35f, 0.0f, -9.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"09_microshift_asymmetric", "Microshift Asymmetric (-6/+9 cents, 25 ms, 35% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.35f, 0.0f, -6.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"10_microshift_linear_xfade", "Microshift Linear Crossfade (-7/+9 cents, 25 ms, 35% mix, Linear)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::Linear},
      {"11_microshift_equal_power_xfade", "Microshift EqualPower Crossfade (-7/+9 cents, 25 ms, 35% mix, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 0.35f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower},
      {"12_microshift_100pct_wet", "Microshift 100% Wet (-7/+9 cents, 25 ms, 100% wet, EqualPower)",
       ChorusMode::Microshift, 0.0f, 0.0f, 1.0f, 0.0f, -7.0f, 9.0f, 25.0f, MicroshiftCrossfade::EqualPower}
  };

  std::ofstream csv(out_dir + "/metrics.csv");
  csv << "id,description,peak_l,peak_r,peak_mono,rms_l,rms_r,rms_stereo,rms_mono,crest_l_db,crest_r_db,lr_corr,mono_diff_db,side_mid_db\n";

  std::printf("\n=================================================================================================================================\n");
  std::printf("%-30s | %-7s | %-7s | %-7s | %-7s | %-7s | %-8s | %-8s\n",
              "Render Target", "Peak L", "Peak R", "RMS L", "RMS R", "L/R Corr", "Mono Diff", "Side/Mid");
  std::printf("=================================================================================================================================\n");

  const size_t total_frames = input.size();
  constexpr size_t kBlock = 64;

  for (const auto &tc : cases) {
    VocalChorus chorus;
    chorus.init(static_cast<float>(sample_rate));
    chorus.set_mode(tc.mode);
    chorus.set_rate_hz(tc.rate);
    chorus.set_depth_ms(tc.depth);
    chorus.set_mix(tc.mix);
    chorus.set_microshift_cents(tc.left_cents, tc.right_cents);
    chorus.set_microshift_window_ms(tc.window_ms);
    chorus.set_microshift_crossfade(tc.xfade);

    std::vector<float> left(total_frames, 0.0f);
    std::vector<float> right(total_frames, 0.0f);
    std::vector<float> mono(total_frames, 0.0f);

    for (size_t pos = 0; pos < total_frames; pos += kBlock) {
      const size_t n = std::min(kBlock, total_frames - pos);
      chorus.process(input.data() + pos, input.data() + pos,
                     left.data() + pos, right.data() + pos, n, 120.0f);
    }

    for (size_t i = 0; i < total_frames; ++i) {
      mono[i] = 0.5f * (left[i] + right[i]);
    }

    const std::string path_stereo = out_dir + "/" + tc.id + ".wav";
    const std::string path_mono = out_dir + "/" + tc.id + "_mono.wav";

    write_wav_pcm16(path_stereo, left.data(), right.data(), total_frames, sample_rate);
    write_wav_pcm16(path_mono, mono.data(), nullptr, total_frames, sample_rate);

    AudioMetrics m = analyze_stereo(left, right);

    std::printf("%-30s | %7.4f | %7.4f | %7.4f | %7.4f | %+7.3f | %+6.2f dB | %+6.2f dB\n",
                tc.id, m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.lr_correlation, m.mono_diff_db, m.side_mid_ratio_db);

    csv << tc.id << ",\"" << tc.description << "\","
        << m.peak_l << "," << m.peak_r << "," << m.peak_mono << ","
        << m.rms_l << "," << m.rms_r << "," << m.rms_stereo << "," << m.rms_mono << ","
        << m.crest_l_db << "," << m.crest_r_db << ","
        << m.lr_correlation << "," << m.mono_diff_db << "," << m.side_mid_ratio_db << "\n";
  }

  csv.close();
  std::printf("=================================================================================================================================\n");
  std::printf("Rendered %zu files and metrics CSV to %s\n", cases.size(), out_dir.c_str());

  return 0;
}
