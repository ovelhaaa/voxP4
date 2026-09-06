# Milestone 2: real-time pitch analysis

## Architecture and real-time contract

The 48 kHz audio callback writes the unmodified input into a bounded history
ring and a 31-tap FIR decimator. Decimated samples enter a fixed-size SPSC FIFO;
if the analysis core falls behind, the producer drops the newest analysis sample
rather than waiting. There are no allocations, locks, logging, or detector work
in `vocal_fx_process`. The lower-priority `vocal_pitch` task on Core 1 drains the
FIFO via `vocal_fx_run_pitch_analysis()` and publishes a coherent latest-state
snapshot. The dry/effects pipeline is unchanged and no synthesis is present.

Defaults are 48 kHz input, 12 kHz analysis, a 512-sample (42.67 ms) rolling
window, and a 60-sample (5 ms) hop. Integer decimation factors 2, 3 and 4 support
future 24, 16 and 12 kHz analysis at 48 kHz. Configuration validates rates,
frequency limits, window/hop, confidence ordering, detector, and counters.

## Decimator

The anti-alias filter is a 31-tap Hamming-windowed sinc with cutoff at 0.45 of
the output sample rate (5.4 kHz for 12 kHz output), an approximate 2.5 kHz
transition, and a constant 15-input-sample (0.3125 ms) group delay. The host test
measured an 11 kHz input at **-57.7 dB** relative to a 1 kHz pass-band sine. The
direct `out[n] = in[n*4]` aliasing shortcut is never used.

## YIN and confidence

`YinDetector` computes the O(N×tau) difference function, cumulative mean
normalized difference (CMND), first valley below the configurable 0.15
threshold, local-minimum descent, and three-point parabolic interpolation. Lags
are derived from `analysis_rate / max_frequency` and
`analysis_rate / min_frequency`; defaults cover 65–1000 Hz. Confidence is
`clamp(1 - CMND(candidate), 0, 1)`: zero means no useful periodic evidence and
one means highly periodic. Silence, DC-scale values and non-finite input are
bounded without division by zero, NaN, or infinity. MPM is represented in the
common algorithm enum but deliberately rejected at initialization until its
experimental backend exists.

## Voicing, continuity, and events

Voicing requires both window RMS above -55 dBFS and confidence. Entry uses 0.80
for two frames; retention uses 0.60 and release takes three bad frames. Onset is
a configurable short-term energy ratio (default 2.5) against the smoothed prior
energy. This classifier therefore does not equate a nonzero frequency with
voicing. Low-level or low-confidence YIN fallback candidates never update the
smoother; leaving the voiced state clears its pitch history so reacquisition
starts from the new reliable measurement rather than a silence-biased value.

Reliable pitch is retained in an eight-frame history. The tracker converts Hz
to cents relative to 440 Hz, rejects a ±1200-cent candidate when its octave is
materially closer to the short median and confidence is not near-perfect, then
uses a 30 ms one-pole smoother sampled once per hop. This reduces jitter while
the host vibrato test confirms that ±30-cent/5 Hz movement is not flattened.
Two consecutive changes beyond 75 cents set `pitch_changed`, preventing a
single-frame outlier from reporting a musical transition.

## Timestamps and latency

`analysis_timestamp_samples` (and its Milestone-1 compatibility alias
`timestamp_samples`) is always in the **original input sample domain** and marks
the **centre of the analysis window**. `period_samples` is also converted to the
original-rate domain. The explicitly reported fixed signal-age component is:

```text
analysis_latency_samples = FIR group delay + window_size/2 * input_rate/analysis_rate
                         = 15 + 256*4 = 1039 samples
                         = 21.646 ms at 48 kHz
```

Scheduling adds between zero and one hop (nominally 0–5 ms) depending on when
the analysis task runs; it is intentionally kept separate from the fixed value.

## Pitch marks

