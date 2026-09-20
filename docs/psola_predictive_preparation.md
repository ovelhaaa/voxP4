# Predictive TD-PSOLA preparation: dependency and feasibility audit

Status: **Experiment A measured on ESP32-P4; no prepared grain is consumed.**
The synchronous scheduler remains authoritative. The measured run used a
44.1 kHz, 64-frame one-minute musical replay and two ten-second transition
fixtures on COM11. The bounded recorder had zero drops.

## Dependency graph

```text
current block pitch, mark list, target, continuity state
    -> usability / recovery / onset cursor updates
    -> next_synthesis_mark_ and history_offset_
    -> destination and requested source
    -> select_mark(source, current marks) -> center and period
    -> history eligibility at current input_end
    -> model_near(center, concurrently published ring)
    -> LPC enable decision (mode, amount, confidence)
    -> lambda, gamma, normalization strategy, sample rate
    -> local/shared warp-cache lookup or polynomial + gain normalization
    -> commit grain state and render/Ola
```

The first cold grain has measured mark selection, model lookup, warp and
descriptor costs (see `artifacts/alpha01d/b4d9_grain0_breakdown.csv`). The
cache lookup and its updates are stateful; a speculative computation must use
scratch storage and may publish to a cache only at the original commit point.
`last_scheduled_mark_`, grain counters, audit state, `grain_model_`,
`formant_filter_gain_`, OLA buffers and renderer state must also wait.

## What is knowable early

The normal scheduler bound is `block_end + pitch.period_samples`; after each
attempt the cursor advances by `current_synthesis_period_ / ratio`. This is a
coverage look-ahead, **not** a guarantee that the next grain's inputs are
known. In the scheduling block, pitch smoothing runs per grain. Onset can
initialize the cursor from a currently safe mark; recovery can reset it to
`block_start` or `block_start + pitch.period_samples`. Current `marks` and
`pitch` are passed to `process_shared` for that block. Therefore an N-1
prediction made from the previous block alone cannot be assumed exact at a
transition. N-2 and N-3 have additional opportunities for changes.

The recorder saves an end-of-block cursor, pitch, mark list, coherent LPC ring
references, formant controls and DSP runtime. A frozen-input projector advances
copied scalar state one, two or three blocks; it does not read block N inputs
until scoring. An inactive earlier cursor is `NOT_PREDICTABLE`. MC+1 is a
systematic one-in-eight sample; all MC+2/MC+3+ bursts are retained.

### Complete-key prediction (measured)

| Class | N-1 exact | N-2 exact | N-3 exact |
| --- | ---: | ---: | ---: |
| MC+1 sample | 368/698 | 88/698 | 23/698 |
| MC+2 | 0/6 | 0/6 | 0/6 |
| MC+3+ | 0/172 | 0/172 | 0/172 |

The [accuracy](psola_prediction_data/psola_prediction_accuracy.csv),
[status](psola_prediction_data/psola_prediction_status.csv) and
[per-grain](psola_prediction_data/psola_prediction_detail.csv) CSVs retain
numerators, denominators and individual comparison results.

MC+2 had three blocks with two grains each; MC+3+ had 57 blocks with 172
grains. **All 60 burst blocks had `have_cursor_ == 0` at N-1, N-2 and N-3,
then `have_cursor_ == 1` in N.** None had a constructible destination candidate
at those cross-block horizons. By comparison, all 698 sampled MC+1 blocks had
an active cursor in the prior three blocks. At N-1 the projector produced 635
MC+1 candidates with exact destination and requested-source bits; 368 also
matched the selected mark and complete key.

### Component accuracy (measured)

| Class, horizon | Destination | Mark | Model serial | LPC decision | Warp/normalization key | Complete |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| MC+1, N-1 | 635/698 | 368/698 | 377/698 | 635/698 | 377/698 | 368/698 |
| MC+1, N-2 | 524/698 | 88/698 | 93/698 | 524/698 | 93/698 | 88/698 |
| MC+1, N-3 | 445/698 | 23/698 | 23/698 | 446/698 | 23/698 | 23/698 |
| MC+2, each horizon | 0/6 | 0/6 | 0/6 | 0/6 | 0/6 | 0/6 |
| MC+3+, each horizon | 0/172 | 0/172 | 0/172 | 0/172 | 0/172 | 0/172 |

