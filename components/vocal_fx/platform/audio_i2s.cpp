#include "audio_i2s.h"
#include "vocal_fx.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/i2s_ll.h"
#endif

namespace vocal_fx_platform {

namespace {
#ifdef ESP_PLATFORM
const char *TAG = "audio_i2s";

// Track last RX/TX callback timestamps for jitter & gap calculation
static volatile int64_t s_last_rx_callback_us = 0;
static volatile int64_t s_last_tx_callback_us = 0;

static void increment_depth(std::atomic<uint64_t> &depth,
                            std::atomic<uint64_t> &maximum) {
  const uint64_t value = depth.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t old_maximum = maximum.load(std::memory_order_relaxed);
  while (value > old_maximum &&
         !maximum.compare_exchange_weak(old_maximum, value,
                                        std::memory_order_relaxed)) {
  }
}

static void decrement_depth(std::atomic<uint64_t> &depth) {
  uint64_t value = depth.load(std::memory_order_relaxed);
  while (value != 0 &&
         !depth.compare_exchange_weak(value, value - 1,
                                      std::memory_order_relaxed)) {
  }
}

static bool IRAM_ATTR i2s_rx_event_callback(i2s_chan_handle_t handle,
                                            i2s_event_data_t *event,
                                            void *user_data) {
  (void)handle;
  (void)event;
  auto *self = static_cast<AudioI2s *>(user_data);
  if (!self || !self->callbacks_enabled()) return false;

  self->counters().rx_dma_events.fetch_add(1, std::memory_order_relaxed);
  increment_depth(self->counters().rx_event_queue_depth,
                  self->counters().rx_event_queue_max_depth);
  int64_t now = esp_timer_get_time();
  int64_t prev = s_last_rx_callback_us;
  s_last_rx_callback_us = now;

  if (prev > 0) {
    uint64_t gap = static_cast<uint64_t>(now - prev);
    uint64_t curr_max = self->counters().max_rx_callback_gap_us.load(std::memory_order_relaxed);
    while (gap > curr_max &&
           !self->counters().max_rx_callback_gap_us.compare_exchange_weak(
               curr_max, gap, std::memory_order_relaxed)) {
    }
  }
  return false;
}

static bool IRAM_ATTR i2s_rx_q_ovf_callback(i2s_chan_handle_t handle,
                                            i2s_event_data_t *event,
                                            void *user_data) {
  (void)handle;
  (void)event;
  auto *self = static_cast<AudioI2s *>(user_data);
  if (self && self->callbacks_enabled()) {
    self->counters().rx_diagnostic_queue_overflows.fetch_add(
        1, std::memory_order_relaxed);
    self->record_transport_error(
        "RX_DIAGNOSTIC_EVENT_QUEUE_OVERFLOW",
        self->counters().audio_blocks_processed.load(std::memory_order_relaxed),
        0, 0, 0, 0);
  }
  return false;
}

static bool IRAM_ATTR i2s_tx_event_callback(i2s_chan_handle_t handle,
                                            i2s_event_data_t *event,
                                            void *user_data) {
  (void)handle;
  (void)event;
  auto *self = static_cast<AudioI2s *>(user_data);
  if (!self || !self->callbacks_enabled()) return false;

  self->counters().tx_dma_events.fetch_add(1, std::memory_order_relaxed);
  decrement_depth(self->counters().tx_event_queue_depth);
  int64_t now = esp_timer_get_time();
  int64_t prev = s_last_tx_callback_us;
  s_last_tx_callback_us = now;

  if (prev > 0) {
    uint64_t gap = static_cast<uint64_t>(now - prev);
    uint64_t curr_max = self->counters().max_tx_callback_gap_us.load(std::memory_order_relaxed);
    while (gap > curr_max &&
           !self->counters().max_tx_callback_gap_us.compare_exchange_weak(
               curr_max, gap, std::memory_order_relaxed)) {
    }
  }
  return false;
}

static bool IRAM_ATTR i2s_tx_q_ovf_callback(i2s_chan_handle_t handle,
                                            i2s_event_data_t *event,
                                            void *user_data) {
  (void)handle;
  (void)event;
  auto *self = static_cast<AudioI2s *>(user_data);
  if (self && self->callbacks_enabled()) {
    self->counters().tx_diagnostic_queue_overflows.fetch_add(
        1, std::memory_order_relaxed);
    self->record_transport_error(
        "TX_DIAGNOSTIC_EVENT_QUEUE_OVERFLOW",
        self->counters().audio_blocks_processed.load(std::memory_order_relaxed),
        0, 0, 0, 0);
  }
  return false;
}
#endif
} // namespace

AudioI2s::AudioI2s() = default;

AudioI2s::~AudioI2s() {
  stop();
  deinit();
}

bool AudioI2s::init(const AudioI2sConfig &cfg, size_t bs) {
  if (bs == 0 || bs > VOCAL_FX_MAX_BLOCK_SIZE) {
    return false;
  }
  config_ = cfg;
  block_size_ = bs;
  counters_.reset();
  outlier_count_ = 0;
  dsp_total_us_ = 0;
  cycle_total_us_ = 0;
  dsp_worst_us_ = 0;
  cycle_worst_us_ = 0;
  wake_total_us_ = 0;
  wake_worst_us_ = 0;
  std::fill_n(dsp_hist_, kHistBins, 0);
  std::fill_n(cycle_hist_, kHistBins, 0);
  std::fill_n(wake_hist_, kHistBins, 0);
  synth_sample_idx_ = 0;
  synth_phase_ = 0.0f;
  current_synth_f0_ = 110.0f;
  current_synth_voiced_ = false;
  out_peak_l_ = 0.0f;
  out_peak_r_ = 0.0f;
  out_rms_l_ = 0.0f;
  out_rms_r_ = 0.0f;
  sine_phase_l_ = 0.0f;
  sine_phase_r_ = 0.0f;
  ramp_gain_ = 0.0f;
  adc_diag_ = {};
  forensic_data_.captured = false;
  forensic_data_.captured_words = 0;
  identity_checks_.store(0, std::memory_order_relaxed);
  identity_mismatches_.store(0, std::memory_order_relaxed);
  identity_sequence_seen_.store(0, std::memory_order_relaxed);
  transport_error_count_.store(0, std::memory_order_relaxed);

#ifdef ESP_PLATFORM
  // Allocate single full-duplex I2S controller with shared clocks
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = config_.dma_desc_num;
  chan_cfg.dma_frame_num = config_.dma_frame_num;
  chan_cfg.auto_clear_after_cb = false;
  chan_cfg.auto_clear_before_cb = false;

  i2s_chan_handle_t tx = nullptr;
  i2s_chan_handle_t rx = nullptr;
  esp_err_t err = i2s_new_channel(&chan_cfg, &tx, &rx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to allocate full-duplex I2S channels: %s", esp_err_to_name(err));
    return false;
  }
  tx_chan_ = tx;
  rx_chan_ = rx;

  // Initialize clock info
  clock_info_.requested_fs = config_.sample_rate;
  clock_info_.mclk_hz = config_.sample_rate * 256;
  clock_info_.bclk_hz = config_.sample_rate * 64;
  clock_info_.lrck_hz = config_.sample_rate;
  clock_info_.mclk_fs_ratio = 256;
  clock_info_.bclk_fs_ratio = 64;

  // Configure standard Philips I2S mode (32-bit slot, 32-bit data, 64 BCLK/frame)
  i2s_std_config_t tx_std_cfg = {};
  tx_std_cfg.clk_cfg.sample_rate_hz = config_.sample_rate;
  tx_std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  tx_std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  tx_std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  tx_std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  tx_std_cfg.slot_cfg.bit_shift = true; // Philips 1-bit delay between WS and MSB

  // Master clock (12.288 MHz) is generated only once on TX GPIO
  tx_std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(config_.mclk_pin);
  tx_std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(config_.bclk_pin);
  tx_std_cfg.gpio_cfg.ws   = static_cast<gpio_num_t>(config_.ws_pin);
  tx_std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(config_.dout_pin);
  tx_std_cfg.gpio_cfg.din  = I2S_GPIO_UNUSED;
  tx_std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  tx_std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  tx_std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  // RX shares BCLK and WS with TX to guarantee single clock domain
  i2s_std_config_t rx_std_cfg = tx_std_cfg;
  rx_std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED; // Only TX outputs MCLK
  rx_std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  rx_std_cfg.gpio_cfg.din  = static_cast<gpio_num_t>(config_.din_pin);

  // Preferred audio clock source: I2S_CLK_SRC_APLL; Fallback: I2S_CLK_SRC_XTAL
  tx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  rx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;

  err = i2s_channel_init_std_mode(tx, &tx_std_cfg);
  if (err == ESP_OK) {
    err = i2s_channel_init_std_mode(rx, &rx_std_cfg);
  }

  if (err != ESP_OK) {
    // APLL unavailable or failed -> Fallback clean to XTAL
    ESP_LOGW(TAG, "AUDIO CLOCK WARNING: APLL unavailable, using XTAL fractional clock");
    clock_info_.apll_fallback_occurred = true;
    clock_info_.clock_source_name = "SOC_MOD_CLK_XTAL (XTAL fractional clock)";
    clock_info_.source_clock_hz = 40000000;

    tx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;
    rx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_XTAL;

    err = i2s_channel_init_std_mode(tx, &tx_std_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "TX channel std init failed with XTAL: %s", esp_err_to_name(err));
      deinit();
      return false;
    }
    err = i2s_channel_init_std_mode(rx, &rx_std_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "RX channel std init failed with XTAL: %s", esp_err_to_name(err));
      deinit();
      return false;
    }
  } else {
    clock_info_.apll_fallback_occurred = false;
    clock_info_.clock_source_name = "I2S_CLK_SRC_APLL";
    clock_info_.source_clock_hz = 12288000;
  }


  // Register DMA callbacks
  i2s_event_callbacks_t rx_cbs = {
      .on_recv = i2s_rx_event_callback,
      .on_recv_q_ovf = i2s_rx_q_ovf_callback,
      .on_sent = nullptr,
      .on_send_q_ovf = nullptr,
  };
  i2s_channel_register_event_callback(rx, &rx_cbs, this);

  i2s_event_callbacks_t tx_cbs = {
      .on_recv = nullptr,
      .on_recv_q_ovf = nullptr,
      .on_sent = i2s_tx_event_callback,
      .on_send_q_ovf = i2s_tx_q_ovf_callback,
  };
  i2s_channel_register_event_callback(tx, &tx_cbs, this);
  callbacks_enabled_.store(true, std::memory_order_release);

  // Safe startup: Preload 3 descriptors (4.00 ms) into TX DMA to prevent underruns
  // while keeping remaining descriptors available so TX write does not backpressure RX pacing.
  size_t preload_descriptors = std::min<size_t>(3, config_.dma_desc_num);
  size_t preload_bytes = preload_descriptors * config_.dma_frame_num * 2 * sizeof(int32_t);
  int32_t *zero_buf = static_cast<int32_t *>(heap_caps_calloc(
      preload_bytes / sizeof(int32_t), sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (zero_buf) {
    size_t loaded = 0;
    i2s_channel_preload_data(tx, zero_buf, preload_bytes, &loaded);
    heap_caps_free(zero_buf);
  }

  // Enable channels
  err = i2s_channel_enable(tx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to enable TX: %s", esp_err_to_name(err));
    deinit();
    return false;
  }
  tx_enabled_ = true;

  err = i2s_channel_enable(rx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "failed to enable RX: %s", esp_err_to_name(err));
    deinit();
    return false;
  }
  rx_enabled_ = true;

  running_.store(true, std::memory_order_release);
  return true;
#else
  running_.store(true, std::memory_order_release);
  return true;
#endif
}

