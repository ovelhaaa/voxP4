#pragma once

#include "boards/wt9932p4_tiny_audio.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vocal_fx_platform {

enum class PcmWidth : uint8_t {
  Bits16 = 16,
  Bits24 = 24,
  Bits32 = 32,
};

enum class AudioI2sMode : uint8_t {
  FullDsp = 0,            // Stage B4A / B4C: ADC -> I2S RX -> Float -> VoxP4 DSP -> Float -> I2S TX -> DAC
  TxBringUp = 1,          // Stage B1: Synthetic tones -> I2S TX -> DAC (ADC disconnected)
  RxBringUp = 2,          // Stage B2: ADC -> I2S RX -> telemetry/diagnostics (DAC idle)
  Bypass = 3,             // Stage B3: ADC -> I2S RX -> PCM24 to float -> Float to PCM32 -> I2S TX -> DAC (NO DSP)
  LatencyPulse = 4,       // Latency test: Sharp impulse generation and roundtrip/echo timing
  SampleForensic = 5,     // Captures first 256 raw words from RX DMA for bit alignment audit
  SyntheticVoicedDsp = 6, // Stage B4B: Deterministic synthetic voiced harmonic tone -> Full DSP -> DAC (100% DMA active)
};

enum class TxSignalType : uint8_t {
  Silence = 0,
  Sine100Hz = 1,
  Sine1kHz = 2,
  Sine10kHz = 3,
  LeftOnly1kHz = 4,
  RightOnly1kHz = 5,
  AlternatingLR = 6,
  ChannelIntegrity = 7,    // Left 1 kHz, Right 2 kHz
  ChannelIntegrityInv = 8, // Left 2 kHz, Right 1 kHz
};

struct AudioI2sConfig {
  uint32_t sample_rate = 48000;
  PcmWidth width = PcmWidth::Bits32;
  bool stereo_input = true;
  int mclk_pin = VOXP4_I2S_MCLK_GPIO;
  int bclk_pin = VOXP4_I2S_BCLK_GPIO;
  int ws_pin = VOXP4_I2S_WS_GPIO;
  int dout_pin = VOXP4_I2S_DOUT_GPIO;
  int din_pin = VOXP4_I2S_DIN_GPIO;

  // DMA buffer configuration
  uint32_t dma_desc_num = 6;   // 4–8 descriptors
  uint32_t dma_frame_num = 64; // 64 frames matching DSP block size

  AudioI2sMode mode = AudioI2sMode::FullDsp;
  TxSignalType tx_signal = TxSignalType::Sine1kHz;
  float tx_level_dbfs = -18.0f; // -18, -12, -6 dBFS

  // Latency test trigger GPIO (-1 if disabled)
  int latency_trigger_gpio = -1;
};

// Permanent lock-free DMA integrity counters (Section 18)
struct AudioTransportCounters {
  std::atomic<uint64_t> rx_dma_events{0};
  std::atomic<uint64_t> tx_dma_events{0};
  std::atomic<uint64_t> rx_overruns{0};
  std::atomic<uint64_t> tx_underruns{0};
  std::atomic<uint64_t> rx_dropped_frames{0};
  std::atomic<uint64_t> tx_dropped_frames{0};
  std::atomic<uint64_t> dma_errors{0};
  std::atomic<uint64_t> audio_blocks_processed{0};
  std::atomic<uint64_t> audio_deadline_misses{0};
  std::atomic<uint64_t> dsp_deadline_misses{0};
  std::atomic<uint64_t> transport_deadline_misses{0};
  std::atomic<uint64_t> nan_inf_count{0};
  std::atomic<uint64_t> max_rx_callback_gap_us{0};
  std::atomic<uint64_t> max_tx_callback_gap_us{0};
  std::atomic<uint64_t> max_audio_task_wake_latency_us{0};

  void reset() {
    rx_dma_events.store(0, std::memory_order_relaxed);
    tx_dma_events.store(0, std::memory_order_relaxed);
    rx_overruns.store(0, std::memory_order_relaxed);
    tx_underruns.store(0, std::memory_order_relaxed);
    rx_dropped_frames.store(0, std::memory_order_relaxed);
    tx_dropped_frames.store(0, std::memory_order_relaxed);
    dma_errors.store(0, std::memory_order_relaxed);
    audio_blocks_processed.store(0, std::memory_order_relaxed);
    audio_deadline_misses.store(0, std::memory_order_relaxed);
    dsp_deadline_misses.store(0, std::memory_order_relaxed);
    transport_deadline_misses.store(0, std::memory_order_relaxed);
    nan_inf_count.store(0, std::memory_order_relaxed);
    max_rx_callback_gap_us.store(0, std::memory_order_relaxed);
    max_tx_callback_gap_us.store(0, std::memory_order_relaxed);
    max_audio_task_wake_latency_us.store(0, std::memory_order_relaxed);
  }
};

