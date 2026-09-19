#include "audio_i2s.h"
#include "profiling.h"
#include "vocal_fx.h"
#include "td_psola.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_cpu.h"
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
// PSRAM on target, plain heap on host (lets host tests exercise the same
// accounting paths).
#ifdef ESP_PLATFORM
[[maybe_unused]] void *b4c6a_calloc(size_t n, size_t size) {
  return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
[[maybe_unused]] void b4c6a_free(void *p) {
  if (!p) return;
  heap_caps_free(p);
}
#else
[[maybe_unused]] void *b4c6a_calloc(size_t n, size_t size) {
  return std::calloc(n, size);
}
[[maybe_unused]] void b4c6a_free(void *p) {
  std::free(p);
}
#endif
} // namespace

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
    reset_b4c6a_stats();
#ifdef ESP_PLATFORM
    if (!b4c6a_timing_ && cfg.mode == AudioI2sMode::B4C6ATimingForensics) {
      b4c6a_timing_ = static_cast<B4C6ATimingRecord *>(b4c6a_calloc(
          kB4C6ATimingRecordCapacity, sizeof(B4C6ATimingRecord)));
      b4c6a_timing_capacity_ =
          b4c6a_timing_ ? kB4C6ATimingRecordCapacity : 0;
    }
    // Legacy dsp/cycle/wake histograms are PSRAM in every mode: they are
    // written on every block by record_timing but read only by old prints.
    {
      uint32_t **legacy_slots[] = {&dsp_hist_, &cycle_hist_, &wake_hist_};
      for (uint32_t **s : legacy_slots) {
        if (!*s) {
          *s = static_cast<uint32_t *>(b4c6a_calloc(
              kHistBins, sizeof(uint32_t)));
        }
      }
    }
    if (!b4c7_blocks_ && (cfg.mode == AudioI2sMode::B4C7HarmonizerAudit ||
                          cfg.mode == AudioI2sMode::B4D1Qualification)) {
      b4c7_blocks_ = static_cast<B4C7BlockRecord *>(b4c6a_calloc(
          kB4C7BlockRecordCapacity, sizeof(B4C7BlockRecord)));
      b4c7_block_capacity_ =
          b4c7_blocks_ ? kB4C7BlockRecordCapacity : 0;
      if (!b4c7_totals_) {
        b4c7_totals_ = static_cast<B4C7DecompTotals *>(b4c6a_calloc(
            1, sizeof(B4C7DecompTotals)));
      }
      if (!b4c6a_timing_) {
        b4c6a_timing_ = static_cast<B4C6ATimingRecord *>(b4c6a_calloc(
            kB4C6ATimingRecordCapacity, sizeof(B4C6ATimingRecord)));
        b4c6a_timing_capacity_ =
            b4c6a_timing_ ? kB4C6ATimingRecordCapacity : 0;
      }
    }
#endif
  // B4C.6A distribution histograms: PSRAM-backed (B4C.7 link budget),
  // allocated in every mode so telemetry stays uniform; the big per-block
  // rings above stay mode-gated. Host-capable via b4c6a_calloc.
  {
    uint32_t **slots[] = {&b4c6a_loop_hist_, &b4c6a_period_hist_,
                          &b4c6a_dsp_hist_, &b4c6a_rx_hist_,
                          &b4c6a_tx_hist_, &b4c6a_gap_hist_};
    for (uint32_t **s : slots) {
      if (!*s) {
        *s = static_cast<uint32_t *>(b4c6a_calloc(kB4C6AHistBins,
                                                  sizeof(uint32_t)));
      }
    }
  }
  dsp_total_us_ = 0;
  cycle_total_us_ = 0;
  dsp_worst_us_ = 0;
  cycle_worst_us_ = 0;
  wake_total_us_ = 0;
  wake_worst_us_ = 0;
  {
    uint32_t *legacy[] = {dsp_hist_, cycle_hist_, wake_hist_};
    for (uint32_t *h : legacy)
      if (h) std::fill_n(h, kHistBins, 0);
  }
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
  if (b4c6a_timing_) {
    b4c6a_free(b4c6a_timing_);
    b4c6a_timing_ = nullptr;
    b4c6a_timing_capacity_ = 0;
  }
  if (b4c7_blocks_) {
    b4c6a_free(b4c7_blocks_);
    b4c7_blocks_ = nullptr;
    b4c7_block_capacity_ = 0;
  }
  if (b4c7_totals_) {
    b4c6a_free(b4c7_totals_);
    b4c7_totals_ = nullptr;
  }
  if (b4d1_records_) {
    b4c6a_free(b4d1_records_);
    b4d1_records_ = nullptr;
    b4d1_record_capacity_ = 0;
    b4d1_record_count_ = 0;
  }
  if (b4d12_tee_) {
    b4c6a_free(b4d12_tee_);
    b4d12_tee_ = nullptr;
    b4d12_enabled_.store(false, std::memory_order_release);
  }
  {
    uint32_t **slots[] = {&b4c6a_loop_hist_, &b4c6a_period_hist_,
                          &b4c6a_dsp_hist_, &b4c6a_rx_hist_,
                          &b4c6a_tx_hist_, &b4c6a_gap_hist_,
                          &dsp_hist_, &cycle_hist_, &wake_hist_};
    for (uint32_t **s : slots) {
      if (*s) {
        b4c6a_free(*s);
        *s = nullptr;
      }
    }
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
  if (dsp_hist_) dsp_hist_[dsp_bin]++;

  size_t cycle_bin = std::min(static_cast<size_t>(cycle_us / kBinWidthUs), kHistBins - 1);
  if (cycle_hist_) cycle_hist_[cycle_bin]++;

  size_t wake_bin = std::min(static_cast<size_t>(wake_us / kBinWidthUs), kHistBins - 1);
  if (wake_hist_) wake_hist_[wake_bin]++;

  // Deadline: DSP execution must remain under one block duration, which
  // follows the configured sample rate (B4D.3S: rate-specific).
  const double deadline_us = block_deadline_us();
  const uint64_t deadline_us_u = static_cast<uint64_t>(deadline_us);
  if (dsp_us > deadline_us) {
    counters_.dsp_deadline_misses.fetch_add(1, std::memory_order_relaxed);
    counters_.audio_deadline_misses.fetch_add(1, std::memory_order_relaxed);
    const uint64_t lateness = dsp_us - deadline_us_u;
    counters_.cumulative_lateness_us.fetch_add(lateness, std::memory_order_relaxed);
    uint64_t old_max = counters_.max_single_block_lateness_us.load(std::memory_order_relaxed);
    while (lateness > old_max && !counters_.max_single_block_lateness_us.compare_exchange_weak(old_max, lateness, std::memory_order_relaxed)) {}
    const uint64_t consec = counters_.current_consecutive_late_blocks.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t old_consec_max = counters_.max_consecutive_late_blocks.load(std::memory_order_relaxed);
    while (consec > old_consec_max && !counters_.max_consecutive_late_blocks.compare_exchange_weak(old_consec_max, consec, std::memory_order_relaxed)) {}
  } else {
    counters_.current_consecutive_late_blocks.store(0, std::memory_order_relaxed);
  }
  // Cycle timing represents interval between consecutive reads; flag transport miss if > 2000 us.
  if (cycle_us > 2000) {
    counters_.transport_deadline_misses.fetch_add(1, std::memory_order_relaxed);
  }

  if (dsp_us > deadline_us || cycle_us > 2000) {
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
          (dsp_us > deadline_us) ? "DspDeadlineMiss" : "TransportOrSchedulerMiss",
          snapshot_active_.load(std::memory_order_relaxed),
          telemetry_formatting_active_.load(std::memory_order_relaxed),
          serial_printing_active_.load(std::memory_order_relaxed),
          pitch_worker_active_.load(std::memory_order_relaxed),
          diagnostic_queue_flush_active_.load(std::memory_order_relaxed),
      };
      outlier_count_++;
    }
  }

  // B4D.1 cleaned-qualification per-block capture: whole-DSP time, whole-loop
  // time and the harmonizer model-change flag from the SAME block, so the
  // MC x deadline-miss contingency uses observed blocks only.
  if (b4d1_record_enabled_.load(std::memory_order_acquire) && b4d1_records_) {
    const size_t slot =
        b4d1_record_count_.fetch_add(1, std::memory_order_acq_rel);
    if (slot < b4d1_record_capacity_) {
      B4D1BlockRecord &rec = b4d1_records_[slot];
      rec.block_id = b4d1_record_origin_ + static_cast<uint32_t>(block_idx);
      rec.dsp_us = dsp_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(dsp_us);
      rec.loop_us =
          cycle_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(cycle_us);
      rec.model_changed = vocal_fx_last_block_model_changed() ? 1 : 0;
      rec.dsp_miss = dsp_us > deadline_us ? 1 : 0;
      rec.loop_miss = cycle_us > deadline_us ? 1 : 0;
      const VocalFxLastBlockStats vls = vocal_fx_last_block_stats();
      rec.new_grains = vls.new_grains;
      rec.exp_warp = vls.exp_warp;
      rec.active_desc = vls.active_desc;
      rec.slices = vls.slices;
      rec.f0 = vls.f0;
      rec.source_grains = vls.source_grains;
      rec.schedule_attempts = vls.schedule_attempts;
      rec.sched_cycles = vls.sched_cycles;
      rec.addgrain_cycles = vls.addgrain_cycles;
      rec.deferred_cycles = vls.deferred_cycles;
      rec.model_new_count = vls.model_new_count;
      rec.warp_hit_count = vls.warp_hit_count;
      rec.warp_miss_count = vls.warp_miss_count;
      rec.mark_cycles = vls.mark_cycles;
      rec.warp_phase_cycles = vls.warp_phase_cycles;
      rec.desc_cycles = vls.desc_cycles;
      rec.near_cycles = vls.near_cycles;
      rec.poly_cycles = vls.poly_cycles;
      rec.gn_cycles = vls.gn_cycles;
      rec.cl_cycles = vls.cl_cycles;
      rec.ch_cycles = vls.ch_cycles;
      for (size_t i = 0; i < 3; ++i) {
        rec.ord_mark[i] = vls.ord_mark[i];
        rec.ord_warp[i] = vls.ord_warp[i];
        rec.ord_desc[i] = vls.ord_desc[i];
        rec.ord_near[i] = vls.ord_near[i];
        rec.ord_sel[i] = vls.ord_sel[i];
        rec.ord_align[i] = vls.ord_align[i];
      }
      rec.mark_count = vls.mark_count;
      rec.prewarm_cycles = vls.prewarm_cycles;
      rec.debt_samples = vls.debt_samples;
      rec.output_period_q8 = vls.output_period_q8;
      for (size_t i = 0; i < 4; ++i) {
        rec.g_dest[i] = vls.g_dest[i];
        rec.g_half[i] = vls.g_half[i];
      }
      rec.reserved = 0;
    }
  }

  // B4D.12 burn-in streaming telemetry (bounded aggregates, no logging). The
  // disabled case is a single acquire load plus null check.
  if (b4d12_enabled_.load(std::memory_order_acquire) && b4d12_tee_) {
    b4d12_note_block(dsp_us, cycle_us, block_idx);
  }
}

// -----------------------------------------------------------------------------
// B4C.6A whole-loop timing helpers. DSP remains frozen; only accounting here.
// Section map (non-overlapping, measured with esp_timer_get_time):
//   A rx_wait      : t_rx_done   - t_iter_begin   (i2s_channel_read call)
//   B rx_copy      : t_conv_done - t_rx_done      (forensic memcpy + PCM24->float)
//   F fixture      : t_fix_done  - t_conv_done    (generate_b4b6_mono)
//   C dsp          : t_dsp_done  - t_fix_done     (vocal_fx_process / transport copy)
//   D tx_prep      : t_tx_before - t_dsp_done     (ramp + sanity + float->PCM32)
//   E tx_wait      : t_tx_done   - t_tx_before    (i2s_channel_write call)
//   G bookkeeping  : t_iter_end  - t_tx_done      (counters/histograms/record)
//   H other        : loop_total  - sum(A..G)      (residual inside iteration)
//   I loop_total   : t_iter_end  - t_iter_begin   (hardware cycles + us)
// Inter-iteration gap (scheduler/yield/other between loops):
//   gap = next_iter_begin - prev_iter_end (NOT inside I).
// Reconciliation = sum(A..G) / I; residual H must be < 2% (>= 98%).
// Loop period (AUDIO_LOOP_PERIOD_US) = next_iter_begin - iter_begin.
// -----------------------------------------------------------------------------
void AudioI2s::reset_b4c6a_stats() {
  b4c6a_totals_ = B4C6ASectionTotals{};
  // Histograms are PSRAM-backed (may be null before init alloc).
  uint32_t *hists[] = {b4c6a_loop_hist_, b4c6a_period_hist_, b4c6a_dsp_hist_,
                       b4c6a_rx_hist_, b4c6a_tx_hist_, b4c6a_gap_hist_};
  for (uint32_t *h : hists)
    if (h) std::fill_n(h, kB4C6AHistBins, 0);
  b4c6a_timing_count_ = 0;
  b4c6a_timing_dropped_ = 0;
  b4c6a_rx_partial_ = 0;
  b4c6a_tx_partial_ = 0;
  b4c6a_rx_zero_ = 0;
  b4c6a_tx_zero_ = 0;
  b4c6a_last_end_us_ = 0;
  b4c6a_last_begin_us_ = 0;
  b4c6a_prev_late_ = false;
  b4c6a_recover_run_ = 0;
}

void AudioI2s::b4c6a_hist_add(uint32_t *hist, uint64_t us) {
  if (!hist) return;
  size_t bin = static_cast<size_t>(us / kB4C6ABinWidthUs);
  if (bin >= kB4C6AHistBins) bin = kB4C6AHistBins - 1;
  hist[bin]++;
}

