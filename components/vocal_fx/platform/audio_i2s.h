#pragma once

#include "boards/wt9932p4_tiny_audio.h"
#include "vocal_fx_types.h"
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
  PitchWorkerLockAudit = 7, // Stage B4B.2: constant 220 Hz pure sine -> all DSP taps
  B4B6RcValidation = 8, // Stage B4B.6: configurable deterministic RC stimuli
  B4C1AudioCoreAudit = 9, // Stage B4C.1: Audio/Render Core DSP Budget Audit
  B4C2AudioCoreAudit = 10, // Stage B4C.2: TD-PSOLA LPC Formant & Window/OLA Throughput Optimization
  B4C3HarmonizerTailAudit = 11, // Stage B4C.3: Harmonizer Tail-Latency & ModelWarp Optimization
  B4C3AWorkloadAudit = 12, // Stage B4C.3A: Profiler Integrity & Grain-Burst Workload Audit
  B4C3BSourceGrainBurst = 13, // Stage B4C.3B: Exact Source-Grain Reuse & Grain-Burst Flattening Audit
  B4C4SingleGrainDeferred = 14, // Stage B4C.4: Single-Grain Cost Attribution & Deferred Grain-Slice Rendering Audit
  B4C4ADeferredStandalone = 15, // Stage B4C.4A: Deferred Harmonizer Standalone Realtime Qualification
  B4C6ATimingForensics = 16, // Stage B4C.6A: end-to-end loop timing forensics
  B4C7HarmonizerAudit = 17, // Stage B4C.7: prestaged input, BOTH-ACTIVE, reuse
  B4D1Qualification = 18, // Stage B4D.1: cleaned 1V realtime window + FX budget
};

enum class B4B6StimulusKind : uint8_t {
  PureTone,
  HarmonicVoiced,
  BroadbandNoise,
  BreathyVoiced,
  Silence,
  NearSilence,
  Step110To220,
  Step220To110,
  Step110To440,
  Step440To110,
  Step65To130,
  Step80To160,
  Step220To440,
  Step147To220To330,
  Gliss80To440,
  Gliss440To80,
  VibratoOneSemitone,
  VibratoTwoSemitones,
  Staccato,
  VocalReplay,
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
  std::atomic<uint64_t> rx_diagnostic_queue_overflows{0};
  std::atomic<uint64_t> tx_diagnostic_queue_overflows{0};
  std::atomic<uint64_t> actual_rx_dma_errors{0};
  std::atomic<uint64_t> actual_tx_dma_errors{0};
  std::atomic<uint64_t> i2s_read_failures{0};
  std::atomic<uint64_t> i2s_write_failures{0};
  std::atomic<uint64_t> i2s_read_successes{0};
  std::atomic<uint64_t> i2s_write_successes{0};
  std::atomic<uint64_t> rx_event_queue_depth{0};
  std::atomic<uint64_t> tx_event_queue_depth{0};
  std::atomic<uint64_t> rx_event_queue_max_depth{0};
  std::atomic<uint64_t> tx_event_queue_max_depth{0};
  std::atomic<uint64_t> rx_dropped_frames{0};
  std::atomic<uint64_t> tx_dropped_frames{0};
  std::atomic<uint64_t> rx_bytes_read{0};
  std::atomic<uint64_t> tx_bytes_written{0};
  std::atomic<uint64_t> rx_frames_read{0};
  std::atomic<uint64_t> tx_frames_written{0};
  std::atomic<uint64_t> rx_blocks_read{0};
  std::atomic<uint64_t> tx_blocks_written{0};
  // B4D.11: FNV-1a over the TX PCM32 samples written to I2S (device render
  // reproducibility). Updated on the audio task only.
  std::atomic<uint32_t> tx_pcm_fnv{2166136261u};
  std::atomic<uint64_t> rx_sequence_gaps{0};
  std::atomic<uint64_t> tx_sequence_gaps{0};
  std::atomic<uint64_t> dma_errors{0};
  std::atomic<uint64_t> audio_blocks_processed{0};
  std::atomic<uint64_t> audio_deadline_misses{0};
  std::atomic<uint64_t> dsp_deadline_misses{0};
  std::atomic<uint64_t> transport_deadline_misses{0};
  std::atomic<uint64_t> nan_inf_count{0};
  std::atomic<uint64_t> max_rx_callback_gap_us{0};
  std::atomic<uint64_t> max_tx_callback_gap_us{0};
  std::atomic<uint64_t> max_audio_task_wake_latency_us{0};
  std::atomic<uint64_t> max_single_block_lateness_us{0};
  std::atomic<uint64_t> cumulative_lateness_us{0};
  std::atomic<uint64_t> current_consecutive_late_blocks{0};
  std::atomic<uint64_t> max_consecutive_late_blocks{0};

