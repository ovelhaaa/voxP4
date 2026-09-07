# Milestone 5: LPC formant preservation

## Motivation and A/B path

TD-PSOLA remains the default and is not replaced. Each of the two harmony
voices independently selects `Off` or `Lpc` and an amount from 0 to 1. The host
tool accepts `--formants off|lpc`, `--formant-amount`, and `--lpc-order`, so the
same input can be rendered both ways.

## Model and real-time architecture

The initial float32 model uses order 16, a 1024-sample Hann window (21.33 ms at
48 kHz), a 384-sample hop (8 ms), and 0.97 pre-emphasis. Order is configurable
from 1 through 20; host tests exercise 10, 12, 16, and 20. Autocorrelation is
computed directly without allocation, followed by Levinson-Durbin. A small
diagonal load handles highly periodic frames. Zero energy, prediction error at
epsilon, non-finite values, or a reflection coefficient at the unit circle
invalidates the frame.

The audio producer feeds one SPSC analysis FIFO. The lower-priority analysis
runner (Core 1 in firmware) performs the single LPC analysis alongside YIN and
publishes 16 timestamped models through per-slot sequence locks. Both Core 0
TD-PSOLA voices query that same ring using the source-grain timestamp—not the
output time—and never wait for a writer. A model farther than one window plus
one hop is rejected. This costs no per-voice waveform history.

## Residual and synthesis

The coefficient convention is `A(z)=1+a1 z^-1+...`. Grain preparation applies
the inverse FIR `e[n]=x[n]+sum(a[k]x[n-k])` directly from shared source history;
TD-PSOLA operates on that residual. A continuous per-voice all-pole synthesis
state then applies `x[n]=e[n]-sum(a[k]x[n-k])`. State is not reset between
compatible LPC grains. The normal and complete LPC round-trip OLA paths remain
separate until output, where confidence and the user amount control a smoothed
blend. Thus amounts approaching zero converge continuously to normal PSOLA.
Invalid, stale,
unvoiced, or low-confidence models leave the established waveform PSOLA path
unchanged and clear the obsolete synthesis state. Non-finite/burst output also
clears filter state and falls back safely.

Direct LPC coefficient interpolation can theoretically cross an unstable
region. This first implementation instead retains the selected stable model
and smooths the wet correction. LSF interpolation remains a future option.
Autocorrelation and inverse/synthesis FIR loops are marked as future P4 Xai
candidates; no target-specific assembly is justified before target profiling.

## Validation and limitations

The deterministic test verifies finite stable solutions at orders 10/12/16/20,
silence rejection, timestamp selection, and analysis-filter plus synthesis-filter
reconstruction. At ratio 1 the isolated filter cascade has relative RMS error
below `1e-4`. Existing pitch, PSOLA, harmony, dynamics, delay, and reverb tests
remain the regression gate.

No real-vocal WAV corpus is present in this repository. Consequently listening,
synthetic vowel peak-distance comparisons, vibrato/glissando envelope metrics,
and subjective artifact rankings remain pending; no quality preference is
claimed. The offline tool is ready to produce those comparisons from supplied
48-kHz PCM16 or float32 material.

| Shift | PSOLA only | PSOLA+LPC | Preferred |
|---:|---|---|---|
| +3 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |
| +4 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |
| +7 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |
| -3 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |
| -4 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |
| -7 | Not subjectively evaluated | Not subjectively evaluated | Not subjectively evaluated |

## CPU and memory

Profiling exposes LPC total analysis plus reserved windowing, autocorrelation,
Levinson-Durbin, and publication sections. TD-PSOLA's grain/window and total
sections include inverse filtering and per-voice synthesis application. Host
timings are machine-dependent and must not be extrapolated to the P4.

The pre-milestone figures below are the recorded baseline. Post-build numbers
come from the pinned-IDF size reports; the added state is one shared FIFO/frame/
16-model ring and 20 synthesis samples per voice.

| Metric | Pre-LPC | LPC | Delta |
|---|---:|---:|---:|
| Firmware `.bin` | ~272,800 B | 279,024 B | +6,224 B |
| DIRAM used | ~348,683 B | 408,203 B | +59,520 B |
| DIRAM free | ~227,781 B | 168,261 B | -59,520 B |
| DRAM/BSS | ~283,692 B | 338,316 B | +54,624 B |
| IRAM | ~57,802 B | 57,802 B | 0 B |
| `libvocal_fx` static RAM | ~277,748 B | 337,268 B | +59,520 B |
| largest symbol | global Engine ~267,976 B | 327,496 B | +59,520 B |

The DIRAM gate passes (168,261 B free, above 150 KiB), although the increment is
above the desired 40 KiB. Correct partial blending requires retaining both the
normal and residual OLA streams (16 KiB per voice). The LPC FIFO uses compact
float/32-bit-position entries, saving 16 KiB versus the pitch analyser's aligned
64-bit sample records without shrinking source history or adding analysis per
voice.

## Decision gate

**C. MORE REAL-VOCAL EVALUATION REQUIRED.** The numerical core and selectable
shared architecture are available, but a benefit cannot responsibly be claimed
without the missing real-vocal listening corpus and envelope measurements.
