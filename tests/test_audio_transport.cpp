#include "audio_i2s.h"
#include "vocal_fx.h"
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace vocal_fx_platform;

void test_pcm24_float_conversion() {
  printf("Running test_pcm24_float_conversion...\n");

  // 1. Zero
  assert(std::fabs(AudioI2s::pcm24_to_float(0)) == 0.0f);

  // 2. Full scale positive (0x7FFFFF00)
  int32_t max_pos_24 = static_cast<int32_t>(0x7FFFFF00);
  float f_max_pos = AudioI2s::pcm24_to_float(max_pos_24);
  assert(f_max_pos > 0.9999f && f_max_pos < 1.0f);

  // 3. Full scale negative (0x80000000)
  int32_t max_neg_24 = static_cast<int32_t>(0x80000000);
  float f_max_neg = AudioI2s::pcm24_to_float(max_neg_24);
  assert(f_max_neg == -1.0f);

  // 4. Mid scale values
  int32_t half_pos = 0x40000000;
  assert(std::fabs(AudioI2s::pcm24_to_float(half_pos) - 0.5f) < 1e-6f);

  int32_t half_neg = static_cast<int32_t>(0xC0000000);
  assert(std::fabs(AudioI2s::pcm24_to_float(half_neg) - (-0.5f)) < 1e-6f);

  printf("  -> PASS\n");
}

void test_float_saturating_conversion() {
  printf("Running test_float_saturating_conversion...\n");

  // 1. Nominal values
  assert(AudioI2s::float_to_pcm32(0.0f) == 0);
  assert(AudioI2s::float_to_pcm32(0.5f) == 1073741824);
  assert(AudioI2s::float_to_pcm32(-0.5f) == -1073741824);

  // 2. Boundary saturation (must NOT overflow or flip sign!)
  assert(AudioI2s::float_to_pcm32(1.0f) == 2147483647);
  assert(AudioI2s::float_to_pcm32(2.5f) == 2147483647); // Saturated positive clamp
  assert(AudioI2s::float_to_pcm32(-1.0f) == -2147483648);
  assert(AudioI2s::float_to_pcm32(-3.0f) == -2147483648); // Saturated negative clamp

  // 3. 24-bit aligned saturation
  int32_t pcm24_pos = AudioI2s::float_to_pcm24(1.5f);
  assert(pcm24_pos == static_cast<int32_t>(0x7FFFFF00));
  assert((pcm24_pos & 0xFF) == 0); // Lower 8 bits masked

  int32_t pcm24_neg = AudioI2s::float_to_pcm24(-1.5f);
  assert(pcm24_neg == static_cast<int32_t>(0x80000000));
  assert((pcm24_neg & 0xFF) == 0);

  printf("  -> PASS\n");
}

void test_roundtrip_precision() {
  printf("Running test_roundtrip_precision...\n");

  // Float -> PCM32 -> Float roundtrip across [-1.0, 1.0]
  for (float v = -1.0f; v <= 1.0f; v += 0.05f) {
    int32_t pcm = AudioI2s::float_to_pcm32(v);
    float reconstructed = AudioI2s::pcm32_to_float(pcm);
    assert(std::fabs(v - reconstructed) < 1e-6f);
  }

  // Float -> PCM24 -> Float roundtrip (24-bit quantization error < 1e-6)
  for (float v = -0.99f; v <= 0.99f; v += 0.05f) {
    int32_t pcm24 = AudioI2s::float_to_pcm24(v);
    float reconstructed = AudioI2s::pcm24_to_float(pcm24);
    assert(std::fabs(v - reconstructed) < 1e-5f);
  }

  printf("  -> PASS\n");
}

void test_byte_swap_detection() {
  printf("Running test_byte_swap_detection...\n");

  constexpr size_t N = 1024;
  std::vector<int32_t> clean_sine(N);
  std::vector<int32_t> swapped_sine(N);

  for (size_t i = 0; i < N; ++i) {
    float s = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
    clean_sine[i] = AudioI2s::float_to_pcm32(s);

    // Byte swap (little-endian <-> big-endian)
    uint32_t val = static_cast<uint32_t>(clean_sine[i]);
    uint32_t swapped = ((val >> 24) & 0xFF) |
                       ((val >> 8) & 0xFF00) |
                       ((val << 8) & 0xFF0000) |
                       ((val << 24) & 0xFF000000U);
    swapped_sine[i] = static_cast<int32_t>(swapped);
  }

  // Clean sine should NOT be flagged as byte-swapped
  assert(!AudioI2s::detect_byte_swap(clean_sine.data(), N));

  // Swapped sine MUST be detected
  assert(AudioI2s::detect_byte_swap(swapped_sine.data(), N));

  printf("  -> PASS\n");
}