  void reset() {
    rx_dma_events.store(0, std::memory_order_relaxed);
    tx_dma_events.store(0, std::memory_order_relaxed);
    rx_overruns.store(0, std::memory_order_relaxed);
    tx_underruns.store(0, std::memory_order_relaxed);
    rx_diagnostic_queue_overflows.store(0, std::memory_order_relaxed);
    tx_diagnostic_queue_overflows.store(0, std::memory_order_relaxed);
    actual_rx_dma_errors.store(0, std::memory_order_relaxed);
    actual_tx_dma_errors.store(0, std::memory_order_relaxed);
    i2s_read_failures.store(0, std::memory_order_relaxed);
    i2s_write_failures.store(0, std::memory_order_relaxed);
    i2s_read_successes.store(0, std::memory_order_relaxed);
    i2s_write_successes.store(0, std::memory_order_relaxed);
    rx_event_queue_depth.store(0, std::memory_order_relaxed);
    tx_event_queue_depth.store(0, std::memory_order_relaxed);
    rx_event_queue_max_depth.store(0, std::memory_order_relaxed);
    tx_event_queue_max_depth.store(0, std::memory_order_relaxed);
    rx_dropped_frames.store(0, std::memory_order_relaxed);
    tx_dropped_frames.store(0, std::memory_order_relaxed);
    rx_bytes_read.store(0, std::memory_order_relaxed);
    tx_bytes_written.store(0, std::memory_order_relaxed);
    rx_frames_read.store(0, std::memory_order_relaxed);
    tx_frames_written.store(0, std::memory_order_relaxed);
    rx_blocks_read.store(0, std::memory_order_relaxed);
    tx_blocks_written.store(0, std::memory_order_relaxed);
    tx_pcm_fnv.store(2166136261u, std::memory_order_relaxed);
    rx_sequence_gaps.store(0, std::memory_order_relaxed);
    tx_sequence_gaps.store(0, std::memory_order_relaxed);
    dma_errors.store(0, std::memory_order_relaxed);
    audio_blocks_processed.store(0, std::memory_order_relaxed);
    audio_deadline_misses.store(0, std::memory_order_relaxed);
    dsp_deadline_misses.store(0, std::memory_order_relaxed);
    transport_deadline_misses.store(0, std::memory_order_relaxed);
    nan_inf_count.store(0, std::memory_order_relaxed);
    max_rx_callback_gap_us.store(0, std::memory_order_relaxed);
    max_tx_callback_gap_us.store(0, std::memory_order_relaxed);
    max_audio_task_wake_latency_us.store(0, std::memory_order_relaxed);
    max_single_block_lateness_us.store(0, std::memory_order_relaxed);
    cumulative_lateness_us.store(0, std::memory_order_relaxed);
    current_consecutive_late_blocks.store(0, std::memory_order_relaxed);
    max_consecutive_late_blocks.store(0, std::memory_order_relaxed);
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
  bool snapshot_active;
  bool telemetry_formatting_active;
  bool serial_printing_active;
  bool pitch_worker_active;
  bool diagnostic_queue_flush_active;
};

// B4C.7 link budget (see td_psola.h slice-ring note): the outlier ring is
// write-only forensics that B4C.7 never prints; shrink it in B4C.7 builds.
// B4D.1/B4D.2 use the dedicated B4D1BlockRecord ring for per-block forensics,
// so the legacy outlier ring is likewise redundant there and shrunk to keep
// the sram_high link budget (B4D.2 reverb profiling added engine .bss).
#if defined(CONFIG_VOXP4_MODE_I2S_B4C_7_HARMONIZER_AUDIT) || \
    defined(CONFIG_VOXP4_MODE_I2S_B4D_1_QUALIFICATION)
constexpr size_t kMaxOutlierRecords = 32;
#else
constexpr size_t kMaxOutlierRecords = 128;
#endif

// Percentile and latency statistics
struct TimingPercentiles {
  double avg_us = 0.0;
  uint64_t p50_us = 0;
  uint64_t p90_us = 0;
  uint64_t p95_us = 0;
  uint64_t p99_us = 0;
  uint64_t p99_bucket_upper_bound_us = 0;
  uint64_t p99_9_us = 0;
  uint64_t max_us = 0;
  uint64_t samples = 0;
};

// B4C.6A per-block sample stored in PSRAM and printed only after the window.
// Aggregates/histograms cover every measured block; this ring is a CSV sample.
struct B4C6ATimingRecord {
  uint64_t iteration_begin_us = 0;
  uint64_t iteration_end_us = 0;
  uint32_t loop_total_us = 0;
  uint32_t loop_period_us = 0;
  uint32_t loop_total_cycles = 0;
  uint32_t rx_wait_us = 0;
  uint32_t rx_copy_us = 0;
  uint32_t fixture_us = 0;
  uint32_t dsp_us = 0;
  uint32_t tx_prep_us = 0;
  uint32_t tx_wait_us = 0;
  uint32_t bookkeeping_us = 0;
  uint32_t other_us = 0;
  uint32_t inter_gap_us = 0;
  uint32_t rx_bytes = 0;
  uint32_t tx_bytes = 0;
  uint16_t rx_frames = 0;
  uint16_t tx_frames = 0;
  int16_t rx_rc = 0;
  int16_t tx_rc = 0;
  uint8_t rx_partial = 0;
  uint8_t tx_partial = 0;
  uint8_t rx_ok = 0;
  uint8_t tx_ok = 0;
};
constexpr size_t kB4C6ATimingRecordCapacity = 16384;
constexpr size_t kB4C6AHistBins = 400;
constexpr uint64_t kB4C6ABinWidthUs = 10;

struct B4C6ASectionTotals {
  uint64_t samples = 0;
  uint64_t rx_wait_us = 0;
  uint64_t rx_copy_us = 0;
  uint64_t fixture_us = 0;
  uint64_t dsp_us = 0;
  uint64_t tx_prep_us = 0;
  uint64_t tx_wait_us = 0;
  uint64_t bookkeeping_us = 0;
  uint64_t other_us = 0;
  uint64_t inter_gap_us = 0;
  uint64_t loop_total_us = 0;
  uint64_t loop_total_max_us = 0;
  uint64_t loop_period_us = 0;
  uint64_t loop_total_cycles = 0;
  uint64_t positive_lateness_us = 0;
  uint64_t late_loop_blocks = 0;
  uint64_t catchup_rx_near_zero = 0;
  uint64_t recover_blocks_sum = 0;
  uint64_t recover_events = 0;
  uint64_t rx_timeouts = 0;
  uint64_t tx_timeouts = 0;
  uint64_t rx_retries = 0;
  uint64_t tx_retries = 0;
  uint64_t rx_wait_max_us = 0;
  uint64_t tx_wait_max_us = 0;
  uint64_t loop_period_max_us = 0;
  uint64_t inter_gap_max_us = 0;
  uint64_t dsp_max_us = 0;
};

// Stimulus state tracking for Stage B4B
enum class StimulusState : uint8_t {
  Silence = 0,
  Attack = 1,
  Tone = 2,
  Release = 3,
};

// B4C.7 production-equivalent input delivery (§12). Prestaged replays a
// coordinator-staged PSRAM buffer (no synthesis inside the timed loop);
// LiveAdc feeds the converted RX mono directly (true product path).
enum class B4C7InputKind : uint8_t {
  Prestaged = 0,
  LiveAdc = 1,
};

// B4C.7 BOTH-ACTIVE classifier (§17) from actual render state: a voice is
// ACTIVE in a block iff it rendered >= 1 deferred slice that block.
enum class B4C7BlockClass : uint8_t {
  BothActive = 0,
  V0Only = 1,
  V1Only = 2,
  NeitherActive = 3,
};

// B4C.7 per-block row (PSRAM ring sample; aggregates cover every block).
struct B4C7BlockRecord {
  uint32_t block = 0;
  uint8_t block_class = 0;
  uint32_t dsp_us = 0;
  uint32_t v0_us = 0;
  uint32_t v1_us = 0;
  uint32_t global_us = 0;
  uint16_t slices_v0 = 0;
  uint16_t slices_v1 = 0;
  uint16_t sched_v0 = 0;
  uint16_t sched_v1 = 0;
};
constexpr size_t kB4C7BlockRecordCapacity = 16384;

// B4D.1 cleaned-qualification per-block record. Whole-DSP and whole-loop
// timings are captured together with the harmonizer model-change flag in the
// same audio block so the MC x deadline-miss contingency is built from
// actually observed blocks (no correlation inference). PSRAM-backed.
struct B4D1BlockRecord {
  uint32_t block_id = 0;
  uint16_t dsp_us = 0;   // vocal_fx_process() wall time, clamped to 65535
  uint16_t loop_us = 0;  // full audio-loop iteration, clamped to 65535
  uint8_t model_changed = 0;
  uint8_t dsp_miss = 0;  // dsp_us > 1333.33 us
  uint8_t loop_miss = 0; // loop_us > 1333.33 us
  // B4D.3 same-block voice-0 renderer counters (no index join needed).
  uint8_t new_grains = 0;
  uint8_t exp_warp = 0;
  uint8_t active_desc = 0;
  uint8_t slices = 0;
  // B4D.5 paired-block matching inputs (voice 0).
  float f0 = 0.0f;
  uint8_t source_grains = 0;
  // B4D.7 scheduler iterations in the block.
  uint16_t schedule_attempts = 0;
  uint32_t sched_cycles = 0;
  uint32_t addgrain_cycles = 0;
  uint32_t deferred_cycles = 0;
  uint8_t model_new_count = 0;
  uint8_t warp_hit_count = 0;
  uint8_t warp_miss_count = 0;
  uint32_t mark_cycles = 0;
  uint32_t warp_phase_cycles = 0;
  uint32_t desc_cycles = 0;
  uint32_t near_cycles = 0;
  uint32_t poly_cycles = 0;
  uint32_t gn_cycles = 0;
  uint32_t cl_cycles = 0;
  uint32_t ch_cycles = 0;
  uint32_t ord_mark[3]{};
  uint32_t ord_warp[3]{};
  uint32_t ord_desc[3]{};
  uint32_t ord_near[3]{};
  uint32_t ord_sel[3]{};
  uint32_t ord_align[3]{};
  uint16_t mark_count = 0;
  uint32_t prewarm_cycles = 0;
  int32_t debt_samples = 0;
  uint32_t output_period_q8 = 0;
  int32_t g_dest[4]{};
  uint16_t g_half[4]{};
  uint8_t reserved = 0;
};
constexpr size_t kB4D1BlockRecordCapacity = 65536;

// -----------------------------------------------------------------------------
// B4D.12 burn-in streaming telemetry.
//
// The B4D.1 per-block ring cannot cover multi-hour soaks (it stores every
// block). B4D.12 instead keeps bounded aggregates and rings, updated once per
// audio block on the audio task. It never logs, and it is only allocated when
// the burn-in harness calls b4d12_telemetry_init(), so production modes pay
// nothing beyond a null-pointer check. PSRAM-backed.
// -----------------------------------------------------------------------------

// One late DSP block (dsp_us > deadline) plus its recovery context. nextN are
// the DSP times of the next blocks; recovery_blocks is the number of blocks
// after this one needed to return at or below the deadline (0xFF pending,
// 0 = still late after three blocks, 8 = saturated).
// Trivial (POD) so the PSRAM block can be zeroed with memset and no huge
// temporary is ever placed on the stack.
struct B4D12LateEvent {
  uint32_t block_id;
  uint16_t dsp_us;
  uint16_t lateness_us;
  uint8_t klass;  // 0=NMC, 1=MC+1, 2=MC+2, 3=MC+3+, 4=MC+0
  uint8_t model_changed;
  uint8_t new_grains;
  uint8_t active_desc;
  uint8_t slices;
  uint8_t prev_miss;
  uint16_t next1_dsp_us;
  uint16_t next2_dsp_us;
  uint16_t next3_dsp_us;
  uint8_t recovery_blocks;
  uint8_t reserved;
  uint16_t backlog_before_ds;  // analysis backlog before, 0.1 ms units
  uint16_t backlog_after_ds;   // analysis backlog at recovery
};
constexpr size_t kB4D12LateEventCapacity = 2048;
constexpr size_t kB4D12TopCapacity = 100;
#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
// Observational MC+2/MC+3 timing context. Kept in the existing PSRAM-backed
// B4D.12 telemetry allocation and written only by the audio task.
struct B4D12PredictionSlackEvent {
  uint32_t block_id;
  uint16_t dsp_us[4];  // N-3, N-2, N-1, N
  int32_t destination_lead[4];  // destination center - scheduling block start
  uint32_t selected_model_serial[4];
  uint32_t prior_model_serial[3];
  uint8_t mark_available[3][4];  // 0=no, 1=yes, 2=no prior snapshot
  uint8_t model_available[3][4]; // same; serial 0 means no model selected
  PsolaGrainAuditRecord grain[4];
  // 0=no candidate, 1=projected from earlier state, 2=not predictable,
  // 3=ambiguous concurrent model publication.
  uint8_t prediction_status[3][4];
  uint64_t predicted_destination_bits[3][4];
  uint64_t predicted_source_bits[3][4];
  uint64_t predicted_mark[3][4];
  uint32_t predicted_period_bits[3][4];
  uint16_t predicted_half[3][4];
  uint32_t predicted_model_serial[3][4];
  uint8_t predicted_lpc_enabled[3][4];
  uint32_t predicted_formant_shift_bits[3];
  uint32_t predicted_formant_amount_bits[3];
  uint32_t predicted_gamma_bits[3];
  uint32_t predicted_lambda_bits[3];
  uint32_t predicted_sample_rate_bits[3];
  uint8_t predicted_formant_mode[3];
  uint8_t predicted_normalization_strategy[3];
  uint32_t actual_pitch_period_bits;
  uint32_t prior_pitch_period_bits[3];
  uint8_t actual_pitch_onset;
  uint8_t actual_pitch_changed;
  uint8_t actual_track_state;
  uint8_t actual_recovery_active;
  uint8_t actual_have_cursor;
  uint8_t prior_have_cursor[3];
  uint8_t klass;
  uint8_t grains;
};
constexpr size_t kB4D12PredictionSlackCapacity = 2048;
#endif
struct B4D12ForensicRecord {
  uint32_t block_id;
  uint32_t dsp_us;
  uint32_t cycle_us;
  HarmonizerBlockTraceRecord harmony;
  PsolaGrainAuditRecord grain[4];
};
constexpr uint32_t kB4D12Late1200Us = 1200;

// One 10-minute window aggregate (capacity covers >150 h at 10-min windows).
constexpr size_t kB4D12WindowHistBins = 24;  // 100 us bins, last is overflow
struct B4D12WindowStats {
  uint64_t blocks;
  uint64_t dsp_sum_us;
  uint32_t dsp_max_us;
  uint32_t misses;
  uint32_t mc2;
  uint32_t mc3;
  uint32_t max_consecutive_late;
  uint32_t backlog_max_ds;
  uint64_t transport_errors;
  uint64_t cumulative_lateness_us;
  uint32_t hist[kB4D12WindowHistBins];
};
constexpr size_t kB4D12WindowCapacity = 1024;

struct B4D12Telemetry {
  static constexpr size_t kLatenessBins = 6;
  static constexpr size_t kDspHistBins = 65536;
  static constexpr size_t kGrainHistBins = 512; // 1024-cycle bins, last saturates
  static constexpr size_t kRecoveryBins = 8;
  // Exact whole-DSP histogram (1 us bins) over every observed block.
  uint32_t dsp_hist[kDspHistBins];
  // Exact class distributions for conditional latency, saturated at 4095 us.
  uint32_t class_hist[5][4096];
  uint64_t class_sum_us[5];
  uint32_t class_max_us[5];
  // Whole-run cache outcomes by model-change/grain class and grain ordinal.
  uint32_t grain_cache_path[5][4][5];
  uint32_t grain_cache_difference[5][4][8];
  uint32_t grain_add_hist[5][4][kGrainHistBins];
  uint64_t grain_add_sum[5][4];
  uint64_t grain_select_sum[5][4];
  uint64_t grain_near_sum[5][4];
  uint64_t grain_lookup_sum[5][4];
  uint64_t grain_poly_sum[5][4];
  uint64_t grain_gain_sum[5][4];
  uint32_t grain_add_max[5][4];
  uint32_t grain_count[5][4];
  B4D12LateEvent late[kB4D12LateEventCapacity];
  B4D12ForensicRecord top[kB4D12TopCapacity];
  B4D12ForensicRecord miss[kB4D12LateEventCapacity];
#if defined(CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER)
  B4D12PredictionSlackEvent prediction_slack[kB4D12PredictionSlackCapacity];
  uint16_t prior_dsp_us[3]{};
  PitchMark prior_marks[3][64]{};
  PsolaPredictionCursor prior_cursor[3]{};
  LpcPublishedRef prior_model_refs[3][16]{};
  uint8_t prior_model_count[3]{};
  uint8_t prior_model_coherent[3]{};
  uint32_t prior_model_serial[3]{};
  uint8_t prior_mark_count[3]{};
  uint8_t prior_snapshot_write = 0;
  uint8_t prior_snapshots_seen = 0;
  uint32_t prediction_slack_count = 0;
  uint32_t prediction_slack_dropped = 0;
  uint32_t prediction_mc1_seen = 0;
  uint32_t prediction_mc1_sampled = 0;
#endif
  uint32_t top_count;
  uint32_t top_min_us;
  uint32_t miss_count;
  uint32_t miss_dropped;
  B4D12WindowStats windows[kB4D12WindowCapacity];
  uint64_t blocks;
  uint64_t dsp_sum_us;
  uint32_t dsp_max_us;
  uint64_t misses;
  uint64_t late_1200_blocks;
  uint64_t current_consecutive_late;
  uint64_t max_consecutive_late;
  uint64_t late_events;
  uint64_t late_events_dropped;
  uint64_t lateness_hist[kLatenessBins];
  uint64_t miss_nmc, miss_mc0, miss_mc1, miss_mc2, miss_mc3;
  uint64_t blocks_nmc, blocks_mc0, blocks_mc1, blocks_mc2, blocks_mc3;
  uint64_t recovery_hist[kRecoveryBins];
  uint64_t recovery_pending;
  uint64_t cumulative_lateness_us;
  uint64_t blocks_per_window;
  uint64_t window_count;
  uint64_t window_index;
  uint64_t window_transport_base;
  uint64_t window_lateness_base;
  B4D12WindowStats window;
  size_t late_write;
  uint8_t open_event;
  uint8_t open_next;
  uint8_t prev_late;
  uint8_t reserved;
};

// B4C.7 decomposition sections (§19): 20 global + 20 per voice.
struct B4C7DecompTotals {
  static constexpr size_t kGlobals = 20;
  static constexpr size_t kVoiceSubs = 25;
  static constexpr size_t kClasses = 4;
  uint64_t count[kClasses]{};
  uint64_t loop_sum[kClasses]{};
  uint64_t acct_sum[kClasses]{};
  uint64_t global[kClasses][kGlobals]{};
  uint64_t voice[kClasses][2][kVoiceSubs]{};
  // TX-prep subsections (§37): ramp / sanity+encode+peaks / rms.
  uint64_t txp_ramp = 0;
  uint64_t txp_sanity_encode = 0;
  uint64_t txp_rms = 0;
  uint64_t txp_ramp_max = 0;
  uint64_t txp_sanity_encode_max = 0;
  uint64_t txp_rms_max = 0;
  // BOTH_ACTIVE DSP distribution (10 us bins, same geometry as B4C6A).
  uint32_t both_hist[kB4C6AHistBins]{};
  uint64_t both_samples = 0;
  uint64_t both_total_us = 0;
  uint64_t both_max_us = 0;
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

struct SyntheticInputIdentityTelemetry {
  uint64_t checks = 0;
  uint64_t mismatches = 0;
  float synthetic_rms = 0.0f;
  float pitch_tap_rms = 0.0f;
  float dsp_input_rms = 0.0f;
  uint32_t synthetic_checksum = 0;
  uint32_t pitch_tap_checksum = 0;
  uint32_t dsp_input_checksum = 0;
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

