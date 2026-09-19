# VoxLink v1 — CYD integration contract

Integration contract for `ovelhaaa/voxP4-control`. A CYD implementation must be
writable from this document plus [`voxlink_v1.md`](voxlink_v1.md) and the
exported artifacts in `integration/`, without reading VoxP4 source code.

The ESP32-P4 is always authoritative for parameters, effect enable state,
capabilities, state revision and telemetry. The CYD is a client.

## 1. Electrical wiring

```text
P4 TX  ──▶  CYD RX
P4 RX  ◀──  CYD TX
P4 GND ───  CYD GND   (common ground required)
```

* 3.3 V logic only. **Never connect 5 V UART.**
* Do not assume the P4 can power the CYD, or vice versa.
* The P4 UART is not isolated.

### Final GPIO assignment

**PENDING.** The WT9932P4-TINY audio pins are GPIO19–GPIO23
(`components/vocal_fx/platform/boards/wt9932p4_tiny_audio.h`); GPIO37/GPIO38 are
reserved for console/JTAG. The final VoxLink TX/RX pins must be verified against
the board schematic before assignment. Until then
`CONFIG_VOXLINK_UART_TX_GPIO`/`_RX_GPIO` default to `-1` and the server refuses
to start. Do not select a pin from this document; select it from the schematic.

## 2. Transport parameters

| Property | Value |
|---|---|
| Baud | 921600 (manual fallback 460800) |
| Format | 8N1, no flow control |
| Byte order | little-endian |
| Version | VoxLink major 1, `VERSION = 0x10` |

The P4 never auto-switches baud on corruption. Changing the fallback is a manual
configuration change on both ends.

## 3. Framing and CRC

```text
A5 5A | VERSION | TYPE | FLAGS | SEQ | LEN_lo LEN_hi | PAYLOAD | CRC_lo CRC_hi
```

* CRC-16/CCITT-FALSE, poly `0x1021`, init `0xFFFF`, refin/refout false,
  xorout `0x0000`, over `VERSION..PAYLOAD`. Check: `crc16("123456789") == 0x29B1`.
* Max payload 512 bytes. Max frame 522 bytes.
* `FLAGS` bit 0 `NOTIFICATION`, bit 1 `ACK_REQUESTED`.
* Parser must accept partial frames, multiple frames per read, garbage before
  SOF, bad CRC/length/version, and must recover without reboot.

Golden vectors: `integration/voxlink_v1_vectors.h`.

## 4. Connection flow

```text
CYD                                 P4
 |--------- HELLO ----------------->|
 |<-------- HELLO_ACK --------------|
 |------ CAPS_REQUEST ------------->|
 |<----- CAPS_BEGIN/PARAM x N/END ---|
 |--------- GET_STATE ------------->|
 |<------- STATE_BEGIN/PARAM/END ---|
 |                                  | ACTIVE
 |<------- HEARTBEAT (1 Hz) --------|
 |--------- HEARTBEAT ------------->|
 |<---------- ACK ------------------|
```

* States: `DISCONNECTED`, `HELLO_RECEIVED`, `ACTIVE`.
* Heartbeat timeout returns the link to `DISCONNECTED` only; audio and product
  state are untouched.
* P4 accepts `HELLO` at any time; there is no pairing state.
* On reconnect, request a fresh `GET_STATE`. **Never push stale CYD state onto
  the P4.** The P4 state wins.

## 5. Parameter model

* 49 public parameters. IDs are stable ABI, namespaced by subsystem:
  `0x01xx` harmony, `0x02xx` dynamics, `0x03xx` delay, `0x04xx` reverb,
  `0x05xx` output. `0x0F00` global bypass is **not implemented**.
* Types: `BOOL(1)`, `INT32(2)`, `FLOAT32(3)`, `ENUM16(4)`.
* Full schema: `integration/voxlink_params.json`.
* C IDs: `integration/VoxP4ParamIds.h`.
* The P4 is authoritative over whether a parameter exists and its numerical
  semantics; `CAPS_RESPONSE` always wins over a stale local table.

### Harmony interval is discrete

`harmony.interval` (`0x0101`) is an integer semitone value in `[-12, +12]`, with
`step = 1`. Do not interpolate or ramp it client-side. Treat it as a discrete
musical value, unlike `harmony.level`.

## 6. SET / GET semantics

