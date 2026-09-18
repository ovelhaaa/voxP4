# VoxP4 product architecture

This document describes the qualified product baseline and the VoxLink control
plane added in the Control Plane v1 milestone. It supersedes `specs.md` for
questions about *what is currently implemented*; `specs.md` remains the original
target/planned architecture and is explicitly labelled as such.

## 1. Current production baseline

The audio engine is B4D-qualified and **frozen**. The control plane adapts to
the engine; the engine does not depend on the control plane.

| Item | Value |
|---|---|
| Target | ESP32-P4, dual-core RISC-V |
| CPU | 360 MHz (`CONFIG_ESP32P4_DEFAULT_CPU_FREQ_360`) |
| Sample rate (production) | 44100 Hz |
| Block size | 64 frames |
| Block deadline | 1451.247 us |
| Harmony voices | **1** (`MAX_HARMONY_VOICES == 1`) |
| Pitch engine | TD-PSOLA, fixed interval |
| Formant preservation | LPC16 + GainNorm |
| FX chain | HPF → gate → compressor → delay → 8-line Hadamard FDN → limiter |
| Scheduling | S0 scheduler |
| Build | `-O2`, fast-math OFF |
| Realtime transport | `TRANSPORT REALTIME: PASS`, `PHYSICAL AUDIO REALTIME: PASS` |
| Hard per-block deadline | `FAIL — ACCEPTED` (do not reopen B4D optimization) |

The host test suite and the `pitch_shift` WAV renderer still default to 48000 Hz;
that is a host tooling default and does not describe the production target. The
B4D.12 burn-in mode sets the production rate to 44100 Hz.

## 2. Core responsibilities

```text
Core 0 — hard realtime                     Core 1 — analysis / control
--------------------------------------     ---------------------------------
I2S DMA / audio callback                   Pitch analysis (YIN, marks)
Input HPF / gate / compressor              VoxLink UART RX/TX task
TD-PSOLA harmony voice                     VoxLink parser / dispatcher
dry/harmony mixer, dry alignment           Product state + registry
delay, FDN reverb, limiter                 Telemetry preparation
drains the bounded SPSC parameter queue    presets/MIDI (future)
```

Rules that must not be broken:

* no dynamic allocation in the audio task;
* no mutex, blocking queue, logging, filesystem or UART access in the audio task;
* no cross-core mutable DSP object sharing;
* the UART task **never** touches realtime DSP objects directly.

## 3. Control-plane data flow

```text
                    ESP32 CYD
                       │
                       │ VoxLink / UART (921600 8N1)
                       ▼
              ┌──────────────────┐
              │ VoxLink RX/TX    │
              │ control task     │      Core 1
              └────────┬─────────┘
                       │
                       ▼
              ┌──────────────────┐
              │ Parameter        │
              │ Registry         │   (single authoritative table)
              └────────┬─────────┘
                       │
              validate / normalize
                       │
                       ▼
              ┌──────────────────┐
              │ Product State    │   (shadow values + monotonic revision)
              └────────┬─────────┘
                       │
                 bounded SPSC
                       │
                       ▼
              ┌──────────────────┐
              │ Audio task       │      Core 0
              │ block boundary   │
              └────────┬─────────┘
                       │
                       ▼
                      DSP
```

The existing `ParameterQueue<64>` is reused unchanged. `vocal_fx_set_parameter`
remains fire-and-forget; the control plane uses the new
`vocal_fx_try_set_parameter`, which reports whether the queue accepted the
update so the server can return `NACK/QUEUE_FULL` instead of silently claiming
success.

## 4. Source-of-truth rules

The ESP32-P4 is authoritative for:

* parameter values and effect enable/bypass state;
* capabilities and parameter numerical semantics;
* presets/DSP configuration (future);
* telemetry;
* firmware and VoxLink version.

The CYD is a client. It must display the P4-confirmed value, never its
optimistic local value. A `SET_PARAM` is followed by an authoritative
`PARAM_CHANGED` carrying the accepted (possibly clamped/quantized) value and
the current `state_revision`.

## 5. Parameter registry

`components/voxlink/include/voxlink_registry.h` defines one descriptor per
public parameter. Capability responses, validation, snapshots and future
presets all derive from it. See [`parameter_registry.md`](parameter_registry.md).

Public IDs are protocol ABI and are namespaced by subsystem:

```text
0x0000–0x00FF  system
0x0100–0x01FF  harmony
0x0200–0x02FF  dynamics (gate + compressor)
0x0300–0x03FF  delay
0x0400–0x04FF  reverb
0x0500–0x05FF  output/master
0x0600–0x06FF  pitch correction (reserved)
0x0700–0x07FF  doubler (reserved)
0x0F00–0x0FFF  global/product (reserved)
```

The engine binding lives in
`components/vocal_fx/control/vocal_fx_param_binding.cpp`. It is the only place
that knows both the public IDs and the engine's `VocalFxParameter` enum, so the
registry and the DSP internals stay decoupled.

## 6. Product state and revision

`voxlink::ProductState` stores one atomic shadow value per registered parameter
plus a monotonic 32-bit `revision`. Writes happen on the control task only; the
revision increments only when a value actually changes. Snapshots and
`PARAM_CHANGED` carry the revision so a controller can detect stale views.
Revision wrap is acceptable and comparison is by signed difference
(`(int32_t)(a - b) > 0`), documented in the protocol spec.

## 7. VoxLink

VoxLink v1 is a trusted, local, wired protocol. It provides discovery,
capability reporting, coherent state snapshots, GET/SET, confirmations,
heartbeats and reserved space for presets, MIDI and actions. The normative wire
format is [`voxlink_v1.md`](voxlink_v1.md). The reference client is
`tools/voxlink_cli.py`.

## 8. Priority order

```text
1. uninterrupted audio
2. coherent product state
3. reliable footswitch/control commands
4. reliable VoxLink synchronization
5. telemetry
6. UI convenience
```

If telemetry or UI traffic conflicts with realtime audio, telemetry is dropped.
If control traffic exceeds bounded capacity it is rejected predictably
(`QUEUE_FULL`). Audio is never blocked.

## 9. Component map

```text
components/voxlink/                 platform-neutral control core
  include/voxlink_protocol.h        framing, message IDs, error codes, flags
  include/voxlink_crc.h             CRC-16/CCITT-FALSE
  include/voxlink_codec.h           explicit little-endian serialization
  include/voxlink_parser.h          bounded streaming parser
  include/voxlink_registry.h        authoritative parameter registry
  include/voxlink_state.h           shadow product state + revision
  include/voxlink_session.h         dispatcher/session + bounded TX ring
  include/voxlink_uart.h            ESP32-P4 UART transport (ESP_PLATFORM)
components/vocal_fx/control/
  vocal_fx_param_binding.{h,cpp}    public ParamId <-> engine setter map
main/voxlink_service.{h,cpp}        starts the server when Kconfig enables it
tools/voxlink_cli.py                reference client
```

## 10. Build-time control

The control server is compiled in but **disabled by default** via
`CONFIG_VOXLINK_ENABLE_SERVER=n`. This keeps the qualified production firmware
behaviour equivalent until the option is turned on. When enabled, the UART task
runs on Core 1 with the pins and baud configured in Kconfig.

Final UART TX/RX GPIOs are **unassigned** (`-1`) pending verification against
the WT9932P4-TINY schematic; the audio pins GPIO19–23 and the console/JTAG pins
GPIO37/38 must not be used. The server refuses to start while the pins are
unassigned, so an accidentally enabled build cannot bind an audio pin. See
`docs/voxlink_cyd_integration.md` §1.