  // Coordinator-only helper: makes pre-staged stimulus generation use the
  // same nominal rate the pipeline will later run at (init() overwrites it
  // with the same value). No effect on I2S until init().
  void set_prestage_sample_rate(uint32_t fs) { config_.sample_rate = fs; }

  // Mode & Signal controls
  void set_mode(AudioI2sMode mode);
  void set_tx_signal(TxSignalType sig, float dbfs);

  // Counters & Timing
  AudioTransportCounters &counters() { return counters_; }
  const AudioTransportCounters &counters() const { return counters_; }

  // B4D.3S: the block deadline follows the configured sample rate
  // (block_size / sample_rate). No hard-coded 1333.33 us.
  double block_deadline_us() const {
    return config_.sample_rate
               ? (1000000.0 * static_cast<double>(block_size_) /
                  static_cast<double>(config_.sample_rate))
               : 1333.3333333333;
  }

  void reset_counters() {
    counters_.reset();
    dsp_total_us_ = 0;
    cycle_total_us_ = 0;
    wake_total_us_ = 0;
    dsp_worst_us_ = 0;
    cycle_worst_us_ = 0;
    wake_worst_us_ = 0;
    // PSRAM histograms are null until init() allocates (reset runs first).
    uint32_t *legacy[] = {dsp_hist_, cycle_hist_, wake_hist_};
    for (uint32_t *h : legacy)
      if (h) std::fill_n(h, kHistBins, 0);
    outlier_count_ = 0;
    reset_b4c6a_stats();
    reset_b4c7_stats();
  }
  TimingPercentiles calculate_dsp_percentiles() const;
  TimingPercentiles calculate_cycle_percentiles() const;
  TimingPercentiles calculate_wake_percentiles() const;
  void print_b4c6a_forensics(bool dump_samples = true) const;
  void reset_b4c6a_stats();
  // B4C.7 extensions.
  void print_b4c7_forensics(bool dump_samples = true) const;
  void reset_b4c7_stats();
  void set_b4c7_input(B4C7InputKind kind) {
    b4c7_input_ = kind;
  }
  void set_b4c7_prestaged(const float *buf, size_t frames) {
    b4c7_pre_buf_ = buf;
    b4c7_pre_frames_ = frames;
    b4c7_pre_idx_ = 0;
    b4c7_pre_overruns_ = 0;
  }
  uint64_t b4c7_prestaged_overruns() const { return b4c7_pre_overruns_; }
  const B4C7DecompTotals *b4c7_totals() const { return b4c7_totals_; }
  uint64_t b4c7_block_rows() const { return b4c7_block_count_; }
  uint64_t b4c7_block_dropped() const { return b4c7_block_dropped_; }

