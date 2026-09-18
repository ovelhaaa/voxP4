#!/usr/bin/env python3
"""B4D.7 parser -> artifacts/alpha01d/b4d7_* ."""
from __future__ import annotations
import argparse, csv, os, re

SECS = ["mark_selection", "grain_scheduling", "grain_history_lookup",
        "plain_window_ola", "lpc_residual_fir", "lpc_model_lookup",
        "lpc_warp_poly", "lpc_warp_gainnorm", "lpc_window_ola",
        "lpc_synthesis_iir", "lpc_state_shift", "formant_gain_match",
        "formant_soft_clip", "formant_blend", "fallback", "articulation",
        "plosive_bridge", "telemetry", "other", "total"]


def kv(line):
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


def parse_summary(path):
    out = {"DSP": {}, "PHYSICAL": {}, "MC": {}, "DEADLINES": {}, "TRANSPORT": {},
           "cls": []}
    with open(path, errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("B4D1_") and not line.startswith("B4D4_CLASS"):
                continue
            if "name=1v_fullfx_44100_60s" not in line and \
               "name=1v_fullfx_44100_60s" not in line:
                if not line.startswith("B4D4_CLASS name=1v_fullfx_44100_60s"):
                    continue
            if line.startswith("B4D4_CLASS"):
                out["cls"].append(kv(line))
                continue
            tag = line.split()[0][5:]
            if tag in out:
                out[tag] = kv(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir

    runs = [("production_run1", "artifacts/alpha01d/b4d7_production_raw.txt"),
            ("production_run2", "artifacts/alpha01d/b4d7_production_raw_run2.txt")]
    rows = []
    for tag, path in runs:
        if not os.path.exists(path):
            continue
        s = parse_summary(path)
        d, p, mc, dl = s["DSP"], s["PHYSICAL"], s["MC"], s["DEADLINES"]
        rows.append([tag, fnum(d, "avg"), inum(d, "p50"), inum(d, "p90"),
                     inum(d, "p95"), inum(d, "p99"), inum(d, "p999"),
                     inum(d, "max"), inum(d, "misses"),
                     fnum(p, "effective_fs"), fnum(p, "err_pct"),
                     inum(mc, "mc_miss"), inum(mc, "nmc_miss"),
                     inum(dl, "max_consecutive_late"),
                     inum(dl, "max_lateness_us")])
    write_csv(os.path.join(od, "b4d7_fullfx_matrix.csv"),
              ["run", "dsp_avg_us", "p50", "p90", "p95", "p99", "p999", "max",
               "dsp_misses", "effective_fs", "err_pct", "mc_misses",
               "nmc_misses", "max_consecutive_late", "max_lateness_us"], rows)

    # per-class distribution
    crows = []
    for tag, path in runs:
        if not os.path.exists(path):
            continue
        s = parse_summary(path)
        for c in s["cls"]:
            crows.append([tag, c.get("cls"), inum(c, "count"), fnum(c, "avg"),
                          inum(c, "p50"), inum(c, "p99"), inum(c, "max"),
                          inum(c, "misses"), fnum(c, "miss_pct")])
    write_csv(os.path.join(od, "b4d7_class_distribution.csv"),
              ["run", "class", "count", "dsp_avg_us", "p50", "p99", "max",
               "misses", "miss_pct"], crows)

    # per-section MC decomposition (from the class-profiled diagnostic run)
    cls_path = "artifacts/alpha01d/b4d7_class_raw.txt"
    sec_map = {}
    blocks_map = {}
    if os.path.exists(cls_path):
        with open(cls_path, errors="replace") as fh:
            for line in fh:
                if not line.startswith("B4D4_CLASS_SECTION"):
                    continue
                d = kv(line)
                if not d.get("name", "").startswith("1v_fullfx"):
                    continue
                b = inum(d, "bucket")
                sec_map.setdefault(b, {})[d.get("sec")] = fnum(d, "avg_us")
                blocks_map[b] = inum(d, "blocks")
    labels = {0: "NMC0", 1: "NMC1", 4: "MC0", 5: "MC1", 6: "MC2", 7: "MC3p"}
    mrows = []
    for sec in SECS:
        row = [sec]
        for b in (0, 1, 5, 6, 7):
            row.append(round(sec_map.get(b, {}).get(sec, 0.0), 2))
        mrows.append(row)
    write_csv(os.path.join(od, "b4d7_mc_decomposition.csv"),
              ["section", "NMC0", "NMC1", "MC1", "MC2", "MC3p"], mrows)

    # tail probe: pathological LATE_BLOCK records with phase timings
    probe_paths = ["artifacts/alpha01d/b4d7_probe5_raw.txt",
                   "artifacts/alpha01d/b4d7_auditgate_raw.txt"]
    prows = []
    for path in probe_paths:
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as fh:
            for line in fh:
                if not line.startswith("B4D1_LATE_BLOCK"):
                    continue
                d = kv(line)
                if not d.get("name", "").startswith("1v_fullfx"):
                    continue
                if inum(d, "new_grains") < 1:
                    continue
                prows.append([d.get("name"), inum(d, "blk"), inum(d, "dsp_us"),
                              inum(d, "mc"), inum(d, "new_grains"),
                              inum(d, "active_desc"), inum(d, "slices"),
                              inum(d, "exp_warp"), inum(d, "att"),
                              fnum(d, "sch_us"), fnum(d, "ag_us"),
                              fnum(d, "df_us"), fnum(d, "mk_us"),
                              fnum(d, "wp_us"), fnum(d, "dc_us"),
                              fnum(d, "nr_us"), fnum(d, "po_us"),
                              fnum(d, "gn_us"), fnum(d, "cl_us"),
                              inum(d, "mn"), inum(d, "wh"), inum(d, "wm")])
    write_csv(os.path.join(od, "b4d7_tail_probe.csv"),
              ["name", "blk", "dsp_us", "mc", "new_grains", "active_desc",
               "slices", "exp_warp", "attempts", "sched_us", "addgrain_us",
               "deferred_us", "mark_us", "warp_us", "desc_us", "near_us",
               "poly_us", "gainnorm_us", "cache_lookup_us", "model_new",
               "warp_hits", "warp_misses"], prows)

    # candidates ledger
    write_csv(os.path.join(od, "b4d7_candidates.csv"),
              ["candidate", "hypothesis", "baseline_cycles", "candidate_cycles",
               "MC1_delta", "MC2_delta", "MC3_delta", "DSP_avg_delta",
               "P99_delta", "P999_delta", "max_delta", "misses_before",
               "misses_after", "RAM_delta", "bit_identical", "status",
               "reason"],
              [["share_common_MC_prep",
                "warp/GainNorm recomputed per grain in a burst", "", "",
                "0", "0", "0", "-0.7", "-1", "-53", "-22", "79", "63", "0",
                "yes", "REJECTED",
                "measured mn=1 (single model) and wh=2 (warp cache hits): no "
                "duplicated warp/GainNorm work"],
               ["forensic_source_grain_audit_off",
                "B4C.7 source-grain duplicate audit (64-entry scan) runs in "
                "production", "", "", "-40", "-80", "-95", "-0.7", "-1",
                "-53", "-22", "79", "63", "-0", "yes", "ADOPTED",
                "descriptor phase dc_us fell 100->20 us on MC+2/3; audit-only "
                "per §23; exact"],
               ["binary_search_select_mark",
                "O(56) linear mark scan is cache-sensitive", "", "", "", "",
                "", "", "", "", "", "63", "63", "0", "n/a", "REJECTED",
                "not isolated; marks array is hot, scan is not the dominant "
                "term once descriptor audit removed"],
               ["block_level_grain_launch_restructure",
                "keep marks/model hot across the burst; one model prep", "",
                "", "", "", "", "", "", "", "", "63", "63", "0", "n/a",
                "CANDIDATE (not implemented)",
                "requires scheduler restructure; risk to grain timing"]])
    print("B4D.7 artifacts written to", od)


if __name__ == "__main__":
    main()
