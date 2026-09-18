# VoxP4 Control Plane v1.1 — target enablement / CYD integration contract

Date: 2026-09-18
Scope: normative-spec closure, target enablement build, memory measurement,
integration-contract export. No DSP changes.

## Result

```text
VOXP4 CONTROL PLANE v1.1 RESULT:

SPEC / ABI:                               PASS
DOCUMENTATION / ENCODER / DECODER / GOLDEN: CONSISTENT
TARGET BUILD (VoxLink OFF):               PASS
TARGET BUILD (VoxLink ON):                PASS
INTEGRATION CONTRACT:                     READY
HOST TESTS:                               42/42 PASS
PHYSICAL UART / REALTIME QUALIFICATION:   PENDING HARDWARE

NO PRODUCTION CLAIM MADE FOR UART YET.
```

This environment has no attached hardware (`AGENTS.md`), so only the physical
UART/realtime gates are pending. All specification, build, memory, and
integration tasks are complete.

## 1. Phase A — normative spec closure

`docs/voxlink_v1.md` is now exact and normative:

* §6 states offsets are absolute within PAYLOAD, no padding, `LENGTH` equals the
  final `offset + width`; all multi-byte fields little-endian.
* `DSP_STATUS` had conflicting illustrative offsets; it is now a single exact
  40-byte sequential layout matching `Session::send_dsp_status()` and locked by
  `voxlink_protocol_conformance_tests`.
* `METER_FRAME` (28 B, seven `f32`) was normalised to a single-column layout.
* Typo `vodlink` fixed.
* §8.1 adds the explicit `ACCEPTED != SAMPLE-EXACT APPLIED` clarification.
* §8.2 documents the revision no-op rule.
* §8.3 documents ACK / PARAM_CHANGED request correlation.
* Every implemented message payload is documented with offset/width/type.

### Conformance test

`voxlink_protocol_conformance_tests` encodes all 19 implemented message types
from their normative payloads and compares byte-for-byte against
`tests/data/voxlink_v1_vectors.h`, then decodes and field-checks each. It also
asserts the `DSP_STATUS` field offsets individually. Documentation, encoder,
decoder and golden vectors now form one ABI.

New/changed tests:

```text
voxlink_protocol_conformance_tests   (new)
voxlink_state_sync_tests             (+ ACK/PARAM_CHANGED correlation,
                                       + ACCEPTED != APPLIED,
                                       + revision no-op via existing checks)
tests/data/voxlink_v1_vectors.h      (+ HELLO_ACK, CAPS_BEGIN/PARAM/END,
                                       STATE_BEGIN/PARAM/END, PARAM_VALUE,
                                       PARAM_CHANGED, NACK, ERROR,
                                       METER/PITCH/DSP_STATUS)
```

## 2. Historical-document banners

Added the standard banner to `docs/td_psola.md` and `docs/harmony_engine.md`:

```text
HISTORICAL MILESTONE DOCUMENT — DO NOT use to infer current product features.
For the current product architecture see docs/product_architecture.md.
```

Existing per-topic baseline notes on 48 kHz / 400 MHz / two-voice material were
retained.

## 3. Target builds

Both builds use the frozen DSP configuration (44.1 kHz, block 64, -O2,
fast-math off). Only `CONFIG_VOXLINK_ENABLE_SERVER` differs.

| | VoxLink OFF | VoxLink ON |
|---|---|---|
| Build | PASS | PASS |
| `vox_p4.bin` | 0x9A370 (631,344 B) | 0x9F250 (651,856 B) |
| Flash total | 551,350 B | 558,162 B |
| DIRAM used | 503,911 B (87.41%) | 517,687 B (89.80%) |
| DIRAM remaining | 72,553 B | 58,777 B |
| `.data` | 9,365 B | 22,525 B |

Enabled deltas (target output, not `sizeof`):

```text
DIRAM  +13,776 B   (Session 13,216 + ProductState 400 + overhead)
Flash  +6,812 B    (.text +1,876, .rodata +4,936)
.bin   +20,512 B
```

The +13,776 B DIRAM delta matches the host `sizeof(Session)+sizeof(ProductState)`
exactly, confirming the static footprint is the control objects and nothing
else. No DSP object size changed.

Full comparison: `docs/milestones/control_plane_v11_memory_comparison.txt`.
Build identity: `docs/milestones/control_plane_v11_device_reference.txt`.

## 4. UART pins — pending board schematic

The WT9932P4-TINY audio header uses GPIO19–23 for I2S and reserves GPIO37/38
for console/JTAG. This repository contains no free-header mapping, so no final
VoxLink pin can be chosen without inventing availability. Therefore:

