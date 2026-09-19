# TD-PSOLA `add_grain()` deadline-tail optimization

## Scope and reproducibility

This follows the [deadline-tail forensic audit](dsp_deadline_tail_forensics.md).
All device runs used the ESP32-P4 rev 1.3 on COM11, pinned ESP-IDF v5.3,
44,100 samples/s, 64-frame blocks, one harmony voice, full FX, and the
deterministic vocal-replay reference. The DSP deadline is 1,451.247 us.
Profiling and this report's per-grain recorder were enabled in every new
comparison build. The one-minute runs are development measurements; the
five-minute run below is the final qualification. The checked-in configuration
was never changed. Diagnostic and cache experiments used separate ignored
`build-host` configurations and build directories.

The recorder captures the first four successful `add_grain()` calls per block
without printing on the audio task. Its miss/top records contain the selected
source mark, destination center, rounded source period, model timestamp and
order, lambda/gamma/sample-rate bits, normalization strategy, local/shared
cache outcome, miss-reason bitmask, and ordinal substage cycles. Exact whole-run
class counters record cache paths and add-grain distributions. The
[extractor](../scripts/analyze_psola_grains.py) emits [per-grain rows](psola_grain_class_1min_grains.csv),
[whole-run cache counts](psola_grain_class_1min_cache.csv), and
[ordinal distributions](psola_grain_class_1min_classes.csv). The bounded
miss/top rows are selected tail samples; the class/cache counters have
whole-run denominators.

## What the additional grains actually cost

The second one-minute run measured 41,375 blocks. All 2 MC+2 and all 53
MC+3 blocks missed; only 2 of 3,532 MC+1 blocks missed. The new ordinal
counter gives these whole-run means at 360 MHz:

| Class and ordinal | Calls | Mean `add_grain` | Mean mark selection | Mean `model_near` | Mean gain normalization |
| --- | ---: | ---: | ---: | ---: | ---: |
| MC+1, grain 1 | 3,532 | 78,004 cycles (217 us) | 11,427 (32 us) | 4,351 (12 us) | 24,789 (69 us) |
| MC+3, grain 1 | 53 | 331,273 (920 us) | 71,391 (198 us) | 25,848 (72 us) | 49,204 (137 us) |
| MC+3, grain 2 | 53 | 26,649 (74 us) | 4,382 (12 us) | 1,900 (5 us) | 907 (3 us) |
| MC+3, grain 3 | 53 | 12,096 (34 us) | 3,200 (9 us) | 1,837 (5 us) | 1,590 (4 us) |

The grain-1 differential between MC+1 and MC+3 is about **703 us**, even
though both classes execute one warp-cache miss. Grains 2 and 3 add about
108 us together, far short of the roughly 1.56 ms whole-block MC+1/MC+3
mean gap (1,081 versus 2,638 us). The first call's cold mark/model/math
path, deferred rendering (319 us median in the earlier diagnostic), and
other scheduling work account for the burst. Nested counters are never
summed as independent pipeline stages. In selected MC+3 miss records, grain
1 had a 336,252-cycle median; grains 2 and 3 had medians of 26,763 and
9,039 cycles. The selection bias of miss records does not affect the
whole-run ordinal comparison above.

For quick development reproduction, the existing deterministic B4D.12
pitch-transition cases `d_step_80_160` and `d_step_147_220_330` have
previously generated MC+2 and MC+3 misses in five-second windows (see the
[forensic case counts](dsp_tail_diagnostic_cases.csv)). The one-minute
musical replay used here yields roughly 50 MC+3 events, so it is also a
practical iteration fixture. Neither replaces the five-minute final run.

## Complete warp key and miss reasons

`FormantWarpCache::is_hit` requires equality of model timestamp, LPC order,
every source coefficient's float bits, lambda bits, gamma bits, sample-rate
bits, and normalization strategy. Its value contains warped coefficients and
the gain normalization. The polynomial uses source coefficients, order,
lambda and gamma. Gain normalization uses the original/warped coefficients,
order, strategy and sample rate. Thus the existing cache covers the complete
mathematical input set, with a conservative additional timestamp guard.

In the whole-run MC+3 population, grain 1 missed in all 53 blocks. Grain 2
hit locally 50 times and missed 3; grain 3 hit locally 48 times and missed 5.
Every recorded miss differed from the previous local entry in **both model
timestamp and coefficient bits**. Order, lambda, gamma, sample rate and
normalization strategy never caused a miss. The miss records show the same
pattern at individual grain centers: a repeated source/model is a hit, while
a newly selected model has different coefficients. Of 110 adjacent grain
pairs in the retained MC+2/MC+3 miss records, 101 had the same model-key
fields and exactly 101 hit the local cache; 62 even reused the same source
center. The remaining nine pairs selected genuinely different models and
missed. The prior full-FX
60-second warp audit likewise counted 3,632 misses and zero timestamp-only
math-equal misses. A second generic warp or gain cache would duplicate the
existing local/shared cache and cannot eliminate these distinct calculations.