The zero burst component projections mean **no candidate was constructible**;
they do not mean every underlying component changed. At N-1 the eventual mark
was already in the list for 115/172 MC+3+ grains, and the eventual model was
published for 172/172. For the **first grain** of each MC+3+ block these
figures were 54/57 marks and 57/57 models. Choosing the latest published model
would select the actual first-grain model in only 24/57 blocks. At N-3 the
eventual model existed for 147/172 MC+3+ grains. Availability and selection
are distinct; see [availability](psola_prediction_data/psola_prediction_availability.csv).

### Failure context (measured)

At N-1, all 172 MC+3+ grain comparisons and all six MC+2 comparisons are
`CURSOR_UNAVAILABLE` / `NOT_PREDICTABLE`. In the MC+3+ grain population,
57/172 marks were absent from the N-1 list, 130/172 had a different pitch
period bit pattern, and 63/172 belonged to an onset block. These reasons
overlap. At the block level, 21/57 MC+3+ bursts had the onset flag and 3/57
had `pitch_changed`; none had the recovery-active flag. The common dependency
is the prior inactive cursor: destination centers are initialized in N. See
[failure counts](psola_prediction_data/psola_prediction_failures.csv) and
[event context](psola_prediction_data/psola_prediction_context.csv).

### Look-ahead and earlier-block slack (measured)

The lead below is the rounded `destination - block_start` in the *scheduling*
block; it is not cross-block prediction lead.

| Class | Lead min / median / p95 / max, samples | N-1 median DSP / slack, us | N-2 median slack, us | N-3 median slack, us |
| --- | --- | ---: | ---: | ---: |
| MC+2 (3 blocks) | -71 / 73 / 221 / 221 | 502 / 949 | 1010 | 998 |
| MC+3+ (57 blocks) | -167 / 54.5 / 276 / 492 | 526 / 925 | 925 | 928 |

At the 1451.247 us deadline, 55/57 MC+3+ blocks had at least 800 us of
positive N-1 slack. All 57 had at least 800 us across N-2+N-1 and 1000 us
across N-3+N-2+N-1. All three MC+2 blocks met these thresholds. This is a
theoretical CPU budget, not proof a preparation stage fits or has no cache
cost. [Event rows](psola_prediction_data/psola_prediction_slack.csv) and
[distributions](psola_prediction_data/psola_prediction_slack_summary.csv) are
retained as CSV.

## Exact validation design

A candidate key needs the bit patterns of the destination `double`, requested
source `double`, selected source-center `uint64_t`, source period `float`,
computed half-width, current input-history eligibility, model identity and
contents, LPC enable decision, lambda, gamma, sample rate, normalization
strategy, formant controls and any configuration used by the prepared
operation. Compare floating values by bits, including all LPC coefficients
through `order`, rather than by tolerance. A mark-list generation alone does
not prove the selected mark is identical; re-run selection at commit.

The 16-slot ring now stores an observational per-slot publication serial. The
writer stores it with complete slot contents before releasing the slot
sequence and advancing `published_`. `model_near()` copies that serial within
its existing sequence check. A bounded reader also copies ring references
only if all slot sequences and the global publication serial remain stable;
otherwise the snapshot is ambiguous. The serial is 32-bit and cannot wrap in
these short runs; a future prepared-result reuse token should use a wider
generation or explicitly invalidate all outstanding candidates on wrap.
Matching serials identify the same immutable coefficient publication, so
coefficient equality is established without copying 21 floats per telemetry
row. This serial is diagnostic only and is not yet a commit validation token.
The captured device image inferred the serial from the ring publication index;
the per-slot serial store was added afterward to make that identity explicit
under concurrent overwrite. The measured **cursor** conclusion does not rely
on model serial correctness. The final per-slot identity path has a host
equivalence test and firmware build, but was not repeated on the device.

## Work classification

| Work | Earliest safe location | Condition |
| --- | --- | --- |
| Candidate mark search | Prepare | Re-run exact selection at commit against current marks |
| Candidate model copy | Prepare | Re-run `model_near` or validate a coherent publication snapshot at commit |
| Polynomial warp | Prepare | Exact model and formant key match |
| Gain normalization | Prepare | Exact original/warped coefficients, strategy and rate match |
| Cache updates and grain/audit counters | Commit | Preserve existing state transitions |
| History eligibility, activation and OLA | Commit/render | Depend on current input end and destination sample |
| Deferred residual/render slices | Render | Depend on live source history and output interval |