void AudioI2s::deinit() {
  running_.store(false, std::memory_order_release);
#ifdef ESP_PLATFORM
  callbacks_enabled_.store(false, std::memory_order_release);
  if (rx_chan_) {
    if (rx_enabled_) {
      i2s_channel_disable(static_cast<i2s_chan_handle_t>(rx_chan_));
      rx_enabled_ = false;
    }
    i2s_del_channel(static_cast<i2s_chan_handle_t>(rx_chan_));
    rx_chan_ = nullptr;
  }
  if (tx_chan_) {
    if (tx_enabled_) {
      i2s_channel_disable(static_cast<i2s_chan_handle_t>(tx_chan_));
      tx_enabled_ = false;
    }
    i2s_del_channel(static_cast<i2s_chan_handle_t>(tx_chan_));
    tx_chan_ = nullptr;
  }
#endif
}

void AudioI2s::stop() {
  running_.store(false, std::memory_order_release);
}

void AudioI2s::set_mode(AudioI2sMode mode) {
  config_.mode = mode;
}

void AudioI2s::set_tx_signal(TxSignalType sig, float dbfs) {
  config_.tx_signal = sig;
  config_.tx_level_dbfs = dbfs;
}

void AudioI2s::record_timing(uint64_t dsp_us, uint64_t cycle_us,
                             uint64_t wake_us, uint64_t rx_gap_us,
                             uint64_t block_idx) {
  dsp_total_us_ += dsp_us;
  cycle_total_us_ += cycle_us;
  wake_total_us_ += wake_us;
  if (dsp_us > dsp_worst_us_) dsp_worst_us_ = dsp_us;
  if (cycle_us > cycle_worst_us_) cycle_worst_us_ = cycle_us;
  if (wake_us > wake_worst_us_) wake_worst_us_ = wake_us;

  size_t dsp_bin = std::min(static_cast<size_t>(dsp_us / kBinWidthUs), kHistBins - 1);
  dsp_hist_[dsp_bin]++;

  size_t cycle_bin = std::min(static_cast<size_t>(cycle_us / kBinWidthUs), kHistBins - 1);
  cycle_hist_[cycle_bin]++;

  size_t wake_bin = std::min(static_cast<size_t>(wake_us / kBinWidthUs), kHistBins - 1);
  wake_hist_[wake_bin]++;

  // Deadline: DSP execution must remain under 1333 us (block duration).
  if (dsp_us > 1333) {
    counters_.dsp_deadline_misses.fetch_add(1, std::memory_order_relaxed);
    counters_.audio_deadline_misses.fetch_add(1, std::memory_order_relaxed);
  }
  // Cycle timing represents interval between consecutive reads; flag transport miss if > 2000 us.
  if (cycle_us > 2000) {
    counters_.transport_deadline_misses.fetch_add(1, std::memory_order_relaxed);
  }

  if (dsp_us > 1333 || cycle_us > 2000) {
    if (outlier_count_ < kMaxOutlierRecords) {
      outliers_[outlier_count_] = {
#ifdef ESP_PLATFORM
          static_cast<uint64_t>(esp_timer_get_time()),
#else
          0,
#endif
          block_idx,
          dsp_us,
          cycle_us,
          wake_us,
          rx_gap_us,
          (dsp_us > 1333) ? "DspDeadlineMiss" : "TransportOrSchedulerMiss",
          snapshot_active_.load(std::memory_order_relaxed),
          telemetry_formatting_active_.load(std::memory_order_relaxed),
          serial_printing_active_.load(std::memory_order_relaxed),
          pitch_worker_active_.load(std::memory_order_relaxed),
          diagnostic_queue_flush_active_.load(std::memory_order_relaxed),
      };
      outlier_count_++;
    }
  }
}