  // B4D.1 cleaned-qualification recorder. Allocation is explicit and happens
  // outside the measured window; reset() clears it and restarts block ids.
  bool b4d1_recorder_init(size_t capacity = kB4D1BlockRecordCapacity);
  void b4d1_recorder_free();
  void b4d1_recorder_reset(uint32_t origin_block_id = 0);
  void b4d1_recorder_enable(bool enabled) {
    b4d1_record_enabled_.store(enabled, std::memory_order_release);
  }
  bool b4d1_recorder_enabled() const {
    return b4d1_record_enabled_.load(std::memory_order_acquire);
  }
  size_t b4d1_recorder_count() const {
    return b4d1_record_count_.load(std::memory_order_acquire);
  }
  bool b4d1_recorder_get(size_t index, B4D1BlockRecord *out) const;

  // B4D.12 burn-in streaming telemetry (PSRAM, off until explicitly enabled).
  bool b4d12_telemetry_init(uint32_t sample_rate,
                            uint32_t window_seconds = 600);
  void b4d12_telemetry_free();
  void b4d12_telemetry_reset();
  void b4d12_telemetry_enable(bool enabled) {
    b4d12_enabled_.store(enabled, std::memory_order_release);
  }
  bool b4d12_telemetry_enabled() const {
    return b4d12_enabled_.load(std::memory_order_acquire);
  }
  const B4D12Telemetry *b4d12_telemetry() const { return b4d12_tee_; }
  // Host-testable B4C.6A accounting helpers (no ESP dependency).
  static double b4c6a_reconciliation_pct(uint64_t accounted_us,
                                         uint64_t total_us);
  static double b4c6a_effective_fs(double frames, double wall_s);
  static double b4c6a_timeline_lost_s(double wall_s, double effective_fs);
  static double b4c6a_expected_blocks(double wall_s);
  TimingPercentiles b4c6a_loop_percentiles() const;
  TimingPercentiles b4c6a_period_percentiles() const;
  TimingPercentiles b4c6a_dsp_forensic_percentiles() const;
  TimingPercentiles b4c6a_rx_percentiles() const;
  TimingPercentiles b4c6a_tx_percentiles() const;
  TimingPercentiles b4c6a_gap_percentiles() const;
  uint64_t b4c6a_sample_count() const { return b4c6a_totals_.samples; }
  uint64_t b4c6a_dropped_samples() const { return b4c6a_timing_dropped_; }
  // Diagnostic feed used by firmware run() and host tests. Sections must be
  // non-overlapping; other_us is the in-iteration residual.
  void b4c6a_note_iteration(uint32_t rx_wait, uint32_t rx_copy, uint32_t fixture,
                            uint32_t dsp, uint32_t tx_prep, uint32_t tx_wait,
                            uint32_t book, uint32_t other, uint32_t loop_total,
                            uint32_t loop_period, uint32_t inter_gap,
                            uint32_t loop_cycles, uint32_t rx_bytes, uint32_t tx_bytes,
                            uint16_t rx_frames, uint16_t tx_frames, int rx_rc,
                            int tx_rc, uint64_t begin_us, uint64_t end_us,
                            bool success);
  void set_b4c6a_detail(uint8_t level) { b4c6a_detail_ = level; }
  uint8_t b4c6a_detail() const { return b4c6a_detail_; }
  const B4C6ASectionTotals &b4c6a_totals() const { return b4c6a_totals_; }

