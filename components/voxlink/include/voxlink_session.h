#pragma once
// VoxLink v1 server session: parser + dispatcher + bounded TX ring. It is
// platform-neutral so it can be exercised entirely in host tests. The UART
// task only feeds bytes in and drains bytes out; it never touches DSP state.
#include "voxlink_parser.h"
#include "voxlink_registry.h"
#include "voxlink_state.h"
#include <cstddef>
#include <cstdint>

namespace voxlink {

enum class ConnectionState : uint8_t {
  Disconnected = 0,
  HelloReceived = 1,
  Active = 2,
};

// Application-installed bridge from a public parameter to the existing
// bounded SPSC runtime queue. Returns false when the queue rejected the update.
using ParamSubmitFn = bool (*)(uint16_t param_id, float value, void *user);

struct SessionConfig {
  uint32_t capability_flags = 0;
  uint32_t sample_rate = 44100;
  uint16_t block_size = 64;
  uint8_t harmony_voice_count = 1;
  uint8_t product_id = 0;
  uint8_t hw_target = 0;
  uint8_t fw_major = 0;
  uint8_t fw_minor = 0;
  uint8_t fw_patch = 0;
  const char *git_sha = nullptr;
  ParamSubmitFn submit = nullptr;
  void *submit_user = nullptr;
  uint32_t heartbeat_timeout_ms = 3000;
  uint32_t heartbeat_interval_ms = 1000;
  // Telemetry is only enqueued while the TX ring is below this many bytes.
  // 0 selects a safe default (3/4 of capacity).
  size_t telemetry_high_water = 0;
};

struct SessionCounters {
  uint64_t frames_rx = 0;
  uint64_t frames_tx = 0;
  uint64_t tx_bytes_dropped = 0;
  uint64_t telemetry_dropped = 0;
  uint64_t protocol_response_dropped = 0;
  uint64_t control_commands_accepted = 0;
  uint64_t control_commands_rejected = 0;
  uint64_t control_queue_full = 0;
  uint64_t unknown_message = 0;
  uint64_t unsupported_version = 0;
  uint64_t heartbeats_rx = 0;
  uint64_t heartbeats_tx = 0;
  uint64_t reconnect_count = 0;
  uint64_t sets_received = 0;
  uint64_t gets_received = 0;
};

class Session {
public:
  static constexpr size_t kTxCapacity = 8192; // power of two
  static_assert((kTxCapacity & (kTxCapacity - 1)) == 0, "TX ring must be pow2");

  void init(const SessionConfig &cfg);

  // Feed arbitrary received bytes; valid frames are dispatched immediately.
  void feed(const uint8_t *data, size_t len);

  // Drain up to `max` bytes of pending TX. Returns bytes written.
  size_t take_tx(uint8_t *dst, size_t max);

  // Drives heartbeat emission and timeout. Call periodically from the control
  // task. Does not alter audio state on disconnect.
  void tick(uint32_t now_ms);

  // Asynchronous notifications (server-initiated, low priority).
  bool send_meter(float input_peak, float input_rms, float harmony_peak,
                  float harmony_rms, float output_peak, float output_rms,
                  float limiter_reduction_db);
  bool send_pitch(float frequency_hz, float confidence, bool voiced,
                  float midi_note, float age_ms);
  bool send_dsp_status(uint32_t sample_rate, uint16_t block_size,
                       float dsp_avg_us, uint16_t dsp_peak_us,
                       uint32_t deadline_misses, uint32_t tx_underruns,
                       uint32_t rx_overruns, float backlog_ms,
                       uint32_t free_internal, uint32_t free_psram,
                       uint32_t uptime_s);
  // Broadcast an already-accepted value change from MIDI/preset/internal.
  bool notify_param_changed(uint16_t id, float value, ParamOrigin origin);

  ConnectionState connection_state() const { return connection_; }
  const SessionCounters &counters() const { return counters_; }
  const ParserCounters &parser_counters() const { return parser_.counters(); }
  const ProductState &state() const { return *state_; }

  void set_product_state(ProductState *state) { state_ = state; }

private:
  void handle_frame(const Frame &frame);
  void handle_hello(const Frame &frame);
  void handle_caps_request(const Frame &frame);
  void handle_get_state(const Frame &frame);
  void handle_get_param(const Frame &frame);
  void handle_set_param(const Frame &frame);

  bool enqueue(MsgType type, uint8_t flags, uint8_t seq, const uint8_t *payload,
               size_t payload_len, bool critical);
  void send_ack(const Frame &frame);
  void send_nack(const Frame &frame, Code code);
  void send_error(const Frame &frame, Code code);
  size_t tx_used() const;

  Parser parser_;
  ProductState *state_ = nullptr;
  SessionConfig cfg_{};
  ConnectionState connection_ = ConnectionState::Disconnected;
  SessionCounters counters_{};
  uint8_t tx_[kTxCapacity] = {0};
  size_t tx_head_ = 0; // write
  size_t tx_tail_ = 0; // read
  uint8_t server_seq_ = 0;
  uint32_t last_rx_ms_ = 0;
  uint32_t last_heartbeat_ms_ = 0;
  uint32_t now_ms_ = 0;
  size_t telemetry_high_water_ = 0;
  bool have_tick_ = false;
  uint8_t client_type_ = 0;
};

} // namespace voxlink
