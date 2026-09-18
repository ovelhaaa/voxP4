# VoxLink v1 specification

Normative wire specification for the VoxP4 control plane. A controller
implementation must be writable from this document alone, without reading P4
source code. The CYD project `ovelhaaa/voxP4-control` shares these definitions.

## 1. Transport

| Property | Value |
|---|---|
| Physical | UART, point-to-point, wired |
| Baud | 921600 (documented fallback: 460800) |
| Framing | 8 data bits, no parity, 1 stop bit (8N1) |
| Flow control | none |
| Byte order | little-endian for every multi-byte field |
| Realms | trusted local link; no authentication or encryption in v1 |

The P4 does not auto-switch baud on corrupted traffic. The baud is fixed by
configuration (`CONFIG_VOXLINK_UART_BAUD`, default 921600).

## 2. Frame format

```text
offset  size  field
0       1     SOF0 = 0xA5
1       1     SOF1 = 0x5A
2       1     VERSION = 0x10 (major<<4 | minor)
3       1     TYPE  (message id, see §5)
4       1     FLAGS
5       1     SEQ   (8-bit sequence)
6       2     LENGTH (payload length, little-endian, 0..512)
8       N     PAYLOAD
8+N     2     CRC16 (little-endian)
```

Header size is 8 bytes, CRC size 2 bytes, maximum payload 512 bytes, maximum
frame 522 bytes. Fields are packed explicitly; compiler structs are never placed
on the wire.

`FLAGS`:

```text
bit 0  NOTIFICATION   server-initiated asynchronous frame (not a direct response)
bit 1  ACK_REQUESTED  sender wants an explicit ACK for a fire-and-forget type
```

## 3. CRC

CRC-16/CCITT-FALSE over the bytes `VERSION..PAYLOAD` (that is, `frame[2]` up to
`frame[8+N)`, excluding SOF and the CRC itself).

```text
poly   = 0x1021 (normal, MSB-first)
init   = 0xFFFF
refin  = false
refout = false
xorout = 0x0000
check  = crc16("123456789") == 0x29B1
```

## 4. Typed values

Every parameter value is a tag byte followed by a fixed-size body:

```text
1  BOOL     1 byte   0x00 or 0x01
2  INT32    4 bytes  signed little-endian
3  FLOAT32  4 bytes  IEEE-754 binary32 little-endian
4  ENUM16   2 bytes  unsigned little-endian
```

The tag must match the registry type for the parameter. A mismatch yields
`BAD_TYPE`. `NaN` and `Inf` are rejected (`MALFORMED`). Strings, when they exist,
are UTF-8 and length-prefixed, never NUL-terminated or NUL-dependent, with a
strict length bound.

## 5. Message IDs

```text
0x01 HELLO              0x02 HELLO_ACK
0x03 CAPS_REQUEST       0x04 CAPS_BEGIN     0x05 CAPS_PARAM     0x06 CAPS_END
0x07 GET_STATE          0x08 STATE_BEGIN    0x09 STATE_PARAM    0x0A STATE_END
0x0B GET_PARAM          0x0C PARAM_VALUE    0x0D SET_PARAM      0x0E PARAM_CHANGED
0x0F ACTION
0x10 ACK                0x11 NACK           0x12 ERROR
0x60 HEARTBEAT          0x61 METER_FRAME    0x62 PITCH_FRAME    0x63 DSP_STATUS

Reserved (must be rejected with NOT_SUPPORTED in v1):
0x20 PRESET_LIST  0x21 PRESET_LOAD  0x22 PRESET_SAVE  0x23 PRESET_RENAME
0x30 MIDI_EVENT   0x40 SCENE_OP
```

Any other type yields `ERROR` with code `UNKNOWN_MESSAGE`.

## 6. Message payloads

All offsets are **absolute byte offsets within PAYLOAD** (not within the frame).
Every layout below is exact: fields are contiguous, there is **no padding**, and
`LENGTH` equals the final `offset + width`. Multi-byte fields are little-endian.
`u8/u16/u32/i32/f32` denote the typed encodings of §4 (`f32` is IEEE-754
binary32).

### HELLO (C→P)
```text
0  u8  protocol version (0x10)
1  u8  client type (0=unknown, 1=CYD, 2=host CLI)
2  u8  fw_len
3  ... client firmware string (optional, <= 40 bytes)
```

### HELLO_ACK (P→C)
```text
0  u8  protocol version (0x10)
1  u32 capability flags
5  u32 sample rate (Hz)
9  u16 block size (frames)
11 u8  harmony voice count
12 u32 state revision
16 u8  product id (1 = VoxP4)
17 u8  hardware target (1 = ESP32-P4)
18 u8  firmware major
19 u8  firmware minor
20 u8  firmware patch
21 u8  git sha length
22 ... git sha (UTF-8, <= 40 bytes)
```

### CAPS_REQUEST (C→P)
Empty. Response is `CAPS_BEGIN`, N × `CAPS_PARAM`, `CAPS_END`.