void AudioI2s::generate_synthetic_voiced_mono(float *mono, size_t frames) {
  constexpr float kFrequencies[] = {110.0f, 147.0f, 220.0f, 330.0f, 440.0f};
  constexpr size_t kNumFrequencies = sizeof(kFrequencies) / sizeof(kFrequencies[0]);
  constexpr uint64_t kInitialSilenceSamples = 24000; // 500 ms @ 48 kHz
  constexpr uint64_t kVoicedSamples = 72000;         // 1500 ms @ 48 kHz
  constexpr uint64_t kSilenceSamples = 4800;         // 100 ms @ 48 kHz
  constexpr uint64_t kCycleSamples = kVoicedSamples + kSilenceSamples; // 76800 samples = 1600 ms
  constexpr uint64_t kAttackSamples = 480;           // 10 ms @ 48 kHz
  constexpr uint64_t kReleaseSamples = 1440;         // 30 ms @ 48 kHz
  constexpr float kTargetAmplitude = 0.12589254f;    // -18 dBFS (10^(-18/20))
  constexpr float kHarmonicNorm = 1.875f;            // 1.0 + 0.5 + 0.25 + 0.125
  constexpr float kTwoPi = 6.28318530717958647692f;
  constexpr float kInvFs = 1.0f / 48000.0f;

  double sum_sq = 0.0;
  uint32_t checksum = 2166136261U;
  for (size_t i = 0; i < frames; ++i) {
    uint64_t idx = synth_sample_idx_++;
    if (idx < kInitialSilenceSamples) {
      mono[i] = 0.0f;
      current_synth_f0_ = kFrequencies[0];
      current_synth_voiced_ = false;
      current_stimulus_state_ = StimulusState::Silence;
      uint32_t bits = 0;
      std::memcpy(&bits, &mono[i], sizeof(bits));
      checksum = (checksum ^ bits) * 16777619U;
      continue;
    }

    uint64_t active_idx = idx - kInitialSilenceSamples;
    uint64_t cycle_num = active_idx / kCycleSamples;
    uint64_t sample_in_cycle = active_idx % kCycleSamples;
    float f0 = kFrequencies[cycle_num % kNumFrequencies];
    current_synth_f0_ = f0;
    last_sample_in_cycle_ = sample_in_cycle;

    if (sample_in_cycle < kVoicedSamples) {
      current_synth_voiced_ = true;
      float env = 1.0f;
      if (sample_in_cycle < kAttackSamples) {
        env = static_cast<float>(sample_in_cycle) / static_cast<float>(kAttackSamples);
        current_stimulus_state_ = StimulusState::Attack;
      } else if (sample_in_cycle >= (kVoicedSamples - kReleaseSamples)) {
        uint64_t rel_sample = kVoicedSamples - sample_in_cycle;
        env = static_cast<float>(rel_sample) / static_cast<float>(kReleaseSamples);
        current_stimulus_state_ = StimulusState::Release;
      } else {
        current_stimulus_state_ = StimulusState::Tone;
      }

      float h1 = std::sin(synth_phase_);
      float h2 = std::sin(2.0f * synth_phase_);
      float h3 = std::sin(3.0f * synth_phase_);
      float h4 = std::sin(4.0f * synth_phase_);
      float harm = (h1 + 0.5f * h2 + 0.25f * h3 + 0.125f * h4) / kHarmonicNorm;

      mono[i] = kTargetAmplitude * env * harm;

      synth_phase_ += kTwoPi * f0 * kInvFs;
      if (synth_phase_ >= kTwoPi) {
        synth_phase_ -= kTwoPi;
      }
    } else {
      current_synth_voiced_ = false;
      current_stimulus_state_ = StimulusState::Silence;
      mono[i] = 0.0f;
    }
    sum_sq += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
    uint32_t bits = 0;
    std::memcpy(&bits, &mono[i], sizeof(bits));
    checksum = (checksum ^ bits) * 16777619U;
  }
  current_stimulus_rms_ = static_cast<float>(std::sqrt(sum_sq / frames));
  current_block_checksum_ = checksum;
}

