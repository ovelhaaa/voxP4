// Protocol conformance: documentation, encoder, decoder and golden vectors must
// form a single consistent ABI. Every implemented message type is encoded from
// its normative payload and compared byte-for-byte with the shared vectors,
// then decoded and field-checked.
#include "data/voxlink_v1_vectors.h"
#include "voxlink_codec.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace voxlink;

namespace {
int g_failures = 0;
void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
  }
}

bool bytes_equal(const uint8_t *a, size_t n, const uint8_t *b, size_t m) {
  return n == m && std::memcmp(a, b, n) == 0;
}

void verify(const char *name, MsgType type, uint8_t flags, uint8_t seq,
            const uint8_t *payload, size_t len, const uint8_t *golden,
            size_t golden_len) {
  uint8_t buf[kMaxFrame];
  const size_t n =
      encode_frame(type, flags, seq, payload, len, buf, sizeof(buf));
  if (!(n == golden_len && bytes_equal(buf, n, golden, golden_len))) {
    std::fprintf(stderr, "FAIL: %s encode (n=%zu golden=%zu)\n", name, n,
                 golden_len);
    ++g_failures;
  }
  Frame f;
  size_t consumed = 0;
  const DecodeStatus status = decode_frame(golden, golden_len, &f, &consumed);
  if (status != DecodeStatus::Ok || consumed != golden_len || f.type != type ||
      f.seq != seq || f.flags != flags || f.payload_len != len ||
      !bytes_equal(f.payload, f.payload_len, payload, len)) {
    std::fprintf(stderr, "FAIL: %s decode status=%s\n", name,
                 decode_status_name(status));
    ++g_failures;
  }
}
} // namespace

