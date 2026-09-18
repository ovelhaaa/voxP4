#include "voxlink_session.h"
#include <cstring>

namespace voxlink {
namespace {

uint8_t group_id_for(const char *group) {
  if (group == nullptr)
    return 0;
  if (std::strcmp(group, "harmony") == 0) return 0;
  if (std::strcmp(group, "dynamics") == 0) return 1;
  if (std::strcmp(group, "delay") == 0) return 2;
  if (std::strcmp(group, "reverb") == 0) return 3;
  if (std::strcmp(group, "output") == 0) return 4;
  return 5;
}

size_t put_value(ValueTag tag, float value, uint8_t *out, size_t capacity) {
  return encode_value(tag, value, out, capacity);
}

} // namespace

void Session::init(const SessionConfig &cfg) {
  cfg_ = cfg;
  parser_.reset();
  counters_ = SessionCounters{};
  tx_head_ = 0;
  tx_tail_ = 0;
  server_seq_ = 0;
  last_rx_ms_ = 0;
  last_heartbeat_ms_ = 0;
  now_ms_ = 0;
  have_tick_ = false;
  client_type_ = 0;
  connection_ = ConnectionState::Disconnected;
  telemetry_high_water_ = cfg.telemetry_high_water != 0
                              ? cfg.telemetry_high_water
                              : (kTxCapacity / 4) * 3;
}

size_t Session::tx_used() const {
  return (tx_head_ + kTxCapacity - tx_tail_) % kTxCapacity;
}

size_t Session::take_tx(uint8_t *dst, size_t max) {
  if (dst == nullptr)
    return 0;
  const size_t used = tx_used();
  const size_t n = (used < max) ? used : max;
  for (size_t i = 0; i < n; ++i) {
    dst[i] = tx_[tx_tail_];
    tx_tail_ = (tx_tail_ + 1) % kTxCapacity;
  }
  return n;
}

bool Session::enqueue(MsgType type, uint8_t flags, uint8_t seq,
                      const uint8_t *payload, size_t payload_len, bool critical) {
  if (payload_len > kMaxPayload)
    return false;
  uint8_t scratch[kMaxFrame];
  const size_t n = encode_frame(type, flags, seq, payload, payload_len, scratch,
                                sizeof(scratch));
  if (n == 0)
    return false;
  const size_t limit = critical ? (kTxCapacity - 1) : telemetry_high_water_;
  if (tx_used() + n > limit) {
    if (critical)
      ++counters_.protocol_response_dropped;
    else
      ++counters_.telemetry_dropped;
    ++counters_.tx_bytes_dropped;
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    tx_[tx_head_] = scratch[i];
    tx_head_ = (tx_head_ + 1) % kTxCapacity;
  }
  ++counters_.frames_tx;
  return true;
}

void Session::feed(const uint8_t *data, size_t len) {
  parser_.feed(data, len);
  Frame frame;
  while (parser_.pop(&frame)) {
    last_rx_ms_ = now_ms_;
    handle_frame(frame);
  }
}

void Session::tick(uint32_t now_ms) {
  now_ms_ = now_ms;
  if (!have_tick_) {
    // Establish a timebase so traffic received before the first tick does not
    // look like a heartbeat timeout.
    have_tick_ = true;
    last_heartbeat_ms_ = now_ms;
    last_rx_ms_ = now_ms;
  }
  if (connection_ != ConnectionState::Active)
    return;
  if (cfg_.heartbeat_timeout_ms != 0 &&
      now_ms - last_rx_ms_ > cfg_.heartbeat_timeout_ms) {
    connection_ = ConnectionState::Disconnected;
    return;
  }
  if (cfg_.heartbeat_interval_ms != 0 &&
      now_ms - last_heartbeat_ms_ >= cfg_.heartbeat_interval_ms) {
    last_heartbeat_ms_ = now_ms;
    const uint8_t seq = server_seq_++;
    if (enqueue(MsgType::Heartbeat, kFlagNotification, seq, nullptr, 0, false))
      ++counters_.heartbeats_tx;
  }
}

// ---------------------------------------------------------------------------
// Direct responses
// ---------------------------------------------------------------------------
void Session::send_ack(const Frame &frame) {
  uint8_t payload[4];
  payload[0] = static_cast<uint8_t>(frame.type);
  payload[1] = frame.seq;
  put_u16(payload + 2, static_cast<uint16_t>(Code::Ok));
  enqueue(MsgType::Ack, kFlagNone, frame.seq, payload, sizeof(payload), true);
}

void Session::send_nack(const Frame &frame, Code code) {
  uint8_t payload[4];
  payload[0] = static_cast<uint8_t>(frame.type);
  payload[1] = frame.seq;
  put_u16(payload + 2, static_cast<uint16_t>(code));
  enqueue(MsgType::Nack, kFlagNone, frame.seq, payload, sizeof(payload), true);
}

void Session::send_error(const Frame &frame, Code code) {
  uint8_t payload[4];
  payload[0] = static_cast<uint8_t>(frame.type);
  payload[1] = frame.seq;
  put_u16(payload + 2, static_cast<uint16_t>(code));
  enqueue(MsgType::Error, kFlagNone, frame.seq, payload, sizeof(payload), true);
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------
void Session::handle_hello(const Frame &frame) {
  if (frame.payload_len < 2) {
    send_nack(frame, Code::BadLength);
    return;
  }
  client_type_ = frame.payload[1];
  connection_ = ConnectionState::HelloReceived;
  ++counters_.reconnect_count;

  uint8_t payload[kMaxPayload];
  size_t n = 0;
  payload[n++] = kVersion;
  put_u32(payload + n, cfg_.capability_flags);
  n += 4;
  put_u32(payload + n, cfg_.sample_rate);
  n += 4;
  put_u16(payload + n, cfg_.block_size);
  n += 2;
  payload[n++] = cfg_.harmony_voice_count;
  const uint32_t revision = state_ != nullptr ? state_->revision() : 0;
  put_u32(payload + n, revision);
  n += 4;
  payload[n++] = cfg_.product_id;
  payload[n++] = cfg_.hw_target;
  payload[n++] = cfg_.fw_major;
  payload[n++] = cfg_.fw_minor;
  payload[n++] = cfg_.fw_patch;
  const char *sha = cfg_.git_sha != nullptr ? cfg_.git_sha : "";
  size_t sha_len = std::strlen(sha);
  if (sha_len > 40)
    sha_len = 40;
  payload[n++] = static_cast<uint8_t>(sha_len);
  if (sha_len > 0) {
    std::memcpy(payload + n, sha, sha_len);
    n += sha_len;
  }
  enqueue(MsgType::HelloAck, kFlagNone, frame.seq, payload, n, true);
  connection_ = ConnectionState::Active;
}

void Session::handle_caps_request(const Frame &frame) {
  const size_t count = registry_count();
  uint8_t payload[kMaxPayload];
  size_t n = 0;
  payload[n++] = kVersion;
  put_u32(payload + n, cfg_.capability_flags);
  n += 4;
  put_u16(payload + n, static_cast<uint16_t>(count));
  n += 2;
  enqueue(MsgType::CapsBegin, kFlagNone, frame.seq, payload, n, true);

  const ParamDescriptor *params = registry_params();
  const uint32_t revision = state_ != nullptr ? state_->revision() : 0;
  (void)revision;
  for (size_t i = 0; i < count; ++i) {
    const ParamDescriptor &d = params[i];
    n = 0;
    put_u16(payload + n, d.id);
    n += 2;
    payload[n++] = static_cast<uint8_t>(d.type);
    put_u32(payload + n, d.flags);
    n += 4;
    put_f32(payload + n, d.min_value); n += 4;
    put_f32(payload + n, d.max_value); n += 4;
    put_f32(payload + n, d.default_value); n += 4;
    put_f32(payload + n, d.step); n += 4;
    float current = d.default_value;
    if (state_ != nullptr)
      state_->get_value(d.id, &current);
    put_f32(payload + n, current); n += 4;
    payload[n++] = group_id_for(d.group);
    enqueue(MsgType::CapsParam, kFlagNone, frame.seq, payload, n, true);
  }

  n = 0;
  put_u16(payload + n, static_cast<uint16_t>(count));
  n += 2;
  enqueue(MsgType::CapsEnd, kFlagNone, frame.seq, payload, n, true);
}

void Session::handle_get_state(const Frame &frame) {
  const size_t count = state_ != nullptr ? state_->value_count() : 0;
  const uint32_t revision = state_ != nullptr ? state_->revision() : 0;
  uint8_t payload[kMaxPayload];
  size_t n = 0;
  put_u32(payload + n, revision); n += 4;
  put_u16(payload + n, static_cast<uint16_t>(count)); n += 2;
  enqueue(MsgType::StateBegin, kFlagNone, frame.seq, payload, n, true);

  for (size_t i = 0; i < count; ++i) {
    uint16_t id = 0;
    float value = 0.0f;
    if (!state_->value_at(i, &id, &value))
      continue;
    const ParamDescriptor *d = find_param(id);
    if (d == nullptr)
      continue;
    n = 0;
    put_u16(payload + n, id); n += 2;
    payload[n++] = static_cast<uint8_t>(d->type);
    const size_t vn = put_value(d->type, value, payload + n, kMaxPayload - n);
    if (vn == 0)
      continue;
    n += vn;
    enqueue(MsgType::StateParam, kFlagNone, frame.seq, payload, n, true);
  }

  n = 0;
  put_u32(payload + n, revision);
  n += 4;
  enqueue(MsgType::StateEnd, kFlagNone, frame.seq, payload, n, true);
}

void Session::handle_get_param(const Frame &frame) {
  ++counters_.gets_received;
  if (frame.payload_len < 2) {
    send_nack(frame, Code::BadLength);
    return;
  }
  const uint16_t id = get_u16(frame.payload);
  const ParamDescriptor *d = find_param(id);
  if (d == nullptr) {
    send_nack(frame, Code::UnknownParam);
    return;
  }
  float value = d->default_value;
  if (state_ != nullptr)
    state_->get_value(id, &value);
  uint8_t payload[kMaxPayload];
  size_t n = 0;
  put_u16(payload + n, id); n += 2;
  put_u32(payload + n, state_ != nullptr ? state_->revision() : 0); n += 4;
  payload[n++] = static_cast<uint8_t>(d->type);
  const size_t vn = put_value(d->type, value, payload + n, kMaxPayload - n);
  n += vn;
  enqueue(MsgType::ParamValue, kFlagNone, frame.seq, payload, n, true);
}

void Session::handle_set_param(const Frame &frame) {
  ++counters_.sets_received;
  if (frame.payload_len < 3) {
    send_nack(frame, Code::BadLength);
    return;
  }
  const uint16_t id = get_u16(frame.payload);
  const ValueTag tag = static_cast<ValueTag>(frame.payload[2]);
  float raw = 0.0f;
  size_t consumed = 0;
  if (!decode_value(tag, frame.payload + 3, frame.payload_len - 3, &raw,
                    &consumed)) {
    send_nack(frame, Code::BadType);
    return;
  }

  float value = 0.0f;
  switch (normalize_param_value(id, tag, static_cast<double>(raw), &value)) {
  case NormalizeStatus::UnknownParam:
    send_nack(frame, Code::UnknownParam);
    return;
  case NormalizeStatus::ReadOnly:
    send_nack(frame, Code::ReadOnly);
    return;
  case NormalizeStatus::TypeMismatch:
    send_nack(frame, Code::BadType);
    return;
  case NormalizeStatus::NonFinite:
    send_nack(frame, Code::Malformed);
    return;
  case NormalizeStatus::EnumInvalid:
    send_nack(frame, Code::InvalidEnum);
    return;
  case NormalizeStatus::Ok:
  case NormalizeStatus::Clamped:
    break;
  }

  if (cfg_.submit != nullptr) {
    if (!cfg_.submit(id, value, cfg_.submit_user)) {
      ++counters_.control_queue_full;
      ++counters_.control_commands_rejected;
      send_nack(frame, Code::QueueFull);
      return;
    }
  }

  const ParamDescriptor *d = find_param(id);
  bool changed = false;
  if (state_ != nullptr)
    changed = state_->set_value(id, value);
  (void)changed;
  ++counters_.control_commands_accepted;

  send_ack(frame);
  // Authoritative confirmation: always emitted so the controller displays the
  // P4-confirmed value even when it matches what was requested.
  uint8_t payload[kMaxPayload];
  size_t n = 0;
  put_u16(payload + n, id); n += 2;
  put_u32(payload + n, state_ != nullptr ? state_->revision() : 0); n += 4;
  payload[n++] = static_cast<uint8_t>(ParamOrigin::Controller);
  payload[n++] = static_cast<uint8_t>(d->type);
  const size_t vn = put_value(d->type, value, payload + n, kMaxPayload - n);
  n += vn;
  enqueue(MsgType::ParamChanged, kFlagNone, frame.seq, payload, n, true);
}

void Session::handle_frame(const Frame &frame) {
  switch (frame.type) {
  case MsgType::Hello:
    handle_hello(frame);
    break;
  case MsgType::Heartbeat:
    ++counters_.heartbeats_rx;
    send_ack(frame);
    break;
  case MsgType::CapsRequest:
    handle_caps_request(frame);
    break;
  case MsgType::GetState:
    handle_get_state(frame);
    break;
  case MsgType::GetParam:
    handle_get_param(frame);
    break;
  case MsgType::SetParam:
    handle_set_param(frame);
    break;
  case MsgType::Ack:
  case MsgType::Nack:
  case MsgType::Error:
  case MsgType::MeterFrame:
  case MsgType::PitchFrame:
  case MsgType::DspStatus:
    // Client-originated traffic we intentionally ignore in v1.
    break;
  case MsgType::Action:
  case MsgType::PresetList:
  case MsgType::PresetLoad:
  case MsgType::PresetSave:
  case MsgType::PresetRename:
  case MsgType::MidiEvent:
  case MsgType::SceneOp:
    send_nack(frame, Code::NotSupported);
    break;
  default:
    ++counters_.unknown_message;
    send_error(frame, Code::UnknownMessage);
    break;
  }
}

// ---------------------------------------------------------------------------
// Asynchronous notifications
// ---------------------------------------------------------------------------
bool Session::send_meter(float input_peak, float input_rms, float harmony_peak,
                         float harmony_rms, float output_peak, float output_rms,
                         float limiter_reduction_db) {
  uint8_t payload[7 * 4];
  size_t n = 0;
  put_f32(payload + n, input_peak); n += 4;
  put_f32(payload + n, input_rms); n += 4;
  put_f32(payload + n, harmony_peak); n += 4;
  put_f32(payload + n, harmony_rms); n += 4;
  put_f32(payload + n, output_peak); n += 4;
  put_f32(payload + n, output_rms); n += 4;
  put_f32(payload + n, limiter_reduction_db); n += 4;
  return enqueue(MsgType::MeterFrame, kFlagNotification, server_seq_++, payload,
                 n, false);
}

bool Session::send_pitch(float frequency_hz, float confidence, bool voiced,
                         float midi_note, float age_ms) {
  uint8_t payload[4 * 4 + 1];
  size_t n = 0;
  put_f32(payload + n, frequency_hz); n += 4;
  put_f32(payload + n, confidence); n += 4;
  payload[n++] = voiced ? 1u : 0u;
  put_f32(payload + n, midi_note); n += 4;
  put_f32(payload + n, age_ms); n += 4;
  return enqueue(MsgType::PitchFrame, kFlagNotification, server_seq_++, payload,
                 n, false);
}

bool Session::send_dsp_status(uint32_t sample_rate, uint16_t block_size,
                              float dsp_avg_us, uint16_t dsp_peak_us,
                              uint32_t deadline_misses, uint32_t tx_underruns,
                              uint32_t rx_overruns, float backlog_ms,
                              uint32_t free_internal, uint32_t free_psram,
                              uint32_t uptime_s) {
  uint8_t payload[64];
  size_t n = 0;
  put_u32(payload + n, sample_rate); n += 4;
  put_u16(payload + n, block_size); n += 2;
  put_f32(payload + n, dsp_avg_us); n += 4;
  put_u16(payload + n, dsp_peak_us); n += 2;
  put_u32(payload + n, deadline_misses); n += 4;
  put_u32(payload + n, tx_underruns); n += 4;
  put_u32(payload + n, rx_overruns); n += 4;
  put_f32(payload + n, backlog_ms); n += 4;
  put_u32(payload + n, free_internal); n += 4;
  put_u32(payload + n, free_psram); n += 4;
  put_u32(payload + n, uptime_s); n += 4;
  return enqueue(MsgType::DspStatus, kFlagNotification, server_seq_++, payload,
                 n, false);
}

bool Session::notify_param_changed(uint16_t id, float value,
                                   ParamOrigin origin) {
  const ParamDescriptor *d = find_param(id);
  if (d == nullptr)
    return false;
  uint8_t payload[kMaxPayload];
  size_t n = 0;
  put_u16(payload + n, id); n += 2;
  put_u32(payload + n, state_ != nullptr ? state_->revision() : 0); n += 4;
  payload[n++] = static_cast<uint8_t>(origin);
  payload[n++] = static_cast<uint8_t>(d->type);
  const size_t vn = put_value(d->type, value, payload + n, kMaxPayload - n);
  n += vn;
  return enqueue(MsgType::ParamChanged, kFlagNotification, server_seq_++, payload,
                 n, true);
}

} // namespace voxlink