// Forensic outlier event record (Section 21)
struct OutlierRecord {
  uint64_t timestamp_us;
  uint64_t block_index;
  uint64_t pure_dsp_us;
  uint64_t block_cycle_us;
  uint64_t wake_latency_us;
  uint64_t rx_gap_us;
  const char *category;
};

constexpr size_t kMaxOutlierRecords = 128;

// Percentile and latency statistics
struct TimingPercentiles {
  double avg_us = 0.0;
  uint64_t p95_us = 0;
  uint64_t p99_us = 0;
  uint64_t p99_bucket_upper_bound_us = 0;
  uint64_t p99_9_us = 0;
  uint64_t max_us = 0;
  uint64_t samples = 0;
};

// Stimulus state tracking for Stage B4B
enum class StimulusState : uint8_t {
  Silence = 0,
  Attack = 1,
  Tone = 2,
  Release = 3,
};

// Transport error timestamp event record (Req 3)
struct TransportErrorEvent {
  uint64_t timestamp_us = 0;
  uint64_t block_index = 0;
  uint64_t dsp_duration_us = 0;
  uint64_t rx_gap_us = 0;
  uint64_t tx_gap_us = 0;
  uint64_t wake_interval_us = 0;
  const char *type = nullptr;
};

constexpr size_t kMaxTransportErrorEvents = 32;

// ADC Diagnostics structure for Stage B2
struct AdcDiagnostics {
  float peak_l = 0.0f;
  float peak_r = 0.0f;
  float rms_l = 0.0f;
  float rms_r = 0.0f;
  float dc_offset_l = 0.0f;
  float dc_offset_r = 0.0f;
  int32_t min_raw_l = 0;
  int32_t max_raw_l = 0;
  int32_t min_raw_r = 0;
  int32_t max_raw_r = 0;
  uint64_t zero_samples = 0;

  uint64_t clipped_samples = 0;
};

// Sample forensic capture buffer (Section 24)
constexpr size_t kForensicWordCount = 256;
struct SampleForensicData {
  int32_t raw_words[kForensicWordCount];
  size_t captured_words = 0;
  bool captured = false;
};

// Audio Clock & PLL Verification structure
struct AudioClockInfo {
  uint32_t requested_fs = 48000;
  const char *clock_source_name = "SOC_MOD_CLK_XTAL";
  uint32_t source_clock_hz = 40000000;
  uint32_t mclk_hz = 12288000;
  uint32_t bclk_hz = 3072000;
  uint32_t lrck_hz = 48000;
  uint32_t mclk_fs_ratio = 256;
  uint32_t bclk_fs_ratio = 64;
  bool apll_fallback_occurred = false;
};

class AudioI2s {
public:
  AudioI2s();
  ~AudioI2s();

  bool init(const AudioI2sConfig &cfg, size_t block_size);
  void deinit();
  void run();
  void stop();

  // Mode & Signal controls
  void set_mode(AudioI2sMode mode);
  void set_tx_signal(TxSignalType sig, float dbfs);

  // Counters & Timing
  AudioTransportCounters &counters() { return counters_; }
  const AudioTransportCounters &counters() const { return counters_; }

  void reset_counters() { counters_.reset(); }
  TimingPercentiles calculate_dsp_percentiles() const;
  TimingPercentiles calculate_cycle_percentiles() const;
  TimingPercentiles calculate_wake_percentiles() const;

  // Synthetic Voiced Source (Stage B4B)
  void generate_synthetic_voiced_mono(float *mono, size_t frames);
  float current_synth_freq_hz() const { return current_synth_f0_; }
  bool is_current_synth_voiced() const { return current_synth_voiced_; }
  StimulusState current_stimulus_state() const { return current_stimulus_state_; }
  float current_stimulus_rms() const { return current_stimulus_rms_; }
  uint32_t current_block_checksum() const { return current_block_checksum_; }
  typedef void (*SyntheticBlockHook)(uint64_t sample_in_cycle, float f0, void *user_data);
  void set_synthetic_block_hook(SyntheticBlockHook hook, void *user_data) {
    synth_hook_ = hook;
    synth_hook_user_data_ = user_data;
  }

  // Transport Error Event Logging (Req 3)
  void record_transport_error(const char *type, uint64_t block_idx, uint64_t dsp_us, uint64_t rx_gap, uint64_t tx_gap, uint64_t wake_us);
  size_t transport_error_count() const { return transport_error_count_; }
  const TransportErrorEvent &transport_error(size_t index) const { return transport_errors_[index % kMaxTransportErrorEvents]; }
  void print_transport_errors() const;

  // Output Sanity & Level Stats
  float output_peak_l() const { return out_peak_l_; }
  float output_peak_r() const { return out_peak_r_; }
  float output_rms_l() const { return out_rms_l_; }
  float output_rms_r() const { return out_rms_r_; }

  // Diagnostics & Info
  const AudioI2sConfig &config() const { return config_; }
  const AudioClockInfo &clock_info() const { return clock_info_; }
  const AdcDiagnostics &adc_diagnostics() const { return adc_diag_; }
  const SampleForensicData &forensic_data() const { return forensic_data_; }

