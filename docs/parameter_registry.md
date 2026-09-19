# VoxP4 parameter registry

The single authoritative registry for every externally controllable product
parameter. The machine-readable source is
`components/voxlink/include/voxlink_registry.h`; this document is generated
from the same inventory and is the product ABI reference.

## Rules

* Every externally controllable value is described exactly once.
* IDs are protocol ABI and never change casually once VoxLink v1 ships.
* The registry never exposes implementation accidents: no cache indices, no
  grain counters, no LPC scratch controls, no scheduler internals, no profiling
  switches. Those remain internal.
* Numeric values are clamped to `[min,max]`; enum values outside range are
  rejected (`INVALID_ENUM`); `NaN`/`Inf` are rejected (`MALFORMED`); a wire tag
  that does not match the descriptor type is rejected (`BAD_TYPE`).

Flags: `P` persistent, `RT` realtime-safe, `S` smoothed, `D` discrete.

## ID namespace

```text
0x0000–0x00FF  system
0x0100–0x01FF  harmony
0x0200–0x02FF  dynamics (gate + compressor)
0x0300–0x03FF  delay
0x0400–0x04FF  reverb
0x0500–0x05FF  output/master
0x0600–0x06FF  pitch correction (reserved, not implemented)
0x0700–0x07FF  doubler (reserved, not implemented)
0x0F00–0x0FFF  global/product (reserved; GlobalBypass is NOT implemented)
```

## Registry table

