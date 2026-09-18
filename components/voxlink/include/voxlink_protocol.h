#pragma once
// VoxLink v1 wire protocol constants shared by the ESP32-P4 server and the
// CYD controller. See docs/voxlink_v1.md for the normative specification.
//
// All multi-byte integer fields are little-endian. Floating point values are
// IEEE-754 binary32. Compiler structs are never placed on the wire directly;
// the codec performs explicit byte-level serialization.
#include <cstddef>
#include <cstdint>

namespace voxlink {

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
constexpr uint8_t kSof0 = 0xA5;
constexpr uint8_t kSof1 = 0x5A;

constexpr uint8_t kVersionMajor = 1;
constexpr uint8_t kVersionMinor = 0;
constexpr uint8_t kVersion = static_cast<uint8_t>((kVersionMajor << 4) | kVersionMinor);

// SOF(2) + version,type,flags,seq(4) + length(2)
constexpr size_t kHeaderSize = 8;
constexpr size_t kCrcSize = 2;
constexpr size_t kMaxPayload = 512;
constexpr size_t kMaxFrame = kHeaderSize + kMaxPayload + kCrcSize;

// Frame flags.
enum FrameFlags : uint8_t {
  kFlagNone = 0,
  // Set on server-initiated asynchronous notifications (not direct responses).
  kFlagNotification = 1u << 0,
  // Set when the sender wants an explicit ACK even for fire-and-forget types.
  kFlagAckRequested = 1u << 1,
};

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------
enum class MsgType : uint8_t {
  Hello = 0x01,
  HelloAck = 0x02,

  CapsRequest = 0x03,
  CapsBegin = 0x04,
  CapsParam = 0x05,
  CapsEnd = 0x06,

  GetState = 0x07,
  StateBegin = 0x08,
  StateParam = 0x09,
  StateEnd = 0x0A,

  GetParam = 0x0B,
  ParamValue = 0x0C,
  SetParam = 0x0D,
  ParamChanged = 0x0E,

  // Reserved actions. v1 returns NOT_SUPPORTED without side effects.
  Action = 0x0F,

  Ack = 0x10,
  Nack = 0x11,
  Error = 0x12,

  Heartbeat = 0x60,
  MeterFrame = 0x61,
  PitchFrame = 0x62,
  DspStatus = 0x63,

  // Reserved for future milestones (never implemented as placeholders).
  PresetList = 0x20,
  PresetLoad = 0x21,
  PresetSave = 0x22,
  PresetRename = 0x23,
  MidiEvent = 0x30,
  SceneOp = 0x40,
};

const char *msg_type_name(MsgType type);
// True for every type defined by VoxLink v1 (including reserved ones). Truly
// unknown wire values are counted and rejected by the parser/dispatcher.
bool msg_type_known(uint8_t raw_type);

// ---------------------------------------------------------------------------
// Error / result codes (also used by NACK)
// ---------------------------------------------------------------------------
enum class Code : uint16_t {
  Ok = 0,
  UnknownMessage = 1,
  UnsupportedVersion = 2,
  BadLength = 3,
  BadType = 4,
  UnknownParam = 5,
  ReadOnly = 6,
  OutOfRange = 7,
  InvalidEnum = 8,
  QueueFull = 9,
  Busy = 10,
  NotSupported = 11,
  InternalError = 12,
  BadCrc = 13,
  Malformed = 14,
};

const char *code_name(Code code);

// ---------------------------------------------------------------------------
// Typed parameter value encoding
// ---------------------------------------------------------------------------
enum class ValueTag : uint8_t {
  Bool = 1,
  Int32 = 2,
  Float32 = 3,
  Enum16 = 4,
};

// Byte size of the value body for a tag, or 0 for an unknown tag.
size_t value_tag_size(ValueTag tag);

// ---------------------------------------------------------------------------
// Parameter namespaces. IDs are protocol ABI and must stay explicit.
// ---------------------------------------------------------------------------
namespace param_group {
constexpr uint16_t kSystem = 0x0000;
constexpr uint16_t kHarmony = 0x0100;
constexpr uint16_t kDynamics = 0x0200;
constexpr uint16_t kDelay = 0x0300;
constexpr uint16_t kReverb = 0x0400;
constexpr uint16_t kOutput = 0x0500;
constexpr uint16_t kPitchCorrection = 0x0600; // reserved
constexpr uint16_t kDoubler = 0x0700;         // reserved
constexpr uint16_t kGlobal = 0x0F00;
} // namespace param_group

// ---------------------------------------------------------------------------
// Capability bits. Only implemented features are ever advertised.
// ---------------------------------------------------------------------------
enum : uint32_t {
  kCapHarmony = 1u << 0,
  kCapCompressor = 1u << 1,
  kCapDelay = 1u << 2,
  kCapReverb = 1u << 3,
  kCapLimiter = 1u << 4,
  kCapGate = 1u << 5,
  kCapMeters = 1u << 6,
  kCapPitchTelemetry = 1u << 7,
  kCapDspStatus = 1u << 8,
  kCapPresets = 1u << 9,
  kCapMidi = 1u << 10,
  kCapScenes = 1u << 11,
  kCapFormantPreservation = 1u << 12,
};

// Parameter descriptor flags.
enum : uint32_t {
  kParamNone = 0,
  kParamPersistent = 1u << 0,
  kParamRealtimeSafe = 1u << 1,
  kParamReadOnly = 1u << 2,
  kParamTelemetry = 1u << 3,
  kParamRequiresReinit = 1u << 4,
  kParamSmoothed = 1u << 5,
  kParamDiscrete = 1u << 6,
};

enum class ParamOrigin : uint8_t {
  Controller = 0,
  Midi = 1,
  Preset = 2,
  Internal = 3,
};

} // namespace voxlink