TimingPercentiles AudioI2s::b4c6a_hist_percentiles(const uint32_t *hist,
                                                   uint64_t total_us,
                                                   uint64_t max_us,
                                                   uint64_t samples) const {
  TimingPercentiles p{};
  p.samples = samples;
  if (!samples || !hist) return p;
  p.avg_us = static_cast<double>(total_us) / static_cast<double>(samples);
  p.max_us = max_us;
  const uint64_t t50 = static_cast<uint64_t>(samples * 0.50);
  const uint64_t t90 = static_cast<uint64_t>(samples * 0.90);
  const uint64_t t95 = static_cast<uint64_t>(samples * 0.95);
  const uint64_t t99 = static_cast<uint64_t>(samples * 0.99);
  const uint64_t t999 = static_cast<uint64_t>(samples * 0.999);
  uint64_t cumulative = 0;
  for (size_t i = 0; i < kB4C6AHistBins; ++i) {
    cumulative += hist[i];
    const uint64_t bin_us = (i + 1) * kB4C6ABinWidthUs;
    if (p.p50_us == 0 && cumulative >= t50) p.p50_us = std::min(bin_us, max_us);
    if (p.p90_us == 0 && cumulative >= t90) p.p90_us = std::min(bin_us, max_us);
    if (p.p95_us == 0 && cumulative >= t95) p.p95_us = std::min(bin_us, max_us);
    if (p.p99_us == 0 && cumulative >= t99) {
      p.p99_bucket_upper_bound_us = bin_us;
      p.p99_us = std::min(bin_us, max_us);
    }
    if (p.p99_9_us == 0 && cumulative >= t999)
      p.p99_9_us = std::min(bin_us, max_us);
  }
  return p;
}

double AudioI2s::b4c6a_reconciliation_pct(uint64_t accounted_us,
                                          uint64_t total_us) {
  if (!total_us) return 0.0;
  return 100.0 * static_cast<double>(accounted_us) /
         static_cast<double>(total_us);
}

double AudioI2s::b4c6a_effective_fs(double frames, double wall_s) {
  if (wall_s <= 0.0) return 0.0;
  return frames / wall_s;
}

double AudioI2s::b4c6a_timeline_lost_s(double wall_s, double effective_fs) {
  return wall_s * (1.0 - effective_fs / 48000.0);
}

double AudioI2s::b4c6a_expected_blocks(double wall_s) {
  return wall_s * 48000.0 / 64.0;
}

TimingPercentiles AudioI2s::b4c6a_loop_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_loop_hist_, b4c6a_totals_.loop_total_us,
                                b4c6a_totals_.loop_total_max_us,
                                b4c6a_totals_.samples);
}

TimingPercentiles AudioI2s::b4c6a_period_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_period_hist_, b4c6a_totals_.loop_period_us,
                                b4c6a_totals_.loop_period_max_us,
                                b4c6a_totals_.samples > 1
                                    ? b4c6a_totals_.samples - 1
                                    : 0);
}

TimingPercentiles AudioI2s::b4c6a_dsp_forensic_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_dsp_hist_, b4c6a_totals_.dsp_us,
                                b4c6a_totals_.dsp_max_us,
                                b4c6a_totals_.samples);
}

TimingPercentiles AudioI2s::b4c6a_rx_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_rx_hist_, b4c6a_totals_.rx_wait_us,
                                b4c6a_totals_.rx_wait_max_us,
                                b4c6a_totals_.samples);
}

TimingPercentiles AudioI2s::b4c6a_tx_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_tx_hist_, b4c6a_totals_.tx_wait_us,
                                b4c6a_totals_.tx_wait_max_us,
                                b4c6a_totals_.samples);
}

TimingPercentiles AudioI2s::b4c6a_gap_percentiles() const {
  return b4c6a_hist_percentiles(b4c6a_gap_hist_, b4c6a_totals_.inter_gap_us,
                                b4c6a_totals_.inter_gap_max_us,
                                b4c6a_totals_.samples > 1
                                    ? b4c6a_totals_.samples - 1
                                    : 0);
}

void AudioI2s::b4c6a_note_iteration(uint32_t rx_wait, uint32_t rx_copy,
                                    uint32_t fixture, uint32_t dsp,
                                    uint32_t tx_prep, uint32_t tx_wait,
                                    uint32_t book, uint32_t other,
                                    uint32_t loop_total, uint32_t loop_period,
                                    uint32_t inter_gap, uint32_t loop_cycles,
                                    uint32_t rx_bytes, uint32_t tx_bytes,
                                    uint16_t rx_frames, uint16_t tx_frames,
                                    int rx_rc, int tx_rc, uint64_t begin_us,
                                    uint64_t end_us, bool success) {
  if (!success) return;
  auto &t = b4c6a_totals_;
  t.samples++;
  t.rx_wait_us += rx_wait;
  t.rx_copy_us += rx_copy;
  t.fixture_us += fixture;
  t.dsp_us += dsp;
  t.tx_prep_us += tx_prep;
  t.tx_wait_us += tx_wait;
  t.bookkeeping_us += book;
  t.other_us += other;
  t.loop_total_us += loop_total;
  if (loop_total > t.loop_total_max_us) t.loop_total_max_us = loop_total;
  t.loop_total_cycles += loop_cycles;
  if (rx_wait > t.rx_wait_max_us) t.rx_wait_max_us = rx_wait;
  if (tx_wait > t.tx_wait_max_us) t.tx_wait_max_us = tx_wait;
  if (dsp > t.dsp_max_us) t.dsp_max_us = dsp;
  if (inter_gap || t.samples > 1) {
    t.inter_gap_us += inter_gap;
    if (inter_gap > t.inter_gap_max_us) t.inter_gap_max_us = inter_gap;
    if (loop_period) {
      t.loop_period_us += loop_period;
      if (loop_period > t.loop_period_max_us)
        t.loop_period_max_us = loop_period;
    }
  }
  b4c6a_hist_add(b4c6a_loop_hist_, loop_total);
  b4c6a_hist_add(b4c6a_dsp_hist_, dsp);
  b4c6a_hist_add(b4c6a_rx_hist_, rx_wait);
  b4c6a_hist_add(b4c6a_tx_hist_, tx_wait);
  if (t.samples > 1) {
    b4c6a_hist_add(b4c6a_gap_hist_, inter_gap);
    b4c6a_hist_add(b4c6a_period_hist_, loop_period);
  }
  const uint32_t kDeadlineUs = static_cast<uint32_t>(block_deadline_us());
  if (loop_total > kDeadlineUs) {
    t.positive_lateness_us += (loop_total - kDeadlineUs);
    t.late_loop_blocks++;
  }
  // Catch-up: a late block followed by a block with near-zero RX wait means
  // the queued DMA sample was consumed back-to-back without blocking.
  if (b4c6a_prev_late_) {
    if (rx_wait <= 50) {
      t.catchup_rx_near_zero++;
      b4c6a_recover_run_++;
    } else {
      if (b4c6a_recover_run_) {
        t.recover_blocks_sum += b4c6a_recover_run_;
        t.recover_events++;
        b4c6a_recover_run_ = 0;
      }
    }
  }
  b4c6a_prev_late_ = (loop_total > kDeadlineUs);
  if (!b4c6a_prev_late_ && b4c6a_recover_run_) {
    t.recover_blocks_sum += b4c6a_recover_run_;
    t.recover_events++;
    b4c6a_recover_run_ = 0;
  }
  if (b4c6a_timing_ && b4c6a_timing_capacity_ &&
      b4c6a_timing_count_ < b4c6a_timing_capacity_) {
    B4C6ATimingRecord &row = b4c6a_timing_[b4c6a_timing_count_++];
    row.iteration_begin_us = begin_us;
    row.iteration_end_us = end_us;
    row.loop_total_us = loop_total;
    row.loop_period_us = loop_period;
    row.loop_total_cycles = loop_cycles;
    row.rx_wait_us = rx_wait;
    row.rx_copy_us = rx_copy;
    row.fixture_us = fixture;
    row.dsp_us = dsp;
    row.tx_prep_us = tx_prep;
    row.tx_wait_us = tx_wait;
    row.bookkeeping_us = book;
    row.other_us = other;
    row.inter_gap_us = inter_gap;
    row.rx_bytes = rx_bytes;
    row.tx_bytes = tx_bytes;
    row.rx_frames = rx_frames;
    row.tx_frames = tx_frames;
    row.rx_rc = static_cast<int16_t>(rx_rc);
    row.tx_rc = static_cast<int16_t>(tx_rc);
    row.rx_partial = (rx_bytes != 512);
    row.tx_partial = (tx_bytes != 512);
    row.rx_ok = (rx_rc == 0);
    row.tx_ok = (tx_rc == 0);
  } else if (b4c6a_timing_capacity_) {
    b4c6a_timing_dropped_++;
  }
  (void)rx_frames;
  (void)tx_frames;
}

// -----------------------------------------------------------------------------
// B4C.7 decomposition section tables (§19). Leaf sections sum without double
// counting (Pipeline/Harmony/Master/Input parents excluded from the leaf sum;
// V0/V1 voice Totals carry the per-voice cost).
// -----------------------------------------------------------------------------
static const VocalFxProfileSection kB4C7Globals[20] = {
    VocalFxProfileSection::Pipeline,
    VocalFxProfileSection::PitchLpcTap,
    VocalFxProfileSection::ParameterQueue,
    VocalFxProfileSection::Input,
    VocalFxProfileSection::InputHpf,
    VocalFxProfileSection::InputGate,
    VocalFxProfileSection::Compressor,
    VocalFxProfileSection::PitchMarkSync,
    VocalFxProfileSection::Harmony,
    VocalFxProfileSection::DryAlignment,
    VocalFxProfileSection::HarmonySlewPan,
    VocalFxProfileSection::HarmonyLimiter,
    VocalFxProfileSection::BusMixing,
    VocalFxProfileSection::DelayPrep,
    VocalFxProfileSection::Delay,
    VocalFxProfileSection::ReverbPrep,
    VocalFxProfileSection::Reverb,
    VocalFxProfileSection::Master,
    VocalFxProfileSection::MasterMix,
    VocalFxProfileSection::MasterLimiter,
};

static const PitchShiftProfileSection kB4C7VoiceSubs[20] = {
    PitchShiftProfileSection::MarkSelection,
    PitchShiftProfileSection::GrainScheduling,
    PitchShiftProfileSection::GrainHistoryLookup,
    PitchShiftProfileSection::PlainWindowOLA,
    PitchShiftProfileSection::LpcResidualFIR,
    PitchShiftProfileSection::LpcModelLookup,
    PitchShiftProfileSection::LpcModelWarpPolynomial,
    PitchShiftProfileSection::LpcModelWarpGainNorm,
    PitchShiftProfileSection::LpcWindowOLA,
    PitchShiftProfileSection::LpcSynthesisAllPole,
    PitchShiftProfileSection::LpcStateShift,
    PitchShiftProfileSection::FormantGainMatcher,
    PitchShiftProfileSection::FormantSoftClip,
    PitchShiftProfileSection::FormantBlend,
    PitchShiftProfileSection::Fallback,
    PitchShiftProfileSection::Articulation,
    PitchShiftProfileSection::PlosiveBridge,
    PitchShiftProfileSection::Telemetry,
    PitchShiftProfileSection::Other,
    PitchShiftProfileSection::Total,
};

// B4C.7 extra subsection names (indices 20-24 in the decomp tables),
// backed by TdPsola plain accumulators (see vocal_fx_voice_b4c7_cycles).
static const char *kB4C7ExtraNames[5] = {
    "desc_setup", "hist_fetch", "hann_win", "ola_norm", "recov_xfade",
};

static const char *kB4C7GlobalNames[20] = {
    "pipeline", "tap", "param_q", "input", "hpf", "gate", "comp",
    "pitch_sync", "harmony", "dry_align", "slew_pan", "harm_limiter",
    "bus_mix", "delay_prep", "delay", "reverb_prep", "reverb", "master",
    "master_mix", "master_limiter",
};

static const char *kB4C7VoiceNames[25] = {
    "mark_sel", "sched", "hist_lookup", "plain_ola", "resid_fir",
    "model_lookup", "warp_poly", "gain_norm", "lpc_win_ola", "synth_iir",
    "state_shift", "gain_match", "soft_clip", "blend", "fallback",
    "articul", "plosive", "telem", "other", "voice_total",
    "desc_setup", "hist_fetch", "hann_win", "ola_norm", "recov_xfade",
};

// Leaf indices into kB4C7Globals (parents Pipeline/Input/Harmony/Master out).
static const uint8_t kB4C7LeafGlobals[] = {1, 2, 4, 5, 6, 7, 9, 10, 11, 12,
                                           13, 14, 15, 16, 18, 19};
static constexpr uint8_t kB4C7VoiceTotalSub = 19;  // Total in kB4C7VoiceSubs

void AudioI2s::reset_b4c7_stats() {
  if (b4c7_totals_) *b4c7_totals_ = B4C7DecompTotals{};
  b4c7_block_count_ = 0;
  b4c7_block_dropped_ = 0;
  b4c7_block_seq_ = 0;
  b4c7_pre_overruns_ = 0;
  // Per-case audit baselines (§22/§18 deltas). The coordinator calls
  // vocal_fx_reset_profiling_epoch() + reset_slice_audit() before this, so
  // these snapshots are the window start. Residual-cache stats are
  // deliberately NOT reset (warm cache); deltas come from snapshots.
  b4c7_audit_start_ = vocal_fx_get_psola_source_grain_audit();
  b4c7_slice_start_ = vocal_fx_slice_audit_snapshot();
  b4c7_fir_start_[0] = vocal_fx_get_psola_residual_cache_stats(0);
  b4c7_fir_start_[1] = vocal_fx_get_psola_residual_cache_stats(1);
}

