// End-to-end VoxLink session tests using an in-process client. Covers HELLO,
// capabilities, coherent snapshots, GET/SET, rejection paths, revision
// semantics and reconnect/timeout behaviour.
#include "voxlink_codec.h"
#include "voxlink_registry.h"
#include "voxlink_session.h"
#include "voxlink_state.h"
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

struct Client {
  Session *session = nullptr;
  Parser parser;
  uint8_t drain[kMaxFrame * 4] = {0};

  std::vector<Frame> request(MsgType type, uint8_t seq, const uint8_t *payload,
                             size_t len) {
    uint8_t frame[kMaxFrame];
    const size_t n =
        encode_frame(type, kFlagNone, seq, payload, len, frame, sizeof(frame));
    session->feed(frame, n);
    std::vector<Frame> out;
    size_t chunk = 0;
    while ((chunk = session->take_tx(drain, sizeof(drain))) > 0)
      parser.feed(drain, chunk,
                  [&out](const Frame &f) { out.push_back(f); });
    return out;
  }
};

const Frame *find(const std::vector<Frame> &frames, MsgType type) {
  for (const Frame &f : frames)
    if (f.type == type)
      return &f;
  return nullptr;
}

size_t count(const std::vector<Frame> &frames, MsgType type) {
  size_t n = 0;
  for (const Frame &f : frames)
    if (f.type == type)
      ++n;
  return n;
}

bool submit_always(uint16_t, float, void *) { return true; }
bool submit_reject(uint16_t, float, void *) { return false; }
} // namespace