  uint64_t max_single_block_lateness_us() const {
    return counters_.max_single_block_lateness_us.load(std::memory_order_relaxed);
  }
  uint64_t cumulative_lateness_us() const {
    return counters_.cumulative_lateness_us.load(std::memory_order_relaxed);
  }
  uint64_t max_consecutive_late_blocks() const {
    return counters_.max_consecutive_late_blocks.load(std::memory_order_relaxed);
  }

  // Synthetic Voiced Source (Stage B4B)
  void generate_synthetic_voiced_mono(float *mono, size_t frames);
  void generate_b4b2_pure_sine(float *mono, size_t frames);
  void generate_b4b6_mono(float *mono, size_t frames);
  void configure_b4b6_stimulus(B4B6StimulusKind kind, float f0_hz,
                               const uint8_t *vocal_mulaw = nullptr,
                               size_t vocal_samples = 0);
  void set_b4b6_dsp_paused(bool paused) {
    b4b6_dsp_paused_.store(paused, std::memory_order_release);
  }
  bool b4b6_dsp_active() const {
    return b4b6_dsp_active_.load(std::memory_order_acquire);
  }
  void set_b4c1_transport_only(bool transport_only) {
    b4c1_transport_only_.store(transport_only, std::memory_order_release);
  }
  bool b4c1_transport_only() const {
    return b4c1_transport_only_.load(std::memory_order_acquire);
  }
  float current_synth_freq_hz() const { return current_synth_f0_; }
  bool is_current_synth_voiced() const { return current_synth_voiced_; }
  StimulusState current_stimulus_state() const { return current_stimulus_state_; }
  float current_stimulus_rms() const { return current_stimulus_rms_; }
  uint32_t current_block_checksum() const { return current_block_checksum_; }
  SyntheticInputIdentityTelemetry synthetic_input_identity() const;
  typedef void (*SyntheticBlockHook)(uint64_t sample_in_cycle, float f0, void *user_data);
  void set_synthetic_block_hook(SyntheticBlockHook hook, void *user_data) {
    synth_hook_ = hook;
    synth_hook_user_data_ = user_data;
  }