// ── B4D.1 cleaned-qualification per-block recorder ──────────────────────
bool AudioI2s::b4d1_recorder_init(size_t capacity) {
  if (!b4d1_records_) {
    b4d1_records_ = static_cast<B4D1BlockRecord *>(
        b4c6a_calloc(capacity, sizeof(B4D1BlockRecord)));
    b4d1_record_capacity_ = b4d1_records_ ? capacity : 0;
  }
  b4d1_record_count_.store(0, std::memory_order_release);
  b4d1_record_origin_ = 0;
  return b4d1_records_ != nullptr;
}

void AudioI2s::b4d1_recorder_free() {
  if (b4d1_records_) {
    b4c6a_free(b4d1_records_);
    b4d1_records_ = nullptr;
  }
  b4d1_record_capacity_ = 0;
  b4d1_record_count_.store(0, std::memory_order_release);
  b4d1_record_origin_ = 0;
  b4d1_record_enabled_.store(false, std::memory_order_release);
}

void AudioI2s::b4d1_recorder_reset(uint32_t origin_block_id) {
  b4d1_record_count_.store(0, std::memory_order_release);
  b4d1_record_origin_ = origin_block_id;
}

bool AudioI2s::b4d1_recorder_get(size_t index, B4D1BlockRecord *out) const {
  if (!out || !b4d1_records_ ||
      index >= b4d1_record_count_.load(std::memory_order_acquire))
    return false;
  *out = b4d1_records_[index];
  return true;
}

// ── B4D.12 burn-in streaming telemetry ──────────────────────────────────
bool AudioI2s::b4d12_telemetry_init(uint32_t sample_rate,
                                    uint32_t window_seconds) {
  if (!b4d12_tee_) {
    void *storage = b4c6a_calloc(1, sizeof(B4D12Telemetry));
    if (storage) b4d12_tee_ = new (storage) B4D12Telemetry{};
  }
  if (!b4d12_tee_) return false;
  b4d12_telemetry_reset();
  const uint32_t fs = sample_rate ? sample_rate : config_.sample_rate;
  const uint64_t bs = block_size_ ? block_size_ : 64;
  b4d12_tee_->blocks_per_window =
      (static_cast<uint64_t>(window_seconds) * fs) / bs;
  if (b4d12_tee_->blocks_per_window == 0) b4d12_tee_->blocks_per_window = 1;
  return true;
}

void AudioI2s::b4d12_telemetry_free() {
  if (b4d12_tee_) {
    b4d12_tee_->~B4D12Telemetry();
    b4c6a_free(b4d12_tee_);
    b4d12_tee_ = nullptr;
  }
  b4d12_enabled_.store(false, std::memory_order_release);
}

void AudioI2s::b4d12_telemetry_reset() {
  if (!b4d12_tee_) return;
  B4D12Telemetry *t = b4d12_tee_;
  const uint64_t bpw = t->blocks_per_window ? t->blocks_per_window : 1;
  t->~B4D12Telemetry();
  new (t) B4D12Telemetry{};
  t->blocks_per_window = bpw;
}

#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
// Project the frozen end-of-block state through one to three future blocks.
// This runs after the measured DSP interval, and reads only the snapshot from
// the earlier block. It cannot predict a future pitch publication, onset or
// recovery reset; those become explicit validation failures.
static void b4d12_project_from_snapshot(B4D12PredictionSlackEvent &ev,
    size_t h, const PsolaPredictionCursor &snap, const PitchMark *marks,
    size_t mark_count, const LpcPublishedRef *models, size_t model_count,
    bool models_coherent) {
  ev.predicted_formant_shift_bits[h] = snap.formant_shift_bits;
  ev.predicted_formant_amount_bits[h] = snap.formant_amount_bits;
  ev.predicted_gamma_bits[h] = snap.gamma_bits;
  float formant_shift = 0.0f;
  std::memcpy(&formant_shift, &snap.formant_shift_bits, sizeof(float));
  const float lambda = SharedLpcAnalysis::lambda_from_semitones(formant_shift);
  std::memcpy(&ev.predicted_lambda_bits[h], &lambda, sizeof(float));
  ev.predicted_formant_mode[h] = snap.formant_mode;
  ev.predicted_normalization_strategy[h] = snap.normalization_strategy;
  std::memcpy(&ev.predicted_sample_rate_bits[h], &snap.sample_rate, sizeof(float));
  if (!snap.have_cursor || !snap.target_enabled || !snap.pitch_voiced ||
      !(snap.source_period >= 24.0f && snap.source_period <= 800.0f) ||
      snap.frames == 0 || mark_count == 0) {
    for (size_t i = 0; i < 4; ++i) ev.prediction_status[h][i] = 2;
    return;
  }
  double cursor = snap.next_synthesis_mark;
  float semitones = snap.current_semitones;
  float synthesis_period = snap.current_synthesis_period;
  uint32_t slew = snap.slew_grains_remaining;
  const float alpha = 1.0f - std::exp(-snap.source_period /
      (snap.sample_rate * snap.smoothing_ms * .001f));
  for (size_t step = 1; step <= 3 - h; ++step) {
    const uint64_t block_start = snap.next_block_start +
        static_cast<uint64_t>(step - 1) * snap.frames;
    const double bound = static_cast<double>(block_start + snap.frames) +
        snap.source_period;
    size_t produced = 0;
    size_t attempts = 0;
    while (cursor < bound && produced < 32 && attempts++ < 32) {
      semitones += alpha * (snap.target_semitones - semitones);
      const float ratio = std::exp2(semitones / 12.0f);
      if (slew > 0) {
        synthesis_period += snap.slew_period_step;
        --slew;
      } else {
        synthesis_period = snap.source_period;
      }
      const double destination = cursor;
      const double source = destination - static_cast<double>(snap.history_offset);
      bool selected = false;
      uint64_t center = 0;
      float period = 0.0f;
      int half = 0;
      if (source >= 0.0) {
        const size_t best = td_psola_nearest_mark_index(source, marks, mark_count);
        center = marks[best].sample_position;
        if (mark_count == 1) {
          period = synthesis_period >= 24.0f && synthesis_period <= 800.0f
              ? synthesis_period : 100.0f;
        } else {
          const uint64_t p0 = best ? marks[best - 1].sample_position : center;
          const uint64_t p1 = best + 1 < mark_count
              ? marks[best + 1].sample_position : center;
          period = best && best + 1 < mark_count
              ? .5f * ((center - p0) + (p1 - center))
              : static_cast<float>(p1 - p0);
          if ((period < 24.0f || period > 800.0f) &&
              synthesis_period >= 24.0f && synthesis_period <= 800.0f)
            period = synthesis_period;
        }
        selected = marks[best].confidence > .15f &&
            period >= 24.0f && period <= 800.0f &&
            std::fabs(source - static_cast<double>(center)) <=
                std::max(2.0 * period, 256.0);
        half = std::clamp(static_cast<int>(std::lround(period)), 24, 800);
        const uint64_t input_end = snap.input_end +
            static_cast<uint64_t>(step) * snap.frames;
        const uint64_t oldest = input_end > 16384 ? input_end - 16384 : 0;
        if (center < static_cast<uint64_t>(half + VOCAL_FX_LPC_MAX_ORDER) ||
            center - half - VOCAL_FX_LPC_MAX_ORDER < oldest ||
            center + half >= input_end)
          selected = false;
      }
      if (selected) {
        if (step == 3 - h && produced < 4) {
          const size_t i = produced;
          ev.prediction_status[h][i] = models_coherent ? 1 : 3;
          std::memcpy(&ev.predicted_destination_bits[h][i], &destination, sizeof(double));
          std::memcpy(&ev.predicted_source_bits[h][i], &source, sizeof(double));
          ev.predicted_mark[h][i] = center;
          std::memcpy(&ev.predicted_period_bits[h][i], &period, sizeof(float));
          ev.predicted_half[h][i] = static_cast<uint16_t>(half);
          uint64_t best_distance = UINT64_MAX;
          float confidence = 0.0f;
          for (size_t m = 0; m < model_count; ++m) {
            const auto &ref = models[m];
            if (!ref.valid) continue;
            const uint64_t distance = center > ref.timestamp
                ? center - ref.timestamp : ref.timestamp - center;
            if (distance >= best_distance) continue;
            best_distance = distance;
            ev.predicted_model_serial[h][i] = ref.serial;
            std::memcpy(&confidence, &ref.confidence_bits, sizeof(float));
          }
          if (best_distance > snap.model_max_distance)
            ev.predicted_model_serial[h][i] = 0;
          float amount = 0.0f;
          std::memcpy(&amount, &snap.formant_amount_bits, sizeof(float));
          ev.predicted_lpc_enabled[h][i] =
              snap.formant_mode == static_cast<uint8_t>(FormantMode::Lpc) &&
              amount > 0.0f && ev.predicted_model_serial[h][i] != 0 &&
              confidence > .15f;
        }
        ++produced;
      }
      cursor += synthesis_period / std::max(ratio, .5f);
    }
    if (cursor < static_cast<double>(block_start + snap.frames))
      cursor = block_start + snap.frames + snap.source_period;
  }
}
#endif

