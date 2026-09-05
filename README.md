# voxP4 vocal effects firmware

ESP-IDF firmware foundation for a low-latency ESP32-P4 vocal processor. The
current milestone deliberately implements the measurable audio core—not pitch
detection or harmonization. [`specs.md`](specs.md) is the architectural source of
truth whenever this overview is incomplete.

## Current architecture

Mono I2S/DMA input → 80 Hz DF-II-transposed HPF → click-safe gate → soft-knee
compressor → interpolated stereo delay → three-allpass diffuser and normalized
8-line Hadamard FDN → safety limiter → stereo I2S/DMA output. Effects have cheap
bypasses and all buffers are allocated during initialization. The audio task does
not allocate, log, access files, or lock. An atomic seqlock mailbox exposes the
latest `PitchResult` without implementing a detector.

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

ESP-IDF **5.3 or newer** is expected because the component uses the standard-mode
channel I2S driver. Configure the target and board pins before running:

```sh
idf.py set-target esp32p4
idf.py build
idf.py flash monitor
```

## Host tests

```sh
cmake -S tests -B build-host
cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
(cd build-host && ./fdn_ir)
```

Tests cover all biquad modes, DC rejection, bypass, finite/stable output, gate,
compressor curve, smoothing monotonicity, circular delay wrap, normalized
Hadamard energy, FDN silence and a ten-second bounded impulse response. See
[`docs/benchmarking.md`](docs/benchmarking.md) for target profiling, memory,
latency, deadline, and impulse-response procedures.

## Next milestone

Add an analysis tap with ×4 downsampling, asynchronous YIN/MPM, voiced/unvoiced
classification, refined pitch marks, and TD-PSOLA. WSOLA, harmony voices, MIDI,
formants, correction, and vocoder remain intentionally unimplemented.