void test_channel_swap_detection() {
  printf("Running test_channel_swap_detection...\n");

  constexpr size_t N = 2400; // 50 ms at 48 kHz
  std::vector<float> left_1khz(N);
  std::vector<float> right_2khz(N);

  for (size_t i = 0; i < N; ++i) {
    left_1khz[i] = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
    right_2khz[i] = 0.5f * std::sin(2.0f * 3.14159265f * 2000.0f * i / 48000.0f);
  }

  // Direct configuration: Left = 1kHz, Right = 2kHz. Should NOT be flagged as swapped.
  bool swapped_direct = AudioI2s::detect_channel_swap(
      left_1khz.data(), right_2khz.data(), N, 1000.0f, 2000.0f, 48000.0f);
  assert(!swapped_direct);

  // Inverted configuration: Left = 2kHz, Right = 1kHz. MUST be flagged as swapped.
  bool swapped_inv = AudioI2s::detect_channel_swap(
      right_2khz.data(), left_1khz.data(), N, 1000.0f, 2000.0f, 48000.0f);
  assert(swapped_inv);

  printf("  -> PASS\n");
}

void test_dma_geometry() {
  printf("Running test_dma_geometry...\n");

  AudioI2sConfig cfg{};
  assert(cfg.sample_rate == 48000);
  assert(cfg.dma_frame_num == 64);
  assert(cfg.dma_desc_num >= 4 && cfg.dma_desc_num <= 8);

  // 64 frames at 48 kHz = 1.33333 ms deadline
  double block_ms = (cfg.dma_frame_num * 1000.0) / cfg.sample_rate;
  assert(std::fabs(block_ms - 1.333333) < 0.001);

  // Total DMA buffering capacity for 6 descriptors = 8.0 ms
  double total_buf_ms = (cfg.dma_desc_num * cfg.dma_frame_num * 1000.0) / cfg.sample_rate;
  assert(std::fabs(total_buf_ms - 8.0) < 0.001);

  printf("  -> PASS\n");
}

void test_clock_info_ratios() {
  printf("Running test_clock_info_ratios...\n");
  AudioClockInfo clk{};
  assert(clk.requested_fs == 48000);
  assert(clk.mclk_hz == 12288000);
  assert(clk.bclk_hz == 3072000);
  assert(clk.lrck_hz == 48000);
  assert(clk.mclk_fs_ratio == 256);
  assert(clk.bclk_fs_ratio == 64);
  printf("  -> PASS\n");
}

void test_synthetic_voiced_generator() {
  printf("Running test_synthetic_voiced_generator...\n");
  AudioI2s audio;
  AudioI2sConfig cfg{};
  cfg.mode = AudioI2sMode::SyntheticVoicedDsp;
  assert(audio.init(cfg, 64));

  float block[64];

  // 1. Initial 500 ms (24000 samples = 375 blocks of 64) MUST be digital silence
  for (int b = 0; b < 375; ++b) {
    audio.generate_synthetic_voiced_mono(block, 64);
    assert(!audio.is_current_synth_voiced());
    for (int i = 0; i < 64; ++i) {
      assert(block[i] == 0.0f);
    }
  }

  // 2. Block 376: First block of 110 Hz tone
  audio.generate_synthetic_voiced_mono(block, 64);
  assert(audio.is_current_synth_voiced());
  assert(std::fabs(audio.current_synth_freq_hz() - 110.0f) < 0.1f);

  // Attack ramp: block 376 is during the first 480 samples, so samples should be small and rising
  float max_sample = 0.0f;
  for (int i = 0; i < 64; ++i) {
    float abs_val = std::fabs(block[i]);
    if (abs_val > max_sample) max_sample = abs_val;
  }
  assert(max_sample > 0.0f);
  assert(max_sample <= 0.13f); // <= -18 dBFS

  // 3. Advance to sustain portion (sample 24000 + 1000 = block 375 + 16 = block 391)
  for (int b = 0; b < 15; ++b) {
    audio.generate_synthetic_voiced_mono(block, 64);
  }
  // Check peak across 8 blocks (~512 samples > 1 full period of 110 Hz = 436 samples)
  float sustain_max = 0.0f;
  for (int b = 0; b < 8; ++b) {
    audio.generate_synthetic_voiced_mono(block, 64);
    assert(audio.is_current_synth_voiced());
    for (int i = 0; i < 64; ++i) {
      float abs_val = std::fabs(block[i]);
      if (abs_val > sustain_max) sustain_max = abs_val;
    }
  }
  assert(sustain_max > 0.08f && sustain_max <= 0.13f);

  // 4. Advance through remaining 72,000 samples of tone into silence
  // Total tone blocks = 72000 / 64 = 1125 blocks.
  // We already read 1 + 15 + 8 = 24 blocks. Remaining in tone = 1125 - 24 = 1101 blocks.
  for (int b = 0; b < 1101; ++b) {
    audio.generate_synthetic_voiced_mono(block, 64);
  }

  // Now we should be in 100 ms silence (4800 samples = 75 blocks)
  audio.generate_synthetic_voiced_mono(block, 64);
  assert(!audio.is_current_synth_voiced());
  for (int i = 0; i < 64; ++i) {
    assert(block[i] == 0.0f);
  }

  // 5. Advance through silence (74 remaining blocks) into next tone (147 Hz)
  for (int b = 0; b < 74; ++b) {
    audio.generate_synthetic_voiced_mono(block, 64);
  }
  // Next block is 147 Hz tone
  audio.generate_synthetic_voiced_mono(block, 64);
  assert(audio.is_current_synth_voiced());
  assert(std::fabs(audio.current_synth_freq_hz() - 147.0f) < 0.1f);

  printf("  -> PASS\n");
}

