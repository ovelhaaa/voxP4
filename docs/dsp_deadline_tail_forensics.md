# DSP deadline-tail forensics (ESP32-P4)

## Scope and measurement

This audit starts at `ddedc3b`. It investigates the existing 64-frame,
one-voice, full-FX B4D.12 vocal-replay case. The 44.1 kHz DSP deadline is
64/44100 seconds = 1451.247 microseconds. The DSP time is the wall time around
`vocal_fx_process`; the cycle/loop and transport counters are reported
separately. The audio task runs on core 0 at `configMAX_PRIORITIES - 2`; the
pitch worker runs on core 1 at `configMAX_PRIORITIES - 5`.

The flight recorder retains the 100 slowest blocks and up to 2,048 complete
deadline-miss records in PSRAM. It reuses `HarmonizerBlockTraceRecord` for
grain, model, pitch and recovery events, and adds six per-block major-stage
cycle deltas. The audio task never prints during the measurement window.
The reference case prints the CSV records after stopping its tasks. Build with
`CONFIG_VOCAL_FX_ENABLE_PROFILING=y` to populate stage cycles; otherwise the
event fields still work but stage fields are zero.

The CSV is a **selected tail sample**, so event frequencies calculated only
from that file are not whole-run rates. B4D.12's full-run model-change/grain
class counts and exact 1-microsecond DSP histogram supply population and
percentile denominators.

## Baseline distribution

Three five-minute reference captures on ESP32-P4 rev 1.3, 360 MHz, pinned ESP-IDF
v5.3, COM11, 44.1 kHz, 64-frame blocks, profiler enabled. The captures used
the optimized DSP at `ddedc3b` plus telemetry only. All percentiles come from
the exact 1-us B4D.12 whole-run histogram, excluding the three-second warmup.

| Run | Blocks | Mean | Median | p90 | p95 | p99 | p99.9 | p99.99 | Max | Misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial | 206,761 | 715.4 us | 761 us | 1,048 us | 1,107 us | 1,220 us | 2,441 us | 2,743 us | 2,932 us | 312 (0.1509%) |
| Class histogram repeat | 206,790 | 705.4 us | 749 us | 1,031 us | 1,087 us | 1,188 us | 2,435 us | 2,752 us | 2,891 us | 299 (0.1446%) |
| Final coherent snapshot | 206,730 | 720.2 us | 760 us | 1,064 us | 1,122 us | 1,221 us | 2,438 us | 2,740 us | 2,873 us | 302 (0.1461%) |

The final run computes its histogram after the audio task stops; its case,
histogram, and class counts all agree at 206,730 blocks. In the earlier
build, the histogram could be snapshotted one to three blocks before the task
stopped.
That has no material effect on the reported percentiles or workload finding,
but the final row is the authoritative distribution.
The [run summary CSV](dsp_tail_run_summary.csv) records all four long runs,
including the 48 kHz comparison and physical gates.

The miss threshold was 1451.247 us; the largest single lateness in the initial
run was 1481 us.
Maximum consecutive misses was one in all three long 44.1 kHz runs. RX/TX
overruns, DMA errors, read/write failures, dropped frames and sequence gaps
were all zero. The final run's observed RX rate was 44,100.145 frames/s
(0.0003% error). The physical B4D.12 gate
passed. This supports a DSP execution overload diagnosis for the measured
misses, not an audio transport failure.
In the repeat 44.1 kHz miss records, median DSP execution was 2538 us and
median whole-loop interval was 2656 us. At 48 kHz those medians were 2489
and 2599 us. The loop is late because the DSP itself ran long in the dominant
class; the loop metric also includes transport and bookkeeping time.

The profiler and recorder affect timing. The prior optimized 60-second
profiling build measured 696.6 us mean and 1158 us p99; this five-minute
forensic build measured 715.4 us and 1220 us. These are different builds and
durations, so the difference is not an algorithm regression estimate.

## Stage attribution and event combinations

The `Harmony` section is the dominant changing cost. Its whole-run mean was
107,763 cycles (299 us); its median in the 312 miss records was 732,336
cycles (2,034 us). In the top 100, its median was 771,964 cycles (2,144 us).
`Delay` stayed near 13,000 cycles in normal and missed blocks; `Master` stayed
near 5,500. `Reverb` rose from 26,759 mean to 31,856 median cycles in misses,
far too little to explain the 1 ms-plus jump. The profiler's stage timing
omits some pipeline sections, so these stage values do not sum to the exact
DSP wall time.
The bounded recorder does not retain every block around the whole-run p99
threshold, so it cannot report an unbiased per-stage median for that slice.
The whole-run stage means, class histograms, complete miss records and
top-100 records provide the attribution stated here.

The existing B4D.12 classifier gives whole-run denominators for the initial
run:

| Event class | Blocks | Misses | P(miss \| class) |
| --- | ---: | ---: | ---: |
| No model change | 188,542 | 7 | 0.0037% |
| Model change + 1 new grain | 17,929 | 12 | 0.0669% |
| Model change + 2 new grains | 15 | 15 | 100% |
| Model change + 3 or more new grains | 278 | 278 | 100% |

