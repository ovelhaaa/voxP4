#include "voxlink_codec.h"
#include "voxlink_crc.h"
#include <cstring>

namespace voxlink {

// ---------------------------------------------------------------------------
// Protocol name helpers
// ---------------------------------------------------------------------------
const char *msg_type_name(MsgType type) {
  switch (type) {
  case MsgType::Hello: return "HELLO";
  case MsgType::HelloAck: return "HELLO_ACK";
  case MsgType::CapsRequest: return "CAPS_REQUEST";
  case MsgType::CapsBegin: return "CAPS_BEGIN";
  case MsgType::CapsParam: return "CAPS_PARAM";
  case MsgType::CapsEnd: return "CAPS_END";
  case MsgType::GetState: return "GET_STATE";
  case MsgType::StateBegin: return "STATE_BEGIN";
  case MsgType::StateParam: return "STATE_PARAM";
  case MsgType::StateEnd: return "STATE_END";
  case MsgType::GetParam: return "GET_PARAM";
  case MsgType::ParamValue: return "PARAM_VALUE";
  case MsgType::SetParam: return "SET_PARAM";
  case MsgType::ParamChanged: return "PARAM_CHANGED";
  case MsgType::Action: return "ACTION";
  case MsgType::Ack: return "ACK";
  case MsgType::Nack: return "NACK";
  case MsgType::Error: return "ERROR";
  case MsgType::Heartbeat: return "HEARTBEAT";
  case MsgType::MeterFrame: return "METER_FRAME";
  case MsgType::PitchFrame: return "PITCH_FRAME";
  case MsgType::DspStatus: return "DSP_STATUS";
  case MsgType::PresetList: return "PRESET_LIST(reserved)";
  case MsgType::PresetLoad: return "PRESET_LOAD(reserved)";
  case MsgType::PresetSave: return "PRESET_SAVE(reserved)";
  case MsgType::PresetRename: return "PRESET_RENAME(reserved)";
  case MsgType::MidiEvent: return "MIDI_EVENT(reserved)";
  case MsgType::SceneOp: return "SCENE_OP(reserved)";
  }
  return "UNKNOWN";
}

bool msg_type_known(uint8_t raw_type) {
  switch (static_cast<MsgType>(raw_type)) {
  case MsgType::Hello:
  case MsgType::HelloAck:
  case MsgType::CapsRequest:
  case MsgType::CapsBegin:
  case MsgType::CapsParam:
  case MsgType::CapsEnd:
  case MsgType::GetState:
  case MsgType::StateBegin:
  case MsgType::StateParam:
  case MsgType::StateEnd:
  case MsgType::GetParam:
  case MsgType::ParamValue:
  case MsgType::SetParam:
  case MsgType::ParamChanged:
  case MsgType::Action:
  case MsgType::Ack:
  case MsgType::Nack:
  case MsgType::Error:
  case MsgType::Heartbeat:
  case MsgType::MeterFrame:
  case MsgType::PitchFrame:
  case MsgType::DspStatus:
  case MsgType::PresetList:
  case MsgType::PresetLoad:
  case MsgType::PresetSave:
  case MsgType::PresetRename:
  case MsgType::MidiEvent:
  case MsgType::SceneOp:
    return true;
  }
  return false;
}

const char *code_name(Code code) {
  switch (code) {
  case Code::Ok: return "OK";
  case Code::UnknownMessage: return "UNKNOWN_MESSAGE";
  case Code::UnsupportedVersion: return "UNSUPPORTED_VERSION";
  case Code::BadLength: return "BAD_LENGTH";
  case Code::BadType: return "BAD_TYPE";
  case Code::UnknownParam: return "UNKNOWN_PARAM";
  case Code::ReadOnly: return "READ_ONLY";
  case Code::OutOfRange: return "OUT_OF_RANGE";
  case Code::InvalidEnum: return "INVALID_ENUM";
  case Code::QueueFull: return "QUEUE_FULL";
  case Code::Busy: return "BUSY";
  case Code::NotSupported: return "NOT_SUPPORTED";
  case Code::InternalError: return "INTERNAL_ERROR";
  case Code::BadCrc: return "BAD_CRC";
  case Code::Malformed: return "MALFORMED";
  }
  return "UNKNOWN";
}

size_t value_tag_size(ValueTag tag) {
  switch (tag) {
  case ValueTag::Bool: return 1;
  case ValueTag::Int32: return 4;
  case ValueTag::Float32: return 4;
  case ValueTag::Enum16: return 2;
  }
  return 0;
}

const char *decode_status_name(DecodeStatus status) {
  switch (status) {
  case DecodeStatus::Ok: return "OK";
  case DecodeStatus::ShortBuffer: return "SHORT_BUFFER";
  case DecodeStatus::BadSof: return "BAD_SOF";
  case DecodeStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
  case DecodeStatus::LengthTooLarge: return "LENGTH_TOO_LARGE";
  case DecodeStatus::BadCrc: return "BAD_CRC";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Byte-order helpers
// ---------------------------------------------------------------------------
void put_u16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}
void put_i32(uint8_t *out, int32_t value) {
  put_u32(out, static_cast<uint32_t>(value));
}
void put_u32(uint8_t *out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}
void put_f32(uint8_t *out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}
uint16_t get_u16(const uint8_t *in) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) |
                               (static_cast<uint16_t>(in[1]) << 8));
}
int32_t get_i32(const uint8_t *in) {
  return static_cast<int32_t>(get_u32(in));
}
uint32_t get_u32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}
float get_f32(const uint8_t *in) {
  const uint32_t bits = get_u32(in);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// ---------------------------------------------------------------------------
// Frame encoding
// ---------------------------------------------------------------------------
size_t encode_frame(MsgType type, uint8_t flags, uint8_t seq,
                    const uint8_t *payload, size_t payload_len, uint8_t *out,
                    size_t out_capacity) {
  if (out == nullptr || payload_len > kMaxPayload)
    return 0;
  const size_t total = kHeaderSize + payload_len + kCrcSize;
  if (out_capacity < total)
    return 0;

  out[0] = kSof0;
  out[1] = kSof1;
  out[2] = kVersion;
  out[3] = static_cast<uint8_t>(type);
  out[4] = flags;
  out[5] = seq;
  put_u16(out + 6, static_cast<uint16_t>(payload_len));
  if (payload_len > 0 && payload != nullptr)
    std::memcpy(out + kHeaderSize, payload, payload_len);

  const uint16_t crc = crc16_ccitt_false(out + 2, payload_len + 6);
  put_u16(out + kHeaderSize + payload_len, crc);
  return total;
}

// ---------------------------------------------------------------------------
// Frame decoding
// ---------------------------------------------------------------------------
DecodeStatus decode_frame(const uint8_t *data, size_t len, Frame *out,
                          size_t *consumed) {
  if (consumed != nullptr)
    *consumed = 0;
  if (data == nullptr || out == nullptr)
    return DecodeStatus::ShortBuffer;
  if (len < 2)
    return DecodeStatus::ShortBuffer;
  if (data[0] != kSof0 || data[1] != kSof1)
    return DecodeStatus::BadSof;
  if (len < kHeaderSize + kCrcSize)
    return DecodeStatus::ShortBuffer;

  const uint8_t version = data[2];
  if ((version >> 4) != kVersionMajor)
    return DecodeStatus::UnsupportedVersion;

  const uint16_t payload_len = get_u16(data + 6);
  if (payload_len > kMaxPayload)
    return DecodeStatus::LengthTooLarge;

  const size_t total = kHeaderSize + static_cast<size_t>(payload_len) + kCrcSize;
  if (len < total)
    return DecodeStatus::ShortBuffer;

  const uint16_t expected_crc =
      get_u16(data + kHeaderSize + payload_len);
  const uint16_t actual_crc = crc16_ccitt_false(data + 2, 6 + payload_len);
  if (expected_crc != actual_crc)
    return DecodeStatus::BadCrc;

  out->version = version;
  out->type = static_cast<MsgType>(data[3]);
  out->flags = data[4];
  out->seq = data[5];
  out->payload_len = payload_len;
  if (payload_len > 0)
    std::memcpy(out->payload, data + kHeaderSize, payload_len);
  if (consumed != nullptr)
    *consumed = total;
  return DecodeStatus::Ok;
}

// ---------------------------------------------------------------------------
// Typed value helpers
// ---------------------------------------------------------------------------
size_t encode_value(ValueTag tag, float value, uint8_t *out, size_t out_capacity) {
  const size_t size = value_tag_size(tag);
  if (out == nullptr || size == 0 || out_capacity < size)
    return 0;
  switch (tag) {
  case ValueTag::Bool:
    out[0] = (value >= 0.5f) ? 1u : 0u;
    break;
  case ValueTag::Int32: {
    const int32_t rounded = static_cast<int32_t>(value + (value >= 0.0f ? 0.5f : -0.5f));
    put_i32(out, rounded);
    break;
  }
  case ValueTag::Float32:
    put_f32(out, value);
    break;
  case ValueTag::Enum16: {
    const uint16_t rounded = static_cast<uint16_t>(value + 0.5f);
    put_u16(out, rounded);
    break;
  }
  }
  return size;
}

bool decode_value(ValueTag tag, const uint8_t *in, size_t len, float *out,
                  size_t *consumed) {
  if (in == nullptr || out == nullptr)
    return false;
  const size_t size = value_tag_size(tag);
  if (size == 0 || len < size)
    return false;
  switch (tag) {
  case ValueTag::Bool:
    *out = (in[0] != 0) ? 1.0f : 0.0f;
    break;
  case ValueTag::Int32:
    *out = static_cast<float>(get_i32(in));
    break;
  case ValueTag::Float32:
    *out = get_f32(in);
    break;
  case ValueTag::Enum16:
    *out = static_cast<float>(get_u16(in));
    break;
  }
  if (consumed != nullptr)
    *consumed = size;
  return true;
}

} // namespace voxlink