int main() {
  ProductState state;
  state.init();

  SessionConfig cfg;
  cfg.capability_flags = kCapHarmony | kCapCompressor | kCapDelay | kCapReverb |
                         kCapLimiter | kCapGate | kCapPitchTelemetry;
  cfg.sample_rate = 44100;
  cfg.block_size = 64;
  cfg.harmony_voice_count = 1;
  cfg.submit = submit_always;
  cfg.heartbeat_timeout_ms = 1000;
  cfg.heartbeat_interval_ms = 500;

  Session session;
  session.init(cfg);
  session.set_product_state(&state);
  Client client;
  client.session = &session;

  // HELLO -> HELLO_ACK.
  {
    const uint8_t hello[3] = {kVersion, 0x01, 0x00};
    auto frames = client.request(MsgType::Hello, 1, hello, sizeof(hello));
    const Frame *ack = find(frames, MsgType::HelloAck);
    check(ack != nullptr, "HELLO yields HELLO_ACK");
    check(session.connection_state() == ConnectionState::Active,
          "connection becomes ACTIVE");
    if (ack != nullptr) {
      check(ack->seq == 1, "HELLO_ACK echoes sequence");
      check(get_u32(ack->payload + 5) == 44100, "HELLO_ACK sample rate");
      check(get_u16(ack->payload + 9) == 64, "HELLO_ACK block size");
    }
  }

  // CAPS_REQUEST -> BEGIN + N PARAM + END.
  {
    auto frames = client.request(MsgType::CapsRequest, 2, nullptr, 0);
    const Frame *begin = find(frames, MsgType::CapsBegin);
    const Frame *end = find(frames, MsgType::CapsEnd);
    check(begin != nullptr && end != nullptr, "CAPS begin/end present");
    check(count(frames, MsgType::CapsParam) == registry_count(),
          "one CAPS_PARAM per registry entry");
    if (begin != nullptr)
      check(get_u16(begin->payload + 5) == registry_count(),
            "CAPS_BEGIN count matches");
  }

  // GET_STATE -> coherent snapshot at revision 0.
  {
    auto frames = client.request(MsgType::GetState, 3, nullptr, 0);
    const Frame *begin = find(frames, MsgType::StateBegin);
    const Frame *end = find(frames, MsgType::StateEnd);
    check(begin != nullptr && end != nullptr, "STATE begin/end present");
    check(count(frames, MsgType::StateParam) == registry_count(),
          "all parameters in snapshot");
    if (begin != nullptr && end != nullptr)
      check(get_u32(begin->payload) == get_u32(end->payload),
            "snapshot revision is coherent");
  }

  // SET_PARAM harmony.interval = +7.
  {
    uint8_t payload[7];
    put_u16(payload, 0x0101);
    payload[2] = static_cast<uint8_t>(ValueTag::Int32);
    put_i32(payload + 3, 7);
    auto frames = client.request(MsgType::SetParam, 10, payload, sizeof(payload));
    check(find(frames, MsgType::Ack) != nullptr, "SET_PARAM accepted (ACK)");
    const Frame *changed = find(frames, MsgType::ParamChanged);
    check(changed != nullptr, "SET_PARAM emits PARAM_CHANGED");
    if (changed != nullptr) {
      check(get_u16(changed->payload) == 0x0101, "PARAM_CHANGED id");
      check(get_u32(changed->payload + 2) == 1, "revision increments to 1");
      check(static_cast<int>(get_i32(changed->payload + 8)) == 7,
            "PARAM_CHANGED value");
    }
    check(state.revision() == 1, "state revision == 1");
  }

  // GET_PARAM returns the changed value.
  {
    uint8_t payload[2];
    put_u16(payload, 0x0101);
    auto frames = client.request(MsgType::GetParam, 11, payload, sizeof(payload));
    const Frame *pv = find(frames, MsgType::ParamValue);
    check(pv != nullptr, "GET_PARAM yields PARAM_VALUE");
    if (pv != nullptr)
      check(static_cast<int>(get_i32(pv->payload + 7)) == 7,
            "PARAM_VALUE reflects accepted value");
  }

  // Unknown parameter rejected without state change.
  {
    uint8_t payload[7] = {0};
    put_u16(payload, 0x9999);
    payload[2] = static_cast<uint8_t>(ValueTag::Float32);
    put_f32(payload + 3, 0.5f);
    auto frames = client.request(MsgType::SetParam, 12, payload, sizeof(payload));
    const Frame *nack = find(frames, MsgType::Nack);
    check(nack != nullptr && get_u16(nack->payload + 2) ==
                                 static_cast<uint16_t>(Code::UnknownParam),
          "unknown parameter NACKed");
    check(state.revision() == 1, "revision unchanged after rejection");
  }

  // Invalid enum rejected.
  {
    uint8_t payload[5];
    put_u16(payload, 0x0503);
    payload[2] = static_cast<uint8_t>(ValueTag::Enum16);
    put_u16(payload + 3, 9);
    auto frames = client.request(MsgType::SetParam, 13, payload, sizeof(payload));
    const Frame *nack = find(frames, MsgType::Nack);
    check(nack != nullptr && get_u16(nack->payload + 2) ==
                                 static_cast<uint16_t>(Code::InvalidEnum),
          "invalid enum NACKed");
  }

  // Out-of-range float is clamped and accepted.
  {
    uint8_t payload[7];
    put_u16(payload, 0x0401);
    payload[2] = static_cast<uint8_t>(ValueTag::Float32);
    put_f32(payload + 3, 2.0f);
    auto frames = client.request(MsgType::SetParam, 14, payload, sizeof(payload));
    const Frame *changed = find(frames, MsgType::ParamChanged);
    check(changed != nullptr, "clamped value still accepted");
    if (changed != nullptr)
      check(get_f32(changed->payload + 8) == 1.0f, "value clamped to 1.0");
    check(state.revision() == 2, "revision increments on changed value");
  }

  // Setting the same value again does not bump the revision.
  {
    uint8_t payload[7];
    put_u16(payload, 0x0401);
    payload[2] = static_cast<uint8_t>(ValueTag::Float32);
    put_f32(payload + 3, 1.0f);
    auto frames = client.request(MsgType::SetParam, 15, payload, sizeof(payload));
    const Frame *ack = find(frames, MsgType::Ack);
    const Frame *changed = find(frames, MsgType::ParamChanged);
    check(ack != nullptr, "idempotent set ACKed");
    check(ack != nullptr && ack->seq == 15 && ack->payload[0] ==
                                 static_cast<uint8_t>(MsgType::SetParam) &&
              ack->payload[1] == 15 && get_u16(ack->payload + 2) ==
                                           static_cast<uint16_t>(Code::Ok),
          "ACK preserves request correlation");
    check(changed != nullptr && changed->seq == 15 &&
              get_u16(changed->payload) == 0x0401 &&
              get_u32(changed->payload + 2) == 2,
          "PARAM_CHANGED preserves correlation and revision");
    check(state.revision() == 2, "revision stable when value unchanged");
  }

  // ACCEPTED != SAMPLE-EXACT APPLIED: the value is confirmed as soon as it is
  // accepted into the realtime command stream, with no DSP consumption needed.
  {
    SessionConfig accepting = cfg;
    accepting.submit = submit_always;
    Session s;
    s.init(accepting);
    ProductState st;
    st.init();
    s.set_product_state(&st);
    Client c3;
    c3.session = &s;
    {
      const uint8_t hello[3] = {kVersion, 0x01, 0x00};
      c3.request(MsgType::Hello, 1, hello, sizeof(hello));
    }
    uint8_t payload[7];
    put_u16(payload, 0x0101);
    payload[2] = static_cast<uint8_t>(ValueTag::Int32);
    put_i32(payload + 3, 4);
    auto frames = c3.request(MsgType::SetParam, 21, payload, sizeof(payload));
    const Frame *changed = find(frames, MsgType::ParamChanged);
    check(changed != nullptr, "confirmation arrives without DSP consumption");
    if (changed != nullptr)
      check(get_i32(changed->payload + 8) == 4,
            "PARAM_CHANGED carries accepted value");
    check(st.revision() == 1, "acceptance bumps revision");
  }

  // Reserved message -> NOT_SUPPORTED.
  {
    auto frames = client.request(MsgType::PresetList, 16, nullptr, 0);
    const Frame *nack = find(frames, MsgType::Nack);
    check(nack != nullptr && get_u16(nack->payload + 2) ==
                                 static_cast<uint16_t>(Code::NotSupported),
          "reserved message returns NOT_SUPPORTED");
  }

  // Queue-full path: submit rejects, state must stay coherent.
  {
    SessionConfig rejecting = cfg;
    rejecting.submit = submit_reject;
    Session busy;
    busy.init(rejecting);
    ProductState busy_state;
    busy_state.init();
    busy.set_product_state(&busy_state);
    Client c2;
    c2.session = &busy;
    {
      const uint8_t hello[3] = {kVersion, 0x01, 0x00};
      c2.request(MsgType::Hello, 1, hello, sizeof(hello));
    }
    uint8_t payload[7];
    put_u16(payload, 0x0101);
    payload[2] = static_cast<uint8_t>(ValueTag::Int32);
    put_i32(payload + 3, 5);
    auto frames = c2.request(MsgType::SetParam, 20, payload, sizeof(payload));
    const Frame *nack = find(frames, MsgType::Nack);
    check(nack != nullptr && get_u16(nack->payload + 2) ==
                                 static_cast<uint16_t>(Code::QueueFull),
          "queue-full NACKed");
    check(busy_state.revision() == 0, "queue-full leaves revision unchanged");
    float interval = -1.0f;
    busy_state.get_value(0x0101, &interval);
    check(interval == 0.0f, "queue-full leaves value unchanged");
    check(busy.counters().control_queue_full == 1, "queue-full counter");
  }

  // Heartbeat timeout returns to DISCONNECTED without touching state.
  {
    uint32_t revision_before = state.revision();
    session.tick(10);
    session.tick(2000);
    check(session.connection_state() == ConnectionState::Disconnected,
          "heartbeat timeout disconnects");
    check(state.revision() == revision_before, "timeout does not alter state");
    // A fresh HELLO reconnects.
    const uint8_t hello[3] = {kVersion, 0x01, 0x00};
    client.request(MsgType::Hello, 30, hello, sizeof(hello));
    check(session.connection_state() == ConnectionState::Active,
          "reconnect after timeout");
  }

  if (g_failures == 0)
    std::puts("voxlink_state_sync_tests: PASS");
  return g_failures == 0 ? 0 : 1;
}