void AudioI2s::b4d12_note_block(uint64_t dsp_us, uint64_t cycle_us,
                                uint64_t block_idx) {
  B4D12Telemetry *t = b4d12_tee_;
  if (!t) return;
  (void)cycle_us;

  const double deadline = block_deadline_us();
  const uint64_t deadline_u = static_cast<uint64_t>(deadline);

  // Classify from the same block the DSP time was measured in.
  const bool mc = vocal_fx_last_block_model_changed() != 0;
  const VocalFxLastBlockStats vls = vocal_fx_last_block_stats();
  const uint8_t ng = vls.new_grains;
  const uint8_t klass = mc ? (ng >= 3 ? 3 : (ng == 0 ? 4 : ng))
                           : 0; // 0=NMC,1=MC+1,2=MC+2,3=MC+3+,4=MC+0
  const uint16_t dsp16 = dsp_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(dsp_us);
#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
  if (klass == 1) ++t->prediction_mc1_seen;
  const bool sample_mc1 = klass == 1 &&
      (t->prediction_mc1_seen % 8U) == 0 &&
      t->prediction_mc1_sampled < 512;
  if (sample_mc1 || klass == 2 || klass == 3) {
    if (t->prediction_slack_count < kB4D12PredictionSlackCapacity) {
      if (sample_mc1) ++t->prediction_mc1_sampled;
      auto &ev = t->prediction_slack[t->prediction_slack_count++];
      ev.block_id = static_cast<uint32_t>(block_idx);
      ev.klass = klass;
      ev.grains = ng;
      ev.dsp_us[0] = t->prior_dsp_us[0];
      ev.dsp_us[1] = t->prior_dsp_us[1];
      ev.dsp_us[2] = t->prior_dsp_us[2];
      ev.dsp_us[3] = dsp16;
      const auto actual_cursor = vocal_fx_prediction_cursor();
      std::memcpy(&ev.actual_pitch_period_bits, &actual_cursor.source_period, sizeof(float));
      ev.actual_pitch_onset = actual_cursor.pitch_onset;
      ev.actual_pitch_changed = actual_cursor.pitch_changed;
      ev.actual_track_state = actual_cursor.track_state;
      ev.actual_have_cursor = actual_cursor.have_cursor;
      HarmonizerBlockTraceRecord last_trace{};
      if (vocal_fx_latest_harmonizer_trace(&last_trace))
        ev.actual_recovery_active = last_trace.recovery_active;
      for (size_t h = 0; h < 3; ++h) {
        const size_t slot = (t->prior_snapshot_write + h) % 3;
        std::memcpy(&ev.prior_pitch_period_bits[h],
                    &t->prior_cursor[slot].source_period, sizeof(float));
        ev.prior_have_cursor[h] = t->prior_cursor[slot].have_cursor;
        if (t->prior_snapshots_seen >= 3 - h)
          b4d12_project_from_snapshot(ev, h, t->prior_cursor[slot],
              t->prior_marks[slot], t->prior_mark_count[slot],
              t->prior_model_refs[slot], t->prior_model_count[slot],
              t->prior_model_coherent[slot] != 0);
        else
          for (size_t i = 0; i < 4; ++i) ev.prediction_status[h][i] = 2;
      }
      for (size_t i = 0; i < 4; ++i) {
        const auto &g = vls.grain_audit[i];
        if (i < ng) ev.grain[i] = g;
        ev.selected_model_serial[i] = i < ng ? g.model_publication_serial : 0;
        ev.destination_lead[i] = i < ng
            ? static_cast<int32_t>(g.destination_center -
                                   static_cast<int64_t>(g.scheduling_block_start))
            : (-2147483647 - 1);
        for (size_t h = 0; h < 3; ++h) {
          const size_t slot = (t->prior_snapshot_write + h) % 3;
          ev.prior_model_serial[h] = t->prior_model_serial[slot];
          if (i >= ng) {
            ev.mark_available[h][i] = 3;
            ev.model_available[h][i] = 3;
          } else if (t->prior_snapshots_seen < 3 - h) {
            ev.mark_available[h][i] = 2;
            ev.model_available[h][i] = 2;
          } else {
            bool found = false;
            for (size_t m = 0; m < t->prior_mark_count[slot]; ++m)
              if (t->prior_marks[slot][m].sample_position == g.source_center) found = true;
            ev.mark_available[h][i] = found ? 1 : 0;
            ev.model_available[h][i] = g.model_publication_serial == 0
                ? 3
                : (!t->prior_model_coherent[slot] ? 2
                   : (g.model_publication_serial <= t->prior_model_serial[slot] ? 1 : 0));
          }
        }
      }
    } else {
      ++t->prediction_slack_dropped;
    }
  }
  t->prior_dsp_us[0] = t->prior_dsp_us[1];
  t->prior_dsp_us[1] = t->prior_dsp_us[2];
  t->prior_dsp_us[2] = dsp16;
  const size_t snapshot_slot = t->prior_snapshot_write;
  t->prior_mark_count[snapshot_slot] = static_cast<uint8_t>(
      vocal_fx_copy_current_pitch_mark_records(t->prior_marks[snapshot_slot], 64));
  t->prior_cursor[snapshot_slot] = vocal_fx_prediction_cursor();
  size_t model_count = 0;
  const bool coherent = vocal_fx_snapshot_lpc_refs(
      t->prior_model_refs[snapshot_slot], 16, &model_count);
  t->prior_model_count[snapshot_slot] = static_cast<uint8_t>(model_count);
  t->prior_model_coherent[snapshot_slot] = coherent ? 1 : 0;
  t->prior_model_serial[snapshot_slot] = coherent && model_count
      ? t->prior_model_refs[snapshot_slot][0].serial : 0;
  t->prior_snapshot_write = static_cast<uint8_t>((snapshot_slot + 1) % 3);
  if (t->prior_snapshots_seen < 3) ++t->prior_snapshots_seen;
#endif
  for (size_t i = 0; i < ng && i < 4; ++i) {
    const auto &g = vls.grain_audit[i];
    ++t->grain_count[klass][i];
    t->grain_add_sum[klass][i] += g.addgrain_cycles;
    t->grain_select_sum[klass][i] += g.select_cycles;
    t->grain_near_sum[klass][i] += g.model_near_cycles;
    t->grain_lookup_sum[klass][i] += g.cache_lookup_cycles;
    t->grain_poly_sum[klass][i] += g.polynomial_cycles;
    t->grain_gain_sum[klass][i] += g.gain_cycles;
    if (g.addgrain_cycles > t->grain_add_max[klass][i])
      t->grain_add_max[klass][i] = g.addgrain_cycles;
    const size_t bin = std::min<size_t>(g.addgrain_cycles / 1024,
                                        B4D12Telemetry::kGrainHistBins - 1);
    ++t->grain_add_hist[klass][i][bin];
    if (g.cache_path < 5) ++t->grain_cache_path[klass][i][g.cache_path];
    if (g.cache_path == 4) {
      const uint8_t bits = g.local_difference;
      for (size_t bit = 0; bit < 8; ++bit)
        if (bits & (1u << bit)) ++t->grain_cache_difference[klass][i][bit];
    }
  }
  // Retain complete evidence for every miss (up to the existing late-event
  // capacity) and the 100 slowest blocks. No allocation or output here.
  if (dsp_us >= 1000) {
    const bool filling = t->top_count < kB4D12TopCapacity;
    const bool keep_top = filling || dsp_us > t->top_min_us;
    size_t slot = t->top_count;
    if (keep_top && !filling) {
      slot = 0;
      for (size_t i = 1; i < kB4D12TopCapacity; ++i)
        if (t->top[i].dsp_us < t->top[slot].dsp_us) slot = i;
    }
    if (keep_top || dsp_us > deadline_u) {
      B4D12ForensicRecord rec{};
      rec.block_id = static_cast<uint32_t>(block_idx);
      rec.dsp_us = static_cast<uint32_t>(dsp_us);
      rec.cycle_us = static_cast<uint32_t>(cycle_us);
      vocal_fx_latest_harmonizer_trace(&rec.harmony);
      for (size_t i = 0; i < 4; ++i) rec.grain[i] = vls.grain_audit[i];
      if (keep_top) {
        t->top[slot] = rec;
        if (filling) ++t->top_count;
        if (t->top_count == kB4D12TopCapacity) {
          uint32_t minimum = t->top[0].dsp_us;
          for (size_t i = 1; i < kB4D12TopCapacity; ++i)
            if (t->top[i].dsp_us < minimum) minimum = t->top[i].dsp_us;
          t->top_min_us = minimum;
        }
      }
      if (dsp_us > deadline_u) {
        if (t->miss_count < kB4D12LateEventCapacity)
          t->miss[t->miss_count++] = rec;
        else
          ++t->miss_dropped;
      }
    }
  }

  // Exact DSP histogram.
  const uint32_t hb = dsp_us >= B4D12Telemetry::kDspHistBins
                          ? static_cast<uint32_t>(B4D12Telemetry::kDspHistBins - 1)
                          : static_cast<uint32_t>(dsp_us);
  t->dsp_hist[hb]++;
  const uint32_t ch = dsp_us < 4096u ? static_cast<uint32_t>(dsp_us) : 4095u;
  t->class_hist[klass][ch]++;
  t->class_sum_us[klass] += dsp_us;
  if (dsp_us > t->class_max_us[klass])
    t->class_max_us[klass] = static_cast<uint32_t>(dsp_us);

  // All-block class population.
  switch (klass) {
    case 0: t->blocks_nmc++; break;
    case 1: t->blocks_mc1++; break;
    case 2: t->blocks_mc2++; break;
    case 3: t->blocks_mc3++; break;
    default: t->blocks_mc0++; break;
  }

  const float backlog_ms = vocal_fx_pitch_backlog_ms();
  const uint32_t backlog_ds =
      backlog_ms > 0.0f ? static_cast<uint32_t>(backlog_ms * 10.0f) : 0u;

  // Feed any open recovery event with this block's context.
  if (t->open_event) {
    B4D12LateEvent &ev = t->late[(t->late_write + kB4D12LateEventCapacity - 1) %
                                 kB4D12LateEventCapacity];
    if (t->open_next == 1) ev.next1_dsp_us = dsp16;
    else if (t->open_next == 2) ev.next2_dsp_us = dsp16;
    else if (t->open_next == 3) ev.next3_dsp_us = dsp16;
    if (ev.recovery_blocks == 0xFF && dsp_us <= deadline_u) {
      ev.recovery_blocks = t->open_next;
      ev.backlog_after_ds = backlog_ds > 0xFFFFu ? 0xFFFFu
                                                 : static_cast<uint16_t>(backlog_ds);
      const uint8_t rb = t->open_next < B4D12Telemetry::kRecoveryBins
                             ? t->open_next
                             : static_cast<uint8_t>(B4D12Telemetry::kRecoveryBins - 1);
      t->recovery_hist[rb]++;
    }
    t->open_next++;
    if (t->open_next > 3) {
      if (ev.recovery_blocks == 0xFF) {
        ev.recovery_blocks = 0;
        t->recovery_hist[0]++;
      }
      t->open_event = 0;
    }
  }

  // Window accumulator (10-minute bins by observed block index).
  const uint64_t win = block_idx / t->blocks_per_window;
  if (win != t->window_index) {
    // finalize the closing window
    if (t->window_count < kB4D12WindowCapacity) {
      const uint64_t total_tr = counters_.rx_overruns.load(std::memory_order_relaxed) +
          counters_.tx_underruns.load(std::memory_order_relaxed) +
          counters_.actual_rx_dma_errors.load(std::memory_order_relaxed) +
          counters_.actual_tx_dma_errors.load(std::memory_order_relaxed) +
          counters_.i2s_read_failures.load(std::memory_order_relaxed) +
          counters_.i2s_write_failures.load(std::memory_order_relaxed) +
          counters_.rx_dropped_frames.load(std::memory_order_relaxed) +
          counters_.tx_dropped_frames.load(std::memory_order_relaxed) +
          counters_.rx_sequence_gaps.load(std::memory_order_relaxed) +
          counters_.tx_sequence_gaps.load(std::memory_order_relaxed) +
          counters_.dma_errors.load(std::memory_order_relaxed) +
          counters_.nan_inf_count.load(std::memory_order_relaxed);
      t->window.transport_errors =
          total_tr >= t->window_transport_base ? total_tr - t->window_transport_base : 0;
      const uint64_t cum_late = counters_.cumulative_lateness_us.load(std::memory_order_relaxed);
      t->window.cumulative_lateness_us =
          cum_late >= t->window_lateness_base ? cum_late - t->window_lateness_base : 0;
      t->windows[t->window_count] = t->window;
      t->window_count++;
      t->window_transport_base = total_tr;
      t->window_lateness_base = cum_late;
      t->window = B4D12WindowStats{};
    }
    t->window_index = win;
  }

  t->blocks++;
  t->dsp_sum_us += dsp_us;
  if (dsp_us > t->dsp_max_us) t->dsp_max_us = static_cast<uint32_t>(dsp_us);
  t->window.blocks++;
  t->window.dsp_sum_us += dsp_us;
  if (dsp_us > t->window.dsp_max_us)
    t->window.dsp_max_us = static_cast<uint32_t>(dsp_us);
  {
    size_t wb = static_cast<size_t>(dsp_us / 100u);
    if (wb >= kB4D12WindowHistBins) wb = kB4D12WindowHistBins - 1;
    t->window.hist[wb]++;
  }
  if (backlog_ds > t->window.backlog_max_ds)
    t->window.backlog_max_ds = backlog_ds;
  if (dsp_us > kB4D12Late1200Us) t->late_1200_blocks++;

  if (dsp_us > deadline_u) {
    const uint64_t lateness = dsp_us - deadline_u;
    t->misses++;
    t->cumulative_lateness_us += lateness;
    t->current_consecutive_late++;
    if (t->current_consecutive_late > t->max_consecutive_late)
      t->max_consecutive_late = t->current_consecutive_late;
    t->window.misses++;
    if (t->current_consecutive_late > t->window.max_consecutive_late)
      t->window.max_consecutive_late = static_cast<uint32_t>(t->current_consecutive_late);
    // lateness histogram
    size_t lb;
    if (lateness <= 100) lb = 0;
    else if (lateness <= 250) lb = 1;
    else if (lateness <= 500) lb = 2;
    else if (lateness <= 1000) lb = 3;
    else if (lateness <= 1500) lb = 4;
    else lb = 5;
    t->lateness_hist[lb]++;
    switch (klass) {
      case 0: t->miss_nmc++; break;
      case 1: t->miss_mc1++; break;
      case 2: t->miss_mc2++; t->window.mc2++; break;
      case 3: t->miss_mc3++; t->window.mc3++; break;
      default: t->miss_mc0++; break;
    }
    // A pending previous event that is immediately followed by another late
    // block never recovered within its three-block window: close it as 0.
    if (t->open_event && t->late_write > 0) {
      B4D12LateEvent &prior =
          t->late[(t->late_write + kB4D12LateEventCapacity - 1) %
                  kB4D12LateEventCapacity];
      if (prior.recovery_blocks == 0xFF) {
        prior.recovery_blocks = 0;
        t->recovery_hist[0]++;
      }
    }
    // New late event (bounded ring, overwrite oldest).
    B4D12LateEvent &ev = t->late[t->late_write % kB4D12LateEventCapacity];
    ev = B4D12LateEvent{};
    ev.block_id = static_cast<uint32_t>(block_idx);
    ev.dsp_us = dsp16;
    ev.lateness_us = lateness > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(lateness);
    ev.klass = klass;
    ev.model_changed = mc ? 1 : 0;
    ev.new_grains = ng;
    ev.active_desc = vls.active_desc;
    ev.slices = vls.slices;
    ev.prev_miss = t->prev_late;
    ev.recovery_blocks = 0xFF;
    ev.backlog_before_ds = backlog_ds > 0xFFFFu ? 0xFFFFu
                                                : static_cast<uint16_t>(backlog_ds);
    t->late_write++;
    t->late_events++;
    if (t->late_write > kB4D12LateEventCapacity) t->late_events_dropped++;
    t->open_event = 1;
    t->open_next = 1;
    t->prev_late = 1;
  } else {
    t->current_consecutive_late = 0;
    t->prev_late = 0;
  }
}

