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

The codec-neutral I2S adapter uses 32-bit stereo slots and performs block PCM ↔
float conversion outside interrupts. Board pin/codec control remains a board
integration responsibility. Hot delay/FDN buffers use normal internal-capable
allocation in this milestone; their measured sizes are exposed for boot reports.

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
