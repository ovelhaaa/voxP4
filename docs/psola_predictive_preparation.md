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

## Decision

**C — information arrives too late for complete-grain prediction under the
current scheduler.** In every measured MC+2/MC+3+ burst, the prior cursor was
inactive and became active in N, when current pitch/marks and the onset
decision were available. The destination and requested source therefore had
no exact N-1/N-2/N-3 candidate. Earlier blocks generally had enough measured
CPU slack, so the evidence does not support D. Many eventual models and first
marks already existed, which may justify a separate *component-only* model
math experiment, but their availability does not provide the complete key or
authorize a prepared grain. No grain scheduling, activation or audio timing
was changed.

## Evidence status

- **Measured:** per-grain N-1/N-2/N-3 projections and availability, burst
  context, predecessor DSP time, device class misses, paired recorder on/off
  timing, host tests and CRCs.
- **Inferred:** available slack is a theoretical preparation budget; matching
  publication serials represent matching immutable LPC contents during this
  bounded run. The C decision applies to complete-grain preparation with the
  current cursor initialization rule.
- **Not measured:** the CPU cost of preparing any component, commit cost,
  prepared-path audio equivalence, and a five-minute post-change qualification.
  No prepared path exists in this experiment.