void test_synthetic_voiced_dsp_pipeline() {
  printf("Running test_synthetic_voiced_dsp_pipeline...\n");
  AudioI2s audio;
  VocalFxConfig cfg{};
  cfg.enable_gate = true;
  cfg.enable_compressor = true;
  cfg.enable_delay = true;
  cfg.enable_reverb = true;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 4.0f;
  cfg.pitch_shift.wet = 1.0f;
  cfg.align_dry_to_harmony = true;
  cfg.enable_harmony_limiter = true;
  cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
  cfg.mute_dry = false;

  assert(vocal_fx_init(cfg));
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_harmony_gain(1, 1.0f);

  float mono[64], l[64], r[64];
  for (int block = 0; block < 5000; ++block) {
    audio.generate_synthetic_voiced_mono(mono, 64);
    vocal_fx_process(mono, l, r, 64);
    vocal_fx_run_pitch_analysis(8);

    if (block == 370 || block == 500 || block == 1000 || block == 1500 || block == 2000 || block == 3000 || block == 4000) {
      auto dbg = vocal_fx_harmony_debug(0);
      auto pr = vocal_fx_latest_pitch();
      auto h0 = vocal_fx_harmony_telemetry(0);
      printf("Block %d: inj_f0=%.1f inj_v=%d det_f0=%.1f det_v=%d conf=%.2f track=%d usable=%d st=%d grains=%llu pm_und=%llu ah_und=%llu\n",
             block, audio.current_synth_freq_hz(), audio.is_current_synth_voiced(),
             pr.frequency_hz, pr.voiced, pr.confidence,
             dbg.pitch_tracker_state, dbg.usable, (int)h0.state,
             (unsigned long long)h0.grains,
             (unsigned long long)h0.pitch_mark_underflows,
             (unsigned long long)h0.audio_history_underflows);
    }
  }

  auto h0 = vocal_fx_harmony_telemetry(0);
  auto h1 = vocal_fx_harmony_telemetry(1);
  printf("H0 grains: %llu, H1 grains: %llu, H0 state: %d, invalid_mark: %llu, invalid_pitch: %llu\n",
         (unsigned long long)h0.grains, (unsigned long long)h1.grains, (int)h0.state,
         (unsigned long long)h0.invalid_mark, (unsigned long long)h0.invalid_pitch);
  assert(h0.grains > 0);
  assert(h1.grains > 0);
  printf("  -> PASS\n");
}

