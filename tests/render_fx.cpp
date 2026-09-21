#include "vocal_fx.h"
#include "tempo.h"
#include "chorus.h"
#include "vocal_drive.h"
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

// Writes 16-bit PCM WAV (stereo if right != nullptr, mono if right == nullptr)
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
  float lr_correlation = 0.0f;
  float mono_diff_db = 0.0f;
};

AudioMetrics analyze_stereo(const std::vector<float> &left, const std::vector<float> &right) {
  const size_t n = left.size();
  AudioMetrics m{};
  if (n == 0) return m;

  double sum_sq_l = 0.0, sum_sq_r = 0.0, sum_sq_mono = 0.0;
  double dot_lr = 0.0;

  for (size_t i = 0; i < n; ++i) {
    const float l = left[i];
    const float r = right[i];
    const float mono = 0.5f * (l + r);

    m.peak_l = std::max(m.peak_l, std::fabs(l));
    m.peak_r = std::max(m.peak_r, std::fabs(r));
    m.peak_mono = std::max(m.peak_mono, std::fabs(mono));

    sum_sq_l += static_cast<double>(l) * l;
    sum_sq_r += static_cast<double>(r) * r;
    sum_sq_mono += static_cast<double>(mono) * mono;
    dot_lr += static_cast<double>(l) * r;
  }

  m.rms_l = static_cast<float>(std::sqrt(sum_sq_l / n));
  m.rms_r = static_cast<float>(std::sqrt(sum_sq_r / n));
  m.rms_stereo = static_cast<float>(std::sqrt(0.5 * (sum_sq_l + sum_sq_r) / n));
  m.rms_mono = static_cast<float>(std::sqrt(sum_sq_mono / n));

  const double denom = std::sqrt(sum_sq_l * sum_sq_r);
  m.lr_correlation = (denom > 1e-9) ? static_cast<float>(dot_lr / denom) : 1.0f;
  m.mono_diff_db = (m.rms_stereo > 1e-7f && m.rms_mono > 1e-7f)
                       ? 20.0f * std::log10(m.rms_mono / m.rms_stereo)
                       : 0.0f;

  return m;
}

struct RenderJob {
  const char *base_name;
  const char *desc;
  VocalFxConfig cfg;
  bool use_pitch_worker = false;
  float harmony_interval = 0.0f;
  float harmony_gain = 0.0f;
  float delay_wet = 0.0f;
  float delay_feedback = 0.0f;
  float reverb_wet = 0.0f;
};

void run_render(const RenderJob &job,
                const std::vector<float> &input,
                uint32_t sample_rate,
                const std::string &out_dir) {
  VocalFxConfig cfg = job.cfg;
  cfg.sample_rate = static_cast<float>(sample_rate);
  cfg.block_size = 64;

  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "FAIL: vocal_fx_init failed for %s\n", job.base_name);
    return;
  }

  if (job.harmony_gain > 0.0f) {
    vocal_fx_set_harmony_enabled(0, true);
    vocal_fx_set_harmony_interval(0, job.harmony_interval);
    vocal_fx_set_harmony_gain(0, job.harmony_gain);
    vocal_fx_set_formant_mode(0, FormantMode::Lpc);
    vocal_fx_set_formant_amount(0, 1.0f);
  }
  if (job.delay_wet > 0.0f) {
    vocal_fx_set_parameter(VocalFxParameter::DelayWet, job.delay_wet);
    vocal_fx_set_parameter(VocalFxParameter::DelayFeedback, job.delay_feedback);
  }
  if (job.reverb_wet > 0.0f) {
    vocal_fx_set_parameter(VocalFxParameter::ReverbWet, job.reverb_wet);
  }

  const size_t total_frames = input.size();
  constexpr size_t kBlock = 64;
  std::vector<float> left(total_frames, 0.0f);
  std::vector<float> right(total_frames, 0.0f);
  std::vector<float> mono(total_frames, 0.0f);

  for (size_t pos = 0; pos < total_frames; pos += kBlock) {
    const size_t n = std::min(kBlock, total_frames - pos);
    vocal_fx_process(input.data() + pos, left.data() + pos, right.data() + pos, n);
    if (job.use_pitch_worker) {
      while (vocal_fx_run_pitch_analysis(8)) {
      }
    }
  }

  for (size_t i = 0; i < total_frames; ++i) {
    mono[i] = 0.5f * (left[i] + right[i]);
  }

  const std::string path_stereo = out_dir + "/" + job.base_name + ".wav";
  const std::string path_mono = out_dir + "/" + job.base_name + "_mono.wav";

  write_wav_pcm16(path_stereo, left.data(), right.data(), total_frames, sample_rate);
  write_wav_pcm16(path_mono, mono.data(), nullptr, total_frames, sample_rate);

  auto m = analyze_stereo(left, right);
  std::printf("%-26s | Peak: %6.4f / %6.4f | RMS: %6.4f / %6.4f | Corr: %6.3f | MonoDiff: %5.1f dB | %s\n",
              job.base_name, m.peak_l, m.peak_r, m.rms_l, m.rms_r, m.lr_correlation, m.mono_diff_db, job.desc);
}