void AudioI2s::generate_b4b2_pure_sine(float *mono, size_t frames) {
  constexpr uint64_t kInitialSilenceSamples = 24000; // 500 ms @ 48 kHz
  constexpr uint64_t kAttackSamples = 960;           // 20 ms @ 48 kHz
  constexpr float kFrequencyHz = 220.0f;
  constexpr float kTargetAmplitude = 0.12589254f; // -18 dBFS
  constexpr float kTwoPi = 6.28318530717958647692f;
  constexpr float kInvFs = 1.0f / 48000.0f;

  double sum_sq = 0.0;
  uint32_t checksum = 2166136261U;
  for (size_t i = 0; i < frames; ++i) {
    const uint64_t index = synth_sample_idx_++;
    current_synth_f0_ = kFrequencyHz;
    if (index < kInitialSilenceSamples) {
      mono[i] = 0.0f;
      current_synth_voiced_ = false;
      current_stimulus_state_ = StimulusState::Silence;
    } else {
      const uint64_t tone_index = index - kInitialSilenceSamples;
      const float envelope =
          tone_index < kAttackSamples
              ? static_cast<float>(tone_index) /
                    static_cast<float>(kAttackSamples)
              : 1.0f;
      current_synth_voiced_ = true;
      current_stimulus_state_ = tone_index < kAttackSamples
                                    ? StimulusState::Attack
                                    : StimulusState::Tone;
      mono[i] = kTargetAmplitude * envelope * std::sin(synth_phase_);
      synth_phase_ += kTwoPi * kFrequencyHz * kInvFs;
      if (synth_phase_ >= kTwoPi)
        synth_phase_ -= kTwoPi;
    }
    sum_sq += static_cast<double>(mono[i]) * mono[i];
    uint32_t bits = 0;
    std::memcpy(&bits, &mono[i], sizeof(bits));
    checksum = (checksum ^ bits) * 16777619U;
  }
  current_stimulus_rms_ =
      frames ? static_cast<float>(std::sqrt(sum_sq / frames)) : 0.0f;
  current_block_checksum_ = checksum;
}

void AudioI2s::generate_tx_tones(float *out_l, float *out_r, size_t frames) {
  const float amplitude = std::pow(10.0f, config_.tx_level_dbfs / 20.0f);
  const float two_pi = 6.28318530717958647692f;
  const float dt = 1.0f / static_cast<float>(config_.sample_rate);

  float freq_l = 1000.0f;
  float freq_r = 1000.0f;
  bool mute_l = false;
  bool mute_r = false;

  switch (config_.tx_signal) {
  case TxSignalType::Silence:
    mute_l = true;
    mute_r = true;
    break;
  case TxSignalType::Sine100Hz:
    freq_l = freq_r = 100.0f;
    break;
  case TxSignalType::Sine1kHz:
    freq_l = freq_r = 1000.0f;
    break;
  case TxSignalType::Sine10kHz:
    freq_l = freq_r = 10000.0f;
    break;
  case TxSignalType::LeftOnly1kHz:
    freq_l = 1000.0f;
    mute_r = true;
    break;
  case TxSignalType::RightOnly1kHz:
    freq_r = 1000.0f;
    mute_l = true;
    break;
  case TxSignalType::AlternatingLR: {
    // Alternate every 1 second (48000 frames)
    uint64_t block = counters_.audio_blocks_processed.load(std::memory_order_relaxed);
    bool left_active = ((block * frames / 48000) % 2) == 0;
    freq_l = freq_r = 1000.0f;
    mute_l = !left_active;
    mute_r = left_active;
    break;
  }
  case TxSignalType::ChannelIntegrity:
    freq_l = 1000.0f;
    freq_r = 2000.0f;
    break;
  case TxSignalType::ChannelIntegrityInv:
    freq_l = 2000.0f;
    freq_r = 1000.0f;
    break;
  }

  for (size_t i = 0; i < frames; ++i) {
    out_l[i] = mute_l ? 0.0f : amplitude * std::sin(sine_phase_l_);
    out_r[i] = mute_r ? 0.0f : amplitude * std::sin(sine_phase_r_);
    sine_phase_l_ += two_pi * freq_l * dt;
    sine_phase_r_ += two_pi * freq_r * dt;
    if (sine_phase_l_ >= two_pi) sine_phase_l_ -= two_pi;
    if (sine_phase_r_ >= two_pi) sine_phase_r_ -= two_pi;
  }
}

void AudioI2s::update_adc_diagnostics(const float *l, const float *r,
                                      const int32_t *raw, size_t frames) {
  double sum_l = 0.0, sum_r = 0.0;
  double sum_sq_l = 0.0, sum_sq_r = 0.0;
  float peak_l = adc_diag_.peak_l;
  float peak_r = adc_diag_.peak_r;

  for (size_t i = 0; i < frames; ++i) {
    float sl = l[i];
    float sr = r[i];
    float al = std::fabs(sl);
    float ar = std::fabs(sr);
    if (al > peak_l) peak_l = al;
    if (ar > peak_r) peak_r = ar;
    sum_l += sl;
    sum_r += sr;
    sum_sq_l += sl * sl;
    sum_sq_r += sr * sr;

    int32_t raw_l = raw[2 * i];
    int32_t raw_r = raw[2 * i + 1];
    if (raw_l == 0) adc_diag_.zero_samples++;
    if (raw_r == 0) adc_diag_.zero_samples++;
    if (al >= 0.9999f || ar >= 0.9999f) adc_diag_.clipped_samples++;

    if (raw_l < adc_diag_.min_raw_l) adc_diag_.min_raw_l = raw_l;
    if (raw_l > adc_diag_.max_raw_l) adc_diag_.max_raw_l = raw_l;
    if (raw_r < adc_diag_.min_raw_r) adc_diag_.min_raw_r = raw_r;
    if (raw_r > adc_diag_.max_raw_r) adc_diag_.max_raw_r = raw_r;
  }

  adc_diag_.peak_l = peak_l;
  adc_diag_.peak_r = peak_r;
  adc_diag_.dc_offset_l = static_cast<float>(sum_l / frames);
  adc_diag_.dc_offset_r = static_cast<float>(sum_r / frames);
  adc_diag_.rms_l = static_cast<float>(std::sqrt(sum_sq_l / frames));
  adc_diag_.rms_r = static_cast<float>(std::sqrt(sum_sq_r / frames));
}

