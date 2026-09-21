#include "vocal_fx.h"
#include "tempo.h"
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

bool write_wav_stereo(const std::string &path, const float *left, const float *right, size_t frames, uint32_t rate) {
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

struct AudioMetrics {
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  float peak_mono = 0.0f;
  float rms_l = 0.0f;
  float rms_r = 0.0f;
  float rms_mono = 0.0f;
  float crest_factor_db_l = 0.0f;
  float crest_factor_db_r = 0.0f;
  float lr_correlation = 0.0f;
  float dc_offset_l = 0.0f;
  float dc_offset_r = 0.0f;
};

AudioMetrics analyze_stereo(const std::vector<float> &left, const std::vector<float> &right) {
  const size_t n = left.size();
  AudioMetrics m{};
  if (n == 0) return m;

  double sum_l = 0.0, sum_r = 0.0;
  double sum_sq_l = 0.0, sum_sq_r = 0.0, sum_sq_mono = 0.0;
  double dot_lr = 0.0;

  for (size_t i = 0; i < n; ++i) {
    const float l = left[i];
    const float r = right[i];
    const float mono = 0.5f * (l + r);

    m.peak_l = std::max(m.peak_l, std::fabs(l));
    m.peak_r = std::max(m.peak_r, std::fabs(r));
    m.peak_mono = std::max(m.peak_mono, std::fabs(mono));

    sum_l += l;
    sum_r += r;
    sum_sq_l += static_cast<double>(l) * l;
    sum_sq_r += static_cast<double>(r) * r;
    sum_sq_mono += static_cast<double>(mono) * mono;
    dot_lr += static_cast<double>(l) * r;
  }

  m.dc_offset_l = static_cast<float>(sum_l / n);
  m.dc_offset_r = static_cast<float>(sum_r / n);
  m.rms_l = static_cast<float>(std::sqrt(sum_sq_l / n));
  m.rms_r = static_cast<float>(std::sqrt(sum_sq_r / n));
  m.rms_mono = static_cast<float>(std::sqrt(sum_sq_mono / n));

  m.crest_factor_db_l = (m.rms_l > 1e-7f) ? 20.0f * std::log10(m.peak_l / m.rms_l) : 0.0f;
  m.crest_factor_db_r = (m.rms_r > 1e-7f) ? 20.0f * std::log10(m.peak_r / m.rms_r) : 0.0f;

  const double denom = std::sqrt(sum_sq_l * sum_sq_r);
  m.lr_correlation = (denom > 1e-9) ? static_cast<float>(dot_lr / denom) : 1.0f;

  return m;
}

int main(int argc, char **argv) {
  std::string input_path = "samples/dry-acapella-leave-this-place_95bpm.wav";
  if (argc > 1) {
    input_path = argv[1];
  }

  std::vector<float> input;
  uint32_t sample_rate = 44100;
  if (!read_wav_mono(input_path, input, sample_rate)) {
    // Try parent directory
    input_path = "../" + input_path;
    if (!read_wav_mono(input_path, input, sample_rate)) {
      std::fprintf(stderr, "Failed to load input audio from %s\n", input_path.c_str());
      return 1;
    }
  }

  std::printf("Loaded input audio: %s (%zu samples, %.2f s, %u Hz)\n",
              input_path.c_str(), input.size(), input.size() / static_cast<double>(sample_rate), sample_rate);

  // Render 1: Dry reference
  write_wav_stereo("dry.wav", input.data(), input.data(), input.size(), sample_rate);
  auto m_dry = analyze_stereo(input, input);
  std::printf("\n=== DRY REFERENCE ===\n");
  std::printf("  Peak: %.4f | RMS: %.4f | Crest: %.2f dB\n", m_dry.peak_l, m_dry.rms_l, m_dry.crest_factor_db_l);

  // Render 2: Delay Sync at 95 BPM (Left = 1/8 = ~315.8 ms, Right = 1/8D = ~473.7 ms)
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = true;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.tempo_bpm = 95.0f;
    cfg.delay_sync_enabled = true;
    cfg.delay_left_subdivision = TempoSubdivision::Eighth;
    cfg.delay_right_subdivision = TempoSubdivision::DottedEighth;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed\n");
      return 1;
    }
    vocal_fx_set_parameter(VocalFxParameter::DelayWet, 0.4f);
    vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, 0.35f);

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("delay_sync.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m_delay = analyze_stereo(left, right);
    std::printf("\n=== DELAY SYNC (95 BPM, 1/8 L, 1/8D R) ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f\n", m_delay.peak_l, m_delay.peak_r, m_delay.rms_l, m_delay.rms_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n", m_delay.lr_correlation, m_delay.peak_mono, m_delay.rms_mono);
    std::printf("  Rendered: delay_sync.wav\n");
  }

  // Render 3: Chorus Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_chorus = true;
    cfg.chorus.mode = ChorusMode::Chorus;
    cfg.chorus.mix = 0.5f;
    cfg.chorus.rate_hz = 1.2f;
    cfg.chorus.depth_ms = 2.5f;
    cfg.chorus.base_delay_ms = 7.0f;
    cfg.chorus.width = 1.0f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for chorus\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("chorus.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== CHORUS MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: chorus.wav\n");
  }

  // Render 4: Ensemble Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_chorus = true;
    cfg.chorus.mode = ChorusMode::Ensemble;
    cfg.chorus.mix = 0.5f;
    cfg.chorus.rate_hz = 0.8f;
    cfg.chorus.depth_ms = 3.5f;
    cfg.chorus.base_delay_ms = 10.0f;
    cfg.chorus.width = 1.0f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for ensemble\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("ensemble.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== ENSEMBLE MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: ensemble.wav\n");
  }

  // Render 5: Dimension Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_chorus = true;
    cfg.chorus.mode = ChorusMode::Dimension;
    cfg.chorus.mix = 0.5f;
    cfg.chorus.rate_hz = 0.5f;
    cfg.chorus.depth_ms = 1.5f;
    cfg.chorus.base_delay_ms = 5.0f;
    cfg.chorus.width = 1.0f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for dimension\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("dimension.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== DIMENSION MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: dimension.wav\n");
  }

  // Render 6: Chorus BPM Sync (95 BPM, 1/4 note)
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.tempo_bpm = 95.0f;
    cfg.enable_chorus = true;
    cfg.chorus.mode = ChorusMode::Chorus;
    cfg.chorus.sync_enabled = true;
    cfg.chorus.subdivision = TempoSubdivision::Quarter;
    cfg.chorus.mix = 0.5f;
    cfg.chorus.depth_ms = 2.5f;
    cfg.chorus.base_delay_ms = 7.0f;
    cfg.chorus.width = 1.0f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for chorus sync\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("chorus_sync.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== CHORUS SYNC (95 BPM, 1/4) ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: chorus_sync.wav\n");
  }

  // Render 7: Drive Warm Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_drive = true;
    cfg.drive.mode = DriveMode::Warm;
    cfg.drive.drive = 0.5f;
    cfg.drive.tone = 0.6f;
    cfg.drive.mix = 1.0f;
    cfg.drive.output_level = 0.9f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for drive warm\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("drive_warm.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== DRIVE WARM MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: drive_warm.wav\n");
  }

  // Render 8: Drive Overdrive Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_drive = true;
    cfg.drive.mode = DriveMode::Overdrive;
    cfg.drive.drive = 0.6f;
    cfg.drive.tone = 0.5f;
    cfg.drive.mix = 1.0f;
    cfg.drive.output_level = 0.85f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for drive overdrive\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("drive_overdrive.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== DRIVE OVERDRIVE MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: drive_overdrive.wav\n");
  }

  // Render 9: Drive Megaphone Mode
  {
    VocalFxConfig cfg{};
    cfg.sample_rate = static_cast<float>(sample_rate);
    cfg.block_size = 64;
    cfg.enable_gate = false;
    cfg.enable_compressor = false;
    cfg.enable_delay = false;
    cfg.enable_reverb = false;
    cfg.enable_pitch_analysis = false;
    cfg.enable_drive = true;
    cfg.drive.mode = DriveMode::Megaphone;
    cfg.drive.drive = 0.5f;
    cfg.drive.tone = 0.8f;
    cfg.drive.mix = 1.0f;
    cfg.drive.output_level = 0.9f;

    if (!vocal_fx_init(cfg)) {
      std::fprintf(stderr, "vocal_fx_init failed for drive megaphone\n");
      return 1;
    }

    std::vector<float> left(input.size()), right(input.size());
    const size_t block_size = 64;
    for (size_t offset = 0; offset < input.size(); offset += block_size) {
      const size_t n = std::min(block_size, input.size() - offset);
      vocal_fx_process(input.data() + offset, left.data() + offset, right.data() + offset, n);
    }

    write_wav_stereo("drive_megaphone.wav", left.data(), right.data(), left.size(), sample_rate);
    auto m = analyze_stereo(left, right);
    std::printf("\n=== DRIVE MEGAPHONE MODE ===\n");
    std::printf("  Peak L/R: %.4f / %.4f | RMS L/R: %.4f / %.4f | Crest: %.2f / %.2f dB\n",
                m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.crest_factor_db_l, m.crest_factor_db_r);
    std::printf("  L/R Correlation: %.4f | Mono Sum Peak/RMS: %.4f / %.4f\n",
                m.lr_correlation, m.peak_mono, m.rms_mono);
    std::printf("  Rendered: drive_megaphone.wav\n");
  }

  return 0;
}