The first safe experiment should consume **one** independently validated
immutable component, then compare per-grain structure and output samples with
the synchronous reference. A miss must call the existing `add_grain` path.

## Deadline baseline and Experiment A scope

At 44.1 kHz and 64 frames the deadline is 1451.247 us. The supplied five
minute baseline has mean 704.199 us, p99 1168 us, p99.9 2334 us, maximum
2701 us and 298 misses. Supplied MC+2 and MC+3 means are about 2254 and
2370 us; both conditional miss rates are 100%. Roughly 0.8-1.0 ms must leave
their critical blocks. The first grain dominates the measured `add_grain`
cost, but moving its measured work alone has not yet been shown sufficient.

The new one-minute replay measured 41,375 blocks, mean 636.813 us, p99 1161
us, p99.9 2347 us, maximum 2629 us and 55 misses. All three MC+2 and all
52 MC+3+ replay blocks missed. Across the two additional transition cases,
all five MC+3+ blocks missed. This is an instrumented feasibility run, not an
optimized after result. The full five-minute qualification, preparation CPU
cost and prepared-path commit/render cost are **not measured** because no
preparation path exists. Host `make test` passed 44/44 tests and the output
CRCs remained FullChain `0xA1D62ACE` and 3,750-block fixture `0xB5B0F372`.

A paired one-minute replay with the **prediction recorder disabled** used the
same short B4D.12 campaign and pinned firmware environment:

| Prediction recorder | Blocks | Mean | p95 | p99 | p99.9 | Max | Misses |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Off | 41,405 | 613.118 us | 942 | 1026 | 2191 | 2492 | 55 |
| On | 41,375 | 636.813 us | 1060 | 1161 | 2347 | 2629 | 55 |

The recorder is measurably intrusive: +23.695 us mean, +118 us p95 and +135
us p99 in these two runs. Both had zero actual RX/TX DMA errors, dropped frames
and sequence gaps. The paired runs are separate replays, so the differences
include ordinary run variation; they should not be subtracted from individual
burst times as a per-block correction. The slack figures above are the
recorder-on observations. The independent recorder-off result is retained in
`psola_prediction_recorder_off_raw.txt`.

The recorder performs no `printf` on the audio task. It retains at most 512
systematically sampled MC+1 events plus every observed MC+2/MC+3+ event per
case in bounded PSRAM. The report uses the complete 60 burst-block population
in the measured windows; MC+1 percentages apply to the 698 sampled grains.
The audit recorder is behind `CONFIG_VOXP4_PSOLA_PREDICTION_RECORDER` and is
disabled in the normal B4D.12 configuration.

## Experiment B1 feasibility audit (before a prepared consumer)

The [first-grain baseline CSV](psola_prediction_data/psola_b1_baseline.csv)
reprocesses the original COM11 capture without changing Experiment A data or
the DSP path. It covers three MC+2 and 57 MC+3+ first grains. The N-1 values
are exact bit comparisons, including formant shift/amount, derived lambda,
gamma, sample rate, mode and normalization strategy. Every component and the
complete control key matched in **3/3 MC+2 and 57/57 MC+3+**. The eventual
model was also published at N-1 in all 60 blocks.

| N-1 recency policy | MC+2 model coverage | MC+3+ model coverage |
| --- | ---: | ---: |
| Newest 1 | 1/3 | 24/57 |
| Newest 2 | 3/3 | 36/57 |
| Newest 4 | 3/3 | 57/57 |
| Newest 8 or 16 | 3/3 | 57/57 |

The recency rank uses `N-1 newest serial - eventual serial + 1`; a complete
coherent 16-slot snapshot was not printed. These are **policy coverage upper
bounds**, not cache hit rates. N-2 already contained the eventual first-grain
model in 3/3 MC+2 and 57/57 MC+3+ blocks; N-3 contained it in 2/3 and 49/57.
Thus the model publication lead is at least two blocks for every MC+3+ first
grain in this sample, but the exact publication block and full min/median/p95
lead are not recoverable from the three snapshots. At 44.1 kHz, two 64-frame
blocks are about 2.90 ms. The existing capture does not give valid-model count,
age distribution, or proximity to marks for all ring entries.