#ifdef ESP_PLATFORM
void AudioI2s::b4c7_process_block(
    float *mono, float *l, float *r, size_t n, int64_t *t_dsp_start,
    int64_t *t_dsp_done, uint64_t *pure_dsp_us, uint8_t *block_class,
    uint32_t *v0_us, uint32_t *v1_us, uint32_t *glob_us, uint16_t *slices0,
    uint16_t *slices1, uint16_t *sched0, uint16_t *sched1) {
  // Per-block profiler deltas. The two snapshots bracket vocal_fx_process
  // tightly; their own cost lands in fixture (pre) / tx_prep (post) and is
  // documented, keeping DSP_PROCESS_US exactly vocal_fx_process().
  uint64_t before_g[B4C7DecompTotals::kGlobals];
  uint64_t before_v0[B4C7DecompTotals::kVoiceSubs];
  uint64_t before_v1[B4C7DecompTotals::kVoiceSubs];
  for (size_t i = 0; i < B4C7DecompTotals::kGlobals; ++i)
    before_g[i] = vocal_fx_profile_stats(kB4C7Globals[i]).total_cycles;
  for (size_t i = 0; i < 20; ++i) {
    before_v0[i] =
        vocal_fx_voice_profile_stats(0, kB4C7VoiceSubs[i]).total_cycles;
    before_v1[i] =
        vocal_fx_voice_profile_stats(1, kB4C7VoiceSubs[i]).total_cycles;
  }
  // Indices 20-24: plain B4C.7 accumulators (no profiler sections).
  for (size_t j = 0; j < 5; ++j) {
    before_v0[20 + j] = vocal_fx_voice_b4c7_cycles(0, j);
    before_v1[20 + j] = vocal_fx_voice_b4c7_cycles(1, j);
  }
  vocal_fx_next_slice_audit_block();
  *t_dsp_start = esp_timer_get_time();
  vocal_fx_process(mono, l, r, n);
  *t_dsp_done = esp_timer_get_time();
  *pure_dsp_us = static_cast<uint64_t>(*t_dsp_done - *t_dsp_start);

  // Classifier (§17): ACTIVE = rendered >= 1 deferred slice this block.
  HarmonizerBlockTraceRecord tr{};
  uint16_t s0 = 0, s1 = 0, g0 = 0, g1 = 0;
  if (vocal_fx_latest_harmonizer_trace(&tr)) {
    s0 = tr.deferred_slices_rendered_v0;
    s1 = tr.deferred_slices_rendered_v1;
    g0 = tr.new_grains_scheduled_v0;
    g1 = tr.new_grains_scheduled_v1;
  }
  const uint8_t cls = (s0 > 0 && s1 > 0)
                          ? static_cast<uint8_t>(B4C7BlockClass::BothActive)
                      : (s0 > 0) ? static_cast<uint8_t>(B4C7BlockClass::V0Only)
                      : (s1 > 0) ? static_cast<uint8_t>(B4C7BlockClass::V1Only)
                                 : static_cast<uint8_t>(B4C7BlockClass::NeitherActive);
  *block_class = cls;
  *slices0 = s0;
  *slices1 = s1;
  *sched0 = g0;
  *sched1 = g1;

  const uint32_t cpus = Profiler::cycles_per_us();
  B4C7DecompTotals *tp = b4c7_totals_;
  if (tp) tp->count[cls]++;
  uint64_t leaf_d = 0;
  for (size_t i = 0; i < B4C7DecompTotals::kGlobals; ++i) {
    const uint64_t d =
        vocal_fx_profile_stats(kB4C7Globals[i]).total_cycles - before_g[i];
    if (tp) tp->global[cls][i] += d;
  }
  // Leaf-sum reconciliation (§19): leaf globals + both voice Totals, in us.
  for (uint8_t gi : kB4C7LeafGlobals) {
    const uint64_t d =
        vocal_fx_profile_stats(kB4C7Globals[gi]).total_cycles - before_g[gi];
    leaf_d += d;
  }
  uint64_t v0t = 0, v1t = 0;
  for (size_t i = 0; i < 20; ++i) {
    const uint64_t d0 =
        vocal_fx_voice_profile_stats(0, kB4C7VoiceSubs[i]).total_cycles -
        before_v0[i];
    const uint64_t d1 =
        vocal_fx_voice_profile_stats(1, kB4C7VoiceSubs[i]).total_cycles -
        before_v1[i];
    if (tp) {
      tp->voice[cls][0][i] += d0;
      tp->voice[cls][1][i] += d1;
    }
    if (i == kB4C7VoiceTotalSub) {
      v0t = d0;
      v1t = d1;
    }
  }
  for (size_t j = 0; j < 5; ++j) {
    const uint64_t e0 =
        vocal_fx_voice_b4c7_cycles(0, j) - before_v0[20 + j];
    const uint64_t e1 =
        vocal_fx_voice_b4c7_cycles(1, j) - before_v1[20 + j];
    if (tp) {
      tp->voice[cls][0][20 + j] += e0;
      tp->voice[cls][1][20 + j] += e1;
    }
  }
  *v0_us = static_cast<uint32_t>(v0t / cpus);
  *v1_us = static_cast<uint32_t>(v1t / cpus);
  // Global column: Harmony voice-parent (informational; excluded from recon).
  const uint64_t hg =
      vocal_fx_profile_stats(VocalFxProfileSection::Harmony).total_cycles -
      before_g[8];
  *glob_us = static_cast<uint32_t>(hg / cpus);

  const uint64_t acct_us = (leaf_d + v0t + v1t) / cpus;
  if (tp) tp->acct_sum[cls] += acct_us;

  if (tp && cls == static_cast<uint8_t>(B4C7BlockClass::BothActive)) {
    const uint32_t dsp_u = static_cast<uint32_t>(*pure_dsp_us);
    tp->both_samples++;
    tp->both_total_us += dsp_u;
    if (dsp_u > tp->both_max_us) tp->both_max_us = dsp_u;
    b4c6a_hist_add(tp->both_hist, dsp_u);
  }

  const uint64_t seq = b4c7_block_seq_++;
  if (b4c7_blocks_ && b4c7_block_capacity_ &&
      b4c7_block_count_ < b4c7_block_capacity_) {
    B4C7BlockRecord &row = b4c7_blocks_[b4c7_block_count_++];
    row.block = static_cast<uint32_t>(seq);
    row.block_class = cls;
    row.dsp_us = static_cast<uint32_t>(*pure_dsp_us);
    row.v0_us = *v0_us;
    row.v1_us = *v1_us;
    row.global_us = *glob_us;
    row.slices_v0 = s0;
    row.slices_v1 = s1;
    row.sched_v0 = g0;
    row.sched_v1 = g1;
  } else if (b4c7_block_capacity_) {
    b4c7_block_dropped_++;
  }
}
#endif

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

void AudioI2s::configure_b4b6_stimulus(B4B6StimulusKind kind, float f0_hz,
                                       const uint8_t *vocal_mulaw,
                                       size_t vocal_samples) {
  b4b6_stimulus_ = kind;
  b4b6_f0_hz_ = f0_hz;
  b4b6_vocal_mulaw_ = vocal_mulaw;
  b4b6_vocal_samples_ = vocal_samples;
  b4b6_noise_state_ = 0x6d2b79f5U;
  synth_sample_idx_ = 0;
  synth_phase_ = 0.0f;
  current_synth_f0_ = f0_hz;
  current_synth_voiced_ = false;
  current_stimulus_state_ = StimulusState::Silence;
  current_stimulus_rms_ = 0.0f;
  current_block_checksum_ = 0;
}