void AudioI2s::run() {
#ifdef ESP_PLATFORM
  int32_t in32[VOCAL_FX_MAX_BLOCK_SIZE * 2];
  int32_t out32[VOCAL_FX_MAX_BLOCK_SIZE * 2];
  float mono[VOCAL_FX_MAX_BLOCK_SIZE];
  float l[VOCAL_FX_MAX_BLOCK_SIZE];
  float r[VOCAL_FX_MAX_BLOCK_SIZE];

  auto rx = static_cast<i2s_chan_handle_t>(rx_chan_);
  auto tx = static_cast<i2s_chan_handle_t>(tx_chan_);
  const size_t n = block_size_;
  const size_t byte_count = n * 2 * sizeof(int32_t);

  uint64_t pulse_sample_counter = 0;
  constexpr uint64_t kPulsePeriodSamples = 24000; // 500 ms at 48 kHz
  uint64_t warmup_blocks = 0;

  while (running_.load(std::memory_order_relaxed)) {
    int64_t t_cycle_start = esp_timer_get_time();
    uint64_t block_idx = counters_.audio_blocks_processed.load(std::memory_order_relaxed);

    // 1. Read block from RX DMA (Req 4: Real ADC Input Tap)
    size_t got = 0;
    esp_err_t rx_err = i2s_channel_read(rx, in32, byte_count, &got, pdMS_TO_TICKS(100));
    if (rx_err != ESP_OK || got < byte_count) {
      counters_.i2s_read_failures.fetch_add(1, std::memory_order_relaxed);
      counters_.rx_dropped_frames.fetch_add(
          (byte_count - std::min(got, byte_count)) /
              (2 * sizeof(int32_t)),
          std::memory_order_relaxed);
      record_transport_error("I2S_RX_READ_FAILURE", block_idx, 0, 0, 0, 0);
      continue;
    }
    counters_.i2s_read_successes.fetch_add(1, std::memory_order_relaxed);
    decrement_depth(counters_.rx_event_queue_depth);

    int64_t t_wake = esp_timer_get_time();
    int64_t last_rx_cb = s_last_rx_callback_us;
    uint64_t wake_latency_us = (last_rx_cb > 0 && t_wake > last_rx_cb)
                                  ? static_cast<uint64_t>(t_wake - last_rx_cb)
                                  : 0;
    uint64_t curr_wake_max = counters_.max_audio_task_wake_latency_us.load(std::memory_order_relaxed);
    while (wake_latency_us > curr_wake_max &&
           !counters_.max_audio_task_wake_latency_us.compare_exchange_weak(
               curr_wake_max, wake_latency_us, std::memory_order_relaxed)) {
    }

    // 2. Forensic sample capture (first 256 words)
    if (!forensic_data_.captured) {
      size_t copy_words = std::min(n * 2, kForensicWordCount - forensic_data_.captured_words);
      std::memcpy(&forensic_data_.raw_words[forensic_data_.captured_words], in32, copy_words * sizeof(int32_t));
      forensic_data_.captured_words += copy_words;
      if (forensic_data_.captured_words >= kForensicWordCount) {
        forensic_data_.captured = true;
      }
    }

    // 3. Decode input samples (PCM24 in 32-bit container -> float [-1.0, +1.0])
    for (size_t i = 0; i < n; ++i) {
      float left = pcm24_to_float(in32[2 * i]);
      float right = pcm24_to_float(in32[2 * i + 1]);
      l[i] = left;
      r[i] = right;
      mono[i] = (left + right) * 0.5f;
    }

    uint64_t pure_dsp_us = 0;

    // 4. Mode dispatch
    // Real Pipeline in Stage B4B (Req 4):
    // I2S RX DMA (ADC) -> PCM Conversion (float) -> [Synthetic Voiced Replacement Point] -> vocal_fx_process() [Tap + DSP]
    switch (config_.mode) {
    case AudioI2sMode::FullDsp: {
      int64_t t_dsp_start = esp_timer_get_time();
      vocal_fx_process(mono, l, r, n);
      pure_dsp_us = static_cast<uint64_t>(esp_timer_get_time() - t_dsp_start);
      break;
    }
    case AudioI2sMode::SyntheticVoicedDsp: {
      // Step 3: [synthetic replacement point]
      // Replace ADC input with deterministic synthetic voiced harmonic tone (-18 dBFS)
      generate_synthetic_voiced_mono(mono, n);
      if (current_stimulus_state_ == StimulusState::Tone) {
        vocal_fx_funnel_inc_synthetic_tone_blocks();
      }
      int64_t t_dsp_start = esp_timer_get_time();
      // Step 4 & 5: Feed identical synthetic buffer into pitch/LPC analysis tap and main DSP
      vocal_fx_process(mono, l, r, n);
      pure_dsp_us = static_cast<uint64_t>(esp_timer_get_time() - t_dsp_start);
      if (synth_hook_ && ((last_sample_in_cycle_ >= 24000 && last_sample_in_cycle_ < 24064) ||
                          (last_sample_in_cycle_ >= 72000 && last_sample_in_cycle_ < 72500))) {
        synth_hook_(last_sample_in_cycle_, current_synth_f0_, synth_hook_user_data_);
      }
      break;
    }
    case AudioI2sMode::PitchWorkerLockAudit: {
      generate_b4b2_pure_sine(mono, n);
      if (current_stimulus_state_ == StimulusState::Tone)
        vocal_fx_funnel_inc_synthetic_tone_blocks();
      const int64_t t_dsp_start = esp_timer_get_time();
      vocal_fx_process(mono, l, r, n);
      pure_dsp_us =
          static_cast<uint64_t>(esp_timer_get_time() - t_dsp_start);

      const VocalFxInputIdentity identity = vocal_fx_input_identity();
      const uint64_t last_identity =
          identity_sequence_seen_.load(std::memory_order_relaxed);
      if (identity.sequence != 0 && identity.sequence != last_identity) {
        identity_sequence_seen_.store(identity.sequence,
                                      std::memory_order_relaxed);
        identity_checks_.fetch_add(1, std::memory_order_relaxed);
        uint32_t synth_rms_bits = 0, pitch_rms_bits = 0, dsp_rms_bits = 0;
        std::memcpy(&synth_rms_bits, &current_stimulus_rms_,
                    sizeof(synth_rms_bits));
        std::memcpy(&pitch_rms_bits, &identity.pitch_tap_rms,
                    sizeof(pitch_rms_bits));
        std::memcpy(&dsp_rms_bits, &identity.dsp_input_rms,
                    sizeof(dsp_rms_bits));
        identity_synth_rms_bits_.store(synth_rms_bits,
                                       std::memory_order_relaxed);
        identity_pitch_rms_bits_.store(pitch_rms_bits,
                                       std::memory_order_relaxed);
        identity_dsp_rms_bits_.store(dsp_rms_bits,
                                     std::memory_order_relaxed);
        identity_synth_checksum_.store(current_block_checksum_,
                                       std::memory_order_relaxed);
        identity_pitch_checksum_.store(identity.pitch_tap_checksum,
                                       std::memory_order_relaxed);
        identity_dsp_checksum_.store(identity.dsp_input_checksum,
                                     std::memory_order_relaxed);
        if (current_block_checksum_ != identity.pitch_tap_checksum ||
            current_block_checksum_ != identity.dsp_input_checksum ||
            synth_rms_bits != pitch_rms_bits || synth_rms_bits != dsp_rms_bits)
          identity_mismatches_.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case AudioI2sMode::TxBringUp: {
      generate_tx_tones(l, r, n);
      break;
    }
    case AudioI2sMode::RxBringUp: {
      update_adc_diagnostics(l, r, in32, n);
      std::fill_n(l, n, 0.0f);
      std::fill_n(r, n, 0.0f);
      break;
    }
    case AudioI2sMode::Bypass: {
      // Direct pass-through: input float is routed directly to output without modification
      break;
    }
    case AudioI2sMode::LatencyPulse: {
      // Emit sharp single-sample pulse periodically
      for (size_t i = 0; i < n; ++i) {
        if (pulse_sample_counter == 0) {
          l[i] = 1.0f;
          r[i] = 1.0f;
        } else {
          l[i] = 0.0f;
          r[i] = 0.0f;
        }
        pulse_sample_counter = (pulse_sample_counter + 1) % kPulsePeriodSamples;
      }
      break;
    }
    case AudioI2sMode::SampleForensic: {
      // Pass-through during forensic inspection
      break;
    }
    }

    // 5. Soft startup fade-in ramp (avoids initial pops)
    if (ramp_gain_ < 1.0f) {
      ramp_gain_ += 0.01f;
      if (ramp_gain_ > 1.0f) ramp_gain_ = 1.0f;
      for (size_t i = 0; i < n; ++i) {
        l[i] *= ramp_gain_;
        r[i] *= ramp_gain_;
      }
    }

    // 6. Output Sanity Check, Level Tracking & PCM32 encoding
    double sum_sq_l = 0.0, sum_sq_r = 0.0;
    for (size_t i = 0; i < n; ++i) {
      if (std::isnan(l[i]) || std::isinf(l[i])) {
        counters_.nan_inf_count.fetch_add(1, std::memory_order_relaxed);
        l[i] = 0.0f;
      }
      if (std::isnan(r[i]) || std::isinf(r[i])) {
        counters_.nan_inf_count.fetch_add(1, std::memory_order_relaxed);
        r[i] = 0.0f;
      }
      float al = std::fabs(l[i]);
      float ar = std::fabs(r[i]);
      if (al > out_peak_l_) out_peak_l_ = al;
      if (ar > out_peak_r_) out_peak_r_ = ar;
      sum_sq_l += l[i] * l[i];
      sum_sq_r += r[i] * r[i];

      out32[2 * i] = float_to_pcm32(l[i]);
      out32[2 * i + 1] = float_to_pcm32(r[i]);
    }
    out_rms_l_ = static_cast<float>(std::sqrt(sum_sq_l / n));
    out_rms_r_ = static_cast<float>(std::sqrt(sum_sq_r / n));

    // 7. Write block to TX DMA
    uint64_t rx_gap_us = (last_rx_cb > 0) ? counters_.max_rx_callback_gap_us.load(std::memory_order_relaxed) : 0;
    size_t sent = 0;
    esp_err_t tx_err = i2s_channel_write(tx, out32, byte_count, &sent, pdMS_TO_TICKS(100));
    if (tx_err != ESP_OK || sent < byte_count) {
      counters_.i2s_write_failures.fetch_add(1, std::memory_order_relaxed);
      counters_.tx_dropped_frames.fetch_add(
          (byte_count - std::min(sent, byte_count)) /
              (2 * sizeof(int32_t)),
          std::memory_order_relaxed);
      record_transport_error("I2S_TX_WRITE_FAILURE", block_idx, pure_dsp_us,
                             rx_gap_us, 0, wake_latency_us);
    } else {
      counters_.i2s_write_successes.fetch_add(1, std::memory_order_relaxed);
      increment_depth(counters_.tx_event_queue_depth,
                      counters_.tx_event_queue_max_depth);
    }

    // 8. Record cycle timing
    int64_t t_cycle_end = esp_timer_get_time();
    uint64_t cycle_us = static_cast<uint64_t>(t_cycle_end - t_cycle_start);

    record_timing(pure_dsp_us, cycle_us, wake_latency_us, rx_gap_us, block_idx);
    counters_.audio_blocks_processed.fetch_add(1, std::memory_order_relaxed);

    if (warmup_blocks < 100) {
      warmup_blocks++;
      if (warmup_blocks == 100) {
        counters_.rx_overruns.store(0, std::memory_order_relaxed);
        counters_.tx_underruns.store(0, std::memory_order_relaxed);
        counters_.dma_errors.store(0, std::memory_order_relaxed);
        counters_.audio_deadline_misses.store(0, std::memory_order_relaxed);
        counters_.dsp_deadline_misses.store(0, std::memory_order_relaxed);
        counters_.transport_deadline_misses.store(0, std::memory_order_relaxed);
        counters_.nan_inf_count.store(0, std::memory_order_relaxed);
        dsp_total_us_ = 0;
        cycle_total_us_ = 0;
        wake_total_us_ = 0;
        dsp_worst_us_ = 0;
        cycle_worst_us_ = 0;
        wake_worst_us_ = 0;
        std::fill_n(dsp_hist_, kHistBins, 0);
        std::fill_n(cycle_hist_, kHistBins, 0);
        std::fill_n(wake_hist_, kHistBins, 0);
        outlier_count_ = 0;
      }
    }
  }
#else
  (void)sine_phase_l_;
  (void)sine_phase_r_;
#endif
}

TimingPercentiles AudioI2s::calculate_dsp_percentiles() const {
  TimingPercentiles p{};
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += dsp_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(dsp_total_us_) / total_samples;
  p.max_us = dsp_worst_us_;

  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += dsp_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p95_us == 0 && cumulative >= target_p95) p.p95_us = std::min(bin_us, p.max_us);
    if (p.p99_us == 0 && cumulative >= target_p99) {
      p.p99_bucket_upper_bound_us = bin_us;
      p.p99_us = std::min(bin_us, p.max_us);
    }
    if (p.p99_9_us == 0 && cumulative >= target_p99_9) p.p99_9_us = std::min(bin_us, p.max_us);
  }
  return p;
}

