#!/usr/bin/env python3
"""B4D.5 parser -> artifacts/alpha01d/b4d5_* .

Usage:
  python scripts/analyze_b4d5.py artifacts/alpha01d/b4d5_raw.txt \
      [--run2 artifacts/alpha01d/b4d5_raw_run2.txt] \
      [--outdir artifacts/alpha01d]
"""
from __future__ import annotations

import argparse
import csv
import os
import pathlib
import re
from collections import OrderedDict

SECS = ["mark_selection", "grain_scheduling", "grain_history_lookup",
        "plain_window_ola", "lpc_residual_fir", "lpc_model_lookup",
        "lpc_warp_poly", "lpc_warp_gainnorm", "lpc_window_ola",
        "lpc_synthesis_iir", "lpc_state_shift", "formant_gain_match",
        "formant_soft_clip", "formant_blend", "fallback", "articulation",
        "plosive_bridge", "telemetry", "other", "total"]

GLOBAL_GROUPS = ["global_pre", "compressor", "delay", "reverb",
                 "global_post", "harmony_voice", "pipeline_residual"]


def kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def fnum(d, k, dflt=0.0):
    try:
        return float(d.get(k, dflt))
    except (TypeError, ValueError):
        return dflt


def inum(d, k, dflt=0):
    try:
        return int(d.get(k, dflt))
    except (TypeError, ValueError):
        return dflt