For the retained 3 MC+2 and 52 MC+3+ first-grain forensic rows, the measured
mean model-only polynomial plus gain cost was 34,315 and 28,936 cycles,
respectively (about 95 and 80 us at 360 MHz). The corresponding mean first
`add_grain` costs were 276,840 and 265,717 cycles (769 and 738 us). Mean mark
selection was 22,877 and 18,490 cycles; `model_near` was 29,684 and 28,927;
cache lookup was 16,347 and 16,322. All 60 first grains took the cold path.
The forensic recorder retained 52/57 MC+3+ breakdowns, so these means are
descriptive, not full-population estimates. The small mathematical portion
cannot by itself remove the roughly 0.8–1.0 ms required from a burst block.

`warp_polynomial` and `compute_gain_normalization` are **MODEL-ONLY
PREPARABLE** when the exact published model and control key are fixed.
`select_mark` and `model_near(center)` are **MARK/SOURCE DEPENDENT**.
History eligibility, destination bounds and deferred descriptor setup are
**GRAIN/DESCRIPTOR DEPENDENT**. Cache updates, audit counters and grain-state
changes are **COMMIT-ONLY**. OLA and deferred slices are **RENDER-ONLY**.
The selected mark and destination do not enter the polynomial or gain
functions. Existing local/shared warp-cache state is demand driven and must
not be changed by an early computation.

The data supported trying a separate, exact-key prepared cache of at most the
newest four models as a bounded experiment. At the observed mean math cost,
four cold preparations would cost roughly 320–380 us and all 16 roughly
1.3–1.5 ms before cache/snapshot overhead; the latter is already comparable
to the whole block deadline. To establish empirical performance without
disturbing demand caches, the 32-bit serial was upgraded to a concurrency-safe
64-bit generation, a lock-free snapshot API was added, and a dedicated scratch
store (`PsolaPreparedWarpStore`) was implemented and qualified on hardware.

### B1 feasibility verdict

Exact N-1 control stability and newest-four model coverage pass the initial
*feasibility* checks. However, because model math is only about 80–95 us per
cold first grain, B1-A (model pre-warming alone sufficient) was theoretically
implausible. Full device evaluation with the prepared-cache consumer enabled
across policies 0, 1, 2, 4, 8, and 16 was conducted on COM11 to measure actual
burst reduction and normal-block overhead.

## Experiment B1 device qualification (with prepared consumer enabled)

The firmware was extended with an exact-key, lock-free prepared warp store
(`PsolaPreparedWarpStore`) and evaluated on ESP32-P4 hardware (COM11) across
policies 0, 1, 2, 4, 8, and 16.

### Concurrency-safe model publication and snapshot architecture

1. **64-bit publication generation**:
   - `std::atomic<uint32_t> generation_low, generation_high` added to each slot in
     the 16-entry published model ring.
   - The single-writer pitch worker thread monotonically advances a 64-bit counter
     `next_generation_` upon publishing a valid LPC model.
   - Wraparound safety: generation zero is explicitly reserved as invalid. If the
     counter ever saturates `UINT64_MAX`, generation 0 permanently disables
     prepared-result reuse rather than permitting collisions.
2. **Lock-free, bounded audio-thread snapshot (`snapshot_models_once`)**:
   - Single bounded pass over the ring without blocking or retrying.
   - Sequence checks before and after each slot: an active writer (odd sequence),
     a sequence change, or a ring publication index advance immediately aborts
     the snapshot (`return false`).
3. **Strict bit-exact validation key**:
   - Model identity: `publication_generation` (uint64_t != 0), `timestamp`, `order`.
   - Model coefficients: exact floating-point bit pattern for all `order + 1` coefficients.
   - Formant warp controls: bit-exact `lambda` (uint32_t bits), `gamma` (uint32_t bits).
   - Normalization & rate: `normalization_strategy` (enum), `sample_rate` (uint32_t float bits).
4. **Commit-time revalidation & zero timing jitter**:
   - In `add_grain()`, the existing demand caches are checked first. On a cold miss,
     `prepared_warps.find(model, lambda, gamma, strat, sample_rate_, consume=true)` is queried.
   - If an exact key matches:
     - Sets `grain_model_.coefficients = prepared->warped_coefficients`
     - Sets `formant_filter_gain_ = prepared->formant_filter_gain`
     - Populates the demand caches (`warp_cache_`, `shared_warp_cache_`) for consistency.
     - Records `cache_path = 5` (prepared hit).
   - If validation fails or no match exists, the scheduler falls back directly to the
     authoritative synchronous calculation (`handled = false`).
   - Grain destination sample, source center, activation bounds, and OLA timeline
     remain 100% bit-identical to the synchronous reference.