  // Transport Error Event Logging (Req 3)
  void record_transport_error(const char *type, uint64_t block_idx, uint64_t dsp_us, uint64_t rx_gap, uint64_t tx_gap, uint64_t wake_us);
  size_t transport_error_count() const {
    return transport_error_count_.load(std::memory_order_relaxed);
  }
  const TransportErrorEvent &transport_error(size_t index) const { return transport_errors_[index % kMaxTransportErrorEvents]; }
  void print_transport_errors() const;

  // Output Sanity & Level Stats
  float output_peak_l() const { return out_peak_l_; }
  float output_peak_r() const { return out_peak_r_; }
  float output_rms_l() const { return out_rms_l_; }
  float output_rms_r() const { return out_rms_r_; }
  // B4D.2: output RMS is telemetry-only; disable it in production to keep the
  // two per-sample multiply-accumulates out of the TX hot path.
  void set_output_rms_enabled(bool enabled) { output_rms_enabled_ = enabled; }
  bool output_rms_enabled() const { return output_rms_enabled_; }

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

  void set_diagnostic_activity(bool snapshot, bool formatting,
                               bool serial_printing, bool queue_flush) {
    snapshot_active_.store(snapshot, std::memory_order_relaxed);
    telemetry_formatting_active_.store(formatting, std::memory_order_relaxed);
    serial_printing_active_.store(serial_printing, std::memory_order_relaxed);
    diagnostic_queue_flush_active_.store(queue_flush,
                                         std::memory_order_relaxed);
  }
  void set_pitch_worker_active(bool active) {
    pitch_worker_active_.store(active, std::memory_order_relaxed);
  }
  bool callbacks_enabled() const {
    return callbacks_enabled_.load(std::memory_order_acquire);
  }

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

