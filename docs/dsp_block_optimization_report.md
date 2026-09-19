# DSP block and memory-pass optimization report

## Scope and measurement setup

Baseline: commit `6387476` in a separate worktree. Candidate: the changes in
this report. Both used pinned ESP-IDF v5.3, ESP32-P4 revision 1.3 on COM11,
360 MHz, 64-frame blocks, the B4D.12 deterministic vocal replay with one
harmony voice, compressor, delay, and FDN reverb enabled. The reference window
was shortened to 60 seconds for comparison; warmup remained 3 seconds.
Profiling was enabled in **both** measurement builds. The 48 kHz runs changed
only the B4D.12 rate, deadline, and fixture allocation/generation rate in both
builds. The committed source retains the production 44.1 kHz setting and the
normal 30-minute B4D.12 reference duration. Captured serial logs are in the
local ignored `build-host/b4d12_*.log` files.

## Baseline

The pipeline had 14 major block traversals when pitch rendering is counted as
one stage. Gate, compressor, harmony limiter, delay, HPF, dry alignment, and
master limiter were called once per sample. The temporary buffers and their
classifications are documented in [dsp_memory_pass_audit.md](dsp_memory_pass_audit.md).
The master path made separate wet-add and limiter/output passes. FDN reverb
already used a tuned block implementation.

## Retained changes and evidence

| Change | Files | Mechanism | Exactness check | P4 stage effect, 44.1 / 48 kHz |
|---|---|---|---|---|
| Gate block path | `dynamics/gate.*`, `src/vocal_fx.cpp` | Cache envelope, gain, hold, and parameters for a block. | Scalar equality over consecutive blocks and parameter changes. | InputGate −800 / −843 cycles per block in Phase A/B. |
| Compressor block path | `dynamics/compressor.*`, `src/vocal_fx.cpp` | Cache envelope and parameters; preserve exact linear guard and transcendental path. | Scalar equality over consecutive blocks and parameter changes. | Compressor −834 / −863 cycles in Phase A/B. |
| Harmony diagnostic localization | `src/vocal_fx.cpp` | Commit pre/post peaks once per block. | Same comparison order and final peak semantics; host DSP suite. | Harmony sections approximately flat; no independent gain claim. |
| Spatial source dispatch | `src/vocal_fx.cpp` | Select DryOnly, HarmonyOnly, or FullMix once per block. | Same source expression in each route; host DSP suite. | DelayPrep −50 / −206 cycles in final runs; near measurement noise. |
| Fused master output | `dsp/limiter.*`, `src/vocal_fx.cpp` | Wet addition, limiter, output store, and peak in one pass. | Bit-exact scalar reference over consecutive blocks. | Master −3,090 / −3,074 cycles in final runs. |
| StereoDelay wet block | `delay/delay.*`, `src/vocal_fx.cpp` | Cache ring pointers, position, feedback, LP states; preserve four smoothing calls and wrap order. | Bit-exact scalar output across impulse/sine, parameter changes, multiple blocks, and subsequent dry mode. | Delay −6,400 / −6,338 cycles versus the prior candidate. |

The harmony limiter block API was tested and reverted: it separated limiter
processing from post-peak diagnostics, adding a full pass and about 694 cycles
per block in the first 44.1 kHz candidate. The scalar limiter loop remains,
with block-local diagnostic peaks.

The optional input fusion experiment was also reverted. At 44.1 kHz it
interleaved HPF, scalar gate, and scalar compressor in one loop. Its host
input-conditioning and full-chain output CRCs matched the retained pipeline
exactly. On the P4, the fused input section used 32,805 cycles per block versus
32,234 cycles for the separate HPF + gate + compressor sections; pipeline
average was 697.012 versus 696.578 µs. This is no measurable improvement and
would remove useful profiler attribution. The separate block paths remain.

## ESP32-P4 results

Stage figures below are average cycles per block. The Phase A/B column includes
gate, compressor, diagnostic localization, spatial dispatch, and master fusion.
Phase C adds only the delay block path. Baseline `MasterMix + MasterLimiter`
must be compared with candidate `MasterMix`, which now includes both; the
enclosing `Master` section is used here. The stage profiler includes warmup
blocks while B4D.12 distribution statistics exclude them.