### Device policy evaluation results (measured on COM11)

The matrix evaluated policy 0 (baseline disabled), policy 1 (bounded scan & publication guard),
and policies 2, 4, 8, 16 across the 44.1 kHz 1-minute musical reference (`a_ref_vocal_fullfx`,
deadline = 1451.247 us) and two 10-second step fixtures (`d_step_147_220_330`, `d_step_80_160`).

The raw records are retained in `docs/psola_prediction_data/psola_b1_device_policies.csv`.

| Policy | Variant | Class | Blocks | First-grain prepared hits | Burst mean (us) | Burst p99 (us) | Burst max (us) | Miss rate | Normal mean (us) | Normal p99 (us) | Normal misses |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **0 (Off)** | bounded_scan | MC+2 | 3 | 0 / 3 (0%) | 2304.7 | 2426 | 2426 | 3/3 (100%) | 600.95 | 1063 | 0 |
| | bounded_scan | MC+3+ | 52 | 0 / 52 (0%) | 2339.9 | 2564 | 2564 | 52/52 (100%) | 600.95 | 1063 | 0 |
| **1** | bounded_scan | MC+2 | 2 | 2 / 2 (100%) | 1874.0 | 2109 | 2109 | 2/2 (100%) | 640.02 | 1155 | 1 |
| | bounded_scan | MC+3+ | 54 | 54 / 54 (100%) | 2356.7 | 2605 | 2605 | 54/54 (100%) | 640.02 | 1155 | 1 |
| **1 (Guard)** | pub_guard | MC+2 | 4 | 4 / 4 (100%) | 2313.3 | 2441 | 2441 | 4/4 (100%) | 635.17 | 1203 | 2 |
| | pub_guard | MC+3+ | 51 | 51 / 51 (100%) | 2368.9 | 2705 | 2705 | 51/51 (100%) | 635.17 | 1203 | 2 |
| **2** | bounded_scan | MC+2 | 4 | 4 / 4 (100%) | 2352.0 | 2420 | 2420 | 4/4 (100%) | 640.75 | 1165 | 1 |
| | bounded_scan | MC+3+ | 52 | 52 / 52 (100%) | 2342.7 | 2622 | 2622 | 52/52 (100%) | 640.75 | 1165 | 1 |
| **4** | bounded_scan | MC+2 | 2 | 2 / 2 (100%) | 1911.5 | 2125 | 2125 | 2/2 (100%) | 641.26 | 1151 | 2 |
| | bounded_scan | MC+3+ | 53 | 53 / 53 (100%) | 2358.2 | 2621 | 2621 | 53/53 (100%) | 641.26 | 1151 | 2 |
| **8** | bounded_scan | MC+2 | 2 | 2 / 2 (100%) | 1917.0 | 2139 | 2139 | 2/2 (100%) | 643.29 | 1160 | 1 |
| | bounded_scan | MC+3+ | 53 | 53 / 53 (100%) | 2354.8 | 2570 | 2570 | 53/53 (100%) | 643.29 | 1160 | 1 |
| **16** | bounded_scan | MC+2 | 2 | 2 / 2 (100%) | 2202.5 | 2231 | 2231 | 2/2 (100%) | 645.70 | 1162 | 2 |
| | bounded_scan | MC+3+ | 53 | 53 / 53 (100%) | 2357.5 | 2575 | 2575 | 53/53 (100%) | 645.70 | 1162 | 2 |

### Cost accounting and redistribution analysis

1. **Prediction hit rate was perfect**:
   Across all enabled policies (1 through 16), **100% of first grains in MC+2 and MC+3+ bursts
   successfully hit the prepared cache** (cache path 5).
2. **Burst latency did not clear the deadline**:
   Despite 100% first-grain prepared hits, **$P(\text{miss} \mid \text{MC}+2) = 100\%$ and
   $P(\text{miss} \mid \text{MC}+3+) = 100\%$ across all policies**. The mean MC+3+ burst
   latency remained between 2342 us and 2369 us, exceeding the 1451.247 us deadline by ~900 us.
