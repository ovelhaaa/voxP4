# Integrated Milestone 4 validation

Validated at commit `9778cab6cd76ba8b711aea19b647cb12926ee4c2` after the
Milestone 4 rebase. The working tree was clean. The pinned environment reported
ESP-IDF v5.3 and `riscv32-esp-elf-gcc` 13.2.0. The generated `sdkconfig` contains
`CONFIG_IDF_TARGET="esp32p4"` and `CONFIG_IDF_TARGET_ESP32P4=y`.

## Build and memory

The real ESP32-P4 build completed without component compiler warnings. ESP-IDF
printed its existing informational duplicate-`IDF_TARGET_LINUX` Kconfig message
and environment activation warning; neither originated in `vocal_fx`.

| Configuration | Image size | DIRAM used | DIRAM free | DRAM/BSS | IRAM | libvocal_fx RAM |
|---|---:|---:|---:|---:|---:|---:|
| Supplied baseline | ~261,824 B | ~199,659 B | ~376,805 B | not supplied | not supplied | ~128,804 B |
| Dual harmony | 272,800 B `.bin` (272,451 B ELF image) | 348,683 B | 227,781 B | 283,692 B BSS | 57,802 B text | 277,748 B |
| Delta | +10,976 B `.bin` | +149,024 B | -149,024 B | n/a | n/a | +148,944 B |

The memory gate passes: 227,781 bytes (222.4 KiB) of DIRAM remain, above the
150 KiB threshold.

The largest application-owned symbol is the statically allocated `Engine` at
267,976 bytes. Target DWARF reports each `TdPsola` at 34,616 bytes, including
32,768 bytes for its private 4,096-float OLA and normalization rings. Therefore
the second voice adds 34,616 static bytes. `SharedPitchShiftResources` contains
one 65,536-byte audio ring, one 8,192-byte Hann LUT, and its cursor (73,736 bytes
total by layout). `PitchAnalysis` separately owns its analysis audio history,
FIFO, YIN working arrays, and the single 64-entry pitch-mark ring. `HarmonyEngine`
is 68 bytes. The target object sizes for `FdnReverb` and `StereoDelay` are 292
and 84 bytes respectively; their long buffers are initialized heap storage and
are not hidden by this milestone.

Both voices store only a pointer to the same `SharedPitchShiftResources` object.
The audio pipeline contains one shared object, one pitch-mark snapshot pair, and
an array of two `TdPsola` voice states. Thus voice 2 does not duplicate synthesis
audio history, pitch-mark history, or the Hann LUT.

## Firmware feature validation

The component build includes `shift/td_psola.cpp`, `harmony/scale.cpp`,
`harmony/midi_harmony.cpp`, and `harmony/harmony_engine.cpp`. The linked ELF
contains `TdPsola::process_shared`, `HarmonyEngine::update`, and
`HarmonyEngine::target_for`. The engine declares exactly two pitch-shift voices,
and all three modes (`FixedInterval`, `Diatonic`, and `MidiChord`) are compiled.

## Host validation

All seven CTest entries passed. The existing five-minute (`300` second)
`psola_benchmark` completed with 0.007% fallback, zero pitch-mark underflows,
zero audio-history underflows, and one maximum grain per block. This benchmark
exercises the existing isolated PSOLA path; the deterministic PSOLA suite also
exercises two independent voice timelines against a shared wrapping history.

## Conclusion

**READY FOR LPC/FORMANT MILESTONE**