```text
CYD: SET_PARAM(id, type, value) SEQ=S
P4 : validate → normalize → enqueue into bounded SPSC queue → record state
P4 : ACK(ref=SET_PARAM, SEQ=S)                code=OK
P4 : PARAM_CHANGED(id, revision, origin, type, accepted_value) SEQ=S
```

* Display the **accepted** value from `PARAM_CHANGED`, never the optimistic
  local value. The P4 may clamp or quantize.
* `ACCEPTED != SAMPLE-EXACT APPLIED`: `PARAM_CHANGED` means the value entered the
  authoritative realtime command stream; the audio task applies it at the next
  block boundary.
* A no-op write (normalizes to the current value) is still ACKed and produces
  `PARAM_CHANGED`, but `state_revision` does not increment.
* `GET_PARAM(id)` → `PARAM_VALUE(id, revision, type, value)`.
* `GET_STATE` → `STATE_BEGIN(revision, count)` / `STATE_PARAM×count` /
  `STATE_END(revision)`; all entries share one revision.

### Rejections

```text
UNKNOWN_PARAM   unknown id
BAD_TYPE        wire tag does not match descriptor type
INVALID_ENUM    enum outside range
MALFORMED       NaN / Inf / malformed
READ_ONLY       read-only parameter
QUEUE_FULL      realtime command queue saturated; state unchanged
NOT_SUPPORTED   reserved feature (presets/MIDI/scenes/actions)
```

On `NACK`, the CYD must keep the previous authoritative value; state is
guaranteed unchanged.

### QUEUE_FULL handling

The realtime parameter queue is bounded (`ParameterQueue<64>`). Under a burst
the P4 returns `NACK(QUEUE_FULL)` rather than blocking. Client policy:

* sliders: send at 20–50 Hz, latest value wins, send the final value on release;
* on `QUEUE_FULL`, drop the intermediate value and re-send only the latest on
  the next tick;
* never spin-retry at high rate.

## 7. Heartbeat

* P4 emits a heartbeat notification about every 1 s while `ACTIVE`.
* CYD may send `HEARTBEAT` at any time; P4 replies `ACK`.
* Loss of heartbeat must not change DSP state on either end.

## 8. Sequence and revision

* Direct responses echo the request `SEQ` exactly.
* Notifications use a server-owned `SEQ` and `FLAGS=NOTIFICATION`.
* `state_revision` is a 32-bit monotonic counter, incremented only on a genuine
  change. A newer revision `a` beats older `b` iff `(int32_t)(a - b) > 0`.
* `SEQ` is 8-bit and wraps modulo 256. Do not reuse a `SEQ` while its request is
  still pending. The P4 rejects a duplicate `SEQ` within one received batch with
  `NACK(BUSY)`; reuse after the response is transmitted is allowed.

### 8.1 Boot-state coherence

At startup, and once the audio engine is ready, the P4 seeds every registry
default into the engine through the same bounded queue. As a result, after the
first audio blocks:

```text
registry default == ProductState value == effective engine target
```

for all 49 parameters. A `GET_STATE` received during that brief window still
returns the registry defaults, which the engine converges to. This is verified
by `voxlink_coherence_tests` on the host (registry -> ProductState -> engine
target, including normalized/clamped values).

## 9. Rate recommendations

| Traffic | Rate |
|---|---|
| Continuous parameter writes | 20–50 Hz, final value on release |
| Discrete/actions | immediately |
| `GET_STATE` | on connect and after reconnect; not continuously |
| Heartbeat | 1 Hz |
| Meters | 20–30 Hz (when telemetry is enabled) |
| Pitch | 10–20 Hz (when telemetry is enabled) |
| DSP status | 2–5 Hz (when telemetry is enabled) |

Telemetry is best-effort and lower priority than control responses. The P4 drops
telemetry before control responses.

## 10. Capabilities

`HELLO_ACK` and `CAPS_BEGIN` carry capability bits. Advertised by the P4 in v1:

```text
CAP_HARMONY | CAP_COMPRESSOR | CAP_DELAY | CAP_REVERB | CAP_LIMITER |
CAP_GATE | CAP_FORMANT_PRESERVATION
```

Not advertised (not implemented): presets, MIDI, scenes, meters, pitch
telemetry, DSP status. Do not build UI for unadvertised capabilities.

## 11. Files to copy into `voxP4-control`

```text
integration/VoxP4ParamIds.h
integration/voxlink_params.json
integration/voxlink_v1_vectors.h
```

Add a client-side test that decodes every golden frame and verifies the CRC, so
the two independently-written implementations cannot silently diverge.