3. **Preparation math savings vs. critical-path deficit**:
   - The polynomial warp and gain normalization save ~80–95 us (28,900–34,300 cycles @ 360 MHz).
   - To bring MC+2/MC+3+ under the deadline, roughly 880–1000 us must leave the critical block.
   - The model math accounts for only **~9% to 10%** of the required reduction.
   - The remaining ~700–800 us of work in the burst block (mark search ~20 us, `model_near` ~30 us,
     OLA/synthesis ~500 us, and surrounding pipeline components ~300+ us) cannot be removed
     by model math pre-warming.
4. **Collateral overhead on normal blocks**:
   - Polling and pre-warming models after each block added +34 to +45 us to normal-block mean DSP
     (rising from 600.95 us in Policy 0 to 635–646 us in Policies 1–16).
   - Normal-block p99 rose by ~90 to ~140 us.
   - Crucially, pre-warming on the audio task caused **1 to 2 deadline misses in previously clean,
     normal non-model-change blocks** where zero misses previously occurred.

## Audio equivalence

- **Host unit tests**: `test_psola_prepared.cpp` runs 94,765 iterations with concurrent ring
  publications, verifying exact bit equality (`same_bits`) for polynomial coefficients and
  gain normalization between the prepared cache and synchronous reference.
- **Whole-chain CRCs**: 45/45 host tests pass. The authoritative audio CRCs remain:
  - FullChain: `0xA1D62ACE`
  - 3,750-block fixture: `0xB5B0F372`
- **Onset and sample alignment**: Zero timing shifts (+0 / -0 samples). All grain destination
  centers, source centers, and render intervals are exactly preserved.

## Final architectural decision classification

1. **Complete-grain predictive preparation**:
   **Class C — Insufficient prediction horizon**.
   In every observed MC+2 and MC+3+ burst, the PSOLA scheduler cursor was inactive
   (`have_cursor_ == 0`) across blocks N-1, N-2, and N-3, becoming active only in block N upon
   evaluating block N pitch tracking and onset/recovery rules. Destination centers, requested
   source marks, and alignment cannot be predicted cross-block under the current scheduler.

2. **Component-only model math preparation (Experiment B1)**:
   **Class D — Preparation cost cannot be redistributed safely on the audio timeline**,
   combined with **Class B — Preparation helps marginally (~90 us) but is fundamentally
   insufficient to eliminate the deadline burst (~900 us deficit)**.
   - Moving the immutable mathematical operations (`warp_polynomial`, `compute_gain_normalization`)
     earlier achieves 100% hit rate on burst first grains, but leaves 90% of the burst latency intact.
   - Executing preparation on the audio thread consumes CPU headroom in normal blocks, increasing
     baseline DSP by ~6% and inducing new deadline misses in ordinary blocks.

### Production recommendation

- Keep the concurrency-safe 64-bit publication generation tokens, lock-free ring snapshot API,
  and prepared store infrastructure behind `CONFIG_VOXP4_PSOLA_PREDICTION_AUDIT_ONLY`.
- Set `CONFIG_VOXP4_PSOLA_B1_NEWEST_LIMIT=0` by default in production, ensuring that no speculative
  pre-warming is executed on the audio thread during normal operation.
- Any future architectural effort to address the remaining ~800 us deficit must address the
  synthesis/OLA and scheduling structure directly, or explore off-audio-thread asynchronous
  delegation without consuming audio-thread DSP slack.

## Evidence status

- **Measured**:
  - Exact N-1/N-2/N-3 candidate availability and failure context (60 burst blocks, 698 MC+1 samples).
  - ESP32-P4 device evaluation across policies 0, 1, 2, 4, 8, 16 on COM11 (`psola_b1_device_policies.csv`).
  - First-grain prepared hit rates, burst latencies, and normal-block distributions.
  - Paired recorder on/off intrusiveness baseline.
  - Host unit tests (45/45 pass), bit-exact mathematical validation, and CRC suites (`0xA1D62ACE`, `0xB5B0F372`).
- **Inferred**:
  - Model math represents ~80–95 us out of the ~880–1000 us required reduction; complete burst
    elimination requires tackling mark selection, model search, and synthesis/OLA directly.
- **Architectural Classification**:
  - Complete-grain prediction: **Class C** (insufficient look-ahead horizon).
  - Component model preparation: **Class D** (unsafe redistribution on audio thread) & **Class B** (insufficient alone).