int main() {
  uint8_t p[kMaxPayload];

  // HELLO
  const uint8_t hello[3] = {0x10, 0x01, 0x00};
  verify("HELLO", MsgType::Hello, 0, 0x2A, hello, sizeof(hello),
         voxlink_vectors::kHello, sizeof(voxlink_vectors::kHello));

  // HELLO_ACK
  {
    size_t n = 0;
    p[n++] = 0x10;
    put_u32(p + n, 0x0000007Fu); n += 4;
    put_u32(p + n, 44100u); n += 4;
    put_u16(p + n, 64); n += 2;
    p[n++] = 1;
    put_u32(p + n, 7u); n += 4;
    p[n++] = 1;
    p[n++] = 1;
    p[n++] = 0;
    p[n++] = 1;
    p[n++] = 0;
    const char *sha = "abc123";
    p[n++] = 6;
    std::memcpy(p + n, sha, 6); n += 6;
    verify("HELLO_ACK", MsgType::HelloAck, 0, 5, p, n,
           voxlink_vectors::kHelloAck, sizeof(voxlink_vectors::kHelloAck));
  }

  // CAPS_REQUEST (empty)
  {
    uint8_t buf[kMaxFrame];
    const size_t n = encode_frame(MsgType::CapsRequest, 0, 1, nullptr, 0, buf,
                                  sizeof(buf));
    Frame f;
    size_t consumed = 0;
    check(n == kHeaderSize + kCrcSize &&
              decode_frame(buf, n, &f, &consumed) == DecodeStatus::Ok &&
              f.type == MsgType::CapsRequest && f.payload_len == 0,
          "CAPS_REQUEST round trip");
  }

  // CAPS_BEGIN
  {
    size_t n = 0;
    p[n++] = 0x10;
    put_u32(p + n, 0x7Fu); n += 4;
    put_u16(p + n, 45); n += 2;
    verify("CAPS_BEGIN", MsgType::CapsBegin, 0, 2, p, n,
           voxlink_vectors::kCapsBegin, sizeof(voxlink_vectors::kCapsBegin));
  }

  // CAPS_PARAM
  {
    size_t n = 0;
    put_u16(p + n, 0x0101); n += 2;
    p[n++] = static_cast<uint8_t>(ValueTag::Int32);
    put_u32(p + n, 0x43u); n += 4;
    put_f32(p + n, -12.0f); n += 4;
    put_f32(p + n, 12.0f); n += 4;
    put_f32(p + n, 0.0f); n += 4;
    put_f32(p + n, 1.0f); n += 4;
    put_f32(p + n, 0.0f); n += 4;
    p[n++] = 0;
    verify("CAPS_PARAM", MsgType::CapsParam, 0, 2, p, n,
           voxlink_vectors::kCapsParam, sizeof(voxlink_vectors::kCapsParam));
  }

  // CAPS_END
  {
    put_u16(p, 45);
    verify("CAPS_END", MsgType::CapsEnd, 0, 2, p, 2, voxlink_vectors::kCapsEnd,
           sizeof(voxlink_vectors::kCapsEnd));
  }

  // GET_STATE (empty)
  {
    uint8_t buf[kMaxFrame];
    const size_t n = encode_frame(MsgType::GetState, 0, 3, nullptr, 0, buf,
                                  sizeof(buf));
    Frame f;
    size_t consumed = 0;
    check(n == kHeaderSize + kCrcSize &&
              decode_frame(buf, n, &f, &consumed) == DecodeStatus::Ok &&
              f.type == MsgType::GetState && f.payload_len == 0,
          "GET_STATE round trip");
  }

  // STATE_BEGIN
  {
    size_t n = 0;
    put_u32(p + n, 7u); n += 4;
    put_u16(p + n, 45); n += 2;
    verify("STATE_BEGIN", MsgType::StateBegin, 0, 3, p, n,
           voxlink_vectors::kStateBegin, sizeof(voxlink_vectors::kStateBegin));
  }

  // STATE_PARAM
  {
    size_t n = 0;
    put_u16(p + n, 0x0101); n += 2;
    p[n++] = static_cast<uint8_t>(ValueTag::Int32);
    put_i32(p + n, 0); n += 4;
    verify("STATE_PARAM", MsgType::StateParam, 0, 3, p, n,
           voxlink_vectors::kStateParam, sizeof(voxlink_vectors::kStateParam));
  }

  // STATE_END
  {
    put_u32(p, 7u);
    verify("STATE_END", MsgType::StateEnd, 0, 3, p, 4,
           voxlink_vectors::kStateEnd, sizeof(voxlink_vectors::kStateEnd));
  }

  // GET_PARAM
  {
    put_u16(p, 0x0401);
    verify("GET_PARAM", MsgType::GetParam, 0, 2, p, 2,
           voxlink_vectors::kGetParam, sizeof(voxlink_vectors::kGetParam));
  }

  // PARAM_VALUE
  {
    size_t n = 0;
    put_u16(p + n, 0x0401); n += 2;
    put_u32(p + n, 5u); n += 4;
    p[n++] = static_cast<uint8_t>(ValueTag::Float32);
    put_f32(p + n, 0.25f); n += 4;
    verify("PARAM_VALUE", MsgType::ParamValue, 0, 4, p, n,
           voxlink_vectors::kParamValue, sizeof(voxlink_vectors::kParamValue));
  }

  // SET_PARAM
  {
    size_t n = 0;
    put_u16(p + n, 0x0101); n += 2;
    p[n++] = static_cast<uint8_t>(ValueTag::Int32);
    put_i32(p + n, 7); n += 4;
    verify("SET_PARAM", MsgType::SetParam, 0, 1, p, n,
           voxlink_vectors::kSetParam, sizeof(voxlink_vectors::kSetParam));
  }

  // PARAM_CHANGED
  {
    size_t n = 0;
    put_u16(p + n, 0x0401); n += 2;
    put_u32(p + n, 6u); n += 4;
    p[n++] = static_cast<uint8_t>(ParamOrigin::Controller);
    p[n++] = static_cast<uint8_t>(ValueTag::Float32);
    put_f32(p + n, 0.30f); n += 4;
    verify("PARAM_CHANGED", MsgType::ParamChanged, 0, 7, p, n,
           voxlink_vectors::kParamChanged,
           sizeof(voxlink_vectors::kParamChanged));
  }

  // ACK
  {
    uint8_t ack[4] = {0x0D, 0x01, 0x00, 0x00};
    verify("ACK", MsgType::Ack, 0, 1, ack, sizeof(ack), voxlink_vectors::kAck,
           sizeof(voxlink_vectors::kAck));
  }

  // NACK
  {
    p[0] = 0x0D; p[1] = 0x01; put_u16(p + 2, 9);
    verify("NACK", MsgType::Nack, 0, 1, p, 4,
           voxlink_vectors::kNackQueueFull,
           sizeof(voxlink_vectors::kNackQueueFull));
  }

  // ERROR
  {
    p[0] = 0xFF; p[1] = 0x00; put_u16(p + 2, 1);
    verify("ERROR", MsgType::Error, 0, 0, p, 4,
           voxlink_vectors::kErrorUnknownMessage,
           sizeof(voxlink_vectors::kErrorUnknownMessage));
  }

  // HEARTBEAT
  verify("HEARTBEAT", MsgType::Heartbeat, 0, 3, nullptr, 0,
         voxlink_vectors::kHeartbeat, sizeof(voxlink_vectors::kHeartbeat));

  // METER_FRAME
  {
    size_t n = 0;
    const float values[7] = {0.5f, 0.25f, 0.4f, 0.2f, 0.9f, 0.45f, 3.0f};
    for (float v : values) { put_f32(p + n, v); n += 4; }
    verify("METER_FRAME", MsgType::MeterFrame, kFlagNotification, 10, p, n,
           voxlink_vectors::kMeterFrame, sizeof(voxlink_vectors::kMeterFrame));
  }

  // PITCH_FRAME
  {
    size_t n = 0;
    put_f32(p + n, 220.0f); n += 4;
    put_f32(p + n, 0.93f); n += 4;
    p[n++] = 1;
    put_f32(p + n, 57.0f); n += 4;
    put_f32(p + n, 12.5f); n += 4;
    verify("PITCH_FRAME", MsgType::PitchFrame, kFlagNotification, 11, p, n,
           voxlink_vectors::kPitchFrame, sizeof(voxlink_vectors::kPitchFrame));
  }

  // DSP_STATUS (exact sequential layout)
  {
    size_t n = 0;
    put_u32(p + n, 44100u); n += 4;
    put_u16(p + n, 64); n += 2;
    put_f32(p + n, 123.5f); n += 4;
    put_u16(p + n, 200); n += 2;
    put_u32(p + n, 3u); n += 4;
    put_u32(p + n, 4u); n += 4;
    put_u32(p + n, 5u); n += 4;
    put_f32(p + n, 12.25f); n += 4;
    put_u32(p + n, 100000u); n += 4;
    put_u32(p + n, 2000000u); n += 4;
    put_u32(p + n, 3600u); n += 4;
    check(n == 40, "DSP_STATUS payload is 40 bytes");
    verify("DSP_STATUS", MsgType::DspStatus, kFlagNotification, 9, p, n,
           voxlink_vectors::kDspStatus, sizeof(voxlink_vectors::kDspStatus));

    // Explicit field-offset assertions lock the normative layout.
    check(get_u32(p + 0) == 44100u, "DSP_STATUS sample_rate offset 0");
    check(get_u16(p + 4) == 64, "DSP_STATUS block_size offset 4");
    check(get_f32(p + 6) == 123.5f, "DSP_STATUS dsp_avg_us offset 6");
    check(get_u16(p + 10) == 200, "DSP_STATUS dsp_peak_us offset 10");
    check(get_u32(p + 12) == 3u, "DSP_STATUS deadline_misses offset 12");
    check(get_u32(p + 16) == 4u, "DSP_STATUS tx_underruns offset 16");
    check(get_u32(p + 20) == 5u, "DSP_STATUS rx_overruns offset 20");
    check(get_f32(p + 24) == 12.25f, "DSP_STATUS backlog_ms offset 24");
    check(get_u32(p + 28) == 100000u, "DSP_STATUS free_internal offset 28");
    check(get_u32(p + 32) == 2000000u, "DSP_STATUS free_psram offset 32");
    check(get_u32(p + 36) == 3600u, "DSP_STATUS uptime_s offset 36");
  }

  if (g_failures == 0)
    std::puts("voxlink_protocol_conformance_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