int main(int argc, char **argv) {
  std::string input_path = "samples/dry-acapella-leave-this-place_95bpm.wav";
  std::string out_dir = "artifacts/fx_qualification";

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
      out_dir = argv[++i];
    } else {
      input_path = argv[i];
    }
  }

  std::vector<float> input;
  uint32_t sample_rate = 44100;
  if (!read_wav_mono(input_path, input, sample_rate)) {
    input_path = "../" + input_path;
    if (!read_wav_mono(input_path, input, sample_rate)) {
      std::fprintf(stderr, "Failed to load input audio from %s\n", input_path.c_str());
      return 1;
    }
  }

  // 12.0 seconds phrase with reverb/delay decay room (529,200 samples)
  const size_t render_frames = std::min<size_t>(input.size(), static_cast<size_t>(12.0f * sample_rate));
  input.resize(render_frames);

  std::printf("========================================================================================================\n");
  std::printf("  VOXP4 QUALIFIED FX LISTENING RENDERS (16-bit PCM, %u Hz, %.2f s, 95 BPM)\n",
              sample_rate, static_cast<double>(render_frames) / sample_rate);
  std::printf("  Output directory: %s\n", out_dir.c_str());
  std::printf("========================================================================================================\n");

  // 1. Dry reference
  {
    RenderJob job{};
    job.base_name = "dry_reference";
    job.desc = "Unprocessed dry vocal acapella";
    job.cfg.enable_gate = false;
    job.cfg.enable_compressor = false;
    job.cfg.enable_delay = false;
    job.cfg.enable_reverb = false;
    job.cfg.enable_pitch_analysis = false;
    run_render(job, input, sample_rate, out_dir);
  }

  // 2. Harmony reference (1V +4st PSOLA)
  {
    RenderJob job{};
    job.base_name = "harmony_reference";
    job.desc = "Vocal + 1 voice +4st PSOLA, LPC formant";
    job.cfg.enable_gate = false;
    job.cfg.enable_compressor = false;
    job.cfg.enable_delay = false;
    job.cfg.enable_reverb = false;
    job.cfg.enable_pitch_analysis = true;
    job.use_pitch_worker = true;
    job.harmony_interval = 4.0f;
    job.harmony_gain = 0.8f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 3. Chorus: subtle, default, deep
  {
    // subtle
    RenderJob job{};
    job.base_name = "chorus_subtle";
    job.desc = "Chorus subtle: rate 0.50Hz, depth 1.0ms, mix 0.20";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Chorus;
    job.cfg.chorus.rate_hz = 0.50f;
    job.cfg.chorus.depth_ms = 1.0f;
    job.cfg.chorus.base_delay_ms = 12.0f;
    job.cfg.chorus.mix = 0.20f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    // default
    RenderJob job{};
    job.base_name = "chorus_default";
    job.desc = "Chorus tuned default: rate 0.75Hz, depth 1.6ms, mix 0.30";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Chorus;
    job.cfg.chorus.rate_hz = 0.75f;
    job.cfg.chorus.depth_ms = 1.6f;
    job.cfg.chorus.base_delay_ms = 12.0f;
    job.cfg.chorus.mix = 0.30f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    // deep
    RenderJob job{};
    job.base_name = "chorus_deep";
    job.desc = "Chorus deep: rate 1.00Hz, depth 2.5ms, mix 0.45";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Chorus;
    job.cfg.chorus.rate_hz = 1.00f;
    job.cfg.chorus.depth_ms = 2.5f;
    job.cfg.chorus.base_delay_ms = 14.0f;
    job.cfg.chorus.mix = 0.45f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 4. Ensemble: subtle, default, dense
  {
    RenderJob job{};
    job.base_name = "ensemble_subtle";
    job.desc = "Ensemble subtle: 3 taps, depth 1.5ms, mix 0.25";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Ensemble;
    job.cfg.chorus.rate_hz = 0.50f;
    job.cfg.chorus.depth_ms = 1.5f;
    job.cfg.chorus.base_delay_ms = 10.0f;
    job.cfg.chorus.mix = 0.25f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "ensemble_default";
    job.desc = "Ensemble tuned default: 3 taps, depth 2.2ms, mix 0.35";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Ensemble;
    job.cfg.chorus.rate_hz = 0.70f;
    job.cfg.chorus.depth_ms = 2.2f;
    job.cfg.chorus.base_delay_ms = 12.0f;
    job.cfg.chorus.mix = 0.35f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "ensemble_dense";
    job.desc = "Ensemble dense: 3 taps, depth 3.2ms, mix 0.50";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Ensemble;
    job.cfg.chorus.rate_hz = 0.90f;
    job.cfg.chorus.depth_ms = 3.2f;
    job.cfg.chorus.base_delay_ms = 14.0f;
    job.cfg.chorus.mix = 0.50f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 5. Dimension: subtle, default, wide
  {
    RenderJob job{};
    job.base_name = "dimension_subtle";
    job.desc = "Dimension subtle: depth 0.5ms, mix 0.25, width 0.7";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Dimension;
    job.cfg.chorus.rate_hz = 0.30f;
    job.cfg.chorus.depth_ms = 0.5f;
    job.cfg.chorus.base_delay_ms = 8.0f;
    job.cfg.chorus.mix = 0.25f;
    job.cfg.chorus.width = 0.7f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "dimension_default";
    job.desc = "Dimension tuned default: depth 0.8ms, mix 0.35, width 1.0";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Dimension;
    job.cfg.chorus.rate_hz = 0.40f;
    job.cfg.chorus.depth_ms = 0.8f;
    job.cfg.chorus.base_delay_ms = 9.0f;
    job.cfg.chorus.mix = 0.35f;
    job.cfg.chorus.width = 1.0f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "dimension_wide";
    job.desc = "Dimension wide: depth 1.2ms, mix 0.50, width 1.0";
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Dimension;
    job.cfg.chorus.rate_hz = 0.50f;
    job.cfg.chorus.depth_ms = 1.2f;
    job.cfg.chorus.base_delay_ms = 10.0f;
    job.cfg.chorus.mix = 0.50f;
    job.cfg.chorus.width = 1.0f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 6. Drive Warm: low, default, high
  {
    RenderJob job{};
    job.base_name = "drive_warm_low";
    job.desc = "Drive Warm subtle: drive 0.20, tone 0.70, mix 0.50";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Warm;
    job.cfg.drive.drive = 0.20f;
    job.cfg.drive.tone = 0.70f;
    job.cfg.drive.mix = 0.50f;
    job.cfg.drive.output_level = 0.98f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_warm_default";
    job.desc = "Drive Warm tuned default: drive 0.40, tone 0.65, mix 0.75";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Warm;
    job.cfg.drive.drive = 0.40f;
    job.cfg.drive.tone = 0.65f;
    job.cfg.drive.mix = 0.75f;
    job.cfg.drive.output_level = 0.95f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_warm_high";
    job.desc = "Drive Warm saturated: drive 0.75, tone 0.60, mix 0.85";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Warm;
    job.cfg.drive.drive = 0.75f;
    job.cfg.drive.tone = 0.60f;
    job.cfg.drive.mix = 0.85f;
    job.cfg.drive.output_level = 0.90f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 7. Drive Overdrive: low, default, high
  {
    RenderJob job{};
    job.base_name = "drive_overdrive_low";
    job.desc = "Drive Overdrive mild: drive 0.30, tone 0.60, mix 0.60";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Overdrive;
    job.cfg.drive.drive = 0.30f;
    job.cfg.drive.tone = 0.60f;
    job.cfg.drive.mix = 0.60f;
    job.cfg.drive.output_level = 0.95f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_overdrive_default";
    job.desc = "Drive Overdrive tuned default: drive 0.50, tone 0.55, mix 0.80";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Overdrive;
    job.cfg.drive.drive = 0.50f;
    job.cfg.drive.tone = 0.55f;
    job.cfg.drive.mix = 0.80f;
    job.cfg.drive.output_level = 0.90f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_overdrive_high";
    job.desc = "Drive Overdrive aggressive: drive 0.80, tone 0.50, mix 0.90";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Overdrive;
    job.cfg.drive.drive = 0.80f;
    job.cfg.drive.tone = 0.50f;
    job.cfg.drive.mix = 0.90f;
    job.cfg.drive.output_level = 0.82f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 8. Drive Megaphone: low, default, high
  {
    RenderJob job{};
    job.base_name = "drive_megaphone_low";
    job.desc = "Megaphone bandpass clean: drive 0.25, tone 0.70, mix 1.0";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Megaphone;
    job.cfg.drive.drive = 0.25f;
    job.cfg.drive.tone = 0.70f;
    job.cfg.drive.mix = 1.0f;
    job.cfg.drive.output_level = 0.95f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_megaphone_default";
    job.desc = "Megaphone vintage telephone default: drive 0.50, tone 0.70, mix 1.0";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Megaphone;
    job.cfg.drive.drive = 0.50f;
    job.cfg.drive.tone = 0.70f;
    job.cfg.drive.mix = 1.0f;
    job.cfg.drive.output_level = 0.95f;
    run_render(job, input, sample_rate, out_dir);
  }
  {
    RenderJob job{};
    job.base_name = "drive_megaphone_high";
    job.desc = "Megaphone clipped speaker: drive 0.80, tone 0.75, mix 1.0";
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Megaphone;
    job.cfg.drive.drive = 0.80f;
    job.cfg.drive.tone = 0.75f;
    job.cfg.drive.mix = 1.0f;
    job.cfg.drive.output_level = 0.92f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 9. Delay Sync default (95 BPM, 1/8 L, 1/8D R)
  {
    RenderJob job{};
    job.base_name = "delay_sync_default";
    job.desc = "Delay Sync 95 BPM: 1/8 (315.8ms) L, 1/8D (473.7ms) R";
    job.cfg.enable_delay = true;
    job.cfg.tempo_bpm = 95.0f;
    job.cfg.delay_sync_enabled = true;
    job.cfg.delay_left_subdivision = TempoSubdivision::Eighth;
    job.cfg.delay_right_subdivision = TempoSubdivision::DottedEighth;
    job.delay_wet = 0.35f;
    job.delay_feedback = 0.35f;
    run_render(job, input, sample_rate, out_dir);
  }

  // 10. Full New FX default
  {
    RenderJob job{};
    job.base_name = "full_fx_default";
    job.desc = "Full FX: Gate, Comp, 1V +4st PSOLA, Drive Warm, Chorus, Delay Sync, Reverb";
    job.cfg.enable_gate = true;
    job.cfg.enable_compressor = true;
    job.cfg.enable_delay = true;
    job.cfg.enable_reverb = true;
    job.cfg.enable_pitch_analysis = true;
    job.use_pitch_worker = true;
    job.harmony_interval = 4.0f;
    job.harmony_gain = 0.70f;
    job.cfg.tempo_bpm = 95.0f;
    job.cfg.delay_sync_enabled = true;
    job.cfg.delay_left_subdivision = TempoSubdivision::Eighth;
    job.cfg.delay_right_subdivision = TempoSubdivision::DottedEighth;
    job.delay_wet = 0.30f;
    job.delay_feedback = 0.30f;
    job.cfg.enable_chorus = true;
    job.cfg.chorus.mode = ChorusMode::Chorus;
    job.cfg.chorus.rate_hz = 0.75f;
    job.cfg.chorus.depth_ms = 1.6f;
    job.cfg.chorus.base_delay_ms = 12.0f;
    job.cfg.chorus.mix = 0.30f;
    job.cfg.enable_drive = true;
    job.cfg.drive.mode = DriveMode::Warm;
    job.cfg.drive.drive = 0.40f;
    job.cfg.drive.tone = 0.65f;
    job.cfg.drive.mix = 0.75f;
    job.cfg.drive.output_level = 0.95f;
    job.reverb_wet = 0.20f;
    run_render(job, input, sample_rate, out_dir);
  }

  std::printf("========================================================================================================\n");
  std::printf("  All 22 qualified render pairs (stereo + mono) written to %s\n", out_dir.c_str());
  std::printf("========================================================================================================\n");

  return 0;
}