The mark search depends on the requested source position and the current
mark array. `model_near()` depends on the selected center and the concurrently
published 16-entry model ring. Some adjacent grains select the same source
center, but a bare same-center reuse would also need to prove the ring
publication state unchanged. Their warmed model lookup costs roughly 5 us,
so this is not a path to the missing 1 ms. The destination and often the
source center/period change for later grain descriptors; their timing and
render setup remain distinct.

| Candidate | Required exact inputs | Observation / safe lifetime | Decision |
| --- | --- | --- | --- |
| Warp polynomial | coefficients, order, lambda, gamma | Already reused by the complete warp cache on 50/53 second and 48/53 third MC+3 grains; misses have different coefficients | No new cache |
| Gain normalization | original and warped coefficients, order, strategy, rate | Stored with the same complete cache entry | No new cache |
| Model lookup | source center and published model-ring state | Center can repeat, but publication can change; warmed repeat is ~5 us | No unconditional reuse |
| Mark search | source query and current mark set | Query advances even when the selected mark repeats | Keep exact search |
| Grain descriptor | destination, source center/half, model and formant state | Destination differs by grain; required for exact onset | Keep per grain |
| Lambda calculation | formant shift semitones | Usually constant within a block, but negligible beside the burst | No batch API |

There is no measured per-model or per-model-plus-ratio calculation repeated
three times with identical inputs. A batch-scheduling API would add state and
invalidation complexity without removing the dominant work.

## Separate placement experiments

The baseline and trials below are one-minute runs of the same fixture and
instrumentation, differing only in the listed ESP32-P4 code placement. Short
runs have fixture/worker variance; the class and substage shifts are more
informative than a single overall miss count. IRAM size is from the linked
ELF `.iram0.text` section; the baseline used 66,642 bytes.

| Build | Added IRAM | MC+3 mean | DSP p99.9 | DSP max | Total misses | Decision |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Flash baseline | 0 | 2,638 us | 2,560 us | 2,906 us | 57 | Reference |
| `nearest_mark_index` in IRAM | 752 B | 2,532 us | 2,448 us | 2,844 us | 56 | Retain |
| Plus `model_near` in IRAM | 1,618 B | 2,527 us | 2,435 us | 2,870 us | 57 | Revert marginal increment |
| Mark index plus warp polynomial and gain normalization | 2,772 B | 2,449 us | 2,413 us | 2,708 us | 57 | Retain |
| Plus `select_mark` wrapper | 3,746 B | 2,358 us | 2,309 us | 2,589 us | 58 | Retain pending long run |

The [experiment matrix](psola_iram_experiment_matrix.csv) includes the
nearby repeat of the retained placement (MC+3 mean 2,371 us, p99.9
2,287 us, 56 misses) and the rejected 128-byte cache-line build.

The retained functions are small: `nearest_mark_index` 752 B,
`select_mark` 974 B, `warp_polynomial` 842 B, and
`compute_gain_normalization` 1,178 B. `add_grain()` itself is about 8.6 KB
and was not moved wholesale. With the mark index in IRAM, the MC+3 first
grain's mark-selection mean fell from 71,391 to 47,887 cycles. With both
mark functions in IRAM it fell to 16,693 cycles. With warp and gain in IRAM,
first-grain warp polynomial and normalization means fell to roughly 1,327
and 27,115 cycles. The MC+3 class remained **100% late in every trial**.

### Cache configuration and memory placement

The reference configuration uses a 128 KB L2 cache with 64-byte lines,
80 MHz DIO flash, 200 MHz HEX PSRAM, and performance compiler optimization.
The pinned IDF offers 256 KB L2, but an isolated build with the same DSP
source failed at final link: `--enable-non-contiguous-regions` discarded
347,734 bytes of sections, including essential `.sbss` symbols. The larger
cache leaves insufficient internal RAM for this firmware, so no device timing
exists and the setting is rejected. A separate 128-byte L2 line-size build
(same 128 KB capacity and identical DSP source) linked and ran, but regressed
badly in the one-minute fixture: DSP mean 740 versus 693 us in the nearby
64-byte repeat, p99 1,375 versus 1,161 us, p99.9 2,506 versus 2,287 us,
MC+3 mean 2,528 versus 2,371 us, and 213 versus 56 deadline misses. Its
physical gate failed after three consecutive late blocks. The 128-byte line
experiment is rejected. Production `sdkconfig` remains at 128 KB / 64-byte
lines. [Cache-line trial class counts](psola_line128_1min_classes.csv) and
[the nearby reference](psola_iram_select_warp_repeat_1min_classes.csv) are
retained.