  // Timing histograms (320 bins of 50 us: 0..16000 us)
  static constexpr size_t kHistBins = 320;
  static constexpr uint64_t kBinWidthUs = 50;
  // PSRAM-backed like the B4C.6A histograms (same rationale).
  uint32_t *dsp_hist_ = nullptr;
  uint32_t *cycle_hist_ = nullptr;
  uint32_t *wake_hist_ = nullptr;
  uint64_t dsp_total_us_ = 0;
  uint64_t cycle_total_us_ = 0;
  uint64_t wake_total_us_ = 0;
  uint64_t dsp_worst_us_ = 0;
  uint64_t cycle_worst_us_ = 0;
  uint64_t wake_worst_us_ = 0;

  B4C7BlockRecord *b4c7_blocks_ = nullptr;
  size_t b4c7_block_capacity_ = 0;
  uint64_t b4c7_block_count_ = 0;
  uint64_t b4c7_block_dropped_ = 0;

  // B4D.1 per-block recorder state (PSRAM ring, off unless enabled).
  B4D1BlockRecord *b4d1_records_ = nullptr;
  size_t b4d1_record_capacity_ = 0;
  std::atomic<size_t> b4d1_record_count_{0};
  uint32_t b4d1_record_origin_ = 0;
  std::atomic<bool> b4d1_record_enabled_{false};
  // B4D.12 streaming telemetry accumulator (PSRAM pointer; 4 bytes of .bss).
  B4D12Telemetry *b4d12_tee_ = nullptr;
  std::atomic<bool> b4d12_enabled_{false};
  void b4d12_note_block(uint64_t dsp_us, uint64_t cycle_us, uint64_t block_idx);
  // PSRAM-backed: ~3.7 KB of per-block counters must not consume internal
  // .bss (the frozen static audio stacks already fill it near the limit).
  B4C7DecompTotals *b4c7_totals_ = nullptr;
  B4C7InputKind b4c7_input_ = B4C7InputKind::Prestaged;
  const float *b4c7_pre_buf_ = nullptr;
  size_t b4c7_pre_frames_ = 0;
  size_t b4c7_pre_idx_ = 0;
  uint64_t b4c7_pre_overruns_ = 0;
  uint64_t b4c7_block_seq_ = 0;
  PsolaSourceGrainAudit b4c7_audit_start_{};
  B4C7SliceAuditSnapshot b4c7_slice_start_{};
  PsolaSourceResidualCacheStats b4c7_fir_start_[2];
  // B4C.7 per-block DSP hook (ESP only; see audio_i2s.cpp).
  void b4c7_process_block(float *mono, float *l, float *r, size_t n,
                          int64_t *t_dsp_start, int64_t *t_dsp_done,
                          uint64_t *pure_dsp_us, uint8_t *block_class,
                          uint32_t *v0_us, uint32_t *v1_us, uint32_t *glob_us,
                          uint16_t *slices0, uint16_t *slices1,
                          uint16_t *sched0, uint16_t *sched1);
  B4C6ATimingRecord *b4c6a_timing_ = nullptr;
  size_t b4c6a_timing_capacity_ = 0;
  uint64_t b4c6a_timing_count_ = 0;
  uint64_t b4c6a_timing_dropped_ = 0;
  uint8_t b4c6a_detail_ = 2;
  B4C6ASectionTotals b4c6a_totals_{};
  // PSRAM-backed (B4C.7 link budget): 6 x 400 x 4 B of per-block
  // distribution counters. Written once per block in bookkeeping (never in
  // the DSP interval); read only in post-window prints.
  uint32_t *b4c6a_loop_hist_ = nullptr;
  uint32_t *b4c6a_period_hist_ = nullptr;
  uint32_t *b4c6a_dsp_hist_ = nullptr;
  uint32_t *b4c6a_rx_hist_ = nullptr;
  uint32_t *b4c6a_tx_hist_ = nullptr;
  uint32_t *b4c6a_gap_hist_ = nullptr;
  // NOTE (B4C.7 link budget): rx_copy/fixture per-block histograms removed;
  // their totals still feed the EQUATION avgs. Saves 3.2 KB internal .bss.
  uint64_t b4c6a_rx_partial_ = 0;
  uint64_t b4c6a_tx_partial_ = 0;
  uint64_t b4c6a_rx_zero_ = 0;
  uint64_t b4c6a_tx_zero_ = 0;
  uint64_t b4c6a_last_end_us_ = 0;
  uint64_t b4c6a_last_begin_us_ = 0;
  bool b4c6a_prev_late_ = false;
  uint32_t b4c6a_recover_run_ = 0;

