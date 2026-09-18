# VoxP4 Control Plane v1 — milestone report

Date: 2026-09-18
Scope: first production-grade control plane, VoxLink v1 server, authoritative
parameter registry and product state.

## Result summary

```text
VOXP4 CONTROL PLANE v1 RESULT: HOST-VERIFIED / TARGET PENDING

PRODUCT CONFIGURATION DOCUMENTATION:  ALIGNED
PARAMETER REGISTRY:                   AUTHORITATIVE / STABLE
PRODUCT STATE:                        AUTHORITATIVE ON ESP32-P4
VOXLINK v1:                           IMPLEMENTED
UART CONTROL:                         IMPLEMENTED, NOT HARDWARE-QUALIFIED
STATE SYNCHRONIZATION:                PASS (host)
AUDIO TRANSPORT UNDER CONTROL STRESS: NOT RUN (no hardware)
B4D-QUALIFIED DSP ARCHITECTURE:       UNCHANGED
```

The milestone was scoped to the host-verifiable core. Flashing and realtime
hardware measurement are unavailable in this environment (see `AGENTS.md`), so
Gate F (hardware control stress) and the B4D.12 timing/backlog comparisons are
explicitly **pending hardware** and are not claimed as passed.

## 1. Architecture implemented

```text
CYD ──VoxLink/UART──▶ UART RX/TX task (Core 1)
                         │  parser + dispatcher
                         ▼
                 Parameter Registry (single authoritative table)
                         │  validate / normalize
                         ▼
                 Product State (atomic shadow values + revision)
                         │  bounded SPSC ParameterQueue<64>
                         ▼
                 Audio task (Core 0), drained at block boundary
                         │
                         ▼
                        DSP (frozen)
```

The UART/control path never touches realtime DSP objects. The existing
`ParameterQueue<64>` is reused unchanged. A control-only entry point
`vocal_fx_try_set_parameter()` was added; `vocal_fx_set_parameter()` keeps its
original fire-and-forget behavior. No DSP algorithm, topology, sample rate,
block size, scheduler or buffering was changed.

## 2. Documentation corrected

* `README.md` no longer claims two harmony voices or an abandoned configuration;
  it now states the 360 MHz / 44100 Hz / 64-frame / one-voice baseline.
* `specs.md` carries a prominent "target/planned, not current production"
  banner separating planned features from the implemented product.
* `docs/benchmarking.md`, `docs/pitch_analysis.md`, `docs/td_psola.md`,
  `docs/formant_preservation.md`, `docs/harmony_engine.md` and
  `docs/harmony_validation.md` now mark historical 48 kHz / 400 MHz / two-voice
  material as historical or host-tooling defaults.
* Added `docs/product_architecture.md`, `docs/parameter_registry.md`,
  `docs/voxlink_v1.md`.

## 3. Parameter inventory

* Registry entries: **45**.
* Groups: harmony (17), dynamics (13), delay (7), reverb (4), output (4).
* All entries are realtime-safe and persistent; continuous parameters are
  marked `S` (smoothed) and discrete/musical values `D`.
* Stable explicit IDs by subsystem; `registry_validate()` enforces unique IDs,
  unique keys, `min <= default <= max`, `step >= 0`, finite metadata, valid
  boolean/enum bounds.
* `voxlink_binding_tests` proves every advertised ID maps to exactly one engine
  setter and that no two public IDs alias the same engine parameter.
* `GlobalBypass` (0x0F00) is reserved but **not advertised**: the engine has no
  global bypass, so it is not invented. `harmony.voice1.gain` was removed to
  avoid two IDs controlling the single voice gain.

## 4. VoxLink wire format

* SOF `A5 5A`; header `VERSION, TYPE, FLAGS, SEQ, LEN(u16 LE)`; payload; CRC16.
* CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF, no reflection, xorout 0;
  check value `0x29B1` for `"123456789"`.
* CRC covers `VERSION..PAYLOAD` (excludes SOF and CRC).
* Little-endian everywhere; explicit pack/unpack, never compiler structs.
* Max payload 512 B; max frame 522 B.
* Typed values: `BOOL(1)`, `INT32(4)`, `FLOAT32(4 IEEE-754)`, `ENUM16(2)`.
* Full message set, error codes and flow: `docs/voxlink_v1.md`.
* Golden vectors shared with `tests/data/voxlink_v1_vectors.h` and
  `tools/voxlink_cli.py selftest`.