### CAPS_BEGIN (P→C)
```text
0 u8  protocol version
1 u32 capability flags
5 u16 parameter count
```

### CAPS_PARAM (P→C)
```text
0  u16 id
2  u8  type
3  u32 flags
7  f32 min
11 f32 max
15 f32 default
19 f32 step
23 f32 current value
27 u8  group (0 harmony, 1 dynamics, 2 delay, 3 reverb, 4 output, 5 other)
```

### CAPS_END (P→C)
```text
0 u16 parameter count
```

### GET_STATE (C→P)
Empty. Response is `STATE_BEGIN`, N × `STATE_PARAM`, `STATE_END`, all carrying
the same `state_revision`.

### STATE_BEGIN (P→C)
```text
0 u32 state revision
4 u16 parameter count
```

### STATE_PARAM (P→C)
```text
0  u16 id
2  u8  type
3  ... value
```

### STATE_END (P→C)
```text
0 u32 state revision
```

### GET_PARAM (C→P)
```text
0 u16 id
```

### PARAM_VALUE (P→C)
```text
0  u16 id
2  u32 state revision
6  u8  type
7  ... value
```

### SET_PARAM (C→P)
```text
0  u16 id
2  u8  type
3  ... requested value
```

### PARAM_CHANGED (P→C)
```text
0  u16 id
2  u32 state revision
6  u8  origin (0 controller, 1 MIDI, 2 preset, 3 internal)
7  u8  type
8  ... accepted value
```

A `PARAM_CHANGED` following a `SET_PARAM` echoes the request `SEQ`; it is the
authoritative confirmation. Asynchronous changes use `NOTIFICATION` and a
server-owned sequence.

### ACK / NACK / ERROR
```text
0 u8  reference type (original TYPE)
1 u8  reference seq  (original SEQ)
2 u16 code
```

`ACK` carries code `OK`. `NACK` carries a specific rejection code. `ERROR` is
used when the request type or sequence cannot be trusted.

### HEARTBEAT
Empty. The server replies with `ACK`. The server also emits a heartbeat
notification approximately every second while the link is active.

### METER_FRAME (P→C, telemetry)
Fixed 28-byte payload, seven consecutive `f32`:
```text
0  f32 input_peak
4  f32 input_rms
8  f32 harmony_peak
12 f32 harmony_rms
16 f32 output_peak
20 f32 output_rms
24 f32 limiter_reduction_db   (>= 0)
```

### PITCH_FRAME (P→C, telemetry)
```text
0  f32 frequency_hz
4  f32 confidence (0..1)
8  u8  voiced
9  f32 midi_note
13 f32 age_ms
```

### DSP_STATUS (P→C, telemetry)
Fixed 40-byte payload, sequential, no padding:
```text
offset  size  type  field
0       4     u32   sample_rate
4       2     u16   block_size
6       4     f32   dsp_avg_us
10      2     u16   dsp_peak_us
12      4     u32   deadline_misses
16      4     u32   tx_underruns
20      4     u32   rx_overruns
24      4     f32   backlog_ms
28      4     u32   free_internal
32      4     u32   free_psram
36      4     u32   uptime_s
```
This layout matches `Session::send_dsp_status()` exactly and is locked by the
`voxlink_protocol_conformance_tests` golden vector. There are no alternative or
illustrative offsets.

## 7. Result codes

```text
0  OK                   1  UNKNOWN_MESSAGE      2  UNSUPPORTED_VERSION
3  BAD_LENGTH           4  BAD_TYPE             5  UNKNOWN_PARAM
6  READ_ONLY            7  OUT_OF_RANGE         8  INVALID_ENUM
9  QUEUE_FULL           10 BUSY                 11 NOT_SUPPORTED
12 INTERNAL_ERROR       13 BAD_CRC              14 MALFORMED
```

## 8. Sequence and revision semantics

* Client requests use an 8-bit sequence; a direct response echoes it exactly.
* Asynchronous notifications use a server-owned 8-bit sequence and set
  `NOTIFICATION`.
* `state_revision` is a monotonically increasing 32-bit counter, incremented
  only when an externally visible value actually changes. Wrap-around is
  acceptable; compare with signed subtraction: newer iff
  `(int32_t)(a - b) > 0`.
* Controller command → confirmation flow:

```text
CYD:  SET_PARAM(reverb.wet = 0.40) seq=7
P4 :  validate → normalize → submit to bounded SPSC queue
P4 :  ACK(ref=SET_PARAM, seq=7)
P4 :  PARAM_CHANGED(id=0x0401, value=<accepted>, revision=N, seq=7)
```

The CYD must display the accepted value from `PARAM_CHANGED`, never the
optimistic local value. If the queue is full, the P4 returns
`NACK(QUEUE_FULL)` and does not change state.

### 8.1 What `PARAM_CHANGED` confirms

```text
ACCEPTED != SAMPLE-EXACT APPLIED
```

A successful `SET_PARAM` produces `ACK` and `PARAM_CHANGED` once the value has
been:

1. validated and normalized, and
2. **accepted into the authoritative realtime command stream** (the bounded SPSC
   `ParameterQueue<64>`), and
