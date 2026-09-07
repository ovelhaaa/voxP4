# Benchmarking

`specs.md` is the source of truth. At 48 kHz the block deadline is
`frames / 48000`: 1.333 ms (64), 2.667 ms (128), or 5.333 ms (256).

## ESP32-P4 procedure

Build with `-DVOCAL_FX_ENABLE_PROFILING=1`. Profiling records timestamps in the
audio task without logging; a low-priority telemetry task should periodically
copy and print the counters. Measure a release build for at least 10 minutes,
including silence, impulses, full-scale noise, and parameter changes. Report
average/max microseconds, convert cycles using the measured CPU clock, and count
callbacks exceeding the calculated deadline. Confirm I2S underruns separately.
The audio-side counters are copied into a coherent seqlock snapshot composed of
32-bit atomics, preventing torn 64-bit telemetry reads on ESP32-P4.

At boot, report `vocal_fx_dsp_memory_bytes()`, DMA allocation, internal heap via
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`, and PSRAM via
`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`. Delay and FDN expose their owned
buffer sizes. Measure round-trip latency by recording an output impulse returned
through the codec; calculated block/DMA latency is not a substitute.

Generate a five-second IEEE-float stereo impulse response with:

```sh
cmake -S tests -B build-host && cmake --build build-host
(cd build-host && ./fdn_ir)
```

Inspect peak, RMS decay versus time, spectrogram, late-tail density, and periodic
patterns in `build-host/fdn_impulse.wav`.

| Module | avg cycles | max cycles | % block budget |
|---|---:|---:|---:|
| HPF | TBD | TBD | TBD |
| Gate | TBD | TBD | TBD |
| Compressor | TBD | TBD | TBD |
| Delay | TBD | TBD | TBD |
| FDN | TBD | TBD | TBD |
| Limiter | TBD | TBD | TBD |
| Full pipeline | TBD | TBD | TBD |

Host timings are regression indicators only; the `<50%` milestone target must be
validated on ESP32-P4 hardware with the real codec and DMA configuration.

Pitch analysis has its own per-stage counters and host driver:

```sh
(cd build-host && ./pitch_benchmark)
```

See [`pitch_analysis.md`](pitch_analysis.md) for timestamp semantics, the
filled host-quality table, analysis memory, and the target benchmark table.

## TD-PSOLA (Milestone 3)

Run the bounded renderer alone and inside the enabled FX graph with:

```sh
build-host/psola_benchmark 60
build-host/psola_benchmark 60 full
```

September 2026 x86 host observations for a 220 Hz two-harmonic signal, +4 st,
64-sample blocks are below. These are regression measurements, **not ESP32-P4
estimates**. Stage averages have different denominators: grain stages are per
grain and sample/block stages are per block.

| Stage | P4 avg cycles | P4 max cycles | Host avg us |
|---|---:|---:|---:|
| Source mark lookup | TBD hardware | TBD hardware | 0.50/grain |
| Grain preparation | TBD hardware | TBD hardware | 0.17/grain |
| Window + OLA | TBD hardware | TBD hardware | 11.54/grain |
| Normalization | TBD hardware | TBD hardware | 1.73/block |
| Historical fallback | TBD hardware | TBD hardware | 0.08/block |
| Crossfade | TBD hardware | TBD hardware | 2.41/block |
| Pitch shift total | TBD hardware | TBD hardware | 10.50/block |
| Full FX chain + synchronous host analysis | TBD hardware | TBD hardware | 235.0 wall/block |

The 60-second isolated run measured 169.4 us wall time/block including
synchronous host YIN, 11.16 profiled audio-thread pitch-shift us/block, 0.369
grains/block average, one maximum grain/block, 0 mark/history underflows after
acquisition, and 0.122% startup fallback samples. The configured hard bound is
eight grains/block. Total reported DSP-owned memory was 1,059,876 bytes with all
existing delay/reverb/analysis storage; `TdPsola` contributes approximately
104 KiB. P4 internal-RAM placement and release-build cycles remain mandatory
hardware measurements before declaring the target CPU budget met.
