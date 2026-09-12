# Stage B4B.2 scope notes

`B4B_2_PITCH_WORKER_LOCK_AUDIT` is a 15-second diagnostic stage. It keeps
PCM1808 RX and PCM5102 TX active while replacing every DSP input tap with one
constant 220 Hz, -18 dBFS pure sine after 500 ms of silence and one 20 ms
attack.

The stage adds passive counters, fixed-size event storage, and low-priority
serial reporting only. It does not alter YIN tuning, voicing thresholds, the
`best > 0.35` coherent-mark threshold, the `coherent_marks >= 3` lock rule,
the PSOLA usable guard, or the Baseline continuity policy.

The hardware currently reports a 32 MB HEX PSRAM device at a runtime speed of
20 MHz. The board has 16 MB physical flash while the image header is configured
for 2 MB. The flash-size mismatch is a later-milestone item; Stage B4B.2 does
not change the partition table, flash configuration, or PSRAM clock.

B4C must not be marked ready until pitch acquisition, diagnostic queue
overflows, and teardown are verified clean on hardware.