| Rate | Variant | DSP avg µs | Median µs | P99 µs | Worst µs | Missed blocks / 60 s | Pipeline cycles | Gate cycles | Compressor cycles | Master cycles | Delay cycles |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 44.1 kHz | Baseline | 728.976 | 768 | 1203 | 2867 | 57 | 260,612 | 3,466 | 27,783 | 8,632 | 19,554 |
| 44.1 kHz | Phase A/B | 714.020 | 754 | 1179 | 2906 | 58 | 255,270 | 2,665 | 26,950 | 5,559 | 19,569 |
| 44.1 kHz | Phase C | 696.578 | 738 | 1158 | 2792 | 58 | 248,941 | 2,715 | 26,863 | 5,541 | 13,170 |
| 48 kHz | Baseline | 728.134 | 774 | 1192 | 2863 | 77 | 260,189 | 3,476 | 27,501 | 8,549 | 19,403 |
| 48 kHz | Phase A/B | 708.623 | 741 | 1183 | 2843 | 68 | 253,307 | 2,632 | 26,637 | 5,668 | 19,828 |
| 48 kHz | Phase C | 697.328 | 741 | 1169 | 2808 | 63 | 249,275 | 2,662 | 26,736 | 5,475 | 13,065 |

Full requested stage attribution, average cycles per block (baseline → final):

| Stage | 44.1 kHz | 48 kHz |
|---|---:|---:|
| Pipeline | 260,612 → 248,941 | 260,189 → 249,275 |
| InputHpf | 2,694 → 2,656 | 2,666 → 2,630 |
| InputGate | 3,466 → 2,715 | 3,476 → 2,662 |
| Compressor | 27,783 → 26,863 | 27,501 → 26,736 |
| Harmony | 103,750 → 103,964 | 103,021 → 104,960 |
| HarmonySlewPan | 3,471 → 3,420 | 3,340 → 3,381 |
| HarmonyLimiter | 4,375 → 4,330 | 4,342 → 4,320 |
| BusMixing | 2,130 → 2,130 | 2,014 → 2,163 |
| DelayPrep | 1,396 → 1,346 | 1,473 → 1,266 |
| Delay | 19,554 → 13,170 | 19,403 → 13,065 |
| ReverbPrep | 2,539 → 2,270 | 2,528 → 2,168 |
| Reverb | 26,512 → 26,497 | 26,175 → 26,120 |
| MasterMix | 1,712 → 4,643 | 1,693 → 4,591 |
| MasterLimiter | 5,469 → 0 (fused) | 5,429 → 0 (fused) |
| Master | 8,632 → 5,541 | 8,549 → 5,475 |

At 44.1 kHz, the 64-frame deadline is 1451.25 µs. Final average utilization
was 48.0%, leaving 754.7 µs average slack; baseline was 50.2%, leaving
722.3 µs. At 48 kHz, the deadline is 1333.33 µs. Final average utilization
was 52.3%, leaving 636.0 µs; baseline was 54.6%, leaving 605.2 µs. The
worst observed blocks exceeded both deadlines, and misses remained. These
structural changes recovered about 31–32 µs of average margin but did not
establish comfortable worst-case real-time operation.

Only one 60-second **profiled** run was captured per variant and rate. Two
additional unprofiled 44.1 kHz baseline runs averaged 642.935 and 645.352 µs,
a 2.417 µs spread; profiler overhead raises absolute times. The stage changes
for gate, master, and delay repeat at both rates and are larger than the small
routing/harmony fluctuations. The total and tail numbers still need longer
repeat runs before treating the 9–14 fewer misses at 48 kHz as a reliable
improvement. No host timing is used for a P4 performance claim.

## Correctness and build

`make test` passed 43/43 host tests. New scalar/block tests require exact float
equality for gate, compressor, fused master output, and delay; their observed
maximum numerical difference was zero. Parameter changes and consecutive
blocks are covered. A separate host build of baseline commit `6387476`
produced identical output CRCs to the final build for all seven deterministic
`host_headless_compare` modules, including FullChain (`0xA1D62ACE`), and its
3,750-block test J (`0xB5B0F372`). This is an end-to-end bit-exact check for
those inputs. ESP-IDF v5.3 built the firmware for `esp32p4` both with
profiling enabled for measurement and with the production setting restored.
The DSP algorithms, sample rate, block size, filter orders, precision, reverb
topology, and pitch-shift implementation were not changed.

## Recommendations

Keep the gate, compressor, master, and delay changes. Treat the small
DelayPrep and harmony diagnostic stage differences as inconclusive for speed;
their structure is simpler and preserves behavior. Do not fuse the harmony
and input paths yet: harmony fusion would collapse useful profiler sections,
and the measured input HPF/gate sections are already small compared with the
compressor and pitch shift. A dedicated A/B experiment is needed before
accepting that coupling. FDN reverb needs no structural churn on this evidence.
The remaining deadline misses correlate with long pitch/harmony blocks more
than the simple buffer passes; investigate worst-case events separately,
without changing audio quality in this pass.
