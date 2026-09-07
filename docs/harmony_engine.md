# Milestone 4: two-voice harmony

## Architecture and musical model

`HarmonyEngine` is a control-rate decision layer, separate from pitch analysis,
TD-PSOLA synthesis, and mixing. It supports `FixedInterval`, `Diatonic`, and
`MidiChord`. Chromatic pitch classes use C=0 through B=11. Diatonic intervals
are signed, zero-based displacements: +2 is a third and +4 is a fifth. Major
and natural-minor scales are implemented, including negative degrees and octave
wrap.

Continuous detector pitch is split into a note identity and its cents
deviation. Diatonic targets transpose only the identity and add the original
deviation, preserving vibrato and expressive intonation. Identity changes only
after moving 65 cents from its current centre, preventing midpoint chatter.
Targets outside 60--1500 Hz are invalidated and faded rather than silently
octave-folded.

The MIDI state is 128 lock-free atomic bytes plus an atomic generation. Note
on/off never locks the audio task. The deterministic strategy assigns a held
tone below the singer to voice 1 and a held tone above to voice 2. Previous
targets are used as a simple movement cost, retaining lower/upper identity and
minimizing jumps. An empty chord invalidates and fades both targets.

## Synthesis, resources, and mixer

One `SharedPitchShiftResources` owns the 16,384-sample input history and
2,048-point Hann table. Both voices reference it. A voice owns only its 4,096
sample audio and normalization OLA rings, cursors, smoothing, state, profiling,
and telemetry. The second voice therefore adds approximately 32 KiB of OLA
storage plus small state, rather than the prior approximately 104 KiB complete
object. No storage is allocated in `process()` and no buffer was hidden on the
heap or moved to PSRAM.

Each voice accepts a target ratio; musical logic is not embedded in TD-PSOLA.
Each voice is bounded to eight grains per block. Target movement uses configured
smoothing (50 ms default). Tracking loss fades harmony over 20 ms. Onsets
suppress contribution using the same fade. Strongly unvoiced audio is not
copied into two full-level voices; the centre dry lead retains consonants. The
stereo mixer uses equal-power pan, conservative -6/-9 dB defaults, and 0.5
(-6 dB) internal headroom before shared delay/reverb and limiter.

## Profiling and limitations

Every `TdPsola` retains independent per-stage and total telemetry, accessible
through `vocal_fx_harmony_telemetry(voice)`. The full-pipeline profiler covers
the combined callback. Hardware cycles, I2S deadlines, codec quality, and heap
telemetry require a physical P4; host timings are regression indicators only.

There is intentionally no LPC/formant preservation. ±3 and ±4 semitones are
normally least objectionable; +7/-7 can make the voice noticeably
smaller/larger because the spectral envelope moves with pitch. There is no
automatic key detection, correction, phase vocoder, vocoder, shimmer, or neural
processing.

## Validation record

The starting commit was `b200fdcb0b670b915aec83475e7db5e6d344ba2c` and already
contained validated single-voice TD-PSOLA. Before edits, all mandated commands
were attempted. This checkout lacked the Makefile/script and the runner had no
`IDF_PATH` or `idf.py`, so a real pre-change map could not be captured. The
externally supplied last-known baseline remains approximately 786 KiB
application flash free, 376.8 KiB DIRAM free, and 128.8 KiB `libvocal_fx` static
RAM; these are not claimed as newly measured. Real P4 rows must be filled by an
ESP-IDF v5.3 runner rather than fabricated.

| Configuration | DIRAM used | DIRAM free | Flash | vocal_fx static RAM |
|---|---:|---:|---:|---:|
| Last-known / 1 PSOLA | unavailable | ~376.8 KiB | ~786 KiB free | ~128.8 KiB |
| 2 PSOLA voices | ESP-IDF runner required | ESP-IDF runner required | ESP-IDF runner required | ESP-IDF runner required |