def write_csv(path, header, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        w.writerows(rows)


def parse(path):
    cases = OrderedDict()
    order = []
    age = {}
    gate = {}
    mark = {}
    model = {}
    warp = {}
    glob = {}          # name -> {(bucket, group): avg}
    paired = {}        # name -> [records]
    memory = {}
    transport = {}
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("B4D1_CASE") and "wall_s" in line:
                n = kv(line).get("name")
                if n:
                    if n not in order:
                        order.append(n)
                    cases[n] = {"CASE": kv(line)}
                continue
            n = kv(line).get("name")
            if line.startswith("B4D4_PITCH_AGE"):
                age[n] = kv(line)
            elif line.startswith("B4D5_AGE_GATE"):
                gate[n] = kv(line)
            elif line.startswith("B4D5_MARK_BREAKDOWN"):
                mark[n] = kv(line)
            elif line.startswith("B4D5_MODEL_BREAKDOWN"):
                model[n] = kv(line)
            elif line.startswith("B4D5_MODEL_WARP"):
                warp[n] = kv(line)
            elif line.startswith("B4D5_GLOBAL_CLASS"):
                d = kv(line)
                glob.setdefault(n, {})[(inum(d, "bucket"), d.get("group"))] = d
            elif line.startswith("B4D5_PAIRED"):
                paired.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D1_MEMORY"):
                memory[kv(line).get("tag", "?")] = kv(line)
            elif line.startswith("B4D1_TRANSPORT"):
                transport[n] = kv(line)
            elif line.startswith("B4D1_") and n:
                tag = line.split()[0][5:]
                if tag in ("DSP", "LOOP", "DEADLINES", "MC", "TXPREP",
                           "SECTIONS", "QUALIFY", "PHYSICAL", "LATE",
                           "LOOPPERIOD"):
                    cases.setdefault(n, {})[tag] = kv(line)
    return dict(cases=cases, order=order, age=age, gate=gate, mark=mark,
                model=model, warp=warp, glob=glob, paired=paired,
                memory=memory, transport=transport)


def pctl(values, q):
    if not values:
        return 0.0
    a = sorted(values)
    return float(a[min(len(a) - 1, int((len(a) - 1) * q))])


def paired_stats(records):
    """Match each NMC+1 against the nearest NMC+0 on f0/ad/sl/sg."""
    nmc1 = [r for r in records
            if inum(r, "mc") == 0 and inum(r, "ng") == 1]
    nmc0 = [r for r in records
            if inum(r, "mc") == 0 and inum(r, "ng") == 0]
    deltas = []
    for r in nmc1:
        f0 = fnum(r, "f0")
        ad = inum(r, "ad")
        sl = inum(r, "sl")
        sg = inum(r, "sg")
        best = None
        best_cost = None
        for c in nmc0:
            cost = (abs(fnum(c, "f0") - f0) / 5.0 + 2 * abs(inum(c, "ad") - ad)
                    + 2 * abs(inum(c, "sl") - sl)
                    + 0.5 * abs(inum(c, "sg") - sg))
            if best_cost is None or cost < best_cost:
                best_cost = cost
                best = c
        if best is not None:
            deltas.append((fnum(r, "dsp_us"), fnum(best, "dsp_us"),
                           fnum(r, "f0"), fnum(best, "f0")))
    return nmc1, nmc0, deltas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run1")
    ap.add_argument("--run2", default=None)
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir
    os.makedirs(od, exist_ok=True)
    R = parse(args.run1)
    R2 = parse(args.run2) if args.run2 else None

    # 1. pitch-age fix
    rows = []
    for n in R["order"]:
        a = R["age"].get(n, {})
        g = R["gate"].get(n, {})
        rows.append([n, inum(a, "rate"), fnum(a, "avg_ms"), fnum(a, "p50_ms"),
                     fnum(a, "p95_ms"), fnum(a, "p99_ms"), fnum(a, "max_ms"),
                     fnum(a, "backlog_avg_ms"), fnum(a, "backlog_p95_ms"),
                     fnum(a, "backlog_p99_ms"), fnum(a, "backlog_max_ms"),
                     inum(g, "age_valid"), inum(g, "backlog_valid"),
                     g.get("result", "?")])
    write_csv(os.path.join(od, "b4d5_pitch_age_fix.csv"),
              ["case", "rate", "age_avg_ms", "age_p50_ms", "age_p95_ms",
               "age_p99_ms", "age_max_ms", "backlog_avg_ms", "backlog_p95_ms",
               "backlog_p99_ms", "backlog_max_ms", "age_valid",
               "backlog_valid", "result"], rows)

    # 2. full-fx grain delta + reconciliation
    rows = []
    recon_rows = []
    for n in ("1v_core_44100_60s", "1v_fullfx_44100_60s"):
        c = R["cases"].get(n, {})
        mc = c.get("MC", {})
        mc_avg = fnum(mc, "mc_dsp_avg")
        nmc_avg = fnum(mc, "nmc_dsp_avg")
        delta = mc_avg - nmc_avg
        g = R["glob"].get(n, {})
        sec_delta = {}
        for grp in GLOBAL_GROUPS:
            b0 = fnum(g.get((0, grp), {}), "avg_us")
            b1 = fnum(g.get((1, grp), {}), "avg_us")
            sec_delta[grp] = b1 - b0
            rows.append([n, grp, b0, b1, round(b1 - b0, 2)])
        # Explicit sections exclude the residual group; the residual is the
        # profiled Pipeline total that no named group owns.
        explicit = sum(sec_delta[grp] for grp in GLOBAL_GROUPS
                       if grp != "pipeline_residual")
        pipeline_delta = sum(sec_delta.values())
        residual = sec_delta.get("pipeline_residual", 0.0)
        recon_rows.append([
            n, round(nmc_avg, 2), round(mc_avg, 2), round(delta, 2),
            round(explicit, 2), round(residual, 2),
            round(100.0 * explicit / delta, 2) if delta else 0.0,
            round(100.0 * explicit / pipeline_delta, 2) if pipeline_delta else 0.0,
            round(100.0 * pipeline_delta / delta, 2) if delta else 0.0])
    write_csv(os.path.join(od, "b4d5_fullfx_grain_delta.csv"),
              ["case", "section", "nmc0_us", "nmc1_us", "delta_us"], rows)
    write_csv(os.path.join(od, "b4d5_fullfx_reconciliation.csv"),
              ["case", "nmc0_dsp_avg", "nmc1_dsp_avg", "dsp_delta_us",
               "explicit_sections_us", "pipeline_residual_us",
               "recon_vs_dsp_pct", "recon_vs_pipeline_pct",
               "pipeline_over_dsp_pct"], recon_rows)

    # 3. paired blocks (fullfx contiguous window)
    recs = R["paired"].get("1v_fullfx_44100_60s", [])
    nmc1, nmc0, deltas = paired_stats(recs)
    dvals = [a - b for a, b, _, _ in deltas]
    write_csv(os.path.join(od, "b4d5_paired_blocks.csv"),
              ["metric", "value"],
              [["window_records", len(recs)], ["nmc1_blocks", len(nmc1)],
               ["nmc0_blocks", len(nmc0)], ["matched_pairs", len(deltas)],
               ["paired_delta_avg_us", round(sum(dvals) / len(dvals), 2)
                if dvals else 0.0],
               ["paired_delta_p50_us", round(pctl(dvals, 0.50), 2)],
               ["paired_delta_p95_us", round(pctl(dvals, 0.95), 2)],
               ["paired_delta_max_us", round(max(dvals), 2) if dvals else 0.0]])

    # 4. mark breakdown + candidates
    rows = []
    for n in R["order"]:
        m = R["mark"].get(n)
        if not m:
            continue
        rows.append([n, inum(m, "calls"), inum(m, "candidates"),
                     fnum(m, "candidates_per_call"), fnum(m, "scan_us_per_call"),
                     fnum(m, "total_us_per_call"),
                     fnum(m, "validity_us_per_call"),
                     fnum(m, "us_per_new_grain")])
    write_csv(os.path.join(od, "b4d5_mark_breakdown.csv"),
              ["case", "calls", "candidates", "candidates_per_call",
               "scan_us_per_call", "total_us_per_call", "validity_us_per_call",
               "us_per_new_grain"], rows)
    write_csv(os.path.join(od, "b4d5_mark_candidates.csv"),
              ["candidate", "status", "evidence"],
              [["cursor_continuation", "REJECTED",
                "marks array is refetched per grain; inputs not provably "
                "identical, and no monotonic cursor was measured"],
               ["binary_lower_bound_lookup", "REJECTED",
                "~55 candidates/call; log2(55)=5.8 probes vs 55 integer "
                "compares -- not the dominant term under Full-FX (memory-bound)"],
               ["integer_distance_key", "ADOPTED",
                "replaces per-candidate software-double fabs; bit-identical "
                "(b4d5_mark_equivalence_tests 0/2022164 mismatches)"],
               ["removal_of_index_conversions", "ADOPTED",
                "uint64 positions compared directly, no double conversion"],
               ["precomputed_mark_intervals", "REJECTED",
                "marks sorted but count<=64; interval table not justified"]])

    # 5. model lookup breakdown + candidates
    rows = []
    for n in R["order"]:
        m = R["model"].get(n)
        w = R["warp"].get(n)
        if not m:
            continue
        rows.append([n, inum(m, "calls"), inum(m, "candidates"),
                     fnum(m, "candidates_per_call"), fnum(m, "scan_us_per_call"),
                     inum(m, "copies"), fnum(m, "copies_per_call"),
                     inum(m, "repeat_copies"), fnum(m, "bytes_per_lookup"),
                     fnum(w, "near_us"), fnum(w, "cache_lookup_us"),
                     fnum(w, "cache_hit_us"), fnum(w, "coeff_copy_us"),
                     fnum(w, "warp_poly_us"), fnum(w, "gain_norm_us"),
                     inum(w, "miss_calls")])
    write_csv(os.path.join(od, "b4d5_model_lookup_breakdown.csv"),
              ["case", "calls", "candidates", "candidates_per_call",
               "scan_us_per_call", "copies", "copies_per_call",
               "repeat_copies", "bytes_per_lookup", "near_us",
               "cache_lookup_us", "cache_hit_us", "coeff_copy_us",
               "warp_poly_us", "gain_norm_us", "miss_calls"], rows)
    write_csv(os.path.join(od, "b4d5_model_candidates.csv"),
              ["candidate", "status", "evidence"],
              [["monotonic_cursor_into_model_history", "REJECTED",
                "timestamps are monotonic but the nearest index moves; no "
                "measured cursor benefit"],
               ["lower_bound_binary_search", "REJECTED",
                "16 candidates/lookup; already trivially small"],
               ["remember_previous_model_index", "REJECTED",
                "selected model changes on ~2.4 copies/lookup"],
               ["conditional_coefficient_copy", "ADOPTED",
                "copies only when a candidate beats the current best; "
                "bit-identical (b4d5_model_equivalence_tests 0 mismatches)"],
               ["pointer_to_immutable_model", "REJECTED",
                "ring slots are overwritten by publish(); ownership/lifetime "
                "not provable"]])

    # 6. nmc0 active-render conditional means (fullfx)
    buckets_ad = {}
    buckets_sl = {}
    for r in nmc0:
        ad = inum(r, "ad")
        key = ad if ad < 4 else 4
        buckets_ad.setdefault(key, []).append(fnum(r, "dsp_us"))
        sl = inum(r, "sl")
        skey = sl if sl < 4 else 4
        buckets_sl.setdefault(skey, []).append(fnum(r, "dsp_us"))
    rows = []
    for key in sorted(buckets_ad):
        v = buckets_ad[key]
        rows.append(["active_desc_%s" % ("4+" if key == 4 else key), len(v),
                     round(sum(v) / len(v), 2), round(pctl(v, 0.99), 2),
                     round(max(v), 2)])
    for key in sorted(buckets_sl):
        v = buckets_sl[key]
        rows.append(["slices_%s" % ("4+" if key == 4 else key), len(v),
                     round(sum(v) / len(v), 2), round(pctl(v, 0.99), 2),
                     round(max(v), 2)])
    write_csv(os.path.join(od, "b4d5_nmc0_active_render.csv"),
              ["bucket", "blocks", "dsp_avg_us", "dsp_p99_us", "dsp_max_us"],
              rows)

    # 7. fx by class
    rows = []
    for n in R["order"]:
        g = R["glob"].get(n)
        if not g:
            continue
        for b in range(4):
            blocks = inum(g.get((b, "global_pre"), {}), "blocks")
            if not blocks:
                continue
            for grp in GLOBAL_GROUPS:
                d = g.get((b, grp))
                if d:
                    rows.append([n, b, blocks, grp, fnum(d, "avg_us")])
    write_csv(os.path.join(od, "b4d5_fx_by_class.csv"),
              ["case", "bucket", "blocks", "group", "avg_us"], rows)

    # 8. variance runs
    rows = []
    def add_run(tag, data):
        c = data["cases"].get("1v_fullfx_44100_60s", {})
        d = c.get("DSP", {})
        p = c.get("PHYSICAL", {})
        s = c.get("SECTIONS", {})
        sw = (fnum(s, "loop_total") - fnum(s, "rx_wait") - fnum(s, "tx_wait"))
        rows.append([tag, fnum(d, "avg"), inum(d, "p99"), inum(d, "max"),
                     inum(d, "misses"), fnum(p, "effective_fs"),
                     round(sw, 2)])
    add_run("b4d5_run1", R)
    if R2:
        add_run("b4d5_run2", R2)
    rows.append(["b4d3s_historical", 1296.85, 1935, 3740, 16836, 40131.972,
                 1376.76])
    rows.append(["b4d4_historical", 1345.40, 2117, 3978, 17221, 38821.334,
                 1433.24])
    write_csv(os.path.join(od, "b4d5_variance_runs.csv"),
              ["run", "dsp_avg_us", "dsp_p99", "dsp_max", "misses",
               "effective_fs", "software_work_us"], rows)

    # 9. before / after
    write_csv(os.path.join(od, "b4d5_before.csv"),
              ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs", "software_work_us"],
              [["1v_fullfx_44100_60s", 1345.40, 2117, 3562, 3978, 17221,
                38821.334, 1433.24]])
    c = R["cases"].get("1v_fullfx_44100_60s", {})
    d = c.get("DSP", {})
    p = c.get("PHYSICAL", {})
    s = c.get("SECTIONS", {})
    sw = fnum(s, "loop_total") - fnum(s, "rx_wait") - fnum(s, "tx_wait")
    write_csv(os.path.join(od, "b4d5_after.csv"),
              ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs", "software_work_us"],
              [["1v_fullfx_44100_60s", fnum(d, "avg"), inum(d, "p99"),
                inum(d, "p999"), inum(d, "max"), inum(d, "misses"),
                fnum(p, "effective_fs"), round(sw, 2)]])

    # 10. fullfx raw dump
    with open(os.path.join(od, "b4d5_fullfx_44100_60s.txt"), "w") as fh:
        for tag, d in R["cases"].get("1v_fullfx_44100_60s", {}).items():
            fh.write("B4D1_%s %s\n" % (tag, " ".join(
                "%s=%s" % (k, v) for k, v in d.items())))
        for n in R["order"]:
            if n.startswith("1v_fullfx"):
                for tag, d in R["cases"][n].items():
                    fh.write("B4D1_%s %s\n" % (tag, " ".join(
                        "%s=%s" % (k, v) for k, v in d.items())))
    with open(os.path.join(od, "b4d5_fullfx_40000_60s.txt"), "w") as fh:
        fh.write("NOT RUN: 44.1 kHz still the primary candidate in B4D.5; "
                 "the 40 kHz fallback was not executed (switch gate §35 not "
                 "triggered because exact low-risk candidates remain).\n")

    # 11. memory / transport
    write_csv(os.path.join(od, "b4d5_memory.csv"),
              ["tag", "internal_free", "internal_largest", "spiram_free",
               "spiram_largest"],
              [[t, inum(d, "internal_free"), inum(d, "internal_largest"),
                inum(d, "spiram_free"), inum(d, "spiram_largest")]
               for t, d in R["memory"].items()])
    write_csv(os.path.join(od, "b4d5_transport.csv"),
              ["case", "rx_overruns", "tx_underruns", "dma_errors",
               "rx_dropped_frames", "tx_dropped_frames"],
              [[n, inum(R["transport"].get(n, {}), "rx_overruns"),
                inum(R["transport"].get(n, {}), "tx_underruns"),
                inum(R["transport"].get(n, {}), "dma_errors"),
                inum(R["transport"].get(n, {}), "rx_dropped_frames"),
                inum(R["transport"].get(n, {}), "tx_dropped_frames")]
               for n in R["order"] if n in R["transport"]])

    print("B4D.5 artifacts written to", od)


if __name__ == "__main__":
    main()
