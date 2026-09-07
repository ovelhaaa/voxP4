# voxP4 vocal effects firmware

ESP-IDF firmware foundation for a low-latency ESP32-P4 vocal processor. The
current milestone implements the measurable audio core, asynchronous vocal
analysis, and one fixed-interval monophonic TD-PSOLA pitch-shift voice.
[`specs.md`](specs.md) is the architectural source of
truth whenever this overview is incomplete.

## Current architecture

Mono I2S/DMA input → 80 Hz DF-II-transposed HPF → click-safe gate → soft-knee
compressor → interpolated stereo delay → three-allpass diffuser and normalized
8-line Hadamard FDN → safety limiter → stereo I2S/DMA output. Effects have cheap
bypasses and all buffers are allocated during initialization. The audio task does
not allocate, log, access files, or lock. An atomic seqlock mailbox exposes the
latest `PitchResult`. A 31-tap anti-alias FIR decimates the input tap to 12 kHz;
a Core-1 task runs rolling-window YIN, voiced hysteresis, cents-domain tracking,
octave suppression, onset detection, and correlation-refined pitch marks.
On Core 0, a 16,384-sample absolute-addressed history feeds bounded TD-PSOLA;
unvoiced and transient regions use the same 32 ms delayed history timeline.

The codec-neutral I2S adapter uses stereo slots matching the configured PCM width
and performs block PCM ↔ float conversion outside interrupts. Board pin/codec control remains a board
integration responsibility. Hot delay/FDN buffers use normal internal-capable
allocation in this milestone; their measured sizes are exposed for boot reports.
PCM16, left-aligned PCM24, and PCM32 slots have matching ESP-IDF transfer and
conversion paths. Mono input selects the left slot; stereo input is safely mixed
to the engine's mono bus.

Runtime parameter writes use a bounded, non-blocking SPSC queue. The audio task
drains it only at block boundaries, so control-core updates cannot race DSP state
or expose partial coefficient sets. Profiling similarly publishes coherent
cross-core snapshots through a 32-bit-atomic seqlock instead of sharing mutable
64-bit counters.

## Build for ESP32-P4

The project pins ESP-IDF in [`scripts/idf-version.sh`](scripts/idf-version.sh).
The existing CI requirement was retained because that stable release supports
ESP32-P4 and the standard-mode channel I2S driver. Set up and build from any
directory without relying on persistent shell exports:

```sh
./scripts/setup-codex.sh
./scripts/check-environment.sh
./scripts/build-p4.sh
```

`IDF_PATH` and `IDF_TOOLS_PATH` may override the defaults under `$HOME`. The
build runs `idf.py set-target esp32p4` only when no configured target exists;
an existing configuration for another target is rejected instead of being
silently destroyed. `sdkconfig.defaults` makes fresh configurations select P4.
To work interactively, use `source scripts/idf-env.sh`.

Every push and pull request also builds the pinned `esp32p4` target in
GitHub Actions. Successful runs retain the application, bootloader, partition
table, and flashing metadata as the `vox-p4-firmware` artifact for 14 days. The
workflow can also be started manually from the Actions tab.

## Host tests

```sh
cmake -S tests -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
(cd build-host && ./fdn_ir)
```

Tests cover all biquad modes, DC rejection, bypass, finite/stable output, gate,
compressor curve, smoothing monotonicity, circular delay wrap, normalized
Hadamard energy, FDN silence and a ten-second bounded impulse response. Pitch
tests cover clean tones from 65–1000 Hz, harmonics/missing fundamental, noise,
vibrato, glissando, note transitions, state hysteresis, pitch marks, numerical
edge cases, and stop-band alias rejection. `pitch_analyze` exports a PCM16 or
float32 WAV to CSV. `pitch_shift input.wav output.wav +4` renders a float32 WAV
for listening (48 kHz PCM16 and float32 inputs). See
[`docs/pitch_analysis.md`](docs/pitch_analysis.md),
[`docs/td_psola.md`](docs/td_psola.md), and
[`docs/benchmarking.md`](docs/benchmarking.md) for target profiling, memory,
latency, deadline, and impulse-response procedures.

The engine now provides two TD-PSOLA harmony voices with shared source history,
fixed intervals, major/natural-minor diatonic targets, preserved cents
deviation, hysteretic note identity, and lock-free MIDI chord targets. See
[`docs/harmony_engine.md`](docs/harmony_engine.md). The optimized quality range
is currently ±7 semitones; the API clamps synthesis to ±12. WSOLA, formant
preservation, correction, and vocoder remain intentionally unimplemented.
