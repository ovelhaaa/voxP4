#pragma once
// Bounded, allocation-free streaming VoxLink v1 frame parser. It accepts
// arbitrary byte chunks, recovers automatically from corruption, and never
// trusts wire lengths beyond the fixed protocol maximum.
//
// Two consumption styles are supported:
//   * feed(data, len, emit): calls emit(const Frame&) for every valid frame.
//     Use this when many frames can arrive in one chunk (e.g. a controller
//     receiving a CAPS_RESPONSE burst).
//   * feed(data, len) + pop(&frame): queues up to kQueueCapacity frames. The
//     on-target server drains immediately, so its small queue never overflows.
#include "voxlink_codec.h"
#include "voxlink_crc.h"
#include <cstddef>
#include <cstdint>

namespace voxlink {

struct ParserCounters {
  uint64_t bytes_fed = 0;
  uint64_t frames_valid = 0;
  uint64_t frames_dropped = 0; // parsed but no queue slot available
  uint64_t crc_errors = 0;
  uint64_t framing_errors = 0;
  uint64_t length_errors = 0;
  uint64_t unsupported_version = 0;
  uint64_t unknown_type = 0;
  uint64_t parser_resyncs = 0;
};

class Parser {
public:
  static constexpr size_t kQueueCapacity = 8;

  void reset() {
    state_ = State::Sof0;
    header_pos_ = 0;
    payload_pos_ = 0;
    crc_pos_ = 0;
    payload_len_ = 0;
    working_ = Frame{};
    head_ = 0;
    tail_ = 0;
    counters_ = ParserCounters{};
  }

  // Callback style: emits every valid frame, no queue involved.
  template <typename Emit>
  void feed(const uint8_t *data, size_t len, Emit &&emit) {
    if (data == nullptr)
      return;
    counters_.bytes_fed += len;
    for (size_t i = 0; i < len; ++i)
      handle_byte(data[i], emit);
  }

  // Queue style: valid frames are queued for pop().
  void feed(const uint8_t *data, size_t len) {
    feed(data, len, [this](const Frame &frame) { enqueue_parsed(frame); });
  }

  bool has_frame() const { return head_ != tail_; }

  bool pop(Frame *out) {
    if (out == nullptr || head_ == tail_)
      return false;
    *out = queue_[tail_];
    tail_ = static_cast<uint8_t>((tail_ + 1) % kQueueCapacity);
    return true;
  }

  const ParserCounters &counters() const { return counters_; }

private:
  enum class State : uint8_t { Sof0, Sof1, Header, Payload, Crc };

  void enqueue_parsed(const Frame &frame) {
    const uint8_t next_head = static_cast<uint8_t>((head_ + 1) % kQueueCapacity);
    if (next_head == tail_) {
      ++counters_.frames_dropped;
      return;
    }
    queue_[head_] = frame;
    head_ = next_head;
  }

  void begin_resync() {
    ++counters_.parser_resyncs;
    state_ = State::Sof0;
    header_pos_ = 0;
    payload_pos_ = 0;
    crc_pos_ = 0;
    payload_len_ = 0;
  }

  template <typename Emit> void complete_frame(Emit &emit) {
    const uint16_t expected =
        static_cast<uint16_t>(static_cast<uint16_t>(crc_bytes_[0]) |
                              (static_cast<uint16_t>(crc_bytes_[1]) << 8));
    uint16_t actual = crc16_ccitt_false(header_, sizeof(header_));
    actual = crc16_ccitt_false_update(actual, working_.payload,
                                      working_.payload_len);
    if (expected != actual) {
      ++counters_.crc_errors;
      begin_resync();
      return;
    }
    ++counters_.frames_valid;
    if (!msg_type_known(static_cast<uint8_t>(working_.type)))
      ++counters_.unknown_type;
    emit(working_);
    state_ = State::Sof0;
    header_pos_ = 0;
    payload_pos_ = 0;
    crc_pos_ = 0;
    payload_len_ = 0;
  }

  template <typename Emit> void handle_byte(uint8_t byte, Emit &emit) {
    switch (state_) {
    case State::Sof0:
      if (byte == kSof0)
        state_ = State::Sof1;
      break;
    case State::Sof1:
      if (byte == kSof1)
        state_ = State::Header;
      else if (byte == kSof0)
        state_ = State::Sof1; // tolerate A5 A5 5A
      else
        state_ = State::Sof0;
      break;
    case State::Header:
      header_[header_pos_++] = byte;
      if (header_pos_ < sizeof(header_))
        break;
      working_.version = header_[0];
      working_.type = static_cast<MsgType>(header_[1]);
      working_.flags = header_[2];
      working_.seq = header_[3];
      payload_len_ = static_cast<uint16_t>(
          static_cast<uint16_t>(header_[4]) |
          (static_cast<uint16_t>(header_[5]) << 8));
      working_.payload_len = payload_len_;
      if ((working_.version >> 4) != kVersionMajor) {
        ++counters_.unsupported_version;
        begin_resync();
        break;
      }
      if (payload_len_ > kMaxPayload) {
        ++counters_.length_errors;
        begin_resync();
        break;
      }
      payload_pos_ = 0;
      if (payload_len_ == 0) {
        state_ = State::Crc;
        crc_pos_ = 0;
      } else {
        state_ = State::Payload;
      }
      break;
    case State::Payload:
      working_.payload[payload_pos_++] = byte;
      if (payload_pos_ >= payload_len_) {
        state_ = State::Crc;
        crc_pos_ = 0;
      }
      break;
    case State::Crc:
      crc_bytes_[crc_pos_++] = byte;
      if (crc_pos_ >= sizeof(crc_bytes_))
        complete_frame(emit);
      break;
    }
  }

  State state_ = State::Sof0;
  uint8_t header_[6] = {0};
  size_t header_pos_ = 0;
  size_t payload_pos_ = 0;
  uint8_t crc_bytes_[2] = {0};
  size_t crc_pos_ = 0;
  uint16_t payload_len_ = 0;
  Frame working_{};
  Frame queue_[kQueueCapacity];
  uint8_t head_ = 0;
  uint8_t tail_ = 0;
  ParserCounters counters_{};
};

} // namespace voxlink