  size_t outlier_count() const { return outlier_count_; }
  const OutlierRecord *outliers() const { return outliers_; }

  void print_hardware_config() const;
  void print_telemetry_summary() const;
  void print_forensic_outliers() const;
  void dump_sample_forensic() const;

  // Sample format conversions (Section 7)
  static inline float pcm32_to_float(int32_t v) {
    return static_cast<float>(v) / 2147483648.0f;
  }

  static inline int32_t float_to_pcm32(float v) {
    // Saturating conversion to 32-bit signed integer (prevents polarity flip on overflow)
    if (v >= 1.0f) return 2147483647;
    if (v <= -1.0f) return -2147483648;
    return static_cast<int32_t>(v * 2147483648.0f);
  }

  static inline float pcm24_to_float(int32_t left_aligned_32bit_word) {
    // PCM1808 outputs 24-bit audio MSB-first in Philips I2S standard mode.
    // In a 32-bit slot, bits [31:8] contain the 24-bit sample with bit 31 as sign bit.
    // Bits [7:0] contain trailing zeros/noise.
    // The 32-bit container is already sign-extended by virtue of bit 31 being the sign bit.
    return static_cast<float>(left_aligned_32bit_word) / 2147483648.0f;
  }

  static inline int32_t float_to_pcm24(float v) {
    return float_to_pcm32(v) & static_cast<int32_t>(0xFFFFFF00);
  }

  static inline float pcm16_to_float(int16_t v) {
    return static_cast<float>(v) / 32768.0f;
  }

  static inline int16_t float_to_pcm16(float v) {
    v = std::clamp(v, -1.0f, 0.9999695f);
    return static_cast<int16_t>(v * 32768.0f);
  }

  // Diagnostic helper functions for format & alignment testing (Section 7)
  static bool detect_bit_shift(const int32_t *raw_samples, size_t count, int *detected_shift);
  static bool detect_byte_swap(const int32_t *raw_samples, size_t count);
  static bool detect_channel_swap(const float *l, const float *r, size_t count,
                                 float expected_freq_l, float expected_freq_r, float sample_rate);

private:
  AudioI2sConfig config_{};
  AudioClockInfo clock_info_{};
  size_t block_size_ = 64;

  std::atomic<bool> running_{false};
  AudioTransportCounters counters_{};

  // Diagnostics & Forensics
  AdcDiagnostics adc_diag_{};
  SampleForensicData forensic_data_{};
  OutlierRecord outliers_[kMaxOutlierRecords]{};
  size_t outlier_count_ = 0;

  // Timing histograms (50 bins of 50 us: 0..2500 us)
  static constexpr size_t kHistBins = 50;
  static constexpr uint64_t kBinWidthUs = 50;
  uint32_t dsp_hist_[kHistBins]{};
  uint32_t cycle_hist_[kHistBins]{};
  uint32_t wake_hist_[kHistBins]{};
  uint64_t dsp_total_us_ = 0;
  uint64_t cycle_total_us_ = 0;
  uint64_t wake_total_us_ = 0;
  uint64_t dsp_worst_us_ = 0;
  uint64_t cycle_worst_us_ = 0;
  uint64_t wake_worst_us_ = 0;

  // Synthetic voiced generator state (Stage B4B)
  uint64_t synth_sample_idx_ = 0;
  float synth_phase_ = 0.0f;
  float current_synth_f0_ = 110.0f;
  bool current_synth_voiced_ = false;
  StimulusState current_stimulus_state_ = StimulusState::Silence;
  float current_stimulus_rms_ = 0.0f;
  uint32_t current_block_checksum_ = 0;
  uint64_t last_sample_in_cycle_ = 0;
  SyntheticBlockHook synth_hook_ = nullptr;
  void *synth_hook_user_data_ = nullptr;

  // Transport error event ring buffer (Req 3)
  TransportErrorEvent transport_errors_[kMaxTransportErrorEvents]{};
  size_t transport_error_count_ = 0;

  // Output level tracking
  float out_peak_l_ = 0.0f;
  float out_peak_r_ = 0.0f;
  float out_rms_l_ = 0.0f;
  float out_rms_r_ = 0.0f;

  // Internal signal generator state
  float sine_phase_l_ = 0.0f;
  float sine_phase_r_ = 0.0f;
  float ramp_gain_ = 0.0f; // Soft fade-in ramp

  void record_timing(uint64_t dsp_us, uint64_t cycle_us, uint64_t wake_us, uint64_t rx_gap_us, uint64_t block_idx);
  void generate_tx_tones(float *out_l, float *out_r, size_t frames);
  void update_adc_diagnostics(const float *l, const float *r, const int32_t *raw, size_t frames);

#ifdef ESP_PLATFORM
  void *tx_chan_ = nullptr;
  void *rx_chan_ = nullptr;
  bool tx_enabled_ = false;
  bool rx_enabled_ = false;
#endif
};

} // namespace vocal_fx_platform