| ID | Key | Display | Type | Min | Max | Default | Step | Unit | Flags | Smooth (ms) | DSP binding |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0x0100 | harmony.enable | Harmony Enable | bool | 0 | 1 | 0 | 1 | | P RT | 0 | `PitchShiftEnabled` |
| 0x0101 | harmony.interval | Harmony Interval | int | -12 | 12 | 0 | 1 | semitones | P RT D | 0 | `PitchShiftSemitones` |
| 0x0102 | harmony.level | Harmony Level | float | 0 | 1 | 1 | 0.001 | | P RT S | 30 | `PitchShiftWet` |
| 0x0103 | harmony.mode | Harmony Mode | enum | 0 | 2 | 0 | 1 | | P RT D | 0 | `HarmonyMode` |
| 0x0104 | harmony.key | Harmony Key | enum | 0 | 11 | 0 | 1 | | P RT D | 0 | `HarmonyKey` |
| 0x0105 | harmony.scale | Harmony Scale | enum | 0 | 11 | 0 | 1 | | P RT D | 0 | `HarmonyScale` |
| 0x0107 | harmony.voice1.pan | Harmony Voice Pan | float | -1 | 1 | 0 | 0.01 | | P RT S | 30 | `HarmonyVoice1Pan` |
| 0x0108 | harmony.voice1.degree | Harmony Voice Degree | int | -7 | 7 | 0 | 1 | degrees | P RT D | 0 | `HarmonyVoice1Degree` |
| 0x0109 | harmony.voice1.smoothing_ms | Harmony Smoothing | float | 1 | 500 | 30 | 1 | ms | P RT S | 0 | `HarmonyVoice1Smoothing` |
| 0x010A | harmony.formant.enable | Formant Preservation | bool | 0 | 1 | 0 | 1 | | P RT | 0 | `FormantVoice1Mode` |
| 0x010B | harmony.formant.amount | Formant Amount | float | 0 | 1 | 1 | 0.01 | | P RT S | 20 | `FormantVoice1Amount` |
| 0x010C | harmony.attack_ms | Harmony Attack | float | 0.1 | 100 | 4 | 0.1 | ms | P RT S | 0 | `HarmonyAttackMs` |
| 0x010D | harmony.release_ms | Harmony Release | float | 1 | 500 | 20 | 1 | ms | P RT S | 0 | `HarmonyReleaseMs` |
| 0x010E | harmony.limiter.enable | Harmony Limiter | bool | 0 | 1 | 1 | 1 | | P RT | 0 | `HarmonyLimiterEnabled` |
| 0x010F | harmony.limiter.threshold_db | Harmony Limiter Threshold | float | -24 | 0 | -3 | 0.5 | dB | P RT S | 0 | `HarmonyLimiterThresholdDb` |
| 0x0110 | harmony.dry_alignment.enable | Dry Alignment | bool | 0 | 1 | 0 | 1 | | P RT | 0 | `DryAlignmentEnabled` |
| 0x0111 | harmony.dry_alignment.ms | Dry Alignment Delay | float | 0 | 120 | 32 | 1 | ms | P RT S | 0 | `DryAlignmentMs` |
| 0x0112 | harmony.voice1.non_scale_policy | Harmony Non-Scale Policy | enum | 0 | 2 | 0 | 1 | | P RT D | 0 | `HarmonyVoice1NonScalePolicy` |
| 0x0113 | harmony.voice1.voice_leading | Harmony Voice Leading | bool | 0 | 1 | 0 | 1 | | P RT | 0 | `HarmonyVoice1VoiceLeadingEnabled` |
| 0x0114 | harmony.voice1.min_midi | Harmony Min MIDI Note | float | 0 | 127 | 0 | 1 | | P RT | 0 | `HarmonyVoice1MinMidi` |
| 0x0115 | harmony.voice1.max_midi | Harmony Max MIDI Note | float | 0 | 127 | 127 | 1 | | P RT | 0 | `HarmonyVoice1MaxMidi` |
| 0x0200 | compressor.enable | Compressor Enable | bool | 0 | 1 | 1 | 1 | | P RT | 0 | `EnableCompressor` |
| 0x0201 | compressor.threshold_db | Compressor Threshold | float | -60 | 0 | -18 | 0.5 | dB | P RT S | 0 | `CompressorThresholdDb` |
| 0x0202 | compressor.ratio | Compressor Ratio | float | 1 | 20 | 3 | 0.1 | :1 | P RT S | 0 | `CompressorRatio` |
| 0x0203 | compressor.attack_ms | Compressor Attack | float | 0.1 | 200 | 10 | 0.1 | ms | P RT S | 0 | `CompressorAttackMs` |
| 0x0204 | compressor.release_ms | Compressor Release | float | 1 | 1000 | 100 | 1 | ms | P RT S | 0 | `CompressorReleaseMs` |
| 0x0205 | compressor.makeup_db | Compressor Makeup | float | 0 | 24 | 3 | 0.5 | dB | P RT S | 0 | `CompressorMakeupDb` |
| 0x0206 | compressor.knee_db | Compressor Knee | float | 0 | 24 | 6 | 0.5 | dB | P RT S | 0 | `CompressorKneeDb` |
| 0x0207 | gate.enable | Gate Enable | bool | 0 | 1 | 1 | 1 | | P RT | 0 | `EnableGate` |
| 0x0208 | gate.threshold_db | Gate Threshold | float | -80 | 0 | -55 | 1 | dB | P RT S | 0 | `GateThresholdDb` |
| 0x0209 | gate.attack_ms | Gate Attack | float | 0.1 | 100 | 5 | 0.1 | ms | P RT S | 0 | `GateAttackMs` |
| 0x020A | gate.hold_ms | Gate Hold | float | 0 | 500 | 40 | 1 | ms | P RT S | 0 | `GateHoldMs` |
| 0x020B | gate.release_ms | Gate Release | float | 1 | 2000 | 120 | 1 | ms | P RT S | 0 | `GateReleaseMs` |
| 0x020C | gate.range_db | Gate Range | float | -80 | 0 | -60 | 1 | dB | P RT S | 0 | `GateRangeDb` |
| 0x0300 | delay.enable | Delay Enable | bool | 0 | 1 | 1 | 1 | | P RT | 0 | `EnableDelay` |
| 0x0301 | delay.left_ms | Delay Left | float | 0 | 2000 | 250 | 1 | ms | P RT S | 0 | `DelayLeftMs` |
| 0x0302 | delay.right_ms | Delay Right | float | 0 | 2000 | 375 | 1 | ms | P RT S | 0 | `DelayRightMs` |
| 0x0303 | delay.feedback | Delay Feedback | float | -0.95 | 0.95 | 0.25 | 0.01 | | P RT S | 0 | `DelayFeedback` |
| 0x0304 | delay.wet | Delay Wet | float | 0 | 1 | 0.2 | 0.001 | | P RT S | 20 | `DelayWet` |
| 0x0305 | delay.dry | Delay Dry | float | 0 | 1 | 1 | 0.001 | | P RT S | 20 | `DelayDry` |
| 0x0306 | delay.feedback_lowpass_hz | Delay Feedback Lowpass | float | 200 | 20000 | 6000 | 1 | Hz | P RT S | 0 | `DelayFeedbackLowpassHz` |
| 0x0400 | reverb.enable | Reverb Enable | bool | 0 | 1 | 1 | 1 | | P RT | 0 | `EnableReverb` |
| 0x0401 | reverb.wet | Reverb Wet | float | 0 | 1 | 0.18 | 0.001 | | P RT S | 30 | `ReverbWet` |
| 0x0402 | reverb.decay_s | Reverb Decay | float | 0.15 | 20 | 2 | 0.01 | s | P RT S | 0 | `ReverbDecaySeconds` |
| 0x0403 | reverb.damping | Reverb Damping | float | 0 | 1 | 0.45 | 0.01 | | P RT S | 0 | `ReverbDamping` |
| 0x0500 | limiter.ceiling | Limiter Ceiling | float | 0.1 | 1 | 0.95 | 0.01 | | P RT S | 0 | `LimiterCeiling` |
| 0x0501 | output.mute_dry | Mute Dry | bool | 0 | 1 | 0 | 1 | | P RT | 0 | `MuteDry` |
| 0x0502 | output.spatial_routing | Spatial Routing | enum | 0 | 1 | 1 | 1 | | P RT D | 0 | `SpatialRouting` |
| 0x0503 | output.spatial_source | Spatial Source | enum | 0 | 2 | 0 | 1 | | P RT D | 0 | `SpatialSource` |

Parameter count: **49**, generated from `components/voxlink/src/voxlink_registry.cpp`
by `tools/export_voxlink_schema.py` (also emitted to `integration/`).

## Smoothing policy

The CYD never performs audio smoothing. Smoothing categories declared by the
registry are informational for this milestone; the engine already smooths
internally where it was designed to (delay dry/wet interpolation, gain
smoothing, reverb wet, spatial routing send). `S` marks parameters with existing
engine-side smoothing. `harmony.interval` is intentionally discrete
(integer semitones) and must not be treated as a continuously interpolated gain.

Where the registry shows `smooth = 0` for a continuous parameter, the engine
applies its existing per-module behaviour and no new smoothing was introduced.
Do not rewrite DSP algorithms to standardise smoothing in this milestone.

## Preset preparation

`PARAM_PERSISTENT` marks the parameters a future preset system should store.
Telemetry and read-only values are never persistent. Actions (`PRESET_NEXT`,
`TAP_TEMPO`, `GLOBAL_PANIC`, ...) are events and must not be modelled as
parameters.