  // Synthetic voiced generator state (Stage B4B)
  uint64_t synth_sample_idx_ = 0;
  float synth_phase_ = 0.0f;
  float current_synth_f0_ = 110.0f;
  bool current_synth_voiced_ = false;
  StimulusState current_stimulus_state_ = StimulusState::Silence;
  float current_stimulus_rms_ = 0.0f;
  uint32_t current_block_checksum_ = 0;
  std::atomic<uint64_t> identity_checks_{0}, identity_mismatches_{0};
  std::atomic<uint64_t> identity_sequence_seen_{0};
  std::atomic<uint32_t> identity_synth_rms_bits_{0},
      identity_pitch_rms_bits_{0}, identity_dsp_rms_bits_{0};
  std::atomic<uint32_t> identity_synth_checksum_{0},
      identity_pitch_checksum_{0}, identity_dsp_checksum_{0};
  uint64_t last_sample_in_cycle_ = 0;
  SyntheticBlockHook synth_hook_ = nullptr;
  void *synth_hook_user_data_ = nullptr;

  // Transport error event ring buffer (Req 3)
  TransportErrorEvent transport_errors_[kMaxTransportErrorEvents]{};
  std::atomic<size_t> transport_error_count_{0};

  std::atomic<bool> snapshot_active_{false},
      telemetry_formatting_active_{false}, serial_printing_active_{false},
      pitch_worker_active_{false}, diagnostic_queue_flush_active_{false};
  std::atomic<bool> callbacks_enabled_{false};

  // Output level tracking
  float out_peak_l_ = 0.0f;
  float out_peak_r_ = 0.0f;
  float out_rms_l_ = 0.0f;
  float out_rms_r_ = 0.0f;
  bool output_rms_enabled_ = true;

  // Internal signal generator state
  float sine_phase_l_ = 0.0f;
  float sine_phase_r_ = 0.0f;
  float ramp_gain_ = 0.0f; // Soft fade-in ramp

  void record_timing(uint64_t dsp_us, uint64_t cycle_us, uint64_t wake_us, uint64_t rx_gap_us, uint64_t block_idx);
  void generate_tx_tones(float *out_l, float *out_r, size_t frames);
  void b4c6a_hist_add(uint32_t *hist, uint64_t us);
  TimingPercentiles b4c6a_hist_percentiles(const uint32_t *hist, uint64_t total_us,
                                           uint64_t max_us, uint64_t samples) const;
  B4B6StimulusKind b4b6_stimulus_ = B4B6StimulusKind::PureTone;
  float b4b6_f0_hz_ = 220.0f;
  const uint8_t *b4b6_vocal_mulaw_ = nullptr;
  size_t b4b6_vocal_samples_ = 0;
  uint32_t b4b6_noise_state_ = 0x6d2b79f5U;
  std::atomic<bool> b4b6_dsp_paused_{false};
  std::atomic<bool> b4b6_dsp_active_{false};
  std::atomic<bool> b4c1_transport_only_{false};
  void update_adc_diagnostics(const float *l, const float *r, const int32_t *raw, size_t frames);

#ifdef ESP_PLATFORM
  void *tx_chan_ = nullptr;
  void *rx_chan_ = nullptr;
  bool tx_enabled_ = false;
  bool rx_enabled_ = false;
#endif
};

} // namespace vocal_fx_platform