Thus 305/312 misses (97.8%) had a model change, and 293/312 (93.9%) had a
model change plus at least two new grains. Those 293 blocks account for **all**
whole-run blocks in their classes. Within miss records, 305/312 had at least
one expensive model warp and 303/312 had fallback active. Pitch change was
present in only 29/312, so a pitch update by itself is not the dominant
signature. No miss record reported a source grain built or FIR sample
computed in that block. This points to the current block's model warp and
deferred grain rendering, rather than source construction or FIR, as the
immediate workload. The model warp averaged about 490 us across misses.

The repeat run reproduced the pattern: 297/299 misses had a model change;
293/299 were model change plus at least two new grains. Exactly 22 MC+2 and
271 MC+3 blocks occurred, and all 293 missed. The class histogram provides
whole-run conditional timing, including ordinary blocks:

| Class, repeat run | Blocks | Mean | Median | p99 | Max | Miss rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| No model change | 188,567 | 670.2 us | 666 us | 1,137 us | 1,572 us | 2/188,567 (0.0011%) |
| Model change + 1 grain | 17,931 | 1,046.6 us | 1,050 us | 1,285 us | 1,790 us | 4/17,931 (0.0223%) |
| Model change + 2 grains | 22 | 2,356.6 us | 2,425 us | 2,692 us | 2,692 us | 22/22 (100%) |
| Model change + ≥3 grains | 271 | 2,489.4 us | 2,549 us | 2,835 us | 2,891 us | 271/271 (100%) |

The repeat run's 100 slowest records were all MC+2 or MC+3 (97 MC+3), with
2,676.5 us median DSP time and 780,094-cycle median Harmony time.
Its complete [flight recorder CSV](dsp_tail_44100_5min_classes.csv) is
retained alongside the initial run.

The coherent final run again had **293/302 misses (97.0%)** in MC+2/MC+3.
It recorded 21 MC+2 and 272 MC+3 blocks; every one missed. Model change
occurred in 300/302 misses (99.3%). Whole-run conditional means were 683 us
without model change, 1085 us for MC+1, 2344 us for MC+2, and 2488 us for
MC+3. The [final flight recorder](dsp_tail_44100_final_5min.csv) contains
all 302 misses and the 100 slowest blocks, including the existing PSOLA
substage cycle counters. Its miss medians were 398,778 scheduling cycles,
347,771 nested `add_grain` cycles, and 104,002 deferred-render cycles.
All 100 final-run top blocks had a model change and at least two new grains;
98 had three and one had four. Their median DSP/Harmony times were 2671.5 us
and 778,992 cycles, respectively.

A separate one-minute 44.1 kHz diagnostic sampled existing PSOLA cycle
counters; it had 41,376 blocks and 58 misses. In the 53 MC+3 miss records,
median scheduling cost was 403,539 cycles (1,121 us), including 348,935
cycles (969 us) in `add_grain`. Median deferred rendering was another
114,657 cycles (319 us). Mark selection was 88,147 cycles (245 us), model
lookup 26,659 (74 us), warp polynomial 14,949 (42 us), and gain
normalization 54,350 (151 us). The counters are nested: `add_grain` and its
model/mark subparts must **not** be summed, nor should the `model_warp_us`
field be added to them. Together they show that the miss is a grain burst
whose scheduling path includes model work, followed by more deferred render
work. This [diagnostic CSV](dsp_tail_44100_diagnostic.csv) has the detailed
substage values.
This short diagnostic was for attribution; its physical gate failed with
two consecutive late blocks, so the two five-minute 44.1 kHz runs remain
the reference qualification evidence.

## Controlled input experiments

The repository already contains deterministic B4D.12 pitch-step fixtures
designed for model-change/grain-burst stress. With five-second measured
windows, the nine fixed-F0 cases (80–400 Hz) plus two glissando cases
produced **zero** MC+2/MC+3 blocks. The six captured pitch-step cases
produced three MC+2 and three MC+3 blocks; all six missed. For example,
`d_step_80_160` produced two MC+2 misses and `d_step_147_220_330` produced
one MC+3 miss. [Per-case class counts](dsp_tail_diagnostic_cases.csv) retain
the machine-readable evidence. One of seven step cases was missed during a
serial-capture handover and is excluded from these totals.

These short cases are a controlled trigger, not a replacement for the long
musical fixture. Their B4D.12 percentile/max snapshot was taken just before
stopping the audio task, whereas class/miss counters were printed after it
stopped; a final block could therefore make a short case's printed maximum
inconsistent with its miss count. The final five-minute reference run uses
the corrected post-stop snapshot.

## 48 kHz deadline comparison

