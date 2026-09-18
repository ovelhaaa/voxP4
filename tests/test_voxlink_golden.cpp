// Golden vector regression: the encoder must reproduce the normative bytes
// shared with ovelhaaa/voxP4-control, and the CRC algorithm must match the
// documented parameters.
#include "data/voxlink_v1_vectors.h"
#include "voxlink_codec.h"
#include "voxlink_crc.h"
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
} // namespace

int main() {
  check(crc16_ccitt_false(reinterpret_cast<const uint8_t *>("123456789"), 9) ==
            voxlink_vectors::kCrcCheckValue123456789,
        "CRC check value matches golden");

  uint8_t buf[kMaxFrame];

  // HELLO.
  const uint8_t hello_payload[3] = {kVersion, 0x01, 0x00};
  size_t n = encode_frame(MsgType::Hello, kFlagNone, 0x2A, hello_payload,
                          sizeof(hello_payload), buf, sizeof(buf));
  check(bytes_equal(buf, n, voxlink_vectors::kHello,
                    sizeof(voxlink_vectors::kHello)),
        "encoded HELLO bytes match golden");

  // SET_PARAM harmony.interval = 7.
  uint8_t set_payload[7];
  put_u16(set_payload, 0x0101);
  set_payload[2] = static_cast<uint8_t>(ValueTag::Int32);
  put_i32(set_payload + 3, 7);
  n = encode_frame(MsgType::SetParam, kFlagNone, 1, set_payload,
                   sizeof(set_payload), buf, sizeof(buf));
  check(bytes_equal(buf, n, voxlink_vectors::kSetParam,
                    sizeof(voxlink_vectors::kSetParam)),
        "encoded SET_PARAM bytes match golden");

  // GET_PARAM reverb.wet.
  uint8_t get_payload[2];
  put_u16(get_payload, 0x0401);
  n = encode_frame(MsgType::GetParam, kFlagNone, 2, get_payload,
                   sizeof(get_payload), buf, sizeof(buf));
  check(bytes_equal(buf, n, voxlink_vectors::kGetParam,
                    sizeof(voxlink_vectors::kGetParam)),
        "encoded GET_PARAM bytes match golden");

  // ACK.
  uint8_t ack_payload[4] = {0x0D, 0x01, 0x00, 0x00};
  n = encode_frame(MsgType::Ack, kFlagNone, 1, ack_payload, sizeof(ack_payload),
                   buf, sizeof(buf));
  check(bytes_equal(buf, n, voxlink_vectors::kAck,
                    sizeof(voxlink_vectors::kAck)),
        "encoded ACK bytes match golden");

  // HEARTBEAT.
  n = encode_frame(MsgType::Heartbeat, kFlagNone, 3, nullptr, 0, buf,
                   sizeof(buf));
  check(bytes_equal(buf, n, voxlink_vectors::kHeartbeat,
                    sizeof(voxlink_vectors::kHeartbeat)),
        "encoded HEARTBEAT bytes match golden");

  if (g_failures == 0)
    std::puts("voxlink_golden_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
