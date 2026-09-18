import re, statistics, csv, os

OD = "artifacts/alpha01d"

def kv(l):
    return dict(re.findall(r"(\w+)=([^\s]+)", l))

rows = []
for f in ["artifacts/alpha01d/b4d11_raw.txt", "artifacts/alpha01d/b4d11_raw2.txt"]:
    if not os.path.exists(f):
        continue
    for l in open(f, errors="replace").read().splitlines():
        if not l.startswith("B4D8_ORD"):
            continue
        d = kv(l)
        if not d.get("name", "").startswith("b4d11_s0_fullfx_44100_r0"):
            continue
        debt = int(d.get("debt", 0))
        op = float(d.get("op", 0.0))
        ng = 3 if (float(d.get("g2_wp", 0.0)) > 0 or float(d.get("g2_sel", 0.0)) > 0) else 2
        rows.append([d.get("blk"), ng, int(d.get("mc", 0)), debt, round(op, 1),
                     round(debt / op, 2) if op > 0 else 0.0,
                     round(float(d.get("g0_sel", 0.0)), 1),
                     round(float(d.get("g0_wp", 0.0)), 1)])

with open(os.path.join(OD, "b4d11_scheduler_debt.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "grains", "mark_count", "debt_samples", "output_period",
                "debt_periods", "g0_select_us", "g0_warp_us"])
    w.writerows(rows)

# phase trace: same data, debt-focused
with open(os.path.join(OD, "b4d11_phase_trace.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "grains", "mark_count", "next_mark_debt_samples",
                "output_period_samples", "debt_in_periods"])
    for r in rows:
        w.writerow([r[0], r[1], r[2], r[3], r[4], r[5]])

# summary
summary = []
for ng in (2, 3):
    v = [r for r in rows if r[1] == ng]
    if v:
        summary.append([ng, len(v), min(r[3] for r in v), max(r[3] for r in v),
                        round(statistics.median([r[3] for r in v])),
                        round(statistics.median([r[4] for r in v])),
                        round(statistics.median([r[5] for r in v]), 2)])

placeholders = {
    "b4d11_unique_coverage.csv": "status,note\nNOT_MEASURED,unique-coverage/OLA-weight analysis not implemented in B4D.11\n",
    "b4d11_ola_reference.csv": "status,note\nNOT_MEASURED,OLA weight analysis not implemented\n",
    "b4d11_ola_candidates.csv": "status,note\nNOT_MEASURED,OLA weight analysis not implemented\n",
    "b4d11_transition_matrix.csv": "status,note\nNOT_MEASURED,synthetic transition matrix not run\n",
    "b4d11_long_term_phase.csv": "status,note\nNOT_MEASURED,long-term cadence test not run\n",
    "b4d11_onset_analysis.csv": "status,note\nNOT_MEASURED,onset analysis not run\n",
    "b4d11_pitch_analysis.csv": "status,note\nNOT_MEASURED,pitch trajectory analysis not run\n",
    "b4d11_transient_analysis.csv": "status,note\nNOT_MEASURED,transient/plosive analysis not run\n",
    "b4d11_pcm_diff.txt": ("B4D.11 PCM difference: NOT MEASURED.\n"
                           "Device TX PCM hash is NOT reproducible across identical S0 runs\n"
                           "(r0 d4b7ee0f, r1 37f0bb56; tx_frames differ by one block), so a\n"
                           "device PCM A/B is not possible. Root cause: the pitch analysis\n"
                           "worker publishes asynchronously on the other core, so each audio\n"
                           "block observes a timing-dependent pitch snapshot. A deterministic\n"
                           "PCM comparison would require a host render with a frozen pitch trace.\n"),
}
for name, text in placeholders.items():
    with open(os.path.join(OD, name), "w") as fh:
        fh.write(text)

# candidate matrix
with open(os.path.join(OD, "b4d11_candidate_matrix.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["candidate", "MC1_blocks", "MC2_blocks", "MC3_blocks",
                "burst_reduction_pct", "zero_support_samples", "min_ola_weight",
                "max_phase_error_samples", "long_term_phase_drift",
                "max_grain_shift_samples", "onset_delta_ms", "pitch_p95_cents",
                "pitch_max_cents", "pcm_rms_delta", "pcm_max_delta", "DSP_avg",
                "P99", "P999", "max", "misses", "audio_gate", "cpu_gate",
                "status"])
    w.writerow(["R0_reference", 3520, 22, 30, 0, "n/a", "n/a", "n/a", "n/a",
                "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", 654.9, 1127, 2548,
                2964, 61, "n/a", "FAIL", "REFERENCE"])
    for name, note in [("R1_normalized_phase", "not implemented"),
                       ("R2_coverage_reanchor", "not implemented"),
                       ("R3_phase_slew", "not implemented"),
                       ("R4_nearest_grid", "not implemented")]:
        w.writerow([name, "", "", "", "", "", "", "", "", "", "", "", "", "",
                    "", "", "", "", "", "", "", "", "NOT IMPLEMENTED: " + note])
print("B4D.11 artifacts written; debt rows =", len(rows))
for s in summary:
    print("grains=%d n=%d debt[%d..%d] med=%d op_med=%d debt/op_med=%.2f" % tuple(s))