## 5. UART configuration

| Property | Value |
|---|---|
| Port | `CONFIG_VOXLINK_UART_PORT` (default 1) |
| Baud | `CONFIG_VOXLINK_UART_BAUD` (default 921600, fallback 460800 documented) |
| Framing | 8N1, no flow control |
| TX/RX GPIO | `CONFIG_VOXLINK_UART_TX_GPIO` / `_RX_GPIO` |
| Task | Core 1, priority 5, 4096-byte stack |
| Enable | `CONFIG_VOXLINK_ENABLE_SERVER` (default **n**) |

With the server disabled the linker drops the entire control core: measured
`libvoxlink.a` contribution to the firmware is zero while unused.

## 6. Memory footprint (measured)

| Object | Bytes |
|---|---|
| `sizeof(Parser)` (working frame + 8-frame queue) | 4,784 |
| `sizeof(Session)` (parser + state ptr + TX ring + counters) | 13,216 |
| `sizeof(ProductState)` | 400 |
| TX ring (`Session::kTxCapacity`) | 8,192 |
| Registry descriptors (45 × 80, rodata) + keys | ≈ 3,600 + ~1 kB |
| UART task stack | 4,096 |

Steady-state protocol path performs **no dynamic allocation**. All buffers are
fixed-capacity members.

## 7. Queue behavior under overload

`SET_PARAM` flow: decode → normalize → `vocal_fx_try_set_parameter` (pushes to
`ParameterQueue<64>`) → on success update product state/revision → ACK +
PARAM_CHANGED. If the SPSC queue is full the server returns `NACK(QUEUE_FULL)`,
increments `control_queue_full`/`control_commands_rejected`, and leaves product
state untouched. Verified by `voxlink_state_sync_tests`.

## 8. Parser robustness

`voxlink_framing_tests` + `voxlink_fuzz_tests` cover: one byte at a time, split
header/payload/CRC, two frames per buffer, 100 back-to-back frames, garbage
before SOF, bad CRC, invalid length, unsupported version, truncated frame, SOF
inside payload, random noise recovery, and 2 MB of mixed noise + injected valid
frames. No crash, no unbounded loop, bounded parser state, final frame recovered
after 2 MB of noise.

## 9. Host test results

```text
Baseline before milestone: 34/34 PASS
After milestone:           41/41 PASS
```

New targets (all pass):

```text
voxlink_registry_tests
voxlink_framing_tests
voxlink_fuzz_tests
voxlink_state_sync_tests
voxlink_concurrency_tests
voxlink_golden_tests
voxlink_binding_tests
```

`voxlink_concurrency_tests` runs a writer thread and a reader thread over the
product state and the bounded SPSC queue and verifies no torn/out-of-range
values and that every accepted queue entry is drained exactly once.

## 10. ESP-IDF build

```text
Target: esp32p4 (ESP-IDF v5.3)
Result: Project build complete
Image:  build/vox_p4.bin = 0x9A370 bytes (631,344 B), 40% app-partition free
DIRAM:  503,911 / 576,464 bytes (87.41%) with control server disabled
```

The production default keeps `CONFIG_VOXLINK_ENABLE_SERVER=n`; the qualified
realtime path is unchanged.

## 11. Not measured (hardware required)

* Message round-trip latency on target.
* Target control-stress qualification (cases A–H), RX/TX overruns, DMA errors.
* DSP timing delta vs B4D.12 (avg/P99/P99.9/max).
* Pitch-analysis backlog delta vs B4D.12.
* UART loopback at 921600 baud.
* Rapid interval-change and effect-bypass stress through VoxLink.

These remain pending hardware and must be executed before the milestone can be
declared fully qualified.

## 12. Mandatory questions