The same five-minute reference case at 48 kHz measured 225,016 blocks. The
deadline was 1,333.333 us. Mean/median/p99/p99.9/max were 702/745/1175/2353/
2840 us. There were 374 misses (0.1662%), versus 299 (0.1446%) in the
44.1 kHz repeat. All 268 MC+2/MC+3 blocks missed again; their class means
were 2640 and 2526 us. The shorter deadline additionally exposed 55
no-model-change and 51 MC+1 misses, compared with 2 and 4 at 44.1 kHz.
Thus the extreme tail has the same event signature at both rates, while the
deadline change affects the marginal tail as expected. The
[48 kHz flight recorder](dsp_tail_48000_5min.csv) retains all 374 misses.
The [class summary CSV](dsp_tail_class_summary.csv) contains the whole-run
conditional distributions and miss denominators for both rates.

RX/TX overruns, DMA errors, drops and sequence gaps remained zero; measured
RX rate was 48,000.808 frames/s (0.0017% error). The 48 kHz physical B4D.12
gate **failed** because maximum consecutive late blocks was two. This is
consistent with the shorter deadline and must not be reported as a passing
real-time qualification.

These are event associations, not isolated per-operation cost estimates.
The ≥2-grain class appears only with a model change in this fixture, so the
capture cannot independently assign the full excess cost to either event.
The follow-up whole-run class histograms and controlled transition fixtures
are intended to tighten that attribution.

## Worst blocks and periodicity

In the initial run, all 100 slowest blocks had a formant model change, three new and three active
grains, and at least one expensive model warp. Their median DSP time was
2,673.5 us. The ten slowest are below; all cycles are from the 360 MHz core.
The [machine-readable flight recorder](dsp_tail_44100_5min.csv) includes
all 100 top blocks and all 312 misses, with full per-block event fields.

| Block | DSP us | Harmony us | New grains | Model warp calls |
| ---: | ---: | ---: | ---: | ---: |
| 170919 | 2932 | 2298 | 3 | 1 |
| 48269 | 2840 | 2305 | 3 | 1 |
| 182633 | 2839 | 2254 | 3 | 2 |
| 130663 | 2834 | 2145 | 3 | 1 |
| 126128 | 2824 | 2248 | 3 | 1 |
| 30158 | 2819 | 2163 | 3 | 1 |
| 168167 | 2812 | 2152 | 3 | 2 |
| 109590 | 2802 | 2312 | 3 | 2 |
| 117863 | 2792 | 2066 | 3 | 1 |
| 20015 | 2791 | 2291 | 3 | 2 |

Miss intervals have median 423 blocks and no simple short cadence. However,
140 of the 312 miss indices have another miss at +44,790 ±1 blocks (about
65 seconds). The deterministic stage buffer is 65 seconds long. This strong
fixture-locked recurrence supports signal/workload causation rather than
rare random scheduler interference.
The repeat 44.1 kHz run had 143 such +44,790 ±1 block matches. At 48 kHz,
220 misses repeated at +48,750 ±1 blocks, exactly the 65-second buffer period
at 750 blocks/s. The coherent final 44.1 kHz run had 140 matches at the same
offset. The recurrence persists across runs and sample rates.

## Root cause and next experiment

**Confirmed:** the dominant tail class is a formant model change coincident
with two or more newly scheduled PSOLA grains. The Harmony stage absorbs the
excess cycles; 293 such blocks occurred and every one missed the deadline.
The first long run's seven non-model-change misses were marginal (1458–1507
us), whereas the model-change misses reached 2932 us.

**Strongly supported:** model-warp work and rendering multiple active grains
are the specific variable operations. The trace shows 1–2 expensive model
warps and 2–4 new/active grains in the large misses. The exact cost split
needs a controlled same-grain-count comparison.

The code path explains the signature: each successful `add_grain` calls
`SharedLpcAnalysis::model_near` for its source center. A new LPC model
timestamp sets `model_changed_this_block_`; a warp-cache miss then executes
`warp_polynomial` and `compute_gain_normalization` before caching the result.
The same block may schedule two or three grains and render their deferred
slices. The existing local/shared warp caches already avoid repeated math
when their complete keys match, so a generic extra cache is not a justified
change without seeing which distinct keys caused the remaining 1–2 misses.

**Possible:** the seven marginal non-model-change misses include ordinary
DSP/cache variation. They do not explain the large tail.

**Ruled out as dominant in this capture:** delay, reverb, master, same-block
source-grain construction, and same-block FIR sample computation.

No DSP algorithm change has been made for this audit. A structural change to
defer model/grain work could alter onset timing and grain alignment; it would
need an event-specific before/after comparison and audio equivalence proof.
The recorder's top-100 selection was subsequently made cheaper by caching
its minimum and scanning only when a block can enter the set. That change
occurs after the timed `vocal_fx_process` call. The long-run DSP timings above
precede this recorder-only change; no after-change physical qualification is
claimed. The saved local build configuration was restored and reflashed to
COM11 after the measurements.

## Correctness

The instrumented source passes the 43 host tests. Deterministic FullChain CRC
is `0xA1D62ACE` and the 3,750-block fixture CRC is `0xB5B0F372`, both matching
the previous optimized build.
The final source also builds for ESP32-P4 with pinned ESP-IDF v5.3, both with
profiling enabled for forensics and with the restored profiling-off local
configuration.