void test_synthetic_voiced_threaded() {
  printf("Running test_synthetic_voiced_threaded...\n");
  AudioI2s audio;
  VocalFxConfig cfg{};
  cfg.enable_gate = true;
  cfg.enable_compressor = true;
  cfg.enable_delay = true;
  cfg.enable_reverb = true;
  cfg.enable_pitch_analysis = true;
  cfg.pitch_shift.enabled = true;
  cfg.pitch_shift.semitones = 4.0f;
  cfg.pitch_shift.wet = 1.0f;
  cfg.align_dry_to_harmony = true;
  cfg.enable_harmony_limiter = true;
  cfg.spatial_routing = SpatialFxRouting::DelayIntoReverb;
  cfg.mute_dry = false;

  assert(vocal_fx_init(cfg));
  vocal_fx_set_harmony_enabled(0, true);
  vocal_fx_set_harmony_interval(0, 4.0f);
  vocal_fx_set_harmony_gain(0, 1.0f);
  vocal_fx_set_formant_mode(0, FormantMode::Lpc);
  vocal_fx_set_harmony_enabled(1, true);
  vocal_fx_set_harmony_interval(1, 7.0f);
  vocal_fx_set_harmony_gain(1, 1.0f);
  vocal_fx_set_formant_mode(1, FormantMode::Lpc);

  std::atomic<bool> running{true};
  std::atomic<uint64_t> total_hops_completed{0};
  std::atomic<uint64_t> total_calls{0};
  std::thread pitch_th([&]() {
    while (running.load(std::memory_order_relaxed)) {
      size_t count = vocal_fx_run_pitch_analysis(4);
      if (count == 0) {
        std::this_thread::yield();
      } else {
        total_hops_completed.fetch_add(count, std::memory_order_relaxed);
      }
      total_calls.fetch_add(1, std::memory_order_relaxed);
    }
  });

  float mono[64], l[64], r[64];
  const auto t0 = std::chrono::steady_clock::now();
  for (int block = 0; block < 2000; ++block) {
    audio.generate_synthetic_voiced_mono(mono, 64);
    vocal_fx_process(mono, l, r, 64);

    // Pace at real-time: 1333.3 us per 64-sample block
    const auto target_time = t0 + std::chrono::microseconds(static_cast<int64_t>(block * 1333.333));
    while (std::chrono::steady_clock::now() < target_time) {
      std::this_thread::yield();
    }
    if (block == 500 || block == 1000 || block == 2000) {
      auto pr = vocal_fx_latest_pitch();
      auto h0 = vocal_fx_harmony_telemetry(0);
      auto dbg = vocal_fx_harmony_debug(0);
      printf("Threaded Block %d: inj=%.1f (v=%d) det=%.1f (v=%d, conf=%.2f, track=%d) st=%d grains=%llu (calls=%llu hops=%llu)\n",
             block, audio.current_synth_freq_hz(), audio.is_current_synth_voiced(),
             pr.frequency_hz, pr.voiced, pr.confidence, dbg.pitch_tracker_state,
             (int)h0.state, (unsigned long long)h0.grains,
             (unsigned long long)total_calls.load(), (unsigned long long)total_hops_completed.load());
      printf("   dbg: usable=%d, f_det_m=%llu, f_win_safe=%llu, f_sched=%llu, sel_src=%llu, age=%llu\n",
             dbg.usable, (unsigned long long)dbg.first_detected_mark,
             (unsigned long long)dbg.first_window_safe_mark,
             (unsigned long long)dbg.first_scheduled_mark,
             (unsigned long long)dbg.selected_source_mark,
             (unsigned long long)dbg.analysis_age);
      printf("   telem: pm_und=%llu, ah_und=%llu, resyncs=%llu, inv_p=%llu, inv_m=%llu, max_grn_ex=%llu\n",
             (unsigned long long)h0.pitch_mark_underflows,
             (unsigned long long)h0.audio_history_underflows,
             (unsigned long long)h0.psola_resyncs,
             (unsigned long long)h0.invalid_pitch,
             (unsigned long long)h0.invalid_mark,
             (unsigned long long)h0.max_grains_exceeded);
    }
  }
  running.store(false);
  pitch_th.join();

  auto h0 = vocal_fx_harmony_telemetry(0);
  auto h1 = vocal_fx_harmony_telemetry(1);
  printf("Threaded Result: H0 grains: %llu, H1 grains: %llu, H0 state: %d (total hops=%llu)\n",
         (unsigned long long)h0.grains, (unsigned long long)h1.grains, (int)h0.state,
         (unsigned long long)total_hops_completed.load());
  printf("Final telem: pm_und=%llu, ah_und=%llu, resyncs=%llu, inv_p=%llu, inv_m=%llu, max_grn_ex=%llu\n",
         (unsigned long long)h0.pitch_mark_underflows,
         (unsigned long long)h0.audio_history_underflows,
         (unsigned long long)h0.psola_resyncs,
         (unsigned long long)h0.invalid_pitch,
         (unsigned long long)h0.invalid_mark,
         (unsigned long long)h0.max_grains_exceeded);
  assert(h0.grains > 0);
  assert(h1.grains > 0);
  printf("  -> PASS\n");
}

int main() {
  printf("=== Audio Transport & Format Integrity Unit Tests ===\n");
  test_pcm24_float_conversion();
  test_float_saturating_conversion();
  test_roundtrip_precision();
  test_byte_swap_detection();
  test_channel_swap_detection();
  test_dma_geometry();
  test_clock_info_ratios();
  test_synthetic_voiced_generator();
  test_synthetic_voiced_dsp_pipeline();
  test_synthetic_voiced_threaded();
  printf("=== ALL AUDIO TRANSPORT TESTS PASSED ===\n");
  return 0;
}