3. recorded in `ProductState`.

It does **not** guarantee that the new value has already affected the
currently-rendering audio sample or block. The audio task drains the queue at
the next block boundary, on its own schedule. This is intentional: VoxLink v1.1
does not add per-sample DSP-application acknowledgements. A controller that needs
audible confirmation must use opinion/latency measurements, not a protocol
guarantee.

### 8.2 Revision no-op rule

If a requested value normalizes to the current stored value (for example the
same gain sent twice, or a clamp that lands on the value already held), then:

```text
state_revision does NOT increment
```

`ACK` and `PARAM_CHANGED` are still returned (`PARAM_CHANGED` reports the
unchanged revision), so the controller always receives a confirmation. Only a
genuine change to an externally visible value increments the revision.

### 8.3 Request correlation

For a successful `SET_PARAM` with request `SEQ = S`:

```text
ACK            : FLAGS=0, SEQ=S, payload ref_type=SET_PARAM, ref_seq=S, code=OK
PARAM_CHANGED  : FLAGS=0, SEQ=S, payload id, revision, origin, type, accepted value
```

Both frames echo the request sequence exactly. Asynchronous/internal changes use
`FLAGS=NOTIFICATION` and a server-owned sequence, and must not be correlated
with a client request.

## 9. Normalization policy

* malformed frame / wrong length → protocol error, no state change;
* `NaN`/`Inf` → `MALFORMED`;
* enum outside range or non-integer → `INVALID_ENUM`;
* numeric value slightly outside `[min,max]` → clamped and accepted;
* value quantized to the descriptor `step`;
* unknown parameter → `UNKNOWN_PARAM`;
* read-only parameter → `READ_ONLY`.

## 10. Connection and reconnect

```text
CYD                                 P4
 |--------- HELLO ----------------->|
 |<-------- HELLO_ACK --------------|
 |------ CAPS_REQUEST ------------->|
 |<----- CAPS_BEGIN/PARAM/END ------|
 |--------- GET_STATE ------------->|
 |<------- STATE_BEGIN/PARAM/END ---|
 |                                  |  ACTIVE
 |<------- HEARTBEAT (1 Hz) --------|
 |--------- HEARTBEAT ------------->|
 |<---------- ACK ------------------|
```

* Connection states: `DISCONNECTED`, `HELLO_RECEIVED`, `ACTIVE`.
* UART electrical presence alone does not imply a valid client.
* Heartbeat timeout returns the P4 to `DISCONNECTED` without changing any audio
  or product state. Audio continues normally if the CYD disappears.
* The P4 accepts `HELLO` at any time and never requires a power-cycle pairing.
* After reconnect the CYD requests a fresh state snapshot. It must **not**
  restore a stale cached state onto the P4; the P4 state wins.

## 11. Rate recommendations

| Traffic | Recommendation |
|---|---|
| Slider/knob continuous | 20–50 Hz, latest value wins, send final value on release |
| Footswitch/actions | send immediately |
| Telemetry | server-paced; server drops telemetry before control responses |
| Heartbeat | 1 Hz |
| Meters | 20–30 Hz (when implemented) |
| Pitch telemetry | 10–20 Hz (when implemented) |
| DSP status | 2–5 Hz (when implemented) |

The P4 never blocks audio to service control traffic. TX is a bounded ring;
telemetry is dropped first (`telemetry_dropped`), and a dropped critical
response is counted separately (`protocol_response_dropped`).

## 12. Golden vectors

Shared with `tests/data/voxlink_v1_vectors.h` and `tools/voxlink_cli.py`.

```text
crc16("123456789") = 0x29B1

HELLO   seq=0x2A payload=10 01 00
        A5 5A 10 01 00 2A 03 00 10 01 00 D5 21

SET_PARAM id=0x0101 type=INT32 value=7 seq=1
        A5 5A 10 0D 00 01 07 00 01 01 02 07 00 00 00 30 1D

GET_PARAM id=0x0401 seq=2
        A5 5A 10 0B 00 02 02 00 01 04 FB 9D

ACK ref=SET_PARAM seq=1 code=OK
        A5 5A 10 10 00 01 04 00 0D 01 00 00 27 96

HEARTBEAT seq=3
        A5 5A 10 60 00 03 00 00 18 54

HELLO with corrupted CRC (must fail)
        A5 5A 10 01 00 2A 03 00 10 01 00 D5 DE
```

## 13. Parser requirements (normative for both ends)

A conforming parser accepts arbitrary byte chunks and correctly handles:
partial frames, multiple frames per read, garbage before SOF, corrupt length,
bad CRC, unknown type, unsupported version, truncated frame, SOF bytes inside a
payload, and back-to-back frames. It recovers automatically after malformed
input without reboot and maintains counters:
`frames_valid`, `frames_dropped`, `crc_errors`, `framing_errors`,
`length_errors`, `unsupported_version`, `unknown_type`, `parser_resyncs`.

Memory is bounded: maximum payload 512 bytes, no allocation in the steady-state
protocol path.