The tracker exposes `Unlocked`, `Acquiring`, and `Locked`. A first reliable mark
anchors at the result timestamp. Each subsequent mark is predicted one smoothed
period later and searched within ±20% of the period. Normalized correlation of
the preceding half-cycle chooses the local candidate. Every elapsed period is
emitted up to the current analysis timestamp, including multiple marks per hop
above the 200 Hz hop rate. Confidence combines pitch
confidence (50%), correlation (35%), and prediction distance (15%). Three
coherent marks lock tracking; unvoiced input, low confidence, onset, or three
failed searches unlock it. A seqlock-protected 64-entry ring supports latest
and bounded range queries without allocation. Correlation and YIN difference
loops are marked as future ESP32-P4 Xai/SIMD candidates.

## Host quality results

Clean-sine results from the host test (September 2026 build):

| Ground truth | Estimate | Absolute cents |
|---:|---:|---:|
| 65 Hz | 65.0406 Hz | 1.082 |
| 80 Hz | 79.9987 Hz | 0.028 |
| 100 Hz | 99.9989 Hz | 0.018 |
| 110 Hz | 109.9987 Hz | 0.021 |
| 220 Hz | 219.9971 Hz | 0.023 |
| 440 Hz | 440.0010 Hz | 0.004 |
| 880 Hz | 879.7226 Hz | 0.546 |
| 1000 Hz | 999.8810 Hz | 0.206 |

Mean absolute error is **0.241 cents** and the sample 95th percentile is 1.082
cents, with zero octave errors. Tests also pass for a weak fundamental/strong
second harmonic, missing fundamental (2F0–5F0), 20 dB synthetic white-noise SNR,
±30-cent 5 Hz vibrato, 110→220 Hz/second glissando, abrupt 220→330 Hz change,
voiced release, and coherent 220 Hz marks. The current deterministic suite does
not replace a labelled real-vocal corpus; stage-noise false-positive rates and
hardware tracking latency remain TODO measurements.

## Profiling and memory

Profiling uses timestamps without processing-time logging for decimation, YIN
difference, CMND, search, interpolation, voiced classification, smoothing,
mark correlation, and total analysis. `pitch_benchmark` prints average/max host
microseconds; firmware telemetry can convert target microseconds to cycles using
the measured P4 clock. The subsystem is 114,376 bytes (approximately 112 KiB)
of static/owned state (16,384-sample input history, 2,048-entry FIFO,
rolling/scratch/YIN arrays, marks and state). No memory is allocated after
initialization.

| ESP32-P4 @ 400 MHz | Avg cycles | Max cycles | Avg us | Max us |
|---|---:|---:|---:|---:|
| Decimator | TBD hardware | TBD hardware | TBD | TBD |
| YIN difference | TBD hardware | TBD hardware | TBD | TBD |
| YIN CMND | TBD hardware | TBD hardware | TBD | TBD |
| YIN search/interpolation | TBD hardware | TBD hardware | TBD | TBD |
| Voiced classifier | TBD hardware | TBD hardware | TBD | TBD |
| Pitch smoother | TBD hardware | TBD hardware | TBD | TBD |
| Pitch mark tracker | TBD hardware | TBD hardware | TBD | TBD |
| Total analysis | TBD hardware | TBD hardware | TBD | TBD |

The host regression currently uses 512/60 at 12 kHz. After emitting every pitch
period it measured 1.10 ms/hop (21.9% of the 5 ms hop) in a representative run,
including 624 us average for the YIN difference function and 392 us for pitch
mark search. Target CPU percentage and worst-case cycles must be filled from
ESP32-P4 release firmware; host timings are regression observations, not target
measurements.

## Offline CSV and next work

`pitch_analyze input.wav output.csv [pitch_marks.csv]` accepts mono or multichannel
PCM16/float32 WAV, downmixes to mono, and emits pitch/event rows plus optional
mark position/time/confidence rows. Future work is real-corpus calibration, MPM
A/B, target-cycle measurements, FIFO-overload telemetry exposure, and SIMD only
after target profiling. Milestone 3 can consume `vocal_fx_latest_pitch()`,
`vocal_fx_get_pitch_marks()`, and `vocal_fx_analysis_latency_samples()`; it must
not reinterpret timestamps or generate equidistant marks.
