#pragma once
// VoxLink v1 explicit frame and typed-value serialization. No compiler struct
// is ever written to the wire; every field is packed by hand in little-endian
// order. See docs/voxlink_v1.md.
#include "voxlink_protocol.h"
#include <cstddef>
#include <cstdint>

namespace voxlink {

struct Frame {
  uint8_t version = kVersion;
  MsgType type = MsgType::Error;
  uint8_t flags = kFlagNone;
  uint8_t seq = 0;
  uint16_t payload_len = 0;
  uint8_t payload[kMaxPayload] = {0};
};

enum class DecodeStatus {
  Ok,
  ShortBuffer,
  BadSof,
  UnsupportedVersion,
  LengthTooLarge,
  BadCrc,
};

const char *decode_status_name(DecodeStatus status);

// Encodes a complete frame. Returns the number of bytes written, or 0 when the
// payload is too large or the destination is too small.
size_t encode_frame(MsgType type, uint8_t flags, uint8_t seq,
                    const uint8_t *payload, size_t payload_len, uint8_t *out,
                    size_t out_capacity);

// Decodes exactly one frame starting at data[0]. On success, *consumed reports
// the frame size so callers can process back-to-back frames. Fills `out`.
DecodeStatus decode_frame(const uint8_t *data, size_t len, Frame *out,
                          size_t *consumed);

// ---------------------------------------------------------------------------
// Typed value helpers (registry values are carried as float internally).
// ---------------------------------------------------------------------------
size_t encode_value(ValueTag tag, float value, uint8_t *out, size_t out_capacity);
bool decode_value(ValueTag tag, const uint8_t *in, size_t len, float *out,
                  size_t *consumed);

// Byte-order helpers exposed for message builders.
void put_u16(uint8_t *out, uint16_t value);
void put_i32(uint8_t *out, int32_t value);
void put_u32(uint8_t *out, uint32_t value);
void put_f32(uint8_t *out, float value);
uint16_t get_u16(const uint8_t *in);
int32_t get_i32(const uint8_t *in);
uint32_t get_u32(const uint8_t *in);
float get_f32(const uint8_t *in);

} // namespace voxlink