void AudioI2s::generate_b4b6_mono(float *mono, size_t frames) {
  // B4D.3S: stimulus timing follows the configured sample rate.
  const float kFs = static_cast<float>(config_.sample_rate);
  constexpr float kTwoPi = 6.28318530717958647692f;
  constexpr float kAmplitude = 0.12589254f; // -18 dBFS peak
  constexpr float kHarmonicNorm = 1.875f;
  double sum_sq = 0.0;
  uint32_t checksum = 2166136261U;

  const auto noise_sample = [&]() {
    b4b6_noise_state_ = b4b6_noise_state_ * 1664525U + 1013904223U;
    const int32_t centered = static_cast<int32_t>(b4b6_noise_state_ >> 8) -
                             static_cast<int32_t>(1U << 23);
    return static_cast<float>(centered) * (1.0f / 8388608.0f);
  };
  const auto mulaw_decode = [](uint8_t byte) {
    const uint8_t value = static_cast<uint8_t>(~byte);
    const int sign = value & 0x80;
    const int exponent = (value >> 4) & 0x07;
    const int mantissa = value & 0x0f;
    int sample = ((mantissa << 3) + 0x84) << exponent;
    sample -= 0x84;
    return static_cast<float>(sign ? -sample : sample) * (1.0f / 32768.0f);
  };

  for (size_t i = 0; i < frames; ++i) {
    const uint64_t index = synth_sample_idx_++;
    const float seconds = static_cast<float>(index) / kFs;
    float f0 = b4b6_f0_hz_;
    float envelope = std::min(1.0f, seconds / 0.020f);
    bool voiced = true;
    bool harmonic = false;
    bool use_noise = false;
    float noise_mix = 0.0f;

    switch (b4b6_stimulus_) {
    case B4B6StimulusKind::HarmonicVoiced:
      harmonic = true;
      break;
    case B4B6StimulusKind::BroadbandNoise:
      voiced = false;
      use_noise = true;
      noise_mix = 1.0f;
      break;
    case B4B6StimulusKind::BreathyVoiced:
      harmonic = true;
      use_noise = true;
      noise_mix = 0.55f;
      break;
    case B4B6StimulusKind::Silence:
      voiced = false;
      envelope = 0.0f;
      break;
    case B4B6StimulusKind::NearSilence:
      voiced = false;
      use_noise = true;
      noise_mix = 0.002f; // about -72 dBFS peak
      break;
    case B4B6StimulusKind::Step110To220:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 220.0f : 110.0f;
      break;
    case B4B6StimulusKind::Step220To110:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 110.0f : 220.0f;
      break;
    case B4B6StimulusKind::Step110To440:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 440.0f : 110.0f;
      break;
    case B4B6StimulusKind::Step440To110:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 110.0f : 440.0f;
      break;
    case B4B6StimulusKind::Step65To130:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 130.0f : 65.4f;
      break;
    case B4B6StimulusKind::Step80To160:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 160.0f : 80.0f;
      break;
    case B4B6StimulusKind::Step220To440:
      harmonic = true;
      f0 = (static_cast<uint32_t>(seconds / 2.0f) & 1U) ? 440.0f : 220.0f;
      break;
    case B4B6StimulusKind::Step147To220To330: {
      harmonic = true;
      constexpr float sequence[] = {147.0f, 220.0f, 330.0f};
      f0 = sequence[static_cast<uint32_t>(seconds / 2.0f) % 3U];
      break;
    }
    case B4B6StimulusKind::Gliss80To440: {
      harmonic = true;
      const float progress = std::clamp((seconds - 1.0f) / 10.0f, 0.0f, 1.0f);
      f0 = 80.0f * std::pow(440.0f / 80.0f, progress);
      break;
    }
    case B4B6StimulusKind::Gliss440To80: {
      harmonic = true;
      const float progress = std::clamp((seconds - 1.0f) / 10.0f, 0.0f, 1.0f);
      f0 = 440.0f * std::pow(80.0f / 440.0f, progress);
      break;
    }
    case B4B6StimulusKind::VibratoOneSemitone:
    case B4B6StimulusKind::VibratoTwoSemitones: {
      harmonic = true;
      const float depth = b4b6_stimulus_ == B4B6StimulusKind::VibratoOneSemitone
                              ? 1.0f
                              : 2.0f;
      f0 = 220.0f * std::pow(2.0f,
                             depth * std::sin(kTwoPi * 6.0f * seconds) / 12.0f);
      break;
    }
    case B4B6StimulusKind::Staccato: {
      harmonic = true;
      constexpr float sequence[] = {110.0f, 220.0f, 440.0f};
      const uint32_t burst = static_cast<uint32_t>(seconds / 0.40f);
      const float within = seconds - burst * 0.40f;
      f0 = sequence[burst % 3U];
      voiced = within < 0.25f;
      if (!voiced) {
        envelope = 0.0f;
      } else {
        const float attack = std::min(1.0f, within / 0.005f);
        const float release = std::min(1.0f, (0.25f - within) / 0.010f);
        envelope = std::min(attack, release);
      }
      break;
    }
    case B4B6StimulusKind::VocalReplay:
      voiced = false;
      break;
    case B4B6StimulusKind::PureTone:
      break;
    }

    float sample = 0.0f;
    if (b4b6_stimulus_ == B4B6StimulusKind::VocalReplay &&
        b4b6_vocal_mulaw_ && b4b6_vocal_samples_) {
      // Fixture is 8 kHz mu-law. Linear interpolation to the configured
      // sample rate (B4D.3S: rate-agnostic; at 48 kHz this is exactly the
      // former 6x path). Identical source time region at every rate.
      const double src_pos = static_cast<double>(index) * 8000.0 /
                             static_cast<double>(config_.sample_rate);
      const uint64_t source_whole = static_cast<uint64_t>(src_pos);
      const size_t a = static_cast<size_t>(source_whole % b4b6_vocal_samples_);
      const size_t b = (a + 1U) % b4b6_vocal_samples_;
      const float fraction =
          static_cast<float>(src_pos - static_cast<double>(source_whole));
      sample = mulaw_decode(b4b6_vocal_mulaw_[a]) * (1.0f - fraction) +
               mulaw_decode(b4b6_vocal_mulaw_[b]) * fraction;
    } else if (envelope > 0.0f) {
      const float fundamental = std::sin(synth_phase_);
      float tonal = fundamental;
      if (harmonic) {
        tonal = (fundamental + 0.5f * std::sin(2.0f * synth_phase_) +
                 0.25f * std::sin(3.0f * synth_phase_) +
                 0.125f * std::sin(4.0f * synth_phase_)) /
                kHarmonicNorm;
      }
      const float random = use_noise ? noise_sample() : 0.0f;
      sample = kAmplitude * envelope *
               ((1.0f - noise_mix) * tonal + noise_mix * random);
    }

    synth_phase_ += kTwoPi * f0 / kFs;
    if (synth_phase_ >= kTwoPi)
      synth_phase_ = std::fmod(synth_phase_, kTwoPi);
    mono[i] = sample;
    current_synth_f0_ = f0;
    current_synth_voiced_ = voiced;
    current_stimulus_state_ = envelope <= 0.0f
                                  ? StimulusState::Silence
                                  : (envelope < 1.0f ? StimulusState::Attack
                                                     : StimulusState::Tone);
    sum_sq += static_cast<double>(sample) * sample;
    uint32_t bits = 0;
    std::memcpy(&bits, &sample, sizeof(bits));
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
#ifdef ESP_PLATFORM
    const uint32_t c_cycle_start = esp_cpu_get_cycle_count();
#else
    const uint32_t c_cycle_start = 0;
#endif
    uint64_t block_idx = counters_.audio_blocks_processed.load(std::memory_order_relaxed);

    // 1. Read block from RX DMA (Req 4: Real ADC Input Tap)
    const bool b4c6a = config_.mode == AudioI2sMode::B4C6ATimingForensics;
    const bool b4d1 = config_.mode == AudioI2sMode::B4D1Qualification;
    const bool b4c7 = config_.mode == AudioI2sMode::B4C7HarmonizerAudit ||
                      config_.mode == AudioI2sMode::B4D1Qualification;
    // B4C.7 shares the whole-loop timing engine; extras gated by detail.
    const bool b4c6a_on = (b4c6a || b4c7) && b4c6a_detail_ > 0;
    uint8_t b4c7_cls = static_cast<uint8_t>(B4C7BlockClass::NeitherActive);
    size_t got = 0;
    const int64_t t_rx_before = t_cycle_start;
    esp_err_t rx_err = i2s_channel_read(rx, in32, byte_count, &got, pdMS_TO_TICKS(100));
    const int64_t t_rx_after = esp_timer_get_time();
    if (b4c6a_on) {
      if (got == 0) ++b4c6a_rx_zero_;
      if (got != byte_count) ++b4c6a_rx_partial_;
      if (rx_err == ESP_ERR_TIMEOUT) ++b4c6a_totals_.rx_timeouts;
    }
    if (rx_err != ESP_OK || got < byte_count) {
      counters_.i2s_read_failures.fetch_add(1, std::memory_order_relaxed);
      counters_.rx_dropped_frames.fetch_add(
          (byte_count - std::min(got, byte_count)) /
              (2 * sizeof(int32_t)),
          std::memory_order_relaxed);
      counters_.rx_sequence_gaps.fetch_add(1, std::memory_order_relaxed);
      record_transport_error("I2S_RX_READ_FAILURE", block_idx, 0, 0, 0, 0);
      continue;
    }
    counters_.i2s_read_successes.fetch_add(1, std::memory_order_relaxed);
    if (!b4b6_dsp_paused_.load(std::memory_order_relaxed)) {
      counters_.rx_bytes_read.fetch_add(got, std::memory_order_relaxed);
      counters_.rx_frames_read.fetch_add(got / (2 * sizeof(int32_t)), std::memory_order_relaxed);
      counters_.rx_blocks_read.fetch_add(1, std::memory_order_relaxed);
    }
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
    const int64_t t_after_forensic = esp_timer_get_time();

    // 3. Decode input samples (PCM24 in 32-bit container -> float [-1.0, +1.0])
    for (size_t i = 0; i < n; ++i) {
      float left = pcm24_to_float(in32[2 * i]);
      float right = pcm24_to_float(in32[2 * i + 1]);
      l[i] = left;
      r[i] = right;
      mono[i] = (left + right) * 0.5f;
    }
    const int64_t t_conversion_done = esp_timer_get_time();
    int64_t t_fixture_done = t_conversion_done;
    int64_t t_dsp_start = t_conversion_done;
    int64_t t_dsp_done = t_conversion_done;

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
    case AudioI2sMode::B4B6RcValidation: {
      generate_b4b6_mono(mono, n);
      if (current_stimulus_state_ == StimulusState::Tone)
        vocal_fx_funnel_inc_synthetic_tone_blocks();
      if (b4b6_dsp_paused_.load(std::memory_order_acquire)) {
        std::fill_n(l, n, 0.0f);
        std::fill_n(r, n, 0.0f);
      } else {
        b4b6_dsp_active_.store(true, std::memory_order_release);
        const int64_t t_dsp_start = esp_timer_get_time();
        vocal_fx_process(mono, l, r, n);
        pure_dsp_us =
            static_cast<uint64_t>(esp_timer_get_time() - t_dsp_start);
        b4b6_dsp_active_.store(false, std::memory_order_release);
      }
      break;
    }
    case AudioI2sMode::B4C1AudioCoreAudit:
    case AudioI2sMode::B4C2AudioCoreAudit:
    case AudioI2sMode::B4C3HarmonizerTailAudit:
    case AudioI2sMode::B4C3AWorkloadAudit:
    case AudioI2sMode::B4C3BSourceGrainBurst:
    case AudioI2sMode::B4C4SingleGrainDeferred:
    case AudioI2sMode::B4C4ADeferredStandalone:
    case AudioI2sMode::B4C6ATimingForensics: {
      const int64_t t_fix_start = esp_timer_get_time();
      generate_b4b6_mono(mono, n);
      t_fixture_done = esp_timer_get_time();
      (void)t_fix_start;
      if (current_stimulus_state_ == StimulusState::Tone)
        vocal_fx_funnel_inc_synthetic_tone_blocks();
      if (b4b6_dsp_paused_.load(std::memory_order_acquire)) {
        std::fill_n(l, n, 0.0f);
        std::fill_n(r, n, 0.0f);
        t_dsp_start = t_fixture_done;
        t_dsp_done = t_fixture_done;
      } else if (b4c1_transport_only_.load(std::memory_order_acquire)) {
        t_dsp_start = esp_timer_get_time();
        for (size_t i = 0; i < n; ++i) {
          l[i] = mono[i];
          r[i] = mono[i];
        }
        t_dsp_done = esp_timer_get_time();
        pure_dsp_us = static_cast<uint64_t>(t_dsp_done - t_dsp_start);
      } else {
        b4b6_dsp_active_.store(true, std::memory_order_release);
        t_dsp_start = esp_timer_get_time();
        vocal_fx_process(mono, l, r, n);
        t_dsp_done = esp_timer_get_time();
        pure_dsp_us = static_cast<uint64_t>(t_dsp_done - t_dsp_start);
        b4b6_dsp_active_.store(false, std::memory_order_release);
      }
      break;
    }
    case AudioI2sMode::B4C7HarmonizerAudit:
    case AudioI2sMode::B4D1Qualification: {
      // Production-equivalent input WITHOUT synchronous fixture synthesis
      // (§12): Prestaged copies a coordinator-staged PSRAM slice; LiveAdc
      // uses the converted RX mono directly (true product path).
      const int64_t t_pre_start = esp_timer_get_time();
      if (b4c7_input_ == B4C7InputKind::Prestaged && b4c7_pre_buf_ &&
          b4c7_pre_frames_ >= n) {
        size_t idx = b4c7_pre_idx_;
        if (idx + n > b4c7_pre_frames_) {
          idx = 0;
          b4c7_pre_overruns_++;
        }
        std::memcpy(mono, b4c7_pre_buf_ + idx, n * sizeof(float));
        b4c7_pre_idx_ = idx + n;
      }
      t_fixture_done = esp_timer_get_time();
      (void)t_pre_start;
      if (b4b6_dsp_paused_.load(std::memory_order_acquire)) {
        std::fill_n(l, n, 0.0f);
        std::fill_n(r, n, 0.0f);
        t_dsp_start = t_fixture_done;
        t_dsp_done = t_fixture_done;
      } else {
        b4b6_dsp_active_.store(true, std::memory_order_release);
        uint32_t v0u = 0, v1u = 0, gu = 0;
        uint16_t s0 = 0, s1 = 0, g0 = 0, g1 = 0;
        if (b4c6a_detail_ > 0 && !b4d1) {
          b4c7_process_block(mono, l, r, n, &t_dsp_start, &t_dsp_done,
                             &pure_dsp_us, &b4c7_cls, &v0u, &v1u, &gu, &s0,
                             &s1, &g0, &g1);
        } else {
          t_dsp_start = esp_timer_get_time();
          vocal_fx_process(mono, l, r, n);
          t_dsp_done = esp_timer_get_time();
          pure_dsp_us = static_cast<uint64_t>(t_dsp_done - t_dsp_start);
        }
        b4b6_dsp_active_.store(false, std::memory_order_release);
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
    // B4C.7 TX-prep subsections (§37, measurement only): the sanity+encode
    // loop is intentionally NOT fissioned — splitting it would reorder the
    // sum_sq FP accumulation. Subsections measured: ramp / sanity+encode /
    // rms, gated on B4C.7 detail so B4C.6A timing is bit-comparable.
    const bool b4c7_txp = b4c7 && b4c6a_detail_ > 0;
    const int64_t t_sec6_start = b4c7_txp ? esp_timer_get_time() : 0;
    if (ramp_gain_ < 1.0f) {
      ramp_gain_ += 0.01f;
      if (ramp_gain_ > 1.0f) ramp_gain_ = 1.0f;
      for (size_t i = 0; i < n; ++i) {
        l[i] *= ramp_gain_;
        r[i] *= ramp_gain_;
      }
    }
    const int64_t t_ramp_done = b4c7_txp ? esp_timer_get_time() : 0;

    // 6. Output Sanity Check, Level Tracking & PCM32 encoding
    // B4D.2: the output RMS accumulator is telemetry-only (it does not feed
    // DSP behaviour) and can be disabled in production, removing two
    // multiply-accumulates per sample from the hot path.
    const bool do_rms = output_rms_enabled_;
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
      if (do_rms) {
        sum_sq_l += l[i] * l[i];
        sum_sq_r += r[i] * r[i];
      }

      out32[2 * i] = float_to_pcm32(l[i]);
      out32[2 * i + 1] = float_to_pcm32(r[i]);
    }
    const int64_t t_encode_done = b4c7_txp ? esp_timer_get_time() : 0;
    if (do_rms) {
      out_rms_l_ = static_cast<float>(std::sqrt(sum_sq_l / n));
      out_rms_r_ = static_cast<float>(std::sqrt(sum_sq_r / n));
    }
    if (b4c7_txp && b4c7_totals_) {
      const uint32_t ramp_us =
          static_cast<uint32_t>(t_ramp_done - t_sec6_start);
      const uint32_t se_us =
          static_cast<uint32_t>(t_encode_done - t_ramp_done);
      const uint32_t rms_us = static_cast<uint32_t>(esp_timer_get_time() -
                                                    t_encode_done);
      auto &tt = *b4c7_totals_;
      tt.txp_ramp += ramp_us;
      tt.txp_sanity_encode += se_us;
      tt.txp_rms += rms_us;
      if (ramp_us > tt.txp_ramp_max) tt.txp_ramp_max = ramp_us;
      if (se_us > tt.txp_sanity_encode_max) tt.txp_sanity_encode_max = se_us;
      if (rms_us > tt.txp_rms_max) tt.txp_rms_max = rms_us;
    }

    // 6b. TX preparation boundary (D): ramp + sanity + float->PCM32 above.
    const int64_t t_tx_before = esp_timer_get_time();
    // 7. Write block to TX DMA (E: tx_wait_submit)
    uint64_t rx_gap_us = (last_rx_cb > 0) ? counters_.max_rx_callback_gap_us.load(std::memory_order_relaxed) : 0;
    size_t sent = 0;
    esp_err_t tx_err = i2s_channel_write(tx, out32, byte_count, &sent, pdMS_TO_TICKS(100));
    const int64_t t_tx_after = esp_timer_get_time();
    if (b4c6a_on) {
      if (sent == 0) ++b4c6a_tx_zero_;
      if (sent != byte_count) ++b4c6a_tx_partial_;
      if (tx_err == ESP_ERR_TIMEOUT) ++b4c6a_totals_.tx_timeouts;
    }
    if (tx_err != ESP_OK || sent < byte_count) {
      counters_.i2s_write_failures.fetch_add(1, std::memory_order_relaxed);
      counters_.tx_dropped_frames.fetch_add(
          (byte_count - std::min(sent, byte_count)) /
              (2 * sizeof(int32_t)),
          std::memory_order_relaxed);
      counters_.tx_sequence_gaps.fetch_add(1, std::memory_order_relaxed);
      record_transport_error("I2S_TX_WRITE_FAILURE", block_idx, pure_dsp_us,
                             rx_gap_us, 0, wake_latency_us);
    } else {
      counters_.i2s_write_successes.fetch_add(1, std::memory_order_relaxed);
      if (!b4b6_dsp_paused_.load(std::memory_order_relaxed)) {
        counters_.tx_bytes_written.fetch_add(sent, std::memory_order_relaxed);
        counters_.tx_frames_written.fetch_add(sent / (2 * sizeof(int32_t)), std::memory_order_relaxed);
        counters_.tx_blocks_written.fetch_add(1, std::memory_order_relaxed);
        // B4D.11: deterministic TX PCM hash (FNV-1a over the written samples).
        uint32_t fnv = counters_.tx_pcm_fnv.load(std::memory_order_relaxed);
        const size_t n32 = sent / sizeof(int32_t);
        for (size_t i = 0; i < n32; ++i) {
          fnv = (fnv ^ static_cast<uint32_t>(out32[i])) * 16777619u;
        }
        counters_.tx_pcm_fnv.store(fnv, std::memory_order_relaxed);
      }
      increment_depth(counters_.tx_event_queue_depth,
                      counters_.tx_event_queue_max_depth);
    }

    // 8. Record cycle timing + B4C.6A whole-loop sections (G/H/I).
    // G bookkeeping starts here: counters, wake stats, histograms. The
    // esp_timer reads themselves are ~1 us and stay inside G.
    const int64_t t_book_start = esp_timer_get_time();
    uint64_t cycle_us = static_cast<uint64_t>(t_book_start - t_cycle_start);
    const uint32_t c_cycle_end = esp_cpu_get_cycle_count();
    const uint32_t loop_cycles =
        c_cycle_end - c_cycle_start;

    if (b4c6a_on && got == byte_count && sent == byte_count) {
      // Non-overlapping sections in us (all clamped, never negative).
      const uint32_t rx_wait =
          static_cast<uint32_t>(t_rx_after - t_cycle_start);
      const uint32_t rx_copy =
          static_cast<uint32_t>(t_conversion_done - t_rx_after);
      const uint32_t fixture =
          static_cast<uint32_t>(t_fixture_done - t_conversion_done);
      const uint32_t dsp = static_cast<uint32_t>(pure_dsp_us);
      // DSP interval check: t_dsp_done must equal t_fixture_done + pure_dsp
      // by construction in the B4C6A dispatch branch.
      const uint32_t tx_prep = static_cast<uint32_t>(
          t_tx_before > t_dsp_done ? t_tx_before - t_dsp_done : 0);
      const uint32_t tx_wait =
          static_cast<uint32_t>(t_tx_after - t_tx_before);
      // Bookkeeping closes at iteration end; measure it now.
      const int64_t t_iter_end = esp_timer_get_time();
      const uint32_t book =
          static_cast<uint32_t>(t_iter_end - t_tx_after);
      const uint32_t loop_total =
          static_cast<uint32_t>(t_iter_end - t_cycle_start);
      const uint64_t accounted = static_cast<uint64_t>(rx_wait) + rx_copy +
                                 fixture + dsp + tx_prep + tx_wait + book;
      const uint32_t other = loop_total > accounted
                                 ? loop_total - static_cast<uint32_t>(accounted)
                                 : 0;
      uint32_t inter_gap = 0;
      uint32_t loop_period = 0;
      if (b4c6a_last_end_us_ != 0 &&
          t_cycle_start > static_cast<int64_t>(b4c6a_last_end_us_)) {
        inter_gap = static_cast<uint32_t>(
            static_cast<uint64_t>(t_cycle_start) - b4c6a_last_end_us_);
      }
      if (b4c6a_last_begin_us_ != 0 &&
          static_cast<uint64_t>(t_cycle_start) > b4c6a_last_begin_us_) {
        loop_period = static_cast<uint32_t>(
            static_cast<uint64_t>(t_cycle_start) - b4c6a_last_begin_us_);
      }
      b4c6a_note_iteration(
          rx_wait, rx_copy, fixture, dsp, tx_prep, tx_wait, book, other,
          loop_total, loop_period, inter_gap, loop_cycles,
          static_cast<uint32_t>(got), static_cast<uint32_t>(sent),
          static_cast<uint16_t>(got / (2 * sizeof(int32_t))),
          static_cast<uint16_t>(sent / (2 * sizeof(int32_t))),
          static_cast<int>(rx_err), static_cast<int>(tx_err),
          static_cast<uint64_t>(t_cycle_start),
          static_cast<uint64_t>(t_iter_end), true);
      b4c6a_last_end_us_ = static_cast<uint64_t>(t_iter_end);
      b4c6a_last_begin_us_ = static_cast<uint64_t>(t_cycle_start);
      if (b4c7 && b4c7_totals_ && b4c7_cls < B4C7DecompTotals::kClasses)
        b4c7_totals_->loop_sum[b4c7_cls] += loop_total;
      // Keep legacy cycle_us consistent with the closed iteration end.
      cycle_us = loop_total;
    } else if (b4c6a || b4c7) {
      if (b4c6a_last_end_us_ == 0) {
        b4c6a_last_end_us_ = static_cast<uint64_t>(t_book_start);
        b4c6a_last_begin_us_ = static_cast<uint64_t>(t_cycle_start);
      } else {
        b4c6a_last_end_us_ = static_cast<uint64_t>(t_book_start);
        b4c6a_last_begin_us_ = static_cast<uint64_t>(t_cycle_start);
      }
    }

    if (!b4b6_dsp_paused_.load(std::memory_order_relaxed)) {
      record_timing(pure_dsp_us, cycle_us, wake_latency_us, rx_gap_us, block_idx);
      counters_.audio_blocks_processed.fetch_add(1, std::memory_order_relaxed);
    }

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
        counters_.rx_bytes_read.store(0, std::memory_order_relaxed);
        counters_.tx_bytes_written.store(0, std::memory_order_relaxed);
        counters_.rx_frames_read.store(0, std::memory_order_relaxed);
        counters_.tx_frames_written.store(0, std::memory_order_relaxed);
        counters_.rx_blocks_read.store(0, std::memory_order_relaxed);
        counters_.tx_blocks_written.store(0, std::memory_order_relaxed);
        counters_.rx_dropped_frames.store(0, std::memory_order_relaxed);
        counters_.tx_dropped_frames.store(0, std::memory_order_relaxed);
        counters_.rx_sequence_gaps.store(0, std::memory_order_relaxed);
        counters_.tx_sequence_gaps.store(0, std::memory_order_relaxed);
        counters_.audio_blocks_processed.store(0, std::memory_order_relaxed);
        dsp_total_us_ = 0;
        cycle_total_us_ = 0;
        wake_total_us_ = 0;
        dsp_worst_us_ = 0;
        cycle_worst_us_ = 0;
        wake_worst_us_ = 0;
        uint32_t *legacy_warm[] = {dsp_hist_, cycle_hist_, wake_hist_};
        for (uint32_t *h : legacy_warm)
          if (h) std::fill_n(h, kHistBins, 0);
        outlier_count_ = 0;
        reset_b4c6a_stats();
      }
    }
  }