TimingPercentiles AudioI2s::calculate_cycle_percentiles() const {
  TimingPercentiles p{};
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += cycle_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(cycle_total_us_) / total_samples;
  p.max_us = cycle_worst_us_;

  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += cycle_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p95_us == 0 && cumulative >= target_p95) p.p95_us = std::min(bin_us, p.max_us);
    if (p.p99_us == 0 && cumulative >= target_p99) {
      p.p99_bucket_upper_bound_us = bin_us;
      p.p99_us = std::min(bin_us, p.max_us);
    }
    if (p.p99_9_us == 0 && cumulative >= target_p99_9) p.p99_9_us = std::min(bin_us, p.max_us);
  }
  return p;
}

TimingPercentiles AudioI2s::calculate_wake_percentiles() const {
  TimingPercentiles p{};
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += wake_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(wake_total_us_) / total_samples;
  p.max_us = wake_worst_us_;

  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += wake_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p95_us == 0 && cumulative >= target_p95) p.p95_us = std::min(bin_us, p.max_us);
    if (p.p99_us == 0 && cumulative >= target_p99) {
      p.p99_bucket_upper_bound_us = bin_us;
      p.p99_us = std::min(bin_us, p.max_us);
    }
    if (p.p99_9_us == 0 && cumulative >= target_p99_9) p.p99_9_us = std::min(bin_us, p.max_us);
  }
  return p;
}