* `CONFIG_VOXLINK_UART_TX_GPIO` / `_RX_GPIO` default to `-1` (unassigned);
* `voxlink_service_start()` logs a warning and refuses to start while either pin
  is negative, so an accidentally enabled build cannot bind an audio pin.

Assign the pins only after checking the schematic. The CYD side currently uses
its own GPIO18 (TX) / GPIO19 (RX) (`CYD_Config.h`), which is expected to be
crossed to the P4 (`P4 TX → CYD RX`, `P4 RX → CYD TX`) once the P4 pins are
fixed.

## 5. Resource / memory gates

| Gate | Status |
|---|---|
| Enabled firmware builds | PASS |
| Static DIRAM within capacity | PASS (58,777 B remaining) |
| No DSP object growth | PASS |
| Runtime free heap / largest block | PENDING HARDWARE |
| Task stack HWMs (audio, pitch, VoxLink) | PENDING HARDWARE |
| UART driver buffers (4 KiB RX + 8 KiB TX) | allocated at start, PENDING HARDWARE |

No memory was migrated to PSRAM. The static control buffers are small; the DIRAM
pressure source is the frozen DSP. If runtime measurement shows problems, the
TX ring (8 KiB) and parser queue are the first candidates; audio/DMA/DSP memory
must not move.

## 6. Companion repo audit (`ovelhaaa/voxP4-control`)

Read-only audit of `main` (cloned shallow, not modified, not pushed). The CYD
project has **not implemented VoxLink yet**: `src/main.cpp` has
`// voxlink_process();` commented out and `protocol/VoxLink.cpp` is listed as
future work. Its `SPECS.md` §15–17 is a pre-implementation draft.

Contract mismatches between the draft `SPECS.md` and the implemented VoxLink v1
(these must be resolved on the CYD side; the P4 implementation and
`docs/voxlink_v1.md` are authoritative):

| Item | CYD draft `SPECS.md` | Implemented VoxLink v1 | Action |
|---|---|---|---|
| SEQ width | `uint16` | `uint8` | CYD adopts v1 |
| Header size | 9 B implied | 8 B | CYD adopts v1 |
| CRC | "CRC16" unspecified | CCITT-FALSE locked, check 0x29B1 | CYD adopts v1 |
| GET_STATE | `0x10` | `0x07` | CYD adopts v1 |
| SET_PARAM | `0x12` | `0x0D` | CYD adopts v1 |
| PARAM_CHANGED | `0x13` | `0x0E` | CYD adopts v1 |
| STATE snapshot | `0x11` single | `0x08/0x09/0x0A` begin/param/end | CYD adopts v1 |
| CAPS_RESPONSE | `0x04` single | `0x04/0x05/0x06` begin/param/end | CYD adopts v1 |
| ACTION | `0x20` | `0x0F` (reserved) | CYD adopts v1 |
| METER/PITCH/DSP | `0x40/0x41/0x42` | `0x61/0x62/0x63` | CYD adopts v1 |
| HEARTBEAT/ACK/NACK/ERROR | `0x70/0x71/0x72/0x73` | `0x60/0x10/0x11/0x12` | CYD adopts v1 |
| Reverb IDs | `0x02xx` | `0x04xx` (dynamics at `0x02xx`) | CYD adopts v1 |
| Delay IDs | none | `0x03xx` | CYD adopts v1 |
| Limiter IDs | `0x03xx` | `0x05xx` output | CYD adopts v1 |
| GET_PARAM/PARAM_VALUE | absent | `0x0B/0x0C` | CYD adds |
| Heartbeat rate | 2 Hz expected | 1 Hz emitted | align (either side) |

Matching items: SOF `0xA5 0x5A`, max payload 512 B, version major 1, baud
921600 / fallback 460800, numeric stable parameter IDs, HELLO/HELLO_ACK intent,
ACK/NACK/ERROR intent.

Resolution: replace `SPECS.md` §15–17 with `docs/voxlink_v1.md` and copy the
exported artifacts (§7). No companion-repo files were modified or pushed.

## 7. Exported integration artifacts

```text
docs/voxlink_cyd_integration.md     client-facing contract
docs/voxlink_v1.md                  normative protocol
integration/VoxP4ParamIds.h         45 IDs + count (generated)
integration/voxlink_params.json     full schema (generated)
integration/voxlink_v1_vectors.h    golden frames for CYD CRC/parser tests
tools/export_voxlink_schema.py      regenerates the two generated files
integration/README.md               copy instructions
```