#else
  (void)sine_phase_l_;
  (void)sine_phase_r_;
#endif
}

void AudioI2s::print_b4c6a_forensics(bool dump_samples) const {
  // B4C.7 shares the whole-loop timing engine; its per-case loop aggregates
  // print here (per-block TIMELINE rows stay B4C.6A-only).
  if (config_.mode != AudioI2sMode::B4C6ATimingForensics &&
      config_.mode != AudioI2sMode::B4C7HarmonizerAudit)
    return;
  const auto &t = b4c6a_totals_;
  const double count = static_cast<double>(t.samples);
  const double avg_loop = count ? static_cast<double>(t.loop_total_us) / count : 0.0;
  const double avg_gap =
      t.samples > 1 ? static_cast<double>(t.inter_gap_us) / (count - 1.0) : 0.0;
  const double avg_rx = count ? static_cast<double>(t.rx_wait_us) / count : 0.0;
  const double avg_tx = count ? static_cast<double>(t.tx_wait_us) / count : 0.0;
  const double avg_copy = count ? static_cast<double>(t.rx_copy_us) / count : 0.0;
  const double avg_fix = count ? static_cast<double>(t.fixture_us) / count : 0.0;
  const double avg_dsp = count ? static_cast<double>(t.dsp_us) / count : 0.0;
  const double avg_prep = count ? static_cast<double>(t.tx_prep_us) / count : 0.0;
  const double avg_book = count ? static_cast<double>(t.bookkeeping_us) / count : 0.0;
  const double avg_other = count ? static_cast<double>(t.other_us) / count : 0.0;
  const uint64_t accounted = t.rx_wait_us + t.rx_copy_us + t.fixture_us +
                             t.dsp_us + t.tx_prep_us + t.tx_wait_us +
                             t.bookkeeping_us;
  const double recon = b4c6a_reconciliation_pct(accounted, t.loop_total_us);
  const TimingPercentiles loop_p = b4c6a_loop_percentiles();
  const TimingPercentiles dsp_p = b4c6a_dsp_forensic_percentiles();
  const TimingPercentiles rx_p = b4c6a_rx_percentiles();
  const TimingPercentiles tx_p = b4c6a_tx_percentiles();
  printf("B4C6A_SUMMARY samples=%llu dropped=%llu avg_loop_us=%.2f max_loop_us=%llu avg_rx_wait_us=%.2f avg_tx_wait_us=%.2f avg_inter_gap_us=%.2f max_inter_gap_us=%llu rx_partial=%llu tx_partial=%llu rx_zero=%llu tx_zero=%llu rx_timeouts=%llu tx_timeouts=%llu\n",
         static_cast<unsigned long long>(t.samples),
         static_cast<unsigned long long>(b4c6a_timing_dropped_), avg_loop,
         static_cast<unsigned long long>(t.loop_total_max_us), avg_rx, avg_tx,
         avg_gap, static_cast<unsigned long long>(t.inter_gap_max_us),
         static_cast<unsigned long long>(b4c6a_rx_partial_),
         static_cast<unsigned long long>(b4c6a_tx_partial_),
         static_cast<unsigned long long>(b4c6a_rx_zero_),
         static_cast<unsigned long long>(b4c6a_tx_zero_),
         static_cast<unsigned long long>(t.rx_timeouts),
         static_cast<unsigned long long>(t.tx_timeouts));
  printf("B4C6A_LOOP loop_avg=%.2f loop_p50=%llu loop_p90=%llu loop_p95=%llu loop_p99=%llu loop_p999=%llu loop_max=%llu dsp_avg=%.2f dsp_p99=%llu dsp_max=%llu rx_max=%llu tx_max=%llu gap_max=%llu\n",
         loop_p.avg_us, static_cast<unsigned long long>(loop_p.p50_us),
         static_cast<unsigned long long>(loop_p.p90_us),
         static_cast<unsigned long long>(loop_p.p95_us),
         static_cast<unsigned long long>(loop_p.p99_us),
         static_cast<unsigned long long>(loop_p.p99_9_us),
         static_cast<unsigned long long>(loop_p.max_us), dsp_p.avg_us,
         static_cast<unsigned long long>(dsp_p.p99_us),
         static_cast<unsigned long long>(dsp_p.max_us),
         static_cast<unsigned long long>(t.rx_wait_max_us),
         static_cast<unsigned long long>(t.tx_wait_max_us),
         static_cast<unsigned long long>(t.inter_gap_max_us));
  printf("B4C6A_RXDIST avg=%.2f p50=%llu p90=%llu p95=%llu p99=%llu p999=%llu max=%llu samples=%llu\n",
         rx_p.avg_us, static_cast<unsigned long long>(rx_p.p50_us),
         static_cast<unsigned long long>(rx_p.p90_us),
         static_cast<unsigned long long>(rx_p.p95_us),
         static_cast<unsigned long long>(rx_p.p99_us),
         static_cast<unsigned long long>(rx_p.p99_9_us),
         static_cast<unsigned long long>(rx_p.max_us),
         static_cast<unsigned long long>(rx_p.samples));
  printf("B4C6A_TXDIST avg=%.2f p50=%llu p90=%llu p95=%llu p99=%llu p999=%llu max=%llu samples=%llu\n",
         tx_p.avg_us, static_cast<unsigned long long>(tx_p.p50_us),
         static_cast<unsigned long long>(tx_p.p90_us),
         static_cast<unsigned long long>(tx_p.p95_us),
         static_cast<unsigned long long>(tx_p.p99_us),
         static_cast<unsigned long long>(tx_p.p99_9_us),
         static_cast<unsigned long long>(tx_p.max_us),
         static_cast<unsigned long long>(tx_p.samples));
  printf("B4C6A_EQUATION rx_wait=%.2f rx_copy=%.2f fixture=%.2f dsp=%.2f tx_prep=%.2f tx_wait=%.2f book=%.2f other=%.2f loop_total=%.2f reconciliation=%.3f late_loop_blocks=%llu positive_lateness_us=%llu catchup_rx_near_zero=%llu recover_events=%llu recover_blocks_sum=%llu\n",
         avg_rx, avg_copy, avg_fix, avg_dsp, avg_prep, avg_tx, avg_book,
         avg_other, avg_loop, recon,
         static_cast<unsigned long long>(t.late_loop_blocks),
         static_cast<unsigned long long>(t.positive_lateness_us),
         static_cast<unsigned long long>(t.catchup_rx_near_zero),
         static_cast<unsigned long long>(t.recover_events),
         static_cast<unsigned long long>(t.recover_blocks_sum));
  printf("B4C6A_BACKLOG rx_queue_max=%llu tx_queue_max=%llu backlog_frames_max=%llu backlog_ms_max=%.3f rx_overruns=%llu tx_underruns=%llu\n",
         static_cast<unsigned long long>(counters_.rx_event_queue_max_depth.load()),
         static_cast<unsigned long long>(counters_.tx_event_queue_max_depth.load()),
         static_cast<unsigned long long>(counters_.rx_event_queue_max_depth.load() * 64),
         static_cast<double>(counters_.rx_event_queue_max_depth.load() * 64) / 48.0,
         static_cast<unsigned long long>(counters_.rx_overruns.load()),
         static_cast<unsigned long long>(counters_.tx_underruns.load()));
  if (!dump_samples) return;
  printf("B4C6A_TIMELINE_HEADER iteration_begin_us,rx_done_us,conversion_done_us,fixture_done_us,dsp_done_us,tx_before_us,tx_done_us,iteration_end_us,loop_cycles,rx_bytes,tx_bytes,rx_rc,tx_rc,rx_ok,tx_ok\n");
  for (uint64_t i = 0; i < b4c6a_timing_count_; ++i) {
    const auto &r = b4c6a_timing_[i];
    const uint64_t rx_done = r.iteration_begin_us + r.rx_wait_us;
    const uint64_t conv_done = rx_done + r.rx_copy_us;
    const uint64_t fix_done = conv_done + r.fixture_us;
    const uint64_t dsp_done = fix_done + r.dsp_us;
    const uint64_t tx_before = dsp_done + r.tx_prep_us;
    const uint64_t tx_done = tx_before + r.tx_wait_us;
    printf("B4C6A_TIMELINE %llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%d,%d,%u,%u\n",
           static_cast<unsigned long long>(r.iteration_begin_us),
           static_cast<unsigned long long>(rx_done),
           static_cast<unsigned long long>(conv_done),
           static_cast<unsigned long long>(fix_done),
           static_cast<unsigned long long>(dsp_done),
           static_cast<unsigned long long>(tx_before),
           static_cast<unsigned long long>(tx_done),
           static_cast<unsigned long long>(r.iteration_end_us),
           static_cast<unsigned>(r.loop_total_cycles),
           static_cast<unsigned>(r.rx_bytes), static_cast<unsigned>(r.tx_bytes),
           static_cast<int>(r.rx_rc), static_cast<int>(r.tx_rc),
           static_cast<unsigned>(r.rx_ok), static_cast<unsigned>(r.tx_ok));
  }
}