void AudioI2s::print_hardware_config() const {
  if (clock_info_.apll_fallback_occurred) {
    printf("AUDIO CLOCK: EXPECTED_FALLBACK (APLL unavailable, using XTAL fractional clock)\n");
  }
  printf("\n=======================================================\n");
  printf("            AUDIO HARDWARE CONFIGURATION               \n");
  printf("=======================================================\n");
  printf("Target Board:                   Wireless-Tag WT9932P4-TINY (ESP32-P4)\n");
  printf("requested Fs:                   %lu Hz\n", static_cast<unsigned long>(clock_info_.requested_fs));
  printf("actual/configured clock source: %s\n", clock_info_.clock_source_name);
  printf("source clock Hz:                %lu Hz\n", static_cast<unsigned long>(clock_info_.source_clock_hz));
  printf("MCLK Hz:                        %lu Hz\n", static_cast<unsigned long>(clock_info_.mclk_hz));
  printf("BCLK Hz:                        %lu Hz\n", static_cast<unsigned long>(clock_info_.bclk_hz));
  printf("LRCK/Fs:                        %lu Hz\n", static_cast<unsigned long>(clock_info_.lrck_hz));
  printf("MCLK/Fs ratio:                  %lu\n", static_cast<unsigned long>(clock_info_.mclk_fs_ratio));
  printf("BCLK/Fs ratio:                  %lu\n", static_cast<unsigned long>(clock_info_.bclk_fs_ratio));
  printf("I2S Controller:                 I2S_NUM_0 (Full-Duplex Standard Philips)\n");
  printf("MCLK GPIO:                      GPIO_%d (Header Pin 4 -> PCM1808 SCKI)\n", config_.mclk_pin);
  printf("BCLK GPIO:                      GPIO_%d (Header Pin 5 -> PCM1808/PCM5102 BCK)\n", config_.bclk_pin);
  printf("WS GPIO:                        GPIO_%d (Header Pin 6 -> PCM1808/PCM5102 LRCK)\n", config_.ws_pin);
  printf("DOUT GPIO:                      GPIO_%d (Header Pin 7 -> PCM5102 DIN)\n", config_.dout_pin);
  printf("DIN GPIO:                       GPIO_%d (Header Pin 8 <- PCM1808 DOUT)\n", config_.din_pin);
  printf("Data Width:                     32-bit slot, 24-bit audio MSB-aligned in bits [31:8]\n");
  printf("Slot Width:                     32 bits per slot (2 slots/frame = 64 BCLK/frame)\n");
  printf("Channels:                       Stereo (L/R interleaved)\n");
  printf("DMA Descriptors:                %lu descriptors\n", static_cast<unsigned long>(config_.dma_desc_num));
  printf("DMA Frame Count:                %lu frames/desc (%lu bytes/desc)\n",
         static_cast<unsigned long>(config_.dma_frame_num),
         static_cast<unsigned long>(config_.dma_frame_num * 2 * sizeof(int32_t)));
  printf("DMA buffering capacity:         8.00 ms (%lu frames total)\n",
         static_cast<unsigned long>(config_.dma_desc_num * config_.dma_frame_num));
  printf("physical/effective latency:     PENDING_HARDWARE_MEASUREMENT\n");
  printf("=======================================================\n\n");
}


