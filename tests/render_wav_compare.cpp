#include "vocal_fx.h"
#include "vocal_fx_types.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

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

bool load_wav_mono(const std::string &path, std::vector<float> &mono_out, uint32_t &sample_rate) {
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

struct RenderOutput {
  std::vector<float> left;
  std::vector<float> right;
  double elapsed_ms = 0.0;
};

RenderOutput render_file(const std::vector<float> &input,
                         PsolaLpcKernel lpc_k, PsolaOlaKernel ola_k,
                         PsolaGrainKernel grain_k, PsolaSynthesisKernel synth_k) {
  VocalFxConfig cfg{};
  cfg.enable_gate = false;
  cfg.enable_compressor = false;
  cfg.enable_delay = false;
  cfg.enable_reverb = false;
  cfg.enable_pitch_analysis = true;
  cfg.isolate_pitch_shift_output = false;
  cfg.mute_dry = true;

  // Pitch shift & Formants default configuration
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.formant_mode = FormantMode::Lpc;
  cfg.pitch_shift.formant_amount = 1.0f;
  cfg.pitch_shift.formant_shift_semitones = 0.0f;
  cfg.pitch_shift.formant_bandwidth_expansion = 0.985f;
  cfg.pitch_shift.lpc_kernel = lpc_k;
  cfg.pitch_shift.ola_kernel = ola_k;
  cfg.pitch_shift.grain_kernel = grain_k;
  cfg.pitch_shift.synthesis_kernel = synth_k;

  if (!vocal_fx_init(cfg)) {
    std::fprintf(stderr, "vocal_fx_init failed\n");
    std::exit(1);
  }

  // Voice 0: +7 semitones (fifth up)
  vocal_fx_set_harmony_mode(HarmonyMode::FixedInterval);
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 7.0f);
  vocal_fx_set_harmony_gain(0, 0.8f);
  vocal_fx_set_harmony_pan(0, -0.3f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_formant_amount(0, 1.0f);

  // Voice 1: -5 semitones (fourth down)
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, -5.0f);
  vocal_fx_set_harmony_gain(1, 0.8f);
  vocal_fx_set_harmony_pan(1, 0.3f);
  vocal_fx_set_formant_mode(1, FormantMode::Lpc);
  vocal_fx_set_formant_amount(1, 1.0f);

  const size_t total_frames = input.size();
  constexpr size_t kBlock = 64;
  std::vector<float> out_l(total_frames, 0.0f);
  std::vector<float> out_r(total_frames, 0.0f);

  auto t_start = std::chrono::steady_clock::now();

  for (size_t pos = 0; pos < total_frames; pos += kBlock) {
    const size_t n = std::min(kBlock, total_frames - pos);
    vocal_fx_process(input.data() + pos, out_l.data() + pos, out_r.data() + pos, n);
    while (vocal_fx_run_pitch_analysis(8)) {
    }
  }

  auto t_end = std::chrono::steady_clock::now();
  double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

  return {std::move(out_l), std::move(out_r), elapsed_ms};
}