1. **Exactly one authoritative parameter registry?** Yes — `voxlink_registry.h/.cpp`; capability, validation, snapshot and binding all derive from it.
2. **Stable documented ParamIds?** Yes — explicit hex values, namespaced, documented in `parameter_registry.md`; regression-guarded in tests.
3. **Capability discovery?** Yes — `CAPS_REQUEST` → `CAPS_BEGIN`/`CAPS_PARAM`×N/`CAPS_END`, generated from the registry.
4. **Coherent state snapshot?** Yes — `GET_STATE` → `STATE_BEGIN`/`STATE_PARAM`×N/`STATE_END`, one revision, serialized on the control task.
5. **GET and SET every writable parameter?** Yes — every registry entry is writable/realtime-safe and bound; `GET_PARAM`/`SET_PARAM` cover them.
6. **Invalid writes safely rejected?** Yes — type/NaN/Inf/enum/unknown/read-only/queue-full all rejected without partial mutation.
7. **P4 remains source of truth?** Yes — ACK + authoritative `PARAM_CHANGED` with accepted value and revision.
8. **Writes reach DSP without blocking audio?** Yes — validated/normalized on Core 1, delivered via the existing bounded SPSC queue drained at the block boundary.
9. **Can slider traffic overflow the realtime queue?** Yes, it can by design.
10. **How handled?** Predictable `NACK(QUEUE_FULL)`, counters incremented, state unchanged, audio untouched.
11. **Parser recovers from arbitrary corruption?** Yes — streaming resync; proven by framing + 2 MB fuzz tests.
12. **CRC covered by golden vectors?** Yes — `0x29B1` check value and five golden frames shared with the CLI.
13. **CYD reset/reconnect without affecting audio?** Yes — heartbeat timeout only changes connection state.
14. **P4 reset/reconnect cleanly?** Yes — `HELLO` accepted at any time; no pairing state.
15. **Telemetry lower priority than control?** Yes — telemetry enqueues only below a high-water mark; control responses are always admitted first.
16. **VoxLink materially increases DSP tail?** Not measured (no hardware); the audio task contains no control work.
17. **Materially increases pitch backlog?** Not measured (no hardware).
18. **Transport errors zero under aggressive control?** Not measured (no hardware); host stress is functionally clean.
19. **Rapid harmony interval control physically safe?** Not measured on target; registry semantics are discrete semitones, routed through the unchanged engine path.
20. **Aggressive effect bypass physically safe?** Not measured on target; enable/bypass routes through the unchanged engine path.
21. **All previous host DSP tests still passing?** Yes — 34/34 original plus 7 new = 41/41.
22. **`docs/voxlink_v1.md` sufficient to implement CYD?** Yes — transport, framing, CRC, typed values, all message payloads, codes, sequence/revision semantics, flows, rates and golden vectors.
23. **Ready for presets without redesign?** Yes — `PARAM_PERSISTENT` selects stored values; registry/state are preset-friendly.
24. **Ready for footswitch actions without abusing parameters?** Yes — `ACTION` is a distinct message type and reserved; parameters remain values.

## 13. Known limitations

* Target stress/qualification not run in this environment.
* Telemetry frames (`METER_FRAME`, `PITCH_FRAME`, `DSP_STATUS`) are implemented
  and bounded but not yet scheduled by the production service; meters are not
  advertised as capabilities until wired.
* No preset/MIDI/scene implementation (deliberately out of scope).
* Coalescing policy for continuous parameters is documented but not yet applied
  server-side; clients are asked to send at 20–50 Hz.
* UART pin/baud defaults are placeholders pending board integration.
* Parser queue holds 8 frames; very large bursts must use the callback feed path
  (used by controllers receiving `CAPS_RESPONSE`).

## 14. Future preset integration plan

1. Store only `PARAM_PERSISTENT` parameters; never telemetry/read-only.
2. Add preset identity to product state and to `STATE_*`/`HELLO_ACK`.
3. Persist on the P4 (NVS/flash); the CYD never owns preset authority.
4. Load = validate → submit through the same bounded SPSC path → bump revision
   → broadcast `PARAM_CHANGED` per changed value.
5. Implement the reserved `PRESET_*` message IDs; keep `ACTION` separate.

## 15. Failure classification

No failure category applies to the host-verifiable scope:

```text
DOCUMENTATION INCOMPLETE .......... addressed
PARAMETER MODEL INCOMPLETE ........ none
PROTOCOL DESIGN FAILURE ........... none
PARSER ROBUSTNESS FAILURE ......... none
STATE CONSISTENCY FAILURE ......... none
REALTIME QUEUE FAILURE ............ none
UART RELIABILITY FAILURE .......... pending hardware
AUDIO REGRESSION .................. none (34/34 original tests pass)
```
