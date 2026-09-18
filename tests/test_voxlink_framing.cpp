// VoxLink codec and streaming parser robustness (non-fuzz cases).
#include "voxlink_codec.h"
#include "voxlink_crc.h"
#include "voxlink_parser.h"
#include "data/voxlink_v1_vectors.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

std::vector<uint8_t> collect(Parser &parser) {
  std::vector<uint8_t> bytes;
  Frame frame;
  while (parser.pop(&frame))
    bytes.push_back(static_cast<uint8_t>(frame.type));
  return bytes;
}
} // namespace

int main() {
  check(crc16_ccitt_false(reinterpret_cast<const uint8_t *>("123456789"), 9) ==
            0x29B1,
        "CRC-16/CCITT-FALSE check value");

  // Round trip: empty and non-empty payloads.
  uint8_t buf[kMaxFrame];
  size_t n = encode_frame(MsgType::Heartbeat, kFlagNotification, 9, nullptr, 0,
                          buf, sizeof(buf));
  check(n == kHeaderSize + kCrcSize, "empty frame size");
  Frame decoded;
  size_t consumed = 0;
  check(decode_frame(buf, n, &decoded, &consumed) == DecodeStatus::Ok &&
            consumed == n && decoded.type == MsgType::Heartbeat &&
            decoded.seq == 9 && decoded.flags == kFlagNotification,
        "empty frame round trip");

  const uint8_t payload[] = {1, 2, 3, 4, 5};
  n = encode_frame(MsgType::SetParam, 0, 7, payload, sizeof(payload), buf,
                   sizeof(buf));
  check(decode_frame(buf, n, &decoded, &consumed) == DecodeStatus::Ok &&
            decoded.payload_len == sizeof(payload) &&
            std::memcmp(decoded.payload, payload, sizeof(payload)) == 0,
        "payload round trip");

  // Golden vectors.
  check(decode_frame(voxlink_vectors::kHello, sizeof(voxlink_vectors::kHello),
                     &decoded, &consumed) == DecodeStatus::Ok &&
            decoded.type == MsgType::Hello && decoded.seq == 0x2A,
        "golden HELLO");
  check(decode_frame(voxlink_vectors::kSetParam,
                     sizeof(voxlink_vectors::kSetParam), &decoded,
                     &consumed) == DecodeStatus::Ok &&
            decoded.type == MsgType::SetParam,
        "golden SET_PARAM");
  check(decode_frame(voxlink_vectors::kGetParam,
                     sizeof(voxlink_vectors::kGetParam), &decoded,
                     &consumed) == DecodeStatus::Ok &&
            decoded.type == MsgType::GetParam,
        "golden GET_PARAM");
  check(decode_frame(voxlink_vectors::kAck, sizeof(voxlink_vectors::kAck),
                     &decoded, &consumed) == DecodeStatus::Ok &&
            decoded.type == MsgType::Ack,
        "golden ACK");
  check(decode_frame(voxlink_vectors::kHeartbeat,
                     sizeof(voxlink_vectors::kHeartbeat), &decoded,
                     &consumed) == DecodeStatus::Ok,
        "golden HEARTBEAT");
  check(decode_frame(voxlink_vectors::kHelloBadCrc,
                     sizeof(voxlink_vectors::kHelloBadCrc), &decoded,
                     &consumed) == DecodeStatus::BadCrc,
        "golden bad CRC");

  // Unsupported version: rewrite version byte and recompute CRC.
  uint8_t unsupported[kMaxFrame];
  std::memcpy(unsupported, voxlink_vectors::kHello,
              sizeof(voxlink_vectors::kHello));
  unsupported[2] = 0x20;
  {
    const size_t total = sizeof(voxlink_vectors::kHello);
    const uint16_t crc = crc16_ccitt_false(unsupported + 2, total - 4);
    put_u16(unsupported + total - 2, crc);
  }
  check(decode_frame(unsupported, sizeof(voxlink_vectors::kHello), &decoded,
                     &consumed) == DecodeStatus::UnsupportedVersion,
        "unsupported version rejected");

  // Oversized advertised length.
  uint8_t oversize[kHeaderSize + kCrcSize] = {kSof0, kSof1, kVersion, 0x0D,
                                              0,     0,      0xFF, 0xFF,
                                              0,     0};
  check(decode_frame(oversize, sizeof(oversize), &decoded, &consumed) ==
            DecodeStatus::LengthTooLarge,
        "oversize length rejected");
  check(decode_frame(oversize, 5, &decoded, &consumed) ==
            DecodeStatus::ShortBuffer,
        "short buffer rejected");
  uint8_t bad_sof[2] = {0x00, 0x00};
  check(decode_frame(bad_sof, sizeof(bad_sof), &decoded, &consumed) ==
            DecodeStatus::BadSof,
        "bad SOF rejected");

  // ---------------- Parser ----------------
  // One byte at a time.
  {
    Parser parser;
    parser.feed(voxlink_vectors::kHello, 1);
    check(!parser.has_frame(), "partial frame not emitted");
    for (size_t i = 1; i < sizeof(voxlink_vectors::kHello); ++i)
      parser.feed(voxlink_vectors::kHello + i, 1);
    Frame f;
    check(parser.pop(&f) && f.type == MsgType::Hello,
          "one-byte-at-a-time decode");
  }

  // Two frames in one buffer, plus garbage before SOF.
  {
    Parser parser;
    std::vector<uint8_t> stream = {0x11, 0x22, 0xFF, 0xA5};
    stream.insert(stream.end(), std::begin(voxlink_vectors::kHello),
                  std::end(voxlink_vectors::kHello));
    stream.insert(stream.end(), std::begin(voxlink_vectors::kGetParam),
                  std::end(voxlink_vectors::kGetParam));
    parser.feed(stream.data(), stream.size());
    Frame f;
    int count = 0;
    while (parser.pop(&f))
      ++count;
    check(count == 2, "two frames with garbage prefix");
  }

  // 100 back-to-back frames.
  {
    Parser parser;
    std::vector<uint8_t> stream;
    for (int i = 0; i < 100; ++i)
      stream.insert(stream.end(), std::begin(voxlink_vectors::kHeartbeat),
                    std::end(voxlink_vectors::kHeartbeat));
    int count = 0;
    parser.feed(stream.data(), stream.size(),
                [&count](const Frame &) { ++count; });
    check(count == 100, "100 back-to-back frames");
    check(parser.counters().frames_valid == 100, "frames_valid == 100");
  }

  // SOF bytes inside payload are not mistaken for a new frame.
  {
    Parser parser;
    const uint8_t tricky[] = {0xA5, 0x5A, 0xA5, 0x5A, 0x00};
    uint8_t encoded[kMaxFrame];
    const size_t len = encode_frame(MsgType::DspStatus, 0, 4, tricky,
                                    sizeof(tricky), encoded, sizeof(encoded));
    parser.feed(encoded, len);
    Frame f;
    check(parser.pop(&f) && f.payload_len == sizeof(tricky) &&
              std::memcmp(f.payload, tricky, sizeof(tricky)) == 0,
          "SOF inside payload survives");
  }

  // Corruption recovery: bad CRC frame then a valid frame.
  {
    Parser parser;
    std::vector<uint8_t> stream(std::begin(voxlink_vectors::kHelloBadCrc),
                                std::end(voxlink_vectors::kHelloBadCrc));
    stream.insert(stream.end(), std::begin(voxlink_vectors::kHello),
                  std::end(voxlink_vectors::kHello));
    parser.feed(stream.data(), stream.size());
    Frame f;
    check(parser.pop(&f) && f.type == MsgType::Hello,
          "recovers after CRC error");
    check(parser.counters().crc_errors == 1, "crc_errors counted");
    check(parser.counters().parser_resyncs >= 1, "resync counted");
  }

  // Random noise before a valid frame.
  {
    Parser parser;
    std::vector<uint8_t> stream;
    uint32_t state = 0xC0FFEEu;
    for (int i = 0; i < 4096; ++i) {
      state = state * 1664525u + 1013904223u;
      stream.push_back(static_cast<uint8_t>(state >> 16));
    }
    stream.insert(stream.end(), std::begin(voxlink_vectors::kHeartbeat),
                  std::end(voxlink_vectors::kHeartbeat));
    parser.feed(stream.data(), stream.size());
    Frame f;
    int valid = 0;
    while (parser.pop(&f))
      ++valid;
    check(valid >= 1, "valid frame recovered from noise");
  }

  // Truncated final frame yields nothing.
  {
    Parser parser;
    parser.feed(voxlink_vectors::kGetParam, sizeof(voxlink_vectors::kGetParam) - 3);
    check(!parser.has_frame() && parser.counters().crc_errors == 0,
          "truncated final frame pending");
  }

  if (g_failures == 0)
    std::puts("voxlink_framing_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