int main(int argc, char **argv) {
  std::string input_path = "samples/dry-acapella-leave-this-place_95bpm.wav";
  if (argc > 1) {
    input_path = argv[1];
  }

  std::printf("=== Stage B4C.2 Mandatory Audio Render Test ===\n");
  std::printf("Input file: %s\n", input_path.c_str());

  std::vector<float> mono_in;
  uint32_t sample_rate = 0;
  if (!load_wav_mono(input_path, mono_in, sample_rate)) {
    std::fprintf(stderr, "FAILED to load input WAV: %s\n", input_path.c_str());
    return 1;
  }
  std::printf("Loaded %zu frames @ %u Hz (%.2f seconds)\n",
              mono_in.size(), sample_rate, static_cast<double>(mono_in.size()) / sample_rate);

  // Limit to first 10 seconds for fast thorough regression if file is long
  const size_t max_test_frames = std::min<size_t>(mono_in.size(), 48000 * 10);
  mono_in.resize(max_test_frames);

  // 1. Render Reference
  std::printf("Rendering Golden Reference (PSOLA_LPC_REFERENCE, Formant ON, 2 voices: +7st, -5st)...\n");
  auto ref = render_file(mono_in,
                         PsolaLpcKernel::Reference, PsolaOlaKernel::Reference,
                         PsolaGrainKernel::Reference, PsolaSynthesisKernel::Reference);
  std::printf("Reference rendered in %.1f ms\n", ref.elapsed_ms);

  // 2. Render Winner Candidate (Candidate E8 CombinedMulti8 + UnrolledExact)
  std::printf("Rendering Winner Candidate (COMBINED_MULTI8 + UNROLLED_EXACT)...\n");
  auto winner = render_file(mono_in,
                            PsolaLpcKernel::ContiguousMulti8, PsolaOlaKernel::Contiguous,
                            PsolaGrainKernel::ContiguousMulti8, PsolaSynthesisKernel::UnrolledExact);
  std::printf("Winner rendered in %.1f ms (%.2fx speedup on host)\n",
              winner.elapsed_ms, ref.elapsed_ms / winner.elapsed_ms);

  // 3. Compute Audio Differences
  double max_diff_l = 0.0, max_diff_r = 0.0;
  double err_energy_l = 0.0, sig_energy_l = 0.0;
  double err_energy_r = 0.0, sig_energy_r = 0.0;
  bool bit_identical_l = true, bit_identical_r = true;

  for (size_t i = 0; i < max_test_frames; ++i) {
    if (ref.left[i] != winner.left[i]) bit_identical_l = false;
    if (ref.right[i] != winner.right[i]) bit_identical_r = false;

    const double dl = std::fabs(static_cast<double>(winner.left[i]) - static_cast<double>(ref.left[i]));
    const double dr = std::fabs(static_cast<double>(winner.right[i]) - static_cast<double>(ref.right[i]));

    if (dl > max_diff_l) max_diff_l = dl;
    if (dr > max_diff_r) max_diff_r = dr;

    err_energy_l += dl * dl;
    sig_energy_l += static_cast<double>(ref.left[i]) * static_cast<double>(ref.left[i]);

    err_energy_r += dr * dr;
    sig_energy_r += static_cast<double>(ref.right[i]) * static_cast<double>(ref.right[i]);
  }

  double snr_l = (err_energy_l > 1e-20) ? (10.0 * std::log10((sig_energy_l + 1e-20) / err_energy_l)) : 999.0;
  double snr_r = (err_energy_r > 1e-20) ? (10.0 * std::log10((sig_energy_r + 1e-20) / err_energy_r)) : 999.0;

  std::printf("\n--- Audio Quality Render Test Results ---\n");
  std::printf("Left channel : bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              bit_identical_l, max_diff_l, snr_l);
  std::printf("Right channel: bit_identical=%d, max_diff=%.2e, SNR=%.1f dB\n",
              bit_identical_r, max_diff_r, snr_r);

  // Write output wav for inspection
  const std::string out_path = "b4c2_rendered_winner.wav";
  if (write_wav_float32(out_path, winner.left.data(), winner.right.data(), max_test_frames, sample_rate)) {
    std::printf("Written output WAV: %s\n", out_path.c_str());
  }

  if (!bit_identical_l && snr_l < 120.0) {
    std::fprintf(stderr, "FAIL: Left channel SNR %.1f dB < 120 dB threshold!\n", snr_l);
    return 1;
  }
  if (!bit_identical_r && snr_r < 120.0) {
    std::fprintf(stderr, "FAIL: Right channel SNR %.1f dB < 120 dB threshold!\n", snr_r);
    return 1;
  }

  std::printf("\n>>> MANDATORY AUDIO RENDER TEST: PASS <<<\n");
  return 0;
}
