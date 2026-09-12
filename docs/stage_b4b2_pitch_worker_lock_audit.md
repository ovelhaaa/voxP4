# Stage B4B.2 — Pitch Worker Cadence & Lock Acquisition Audit Report

Date: 2026-09-12  
Target: ESP32-P4 revision 1.3, ESP-IDF v5.3  
Stage: `B4B_2_PITCH_WORKER_LOCK_AUDIT`, 15 seconds

## Stimulus and transport

The hardware run used 500 ms of silence, one 20 ms attack, and a constant
220.0 Hz pure sine at -18 dBFS for the remainder of the test. PCM1808 RX DMA,
real I2S reads, I2S TX DMA, and PCM5102 output remained active. The ADC payload
was replaced before every DSP tap.

The periodic identity check reported 236 checks and zero mismatches. Synthetic,
pitch-tap, and DSP-input RMS bit patterns and FNV checksums were identical in
every sampled block.

## Results

1. Pitch worker execution rate: **8.47 calls/s** (127 iterations, wakeups, and
   yields).
2. Analysis throughput: **28.27 hops/s**.
3. The nominal approximately 200 hops/s was **not reached**.
4. Hops per call: average 3.3386, maximum 4; 21 zero-hop, 0 one-hop, and 106
   multi-hop calls; 508 requested and 424 processed.
5. FIFO backlog: current 2047, average 1940.29, maximum 2047; 241,984 pushes
   and 25,951 pops. It accumulated and remained saturated.
6. FIFO drops and overflow attempts: **213,986** each.
7. Maximum worker backlog: **84,973 samples / 1,770.271 ms**. End backlog was
   84,333 samples / 1,756.938 ms. Algorithmic latency was separately measured
   as 1,039 samples / 21.646 ms.
8. Pitch age: average 1,553.747 ms, P95 <= 1,800 ms, P99 <= 1,800 ms, maximum
   1,790.583 ms, and final 1,778.583 ms. Percentiles are 50 ms histogram bounds.
9. The sine was initially observed near 220 Hz during acquisition (220.344 Hz),
   but was not detected correctly over the run: mean reported F0 was 122.5116
   Hz and final F0 was 118.7450 Hz while processing stale data.
10. Mean absolute F0 error was 1,021.591 cents; maximum was 1,168.301 cents.
    These values describe the starved/backlogged run and are not an isolated
    detector-quality measurement.
11. The tracker did **not** reach `LOCKED`.
12. Time to `LOCKED`: not reached (reported as 0 ms).
13. Maximum `coherent_marks`: **1**.
14. There were 374 coherent-mark resets, all classified as mark timeout. There
    were no correlation observations, accepted marks, or rejected marks: the
    stale analysis position timed out before coherence correlation could be
    exercised.
15. PSOLA usable: **no**.
16. Grain schedule attempts: **0**.
17. Grains rendered: **0**.
18. No 8–9.5 ms `PureDsp` spike was reproduced. Context flags were captured,
    but this run cannot attribute the earlier B4B.1 spikes to DSP or harness.
19. RX/TX diagnostic event-queue overflows were 529/530. Actual RX/TX DMA
    errors, I2S read/write failures, and dropped audio frames were all zero.
    RX event producer/consumer rates were 1,043.87/1,008.27 per second, with
    current/max estimated diagnostic depth 534/534. TX write/completion rates
    were 1,008.27/1,043.87 per second, with current/max depth 0/5. The evidence
    identifies diagnostic event delivery/accounting pressure, not a hardware
    RX overrun or TX underrun; the RX application consumer continued running
    but lagged the callback producer.
20. The B4B.1 spinlock crash was **not reproduced**.
21. Teardown completed cleanly in the enforced order: both tasks confirmed
    stopped, callbacks gated, RX disabled, TX disabled, driver queues deleted,
    and DMA buffers released. Heap was stable from 5 s to 10 s
    (104,827 internal / 32,782,272 PSRAM bytes at both snapshots). The positive
    end-minus-baseline delta reflects fixed task/driver resources released at
    teardown, not a monotonic leak.
22. Final classification: **`PITCH_WORKER_STARVATION`**.

The first failed prerequisite is `synthetic input -> timely analysis`. Because
the worker does not keep up, `Injected F0 -> Detected F0` cannot be evaluated
as an independent YIN-stability test, and mark coherence is invalidated by
history timeout. No DSP algorithm, YIN parameter, voicing threshold,
coherent-mark threshold, PSOLA guard, FIFO size, or continuity policy was
changed.

## Platform observations

- PSRAM device: 32 MB HEX; reported runtime speed: 20 MHz.
- Physical flash: 16 MB; binary image header: 2 MB. This mismatch is deferred.
- APLL initialization fell back to XTAL and was classified
  `EXPECTED_FALLBACK`.
- B4C remains not ready.
- The optional harmonic-tone test was not run because the pure-sine test did
  not reach `LOCKED` or render grains.
