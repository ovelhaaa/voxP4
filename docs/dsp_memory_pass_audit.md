# DSP block and memory-pass audit

## Baseline map (commit 6387476)

The 64-frame production path at 44.1 kHz traverses the block in these major
stages: input HPF, gate, compressor, pitch-shift rendering, dry alignment,
harmony slew/pan, harmony limiter, bus mixing, delay source preparation, delay,
reverb input preparation, FDN reverb, master mix, and master limiter/output.
That is 14 important traversals counting pitch rendering as one stage; its
internal work contains further passes. Gate, compressor, harmony limiter,
delay, HPF, dry alignment, and master limiter call scalar processors per sample.
The original master mix and limiter/output traverse the same base buffers
twice. Harmony diagnostics update global peak fields inside both sample loops.
The spatial-source branch is block-constant but was evaluated for each sample.

The baseline already hoists harmony pan coefficients, wanted mix, and attack
and release steps. FDN reverb has a tuned block path with local ring pointers,
positions, damping, feedback, and state writeback; it was left alone. Input
HPF, gate, and compressor retain independent profiler sections. The current
checkout predates PR #11, so its master `Limiter` has only a scalar API.

## Buffer classification

| Buffer | Classification | Reason |
|---|---|---|
| `work` | REQUIRED | Shared input to analysis, pitch shift, and dry alignment. |
| `shifted` | REQUIRED | Pitch shift output also feeds isolated and telemetry paths. |
| `dry_bus` | REQUIRED | Alignment result is used by bus mixing and spatial routing. |
| `harm_bus_l/r` | ALIASABLE | Could feed a fused harmony/output processor; kept for clear limiter boundary. |
| `harm_bus_mono` | AVOIDABLE IN SOME ROUTING MODES | Only needed for HarmonyOnly spatial send. |
| `source_mono` | AVOIDABLE IN SOME ROUTING MODES | DryOnly and HarmonyOnly could pass an existing buffer directly; FullMix needs a materialized mono send for both effects. |
| `delay_wet_l/r` | REQUIRED | Separate sends feed reverb prep and final output. |
| `rev_in` | REQUIRED | FDN consumes a mono block after delay-send smoothing. |
| `rev_wet_l/r` | REQUIRED | FDN output is added after its block call. |
| `left/right` | REQUIRED | Stereo base bus and spatial FullMix source precede delay and reverb. |

Alias changes were deferred because these buffers have overlapping consumers
and little evidence yet that their storage, rather than their traversal, is
the bottleneck.

## Changes under measurement

Gate and compressor now have block APIs that cache mutable state and parameters.
Harmony peak diagnostics commit one value per block. A harmony limiter block
API was tested but reverted after it made the measured limiter section slower
by introducing a second pass for post-peak diagnostics.
DelayPrep dispatches on spatial source once per block. The master output uses
one fused pass for wet additions, limiter gain evolution, output store, and peak
measurement. Its `MasterMix` profiler section now covers work formerly split
between `MasterMix` and `MasterLimiter`; compare their **sum** in the baseline
against `MasterMix` in the candidate, or use the enclosing `Master` section.

Each retained block processor has a scalar reference comparison in `dsp_tests`.
These tests require exact float output, including consecutive blocks and a
midstream parameter update. Host correctness alone does not establish a P4
performance improvement.

## Instrumentation classification

Limiter peak and reduction fields are cheap always-on observability. The
per-section profiler is compile-time controlled by
`VOCAL_FX_ENABLE_PROFILING`; B4D cycle attribution and trace records remain
in the pipeline and should be classified as audit-only for a future dedicated
gate. Host sample telemetry is host-only. No instrumentation was removed in
this pass because B4D comparisons need the existing counters.

## Deferred experiments

StereoDelay's new block path still calls `SmoothedValue::next()` for left/right
time, wet, and dry on every sample to preserve all four trajectories. It now
caches ring state and pointers for the block. Harmony fusion
could combine slew/pan, limiter, mono generation, and diagnostics, but should
preserve the limiter profiler boundary or demonstrate a material P4 gain.
The measured input-fusion experiment did not justify collapsing three useful
profiler sections. FDN reverb has no identified reason for churn.
