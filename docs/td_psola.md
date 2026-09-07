# Milestone 3: monophonic TD-PSOLA pitch shifting

## Signal path and real-time contract

The audio task writes post-input-strip mono samples into a 16,384-sample
(341.3 ms at 48 kHz) power-of-two history, renders one TD-PSOLA voice, mixes its
wet/dry output, and then enters delay/reverb. Analysis remains on Core 1. The
synthesizer consumes only the latest atomic `PitchResult`, tracker state, and a
bounded snapshot of at most 64 real pitch marks; it never runs YIN, waits, takes
a mutex, or allocates. `MAX_GRAINS_PER_BLOCK` is eight and backlog recovery
falls back rather than looping.

```text
input position E -------- analysis centre E-1039 (- scheduler age)
       |                  source position D-1536
       |                         |
       +--> history: ... A B C [source mark] ... E
output position D --> synthesis marks a--b--c --> OLA + normalization
```

Every index above is an absolute 64-bit input/output stream position. For output
position `D`, historical passthrough reads `D-1536`. A synthesis mark at `S`
selects the real analysis mark nearest `S-1536`. The fixed 1,536-sample source
offset is 32.0 ms. The 1,039-sample fixed detector age and nominal 0–240-sample
scheduler age leave causal margin; explicit mark/range checks reject unavailable
future or overwritten data. Grain scheduling only looks ahead by the current
period. The wet-path reported latency is therefore **1,536 samples / 32 ms**;
I/O DMA buffering is separate, while analysis latency is a prerequisite rather
than an additional serial delay. The zero-latency dry contribution remains
available when wet is below one.

## Grains, timelines, and normalization

Source and synthesis timelines are deliberately independent. Source time follows
real time (`source = synthesis_position - 1536`) so duration does not stretch.
The nearest actual analysis mark supplies the grain centre and neighboring mark
differences supply its local period. Synthesis spacing is
`local_period / 2^(semitones/12)`. The target is clamped to ±12 semitones,
smoothed in semitone space (30 ms default), and converted once per grain—not per
sample. ±7 semitones is the currently optimized range.

Grains cover `[-P,+P]`, with half-length clamped to 24–800 samples (48–1,601
samples inclusive). A precomputed 2,048-point Hann LUT is resampled by integer
index; no runtime cosine is evaluated. Audio and window sums accumulate into
4,096-sample power-of-two OLA rings. Each emitted sample divides audio sum by
`max(window sum, epsilon)`, avoiding gain pumping as overlap changes. Integer
marks are used now; the double synthesis/source cursors permit later fractional
mark interpolation.

## Voiced, unvoiced, events, and state

`Bypass -> WaitingForAnalysis -> Acquiring -> Active` is explicit. Unlocked,
unvoiced, invalid, underflow, and onset conditions enter `Fallback`. Acquisition
and loss use a 20 ms linear PSOLA-gain ramp. The fallback is never current input:
it reads the exact 32 ms historical timeline, preserving temporal alignment.
An onset suppresses PSOLA for 12 ms; `pitch_changed` shortens that suppression
to at least 6 ms and records a soft resynchronization without clearing audible
output. Ending voice fades pending grains instead of repeating the last grain.
Disabled bypass still maintains history for hot enable but skips mark lookup,
grain and OLA work; after the 20 ms hot-disable fade it outputs current dry samples exactly.

The public thread-safe controls use the existing bounded SPSC parameter queue:
`vocal_fx_set_pitch_shift_enabled`, `vocal_fx_set_pitch_shift_semitones`, and
`vocal_fx_set_pitch_shift_mix`. `reset()` clears history, cursors, OLA/norm,
fades, events, state, profiling, and counters without allocation.

## Safety, profiling, and memory

Before grain reads, both ends are checked against `[input_end-history_size,
input_end)`. Mark selection rejects low-confidence, invalid-period, distant, and
insufficient snapshots. Failures produce aligned fallback and increment
`pitch_mark_underflows`, `audio_history_underflows`, `invalid_pitch`,
`invalid_mark`, `max_grains_exceeded`, `psola_resyncs`, and `fallback_frames` as
applicable. Per-stage profilers cover lookup, grain preparation, Hann/OLA,
normalization, fallback, crossfade, and total.

Static synthesis storage is about 104 KiB: 64 KiB history, 32 KiB OLA/norm,
8 KiB Hann LUT, plus small state. It is ordinary object storage intended for
internal RAM; no automatic PSRAM migration occurs.

## Host evaluation

```sh
cmake -S tests -B build-host && cmake --build build-host -j
ctest --test-dir build-host --output-on-failure
build-host/psola_benchmark 60
build-host/pitch_shift vocal.wav vocal_plus4.wav 4
for s in -7 -4 -3 3 4 7; do build-host/pitch_shift vocal.wav out_${s}.wav $s; done
```

The deterministic test uses sine plus harmonics for -7, -5, -4, -3, +3, +4,
+5 and +7, forces thousands of history wraps with silence, and exercises a
voiced/noise/voiced reacquisition and onset without non-finite output or click
bursts. Existing analysis tests retain vibrato, glissando and note-transition
coverage. Generated listening WAVs belong in ignored `tests/output/`.

## Known limitations / next work

No explicit formant preservation is present, so ±7 can sound smaller/larger.
Unvoiced audio is delay-aligned passthrough rather than WSOLA. Real-vocal corpus
listening, ESP32-P4 cycle measurements, internal-RAM placement verification,
fractional-mark interpolation, and output-domain vibrato/glissando scoring remain
hardware/corpus TODOs. This milestone intentionally has no scale quantization,
MIDI harmony, correction, phase vocoder, LPC, or second voice.
