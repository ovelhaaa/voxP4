// Property-style parser stress: randomized byte streams with occasional valid
// VoxLink frames. Must never crash, never loop unbounded, and must recover the
// injected frames despite arbitrary corruption.
#include "voxlink_codec.h"
#include "voxlink_parser.h"
#include <cstdint>
#include <cstdio>
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

uint8_t next_byte(uint32_t &state) {
  state = state * 1664525u + 1013904223u;
  // Bias some bytes toward SOF so the parser is constantly challenged.
  const uint32_t r = state >> 16;
  if ((r & 0x1F) == 0)
    return (r & 0x40) ? kSof0 : kSof1;
  return static_cast<uint8_t>(r);
}
} // namespace

int main() {
  constexpr size_t kTotalBytes = 2000000;
  const uint8_t payloads[][4] = {{0x10, 0x01, 0x00, 0x00},
                                 {0x01, 0x01, 0x02, 0x07},
                                 {0x01, 0x04, 0x00, 0x00}};
  const size_t payload_lens[3] = {3, 4, 2};

  Parser parser;
  uint32_t rand = 0x12345u;
  size_t injected = 0;
  size_t recovered = 0;
  size_t bytes = 0;

  size_t since_inject = 0;
  while (bytes < kTotalBytes) {
    // Inject a valid frame roughly every 4 KB.
    if (since_inject >= 4000) {
      since_inject = 0;
      const int k = static_cast<int>((rand % 3));
      uint8_t frame[kMaxFrame];
      const size_t n = encode_frame(
          static_cast<MsgType>(k == 0 ? 0x01 : (k == 1 ? 0x0D : 0x0B)), 0,
          static_cast<uint8_t>(injected & 0xFF), payloads[k], payload_lens[k],
          frame, sizeof(frame));
      parser.feed(frame, n);
      ++injected;
      bytes += n;
      Frame f;
      while (parser.pop(&f))
        ++recovered;
      continue;
    }
    const size_t chunk = 1 + (rand % 17);
    uint8_t scratch[17];
    for (size_t i = 0; i < chunk; ++i)
      scratch[i] = next_byte(rand);
    parser.feed(scratch, chunk);
    bytes += chunk;
    since_inject += chunk;
    Frame f;
    while (parser.pop(&f))
      ++recovered;
  }

  // Trailing valid frame must always be recovered even after 2 MB of noise.
  uint8_t last[kMaxFrame];
  const size_t last_n =
      encode_frame(MsgType::Heartbeat, 0, 0x55, nullptr, 0, last, sizeof(last));
  parser.feed(last, last_n);
  Frame f;
  bool saw_last = false;
  while (parser.pop(&f)) {
    ++recovered;
    if (f.type == MsgType::Heartbeat && f.seq == 0x55)
      saw_last = true;
  }

  check(parser.counters().bytes_fed >= kTotalBytes, "bytes fed counted");
  check(saw_last, "final valid frame recovered after noise");
  // Allow a few queue-slot drops, but the vast majority must survive.
  check(recovered + parser.counters().frames_dropped >= injected,
        "injected frames accounted for");
  check(recovered >= injected, "no injected frame lost to corruption");
  check(parser.counters().parser_resyncs > 0, "resyncs occurred");

  if (g_failures == 0)
    std::puts("voxlink_fuzz_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