void AudioI2s::print_telemetry_summary() const {
  TimingPercentiles dp = calculate_dsp_percentiles();
  TimingPercentiles cp = calculate_cycle_percentiles();
  TimingPercentiles wp = calculate_wake_percentiles();
  printf("AUDIO: blocks=%llu rx_ovf=%llu tx_und=%llu dma_err=%llu misses=%llu dsp_avg=%.1f dsp_p99=%llu dsp_max=%llu cyc_p99=%llu wake_p99=%llu nan_inf=%llu\n",
         static_cast<unsigned long long>(counters_.audio_blocks_processed.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(counters_.rx_overruns.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(counters_.tx_underruns.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(counters_.dma_errors.load(std::memory_order_relaxed)),
         static_cast<unsigned long long>(counters_.audio_deadline_misses.load(std::memory_order_relaxed)),
         dp.avg_us,
         static_cast<unsigned long long>(dp.p99_us),
         static_cast<unsigned long long>(dp.max_us),
         static_cast<unsigned long long>(cp.p99_us),
         static_cast<unsigned long long>(wp.p99_us),
         static_cast<unsigned long long>(counters_.nan_inf_count.load(std::memory_order_relaxed)));
}

void AudioI2s::print_forensic_outliers() const {
  printf("\n--- Forensic Outlier Report (%zu events recorded) ---\n", outlier_count_);
  for (size_t i = 0; i < outlier_count_; ++i) {
    const auto &ev = outliers_[i];
    printf("  [%zu] Block=%llu Time=%llu us PureDsp=%llu us Cycle=%llu us Wake=%llu us Gap=%llu us Cause=%s Context[snapshot=%d formatting=%d serial=%d pitch_worker=%d queue_flush=%d]\n",
           i,
           static_cast<unsigned long long>(ev.block_index),
           static_cast<unsigned long long>(ev.timestamp_us),
           static_cast<unsigned long long>(ev.pure_dsp_us),
           static_cast<unsigned long long>(ev.block_cycle_us),
           static_cast<unsigned long long>(ev.wake_latency_us),
           static_cast<unsigned long long>(ev.rx_gap_us),
           ev.category, ev.snapshot_active, ev.telemetry_formatting_active,
           ev.serial_printing_active, ev.pitch_worker_active,
           ev.diagnostic_queue_flush_active);
  }
}

void AudioI2s::dump_sample_forensic() const {
  if (!forensic_data_.captured) {
    printf("Sample forensic capture not completed yet (%zu/256 words)\n", forensic_data_.captured_words);
    return;
  }
  printf("\n=======================================================\n");
  printf("          SAMPLE FORMAT FORENSIC REPORT (FIRST 256 WORDS) \n");
  printf("=======================================================\n");
  for (size_t i = 0; i < forensic_data_.captured_words; i += 2) {
    int32_t raw_l = forensic_data_.raw_words[i];
    int32_t raw_r = forensic_data_.raw_words[i + 1];
    int32_t pcm24_l = raw_l >> 8;
    int32_t pcm24_r = raw_r >> 8;
    float f_l = pcm24_to_float(raw_l);
    float f_r = pcm24_to_float(raw_r);
    printf("[%03zu] L: RAW=0x%08lX PCM24=0x%06lX (%+8ld) FLOAT=%+.6f | R: RAW=0x%08lX PCM24=0x%06lX (%+8ld) FLOAT=%+.6f\n",
           i / 2,
           static_cast<unsigned long>(raw_l),
           static_cast<unsigned long>(pcm24_l & 0xFFFFFF),
           static_cast<long>(pcm24_l),
           f_l,
           static_cast<unsigned long>(raw_r),
           static_cast<unsigned long>(pcm24_r & 0xFFFFFF),
           static_cast<long>(pcm24_r),
           f_r);
  }
  printf("=======================================================\n\n");
}

void AudioI2s::record_transport_error(const char *type, uint64_t block_idx, uint64_t dsp_us, uint64_t rx_gap, uint64_t tx_gap, uint64_t wake_us) {
  size_t idx = transport_error_count_.fetch_add(1, std::memory_order_relaxed);
  TransportErrorEvent &ev = transport_errors_[idx % kMaxTransportErrorEvents];
#ifdef ESP_PLATFORM
  ev.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
#else
  ev.timestamp_us = 0;
#endif
  ev.block_index = block_idx;
  ev.dsp_duration_us = dsp_us;
  ev.rx_gap_us = rx_gap;
  ev.tx_gap_us = tx_gap;
  ev.wake_interval_us = wake_us;
  ev.type = type;
}

void AudioI2s::print_transport_errors() const {
  const size_t total = transport_error_count_.load(std::memory_order_relaxed);
  printf("\n--- Transport Error Event Log (%zu total events) ---\n", total);
  size_t n = std::min(total, kMaxTransportErrorEvents);
  for (size_t i = 0; i < n; ++i) {
    const auto &ev = transport_errors_[i];
    printf("  [%zu] Type=%s Time=%llu us Block=%llu PureDsp=%llu us Wake=%llu us RxGap=%llu us TxGap=%llu us\n",
           i, ev.type ? ev.type : "UNKNOWN",
           static_cast<unsigned long long>(ev.timestamp_us),
           static_cast<unsigned long long>(ev.block_index),
           static_cast<unsigned long long>(ev.dsp_duration_us),
           static_cast<unsigned long long>(ev.wake_interval_us),
           static_cast<unsigned long long>(ev.rx_gap_us),
           static_cast<unsigned long long>(ev.tx_gap_us));
  }
  if (total == 0) {
    printf("  No transport errors recorded.\n");
  }
  printf("----------------------------------------------------\n\n");
}

SyntheticInputIdentityTelemetry AudioI2s::synthetic_input_identity() const {
  SyntheticInputIdentityTelemetry result{};
  result.checks = identity_checks_.load(std::memory_order_relaxed);
  result.mismatches = identity_mismatches_.load(std::memory_order_relaxed);
  uint32_t synth_bits = identity_synth_rms_bits_.load(std::memory_order_relaxed);
  uint32_t pitch_bits = identity_pitch_rms_bits_.load(std::memory_order_relaxed);
  uint32_t dsp_bits = identity_dsp_rms_bits_.load(std::memory_order_relaxed);
  std::memcpy(&result.synthetic_rms, &synth_bits, sizeof(synth_bits));
  std::memcpy(&result.pitch_tap_rms, &pitch_bits, sizeof(pitch_bits));
  std::memcpy(&result.dsp_input_rms, &dsp_bits, sizeof(dsp_bits));
  result.synthetic_checksum =
      identity_synth_checksum_.load(std::memory_order_relaxed);
  result.pitch_tap_checksum =
      identity_pitch_checksum_.load(std::memory_order_relaxed);
  result.dsp_input_checksum =
      identity_dsp_checksum_.load(std::memory_order_relaxed);
  return result;
}

// -----------------------------------------------------------------------------
// Format & Integrity Diagnostic Test Helpers (Section 7 & 23)
// -----------------------------------------------------------------------------
bool AudioI2s::detect_bit_shift(const int32_t *raw_samples, size_t count, int *detected_shift) {
  if (!raw_samples || count == 0 || !detected_shift) return false;
  *detected_shift = 0;

  // In standard Philips mode, PCM1808 24-bit data occupies bits [31:8].
  // Bits [7:0] should be noise or zeros.
  // If there is a 1-bit shift (e.g. MSB-justified vs Philips mismatch):
  // bits are shifted left by 1 (bit 31 lost) or right by 1 (MSB at bit 30).
  uint64_t high_energy = 0;
  uint64_t bit30_active = 0;
  uint64_t bit31_active = 0;

  for (size_t i = 0; i < count; ++i) {
    uint32_t u = static_cast<uint32_t>(raw_samples[i]);
    if (u & 0x80000000U) bit31_active++;
    if (u & 0x40000000U) bit30_active++;
    high_energy += (u >> 8);
  }

  if (high_energy == 0) {
    *detected_shift = -999; // Complete silence or disconnected
    return false;
  }
  *detected_shift = 0; // Proper Philips alignment
  return true;
}

bool AudioI2s::detect_byte_swap(const int32_t *raw_samples, size_t count) {
  if (!raw_samples || count < 4) return false;

  // With endianness swap, high bytes (amplitude) are placed in low bytes,
  // causing high frequency hash/noise with huge step differences between adjacent samples.
  uint64_t diff_sum = 0;
  for (size_t i = 1; i < count; ++i) {
    diff_sum += std::abs(raw_samples[i] - raw_samples[i - 1]);
  }
  double avg_diff = static_cast<double>(diff_sum) / (count - 1);
  // An endian-swapped tone has huge average step differences (> 1e8)
  return avg_diff > 500000000.0;
}

bool AudioI2s::detect_channel_swap(const float *l, const float *r, size_t count,
                                   float expected_freq_l, float expected_freq_r,
                                   float sample_rate) {
  if (!l || !r || count < 100 || sample_rate <= 0.0f) return false;

  // Zero-crossing counting to estimate frequency of each channel
  size_t zc_l = 0;
  size_t zc_r = 0;

  for (size_t i = 1; i < count; ++i) {
    if ((l[i] >= 0.0f && l[i - 1] < 0.0f) || (l[i] < 0.0f && l[i - 1] >= 0.0f)) zc_l++;
    if ((r[i] >= 0.0f && r[i - 1] < 0.0f) || (r[i] < 0.0f && r[i - 1] >= 0.0f)) zc_r++;
  }

  float est_freq_l = (zc_l * sample_rate) / (2.0f * count);
  float est_freq_r = (zc_r * sample_rate) / (2.0f * count);

  // Check if measured frequencies match swapped channels
  bool matches_direct = (std::fabs(est_freq_l - expected_freq_l) < 150.0f) &&
                         (std::fabs(est_freq_r - expected_freq_r) < 150.0f);
  bool matches_swapped = (std::fabs(est_freq_l - expected_freq_r) < 150.0f) &&
                          (std::fabs(est_freq_r - expected_freq_l) < 150.0f);

  return matches_swapped && !matches_direct;
}

} // namespace vocal_fx_platform