The active engine object (`e`, 0x53668 bytes in the ELF) is in internal
DRAM, including the voice's warp cache and shared pitch history. The 16
deferred-grain descriptors are explicitly allocated with
`MALLOC_CAP_INTERNAL`; the small default residual cache also requests
internal memory. The LPC sine/cosine gain-normalization bases are each
0x7e0 bytes in DRAM; its 4 KB Hann array is in TCM. B4D.12's large flight
recorder and the pre-staged fixture are in PSRAM. No small, frequently
accessed PSOLA metadata was found in PSRAM to move; no speculative buffer
migration was retained. State localization and a batch API likewise had no
proven repeated expensive inputs to target.

## Correctness and architectural next step

The host FullChain and 3,750-block fixture still produce CRCs `0xA1D62ACE`
and `0xB5B0F372`. The 44 host tests and ESP32-P4 build passed after the
recorder changes; final tests and build are repeated after source selection.
The retained IRAM attributes change code location only. No grain onset,
pitch mark, LPC math, overlap, render order, or output sample calculation
changed.

The measured safe in-block gains are useful but below the roughly 1 ms
needed to meet the deadline. A follow-on preparation experiment should
predict candidate marks/models before their onset block, compute only
immutable preparation early, and revalidate the **complete** mark/model/
control key at the original scheduling sample. On any mismatch it must run
the current synchronous path. The grain descriptor and rendering must still
start at exactly the original destination sample. This requires an explicit
published-model generation or equivalent immutable snapshot; the current
`model_near()` ring does not expose a sufficient reuse token. Compare onset
sample, centers, envelope and audio CRC before using such a path in firmware.

## Five-minute qualification

The final 44.1 kHz full-FX run on COM11 covered 206,740 measured blocks
(five minutes) with the retained IRAM placement, 128 KB/64-byte L2 cache,
and the per-grain recorder. Its DSP mean was **704.199 us**, p99 **1,168 us**,
p99.9 **2,334 us**, maximum **2,701 us**, and **298 deadline misses**
(59.594 per minute). The independent physical transport qualification
passed: correct sample rate, maximum one consecutive late block, and 7.007 ms
maximum backlog. Physical gate pass does not imply the DSP deadline was met.

| Model-change class | Blocks | Mean (us) | p50 | p95 | p99 | Max | Misses / conditional rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| No model change | 188,516 | 668.72 | 681 | 995 | 1,109 | 1,420 | 0 / 0% |
| MC+1 | 17,930 | 1,050.03 | 1,055 | 1,178 | 1,263 | 1,705 | 4 / 0.022% |
| MC+2 | 22 | 2,253.82 | 2,307 | 2,448 | 2,507 | 2,507 | 22 / **100%** |
| MC+3 | 272 | 2,370.21 | 2,426 | 2,587 | 2,679 | 2,701 | 272 / **100%** |

The first MC+3 grain in retained miss records had a 248,901-cycle median
(~691 us); second and third grains had 27,816 (~77 us) and 8,774 (~24 us).
Of 294 second grains in these tail records, 274 hit the local warp cache;
251 of 272 third grains hit. These rows are selected miss/top samples,
whereas the table above has full-run denominators. The full-run
[grain rows](psola_final_5min_grains.csv), [cache counts](psola_final_5min_cache.csv),
[class distributions](psola_final_5min_classes.csv), and
[forensic miss records](psola_final_5min_forensic.csv) are retained.

The earlier pre-task long baseline had 206,730 blocks, 720.194 us mean,
1,221 us p99, 2,438 us p99.9, 2,873 us maximum and 302 misses. Relative
to it, the final run was 16.0 us lower in mean, 53 us lower at p99,
104 us lower at p99.9 and 172 us lower in maximum, with four fewer misses.
The baseline did not contain the new recorder and is a different run, so
these deltas are observational rather than an isolated causal estimate of
code placement. Both runs had **100% MC+2 and MC+3 conditional miss rates**.
The safe placement work reduces tail latency but does not close the
remaining approximately 0.8–1.2 ms burst-class deadline gap. The
preparation-and-revalidation design above remains follow-on work.