`tools/export_voxlink_schema.py` parses the live registry source, so the exports
cannot drift from the P4 ABI.

## 8. Mandatory final questions

1. **Every VoxLink payload exact/normative?** Yes — `docs/voxlink_v1.md` §6; offsets, widths and no-padding rule stated.
2. **Is DSP_STATUS unambiguous?** Yes — one 40-byte sequential layout, matching the encoder and a golden vector.
3. **Docs match encoder/decoder?** Yes — `voxlink_protocol_conformance_tests` encodes all 19 messages and compares against shared vectors.
4. **What does PARAM_CHANGED confirm?** Acceptance into the authoritative realtime command stream and ProductState; `ACCEPTED != SAMPLE-EXACT APPLIED`.
5. **Does a no-op SET_PARAM increment revision?** No; ACK + PARAM_CHANGED are still returned. Tested.
6. **ACK/PARAM_CHANGED correlation?** Both echo the request SEQ; tested.
7. **ESP32-P4 builds with VoxLink enabled?** Yes (v5.3, esp32p4, -O2).
8. **Exact DIRAM delta?** +13,776 B (503,911 → 517,687).
9. **Internal free heap after boot (ON)?** PENDING HARDWARE.
10. **Largest internal block?** PENDING HARDWARE.
11. **Audio/pitch/VoxLink task stack HWMs?** PENDING HARDWARE.
12. **Final UART GPIOs?** Unassigned/-1 pending schematic; refuse-to-start guard in place.
13. **Is 921600 reliable on hardware?** PENDING HARDWARE.
14. **CLI HELLO/CAPS/STATE successfully?** Protocol and host session verified; physical run PENDING HARDWARE.
15. **Exactly 45 parameters?** Yes.
16. **GET_PARAM works?** Yes on host for all registered IDs; physical PENDING HARDWARE.
17. **SET_PARAM reaches the DSP path?** Bridge is `voxlink_submit → vocal_fx_try_set_parameter → ParameterQueue<64>`; verified on host, physical PENDING HARDWARE.
18. **Normalized/clamped values confirmed?** Yes — clamp tested (reverb.wet 2.0 → 1.0).
19. **QUEUE_FULL preserves state?** Yes — NACK, counters, state/revision unchanged; tested.
20. **Malformed traffic recovers without reboot?** Yes — framing + 2 MB fuzz tests.
21. **Client disconnect leaves audio unchanged?** Design: heartbeat timeout only changes link state; physical PENDING HARDWARE.
22. **Reconnect gets fresh authoritative state?** Yes — HELLO→CAPS→GET_STATE on host; physical PENDING HARDWARE.
23. **Normal VoxLink traffic worsens DSP timing?** PENDING HARDWARE.
24. **Worsens pitch backlog?** PENDING HARDWARE.
25. **Interval stress comparable to B4D.12?** PENDING HARDWARE.
26. **Effect-toggle stress transport perfect?** PENDING HARDWARE.
27. **All audio transport counters zero?** PENDING HARDWARE.
28. **Backlog bounded?** PENDING HARDWARE.
29. **New consecutive-late pathology?** PENDING HARDWARE.
30. **Contract consumable by CYD without reading P4 source?** Yes — `docs/voxlink_cyd_integration.md` + `docs/voxlink_v1.md` + `integration/` artifacts.

## 9. Artifacts

```text
docs/voxlink_v1.md                                 (corrected normative spec)
docs/voxlink_cyd_integration.md
docs/product_architecture.md                       (pin note updated)
docs/milestones/control_plane_v11_report.md
docs/milestones/control_plane_v11_device_reference.txt
docs/milestones/control_plane_v11_memory_comparison.txt
docs/milestones/control_plane_v11_host_tests.txt
docs/milestones/control_plane_v11_target_matrix.csv (rows NOT_RUN_HW)
integration/VoxP4ParamIds.h
integration/voxlink_params.json
integration/voxlink_v1_vectors.h
integration/README.md
tools/export_voxlink_schema.py
tests/data/voxlink_v1_vectors.h                    (extended)
tests/test_voxlink_conformance.cpp
```

Physical-run artifacts (`control_plane_v11_cli_session.txt`,
`..._normal_control_raw.txt`, `..._interval_stress_raw.txt`,
`..._bypass_stress_raw.txt`, `..._soak_raw.txt`) are not produced; they require
hardware.

## 10. Decision

```text
OUTCOME B — HOST / BUILD COMPLETE, HARDWARE PENDING
```