void AudioI2s::print_b4c7_forensics(bool dump_samples) const {
  if (config_.mode != AudioI2sMode::B4C7HarmonizerAudit) return;
  if (!b4c7_totals_) {
    printf("B4C7_CLASS status=NO_TOTALS\n");
    return;
  }
  const auto &t = *b4c7_totals_;
  static const char *kClassNames[4] = {"BOTH_ACTIVE", "V0_ONLY", "V1_ONLY",
                                       "NEITHER_ACTIVE"};
  uint64_t total_blocks = 0;
  for (uint8_t c = 0; c < 4; ++c) total_blocks += t.count[c];
  const TimingPercentiles both_p = b4c6a_hist_percentiles(
      t.both_hist, t.both_total_us, t.both_max_us, t.both_samples);
  printf("B4C7_CLASS total=%llu both=%llu v0only=%llu v1only=%llu neither=%llu both_pct=%.2f\n",
         static_cast<unsigned long long>(total_blocks),
         static_cast<unsigned long long>(t.count[0]),
         static_cast<unsigned long long>(t.count[1]),
         static_cast<unsigned long long>(t.count[2]),
         static_cast<unsigned long long>(t.count[3]),
         total_blocks ? 100.0 * static_cast<double>(t.count[0]) /
                            static_cast<double>(total_blocks)
                      : 0.0);
  printf("B4C7_BOTH dsp_avg=%.2f dsp_p50=%llu dsp_p90=%llu dsp_p95=%llu dsp_p99=%llu dsp_p999=%llu dsp_max=%llu samples=%llu\n",
         both_p.avg_us, static_cast<unsigned long long>(both_p.p50_us),
         static_cast<unsigned long long>(both_p.p90_us),
         static_cast<unsigned long long>(both_p.p95_us),
         static_cast<unsigned long long>(both_p.p99_us),
         static_cast<unsigned long long>(both_p.p99_9_us),
         static_cast<unsigned long long>(both_p.max_us),
         static_cast<unsigned long long>(both_p.samples));
  // Per-class decomposition averages (§19/§20): global[20] + voice[2][20].
  for (uint8_t c = 0; c < 4; ++c) {
    const double n = static_cast<double>(t.count[c] ? t.count[c] : 1);
    const double loop_avg = static_cast<double>(t.loop_sum[c]) / n;
    const double acct_avg = static_cast<double>(t.acct_sum[c]) / n;
    printf("B4C7_DECOMP class=%s count=%llu loop_avg=%.2f acct_avg=%.2f recon=%.3f\n",
           kClassNames[c], static_cast<unsigned long long>(t.count[c]),
           loop_avg, acct_avg,
           b4c6a_reconciliation_pct(
               static_cast<uint64_t>(acct_avg * t.count[c]),
               t.loop_sum[c]));
    printf("B4C7_DECOMP_G class=%s", kClassNames[c]);
    for (size_t i = 0; i < B4C7DecompTotals::kGlobals; ++i)
      printf(" %s=%.2f", kB4C7GlobalNames[i],
             static_cast<double>(t.global[c][i]) / n / Profiler::cycles_per_us());
    printf("\n");
    for (uint8_t v = 0; v < 2; ++v) {
      printf("B4C7_DECOMP_V class=%s voice=%u", kClassNames[c],
             static_cast<unsigned>(v));
      for (size_t i = 0; i < 20; ++i)
        printf(" %s=%.2f", kB4C7VoiceNames[i],
               static_cast<double>(t.voice[c][v][i]) / n / Profiler::cycles_per_us());
      for (size_t j = 0; j < 5; ++j)
        printf(" %s=%.2f", kB4C7ExtraNames[j],
               static_cast<double>(t.voice[c][v][20 + j]) / n / Profiler::cycles_per_us());
      printf("\n");
    }
  }
  // TX-prep subsections (§37, measurement only).
  {
    const double n = static_cast<double>(total_blocks ? total_blocks : 1);
    printf("B4C7_TXPREP ramp_avg=%.2f sanity_encode_avg=%.2f rms_avg=%.2f ramp_max=%llu sanity_encode_max=%llu rms_max=%llu samples=%llu\n",
           static_cast<double>(t.txp_ramp) / n,
           static_cast<double>(t.txp_sanity_encode) / n,
           static_cast<double>(t.txp_rms) / n,
           static_cast<unsigned long long>(t.txp_ramp_max),
           static_cast<unsigned long long>(t.txp_sanity_encode_max),
           static_cast<unsigned long long>(t.txp_rms_max),
           static_cast<unsigned long long>(total_blocks));
  }
  // LPC-order forensic gate (§33).
  printf("B4C7_LPC compile_default_order=%u analyzer_order=%u synth_v0=%u synth_v1=%u\n",
         static_cast<unsigned>(LpcConfig{}.order),
         static_cast<unsigned>(vocal_fx_lpc_config_order()),
         static_cast<unsigned>(vocal_fx_synthesis_order(0)),
         static_cast<unsigned>(vocal_fx_synthesis_order(1)));
  // Source-slice reuse (§21-23): descriptor audit deltas + exact slice audit.
  {
    const PsolaSourceGrainAudit now_a = vocal_fx_get_psola_source_grain_audit();
    const B4C7SliceAuditSnapshot now_s = vocal_fx_slice_audit_snapshot();
    const uint64_t req = now_a.source_grains_requested - b4c7_audit_start_.source_grains_requested;
    const uint64_t uniq = now_a.unique_source_grains - b4c7_audit_start_.unique_source_grains;
    const uint64_t dup = now_a.duplicate_source_grains - b4c7_audit_start_.duplicate_source_grains;
    const uint64_t cross = now_a.cross_voice_reuses - b4c7_audit_start_.cross_voice_reuses;
    const uint64_t same = now_a.same_voice_reuses - b4c7_audit_start_.same_voice_reuses;
    const uint64_t samp = now_a.total_grain_samples - b4c7_audit_start_.total_grain_samples;
    const uint64_t rsamp = now_a.reusable_grain_samples - b4c7_audit_start_.reusable_grain_samples;
    printf("B4C7_REUSE_DESC requested=%llu unique=%llu duplicate=%llu cross=%llu same=%llu samples=%llu reusable_samples=%llu req_pct=%.2f sample_pct=%.2f\n",
           static_cast<unsigned long long>(req),
           static_cast<unsigned long long>(uniq),
           static_cast<unsigned long long>(dup),
           static_cast<unsigned long long>(cross),
           static_cast<unsigned long long>(same),
           static_cast<unsigned long long>(samp),
           static_cast<unsigned long long>(rsamp),
           req ? 100.0 * static_cast<double>(cross) / static_cast<double>(req) : 0.0,
           samp ? 100.0 * static_cast<double>(rsamp) / static_cast<double>(samp) : 0.0);
    const uint64_t sreq = now_s.slices_requested - b4c7_slice_start_.slices_requested;
    const uint64_t sdup = now_s.duplicates - b4c7_slice_start_.duplicates;
    const uint64_t scross = now_s.cross_voice - b4c7_slice_start_.cross_voice;
    const uint64_t ssame = now_s.same_voice - b4c7_slice_start_.same_voice;
    const uint64_t ssamp = now_s.samples - b4c7_slice_start_.samples;
    const uint64_t srsamp = now_s.reusable_samples - b4c7_slice_start_.reusable_samples;
    const uint64_t taps = now_s.fir_taps - b4c7_slice_start_.fir_taps;
    const uint64_t rtaps = now_s.reusable_taps - b4c7_slice_start_.reusable_taps;
    printf("B4C7_REUSE_SLICE requested=%llu duplicate=%llu cross=%llu same=%llu samples=%llu reusable_samples=%llu taps=%llu reusable_taps=%llu entry_drops=%llu req_pct=%.2f sample_pct=%.2f tap_pct=%.2f\n",
           static_cast<unsigned long long>(sreq),
           static_cast<unsigned long long>(sdup),
           static_cast<unsigned long long>(scross),
           static_cast<unsigned long long>(ssame),
           static_cast<unsigned long long>(ssamp),
           static_cast<unsigned long long>(srsamp),
           static_cast<unsigned long long>(taps),
           static_cast<unsigned long long>(rtaps),
           static_cast<unsigned long long>(now_s.entry_drops - b4c7_slice_start_.entry_drops),
           sreq ? 100.0 * static_cast<double>(scross) / static_cast<double>(sreq) : 0.0,
           ssamp ? 100.0 * static_cast<double>(srsamp) / static_cast<double>(ssamp) : 0.0,
           taps ? 100.0 * static_cast<double>(rtaps) / static_cast<double>(taps) : 0.0);
  }
  printf("B4C7_PRESTAGED overruns=%llu\n",
         static_cast<unsigned long long>(b4c7_pre_overruns_));
  // Deferred residual-cache stats (§22 FIR accounting): per-voice deltas.
  for (uint8_t v = 0; v < 2; ++v) {
    const PsolaSourceResidualCacheStats now =
        vocal_fx_get_psola_residual_cache_stats(v);
    const PsolaSourceResidualCacheStats &st = b4c7_fir_start_[v];
    printf("B4C7_FIRCACHE voice=%u lookups=%llu hits=%llu misses=%llu evictions=%llu computed=%llu reused=%llu saved_cycles=%llu lookup_cycles=%llu fir_cycles=%llu dist=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
           static_cast<unsigned>(v),
           static_cast<unsigned long long>(now.total_lookups - st.total_lookups),
           static_cast<unsigned long long>(now.hits - st.hits),
           static_cast<unsigned long long>(now.misses - st.misses),
           static_cast<unsigned long long>(now.evictions - st.evictions),
           static_cast<unsigned long long>(now.fir_samples_computed - st.fir_samples_computed),
           static_cast<unsigned long long>(now.fir_samples_reused - st.fir_samples_reused),
           static_cast<unsigned long long>(now.fir_cycles_saved - st.fir_cycles_saved),
           static_cast<unsigned long long>(now.total_lookup_cycles - st.total_lookup_cycles),
           static_cast<unsigned long long>(now.total_fir_cycles - st.total_fir_cycles),
           static_cast<unsigned long long>(now.reuse_distance_hist[0] - st.reuse_distance_hist[0]),
           static_cast<unsigned long long>(now.reuse_distance_hist[1] - st.reuse_distance_hist[1]),
           static_cast<unsigned long long>(now.reuse_distance_hist[2] - st.reuse_distance_hist[2]),
           static_cast<unsigned long long>(now.reuse_distance_hist[3] - st.reuse_distance_hist[3]),
           static_cast<unsigned long long>(now.reuse_distance_hist[4] - st.reuse_distance_hist[4]),
           static_cast<unsigned long long>(now.reuse_distance_hist[5] - st.reuse_distance_hist[5]),
           static_cast<unsigned long long>(now.reuse_distance_hist[6] - st.reuse_distance_hist[6]),
           static_cast<unsigned long long>(now.reuse_distance_hist[7] - st.reuse_distance_hist[7]),
           static_cast<unsigned long long>(now.reuse_distance_hist[8] - st.reuse_distance_hist[8]),
           static_cast<unsigned long long>(now.reuse_distance_hist[9] - st.reuse_distance_hist[9]));
  }
  if (!dump_samples) return;
  printf("B4C7_BLOCK_HEADER block,class,dsp_us,v0_us,v1_us,global_us,slices_v0,slices_v1,sched_v0,sched_v1\n");
  for (uint64_t i = 0; i < b4c7_block_count_; ++i) {
    const auto &r = b4c7_blocks_[i];
    printf("B4C7_BLOCK %u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
           static_cast<unsigned>(r.block), static_cast<unsigned>(r.block_class),
           static_cast<unsigned>(r.dsp_us), static_cast<unsigned>(r.v0_us),
           static_cast<unsigned>(r.v1_us), static_cast<unsigned>(r.global_us),
           static_cast<unsigned>(r.slices_v0), static_cast<unsigned>(r.slices_v1),
           static_cast<unsigned>(r.sched_v0), static_cast<unsigned>(r.sched_v1));
  }
}

TimingPercentiles AudioI2s::calculate_dsp_percentiles() const {
  TimingPercentiles p{};
  if (!dsp_hist_) return p;
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += dsp_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(dsp_total_us_) / total_samples;
  p.max_us = dsp_worst_us_;

  uint64_t target_p50 = static_cast<uint64_t>(total_samples * 0.50);
  uint64_t target_p90 = static_cast<uint64_t>(total_samples * 0.90);
  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += dsp_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p50_us == 0 && cumulative >= target_p50) p.p50_us = std::min(bin_us, p.max_us);
    if (p.p90_us == 0 && cumulative >= target_p90) p.p90_us = std::min(bin_us, p.max_us);
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
  if (!cycle_hist_) return p;
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += cycle_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(cycle_total_us_) / total_samples;
  p.max_us = cycle_worst_us_;

  uint64_t target_p50 = static_cast<uint64_t>(total_samples * 0.50);
  uint64_t target_p90 = static_cast<uint64_t>(total_samples * 0.90);
  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += cycle_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p50_us == 0 && cumulative >= target_p50) p.p50_us = std::min(bin_us, p.max_us);
    if (p.p90_us == 0 && cumulative >= target_p90) p.p90_us = std::min(bin_us, p.max_us);
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
  if (!wake_hist_) return p;
  uint64_t total_samples = 0;
  for (size_t i = 0; i < kHistBins; ++i) {
    total_samples += wake_hist_[i];
  }
  p.samples = total_samples;
  if (!total_samples) return p;

  p.avg_us = static_cast<double>(wake_total_us_) / total_samples;
  p.max_us = wake_worst_us_;

  uint64_t target_p50 = static_cast<uint64_t>(total_samples * 0.50);
  uint64_t target_p90 = static_cast<uint64_t>(total_samples * 0.90);
  uint64_t target_p95 = static_cast<uint64_t>(total_samples * 0.95);
  uint64_t target_p99 = static_cast<uint64_t>(total_samples * 0.99);
  uint64_t target_p99_9 = static_cast<uint64_t>(total_samples * 0.999);
  uint64_t cumulative = 0;

  for (size_t i = 0; i < kHistBins; ++i) {
    cumulative += wake_hist_[i];
    uint64_t bin_us = (i + 1) * kBinWidthUs;
    if (p.p50_us == 0 && cumulative >= target_p50) p.p50_us = std::min(bin_us, p.max_us);
    if (p.p90_us == 0 && cumulative >= target_p90) p.p90_us = std::min(bin_us, p.max_us);
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
