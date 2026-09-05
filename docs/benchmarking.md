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
