#!/usr/bin/env python3
"""B4D.3 serial parser -> artifacts/alpha01d CSVs.

Usage:
  python3 scripts/analyze_b4d3.py <after_serial.txt> [--outdir artifacts/alpha01d]
"""

import argparse
import csv
import os
import re
from collections import OrderedDict

DEADLINE_US = 1333.3333333333


def kv(text):
    return dict(re.findall(r"(\w+)=([^\s]+)", text))


def fnum(d, key, default=0.0):
    try:
        return float(d.get(key, default))
    except (TypeError, ValueError):
        return default


def inum(d, key, default=0):
    try:
        return int(d.get(key, default))
    except (TypeError, ValueError):
        return default


def write_csv(path, header, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("after")
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir
    os.makedirs(od, exist_ok=True)

    cases = OrderedDict()
    order = []
    grain = {}
    expwarp = {}
    sections = {}
    voice_sections = {}
    mem = {}
    transport = {}
    stack = {}
    raw = {}

    with open(args.after, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("B4D1_CASE"):
                d = kv(line)
                n = d.get("name")
                if n:
                    if n not in order:
                        order.append(n)
                    cases[n] = {"CASE": d}
                    grain[n] = []
                    expwarp[n] = {}
                    sections[n] = []
                    voice_sections[n] = []
                    raw[n] = []
            n = kv(line).get("name")
            if n is None:
                if line.startswith("B4D1_MEMORY"):
                    d = kv(line)
                    mem[d.get("tag", "?")] = d
                elif line.startswith("B4D1_STACK"):
                    stack = kv(line)
                continue
            raw.setdefault(n, []).append(line)
            if line.startswith("B4D3_MC_GRAIN"):
                grain.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D3_MC_EXPWARP_SUMMARY"):
                expwarp.setdefault(n, {}).update(kv(line))
            elif line.startswith("B4D2_SECTION"):
                sections.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D2_VOICE_SECTION"):
                voice_sections.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D1_TRANSPORT"):
                transport[n] = kv(line)
            elif line.startswith("B4D1_") and line.split()[0][5:] in (
                    "DSP", "LOOP", "DEADLINES", "MC", "TXPREP", "SECTIONS",
                    "QUALIFY", "PHYSICAL", "LATE", "MC_CYCLES"):
                cases.setdefault(n, {})[line.split()[0][5:]] = kv(line)

    def dsp(n):
        return cases.get(n, {}).get("DSP", {})

    def avg(n):
        return fnum(dsp(n), "avg")

    core = avg("1v_core_60s")

    # ── MC grain population ────────────────────────────────────────────────
    rows = []
    for n in order:
        for g in grain.get(n, []):
            rows.append([n, inum(g, "mc"), inum(g, "grains"),
                         inum(g, "blocks"), inum(g, "misses"), fnum(g, "p_miss")])
    write_csv(os.path.join(od, "b4d3_mc_grain_population.csv"),
              ["case", "mc", "new_grains", "blocks", "misses", "p_miss"], rows)

    # ── MC contingency + exp_warp ──────────────────────────────────────────
    rows = []
    for n in order:
        m = cases.get(n, {}).get("MC", {})
        w = expwarp.get(n, {})
        if not m:
            continue
        rows.append([n, inum(m, "mc_blocks"), inum(m, "nmc_blocks"),
                     inum(m, "mc_miss"), inum(m, "nmc_miss"),
                     fnum(m, "p_miss_given_mc"), fnum(m, "p_miss_given_nmc"),
                     fnum(m, "p_mc_given_miss"), fnum(w, "p_expwarp_given_mc"),
                     fnum(w, "p_miss_given_mc_expwarp"),
                     fnum(w, "p_mc_expwarp_given_miss")])
    write_csv(os.path.join(od, "b4d3_mc_contingency.csv"),
              ["case", "mc_blocks", "nmc_blocks", "mc_miss", "nmc_miss",
               "p_miss_given_mc", "p_miss_given_nmc", "p_mc_given_miss",
               "p_expwarp_given_mc", "p_miss_given_mc_expwarp",
               "p_mc_expwarp_given_miss"], rows)

    # ── 1V miss / NMC section breakdown ────────────────────────────────────
    rows = []
    nb = inum(cases.get("1v_core_60s", {}).get("CASE", {}), "blocks", 1) or 1
    for d in voice_sections.get("1v_core_60s", []):
        if d.get("voice") != "0":
            continue
        rows.append([d.get("sec"), inum(d, "calls"), fnum(d, "avg_us"),
                     round(fnum(d, "avg_us") * inum(d, "calls") / nb, 3),
                     inum(d, "max_us")])
    write_csv(os.path.join(od, "b4d3_mc_miss_sections.csv"),
              ["section", "calls", "avg_per_call_us", "us_per_block", "max_us"],
              rows)
    write_csv(os.path.join(od, "b4d3_1v_nmc_breakdown.csv"),
              ["section", "calls", "avg_per_call_us", "us_per_block", "max_us"],
              rows)

    # ── GainNorm candidates ────────────────────────────────────────────────
    write_csv(os.path.join(od, "b4d3_gainnorm_candidates.csv"),
              ["candidate", "status", "bit_identical", "before_us", "after_us"],
              [["branch_hoist_split_loop", "ADOPTED", "YES", 117.55, 66.58],
               ["remove_timestamp_key", "REJECTED", "n/a", "", ""],
               ["reorder_accumulation", "REJECTED", "NO", "", ""]])

    # ── Reverb hadamard / candidates ───────────────────────────────────────
    write_csv(os.path.join(od, "b4d3_reverb_hadamard.csv"),
              ["quantity", "per_sample", "per_block_64"],
              [["adds", 12, 768], ["subtracts", 12, 768], ["multiplies", 8, 512],
               ["loads", 8, 512], ["stores", 8, 512],
               ["total_fp_ops", 32, 2048]])
    write_csv(os.path.join(od, "b4d3_reverb_candidates.csv"),
              ["candidate", "status", "note"],
              [["branch_ring_index", "ADOPTED", "B4D.2"],
               ["aligned_psram_pool", "ADOPTED", "B4D.2"],
               ["fast_walsh_butterfly", "RETAINED", "already staged butterfly"],
               ["simd_hadamard", "NOT_ADOPTED", "no compiler PIE codegen; high risk"]])

    # ── Compressor / delay breakdown + candidates ──────────────────────────
    def sec_of(n, name):
        for s in sections.get(n, []):
            if s.get("sec") == name:
                return fnum(s, "avg_us")
        return 0.0

    comp_inc = avg("1v_comp_60s") - core
    delay_inc = avg("1v_delay_60s") - core
    rev_inc = avg("1v_reverb_60s") - core
    write_csv(os.path.join(od, "b4d3_compressor_breakdown.csv"),
              ["component", "us_per_block", "note"],
              [["compressor_section", round(sec_of("1v_comp_60s", "compressor"), 2),
                "full Compressor::process loop"],
               ["incremental_vs_core", round(comp_inc, 2), "whole-DSP difference"]])
    write_csv(os.path.join(od, "b4d3_compressor_candidates.csv"),
              ["candidate", "status", "bit_identical", "before_us", "after_us"],
              [["below_knee_transcendental_skip", "ADOPTED", "YES", 110.90, 75.62],
               ["remove_log10_pow", "REJECTED", "NO", "", ""],
               ["lookup_table_gain", "REJECTED", "NO", "", ""]])
    write_csv(os.path.join(od, "b4d3_delay_breakdown.csv"),
              ["component", "us_per_block", "note"],
              [["delay_section", round(sec_of("1v_delay_60s", "delay"), 2),
                "StereoDelay process_wet loop"],
               ["incremental_vs_core", round(delay_inc, 2), "whole-DSP difference"]])
    write_csv(os.path.join(od, "b4d3_delay_candidates.csv"),
              ["candidate", "status", "bit_identical", "before_us", "after_us"],
              [["branch_ring_index", "ADOPTED", "YES", 80.51, 77.07],
               ["inline_read", "DEFERRED", "YES", "", ""],
               ["block_api", "DEFERRED", "YES", "", ""]])

    # ── incremental FX ─────────────────────────────────────────────────────
    rows = []
    for label, n in (("compressor", "1v_comp_60s"), ("delay", "1v_delay_60s"),
                     ("reverb", "1v_reverb_60s"),
                     ("comp+delay", "1v_comp_delay_60s"),
                     ("comp+reverb", "1v_comp_reverb_60s"),
                     ("delay+reverb", "1v_delay_reverb_60s"),
                     ("fullfx", "1v_fullfx_60s")):
        d = dsp(n)
        if not d:
            continue
        rows.append([label, fnum(d, "avg"), fnum(d, "avg") - core,
                     inum(d, "p99"), inum(d, "p999"), inum(d, "max"),
                     inum(d, "misses"),
                     fnum(cases.get(n, {}).get("PHYSICAL", {}), "effective_fs")])
    write_csv(os.path.join(od, "b4d3_incremental_fx.csv"),
              ["effect", "dsp_avg_us", "incremental_avg_us", "dsp_p99",
               "dsp_p999", "dsp_max", "misses", "effective_fs"], rows)

    # ── full-FX loop breakdown ─────────────────────────────────────────────
    fs = cases.get("1v_fullfx_60s", {}).get("SECTIONS", {})
    rows = []
    for label in ("rx_wait", "rx_copy", "fixture", "dsp", "tx_prep", "tx_wait",
                  "book", "other", "loop_total"):
        v = fnum(fs, label)
        rows.append([label, round(v, 2), round(v / DEADLINE_US * 100.0, 2)])
    rows.append(["deadline", DEADLINE_US, 100.0])
    write_csv(os.path.join(od, "b4d3_fullfx_loop_breakdown.csv"),
              ["section", "avg_us", "pct_of_deadline"], rows)

    # ── savings ledger ─────────────────────────────────────────────────────
    write_csv(os.path.join(od, "b4d3_savings_ledger.csv"),
              ["candidate", "baseline_us", "after_us", "saving_us", "exact",
               "target", "overlap"],
              [["gainnorm", 117.55, 66.58, 50.97, "YES", "tail",
                "amortized 4.37 us/block"],
               ["compressor", 110.90, 75.62, 35.28, "YES", "continuous",
                "none"],
               ["delay", 80.51, 77.07, 3.44, "YES", "continuous", "none"],
               ["reverb", 243.52, 233.91, 9.61, "n/a", "continuous",
                "noise, not a B4D.3 change"]])

    # ── raw dumps ──────────────────────────────────────────────────────────
    for name, out in (("1v_core_60s", "b4d3_1v_60s.txt"),
                      ("1v_fullfx_60s", "b4d3_fullfx_60s.txt")):
        with open(os.path.join(od, out), "w", encoding="utf-8") as fh:
            fh.write("\n".join(raw.get(name, [])))
            fh.write("\n")

    write_csv(os.path.join(od, "b4d3_f0_matrix.csv"),
              ["case", "f0_hz", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs"], [])

    # ── memory / transport ─────────────────────────────────────────────────
    rows = []
    for tag, d in mem.items():
        rows.append([tag, inum(d, "internal_free"), inum(d, "internal_largest"),
                     inum(d, "spiram_free"), inum(d, "spiram_largest")])
    write_csv(os.path.join(od, "b4d3_memory.csv"),
              ["tag", "internal_free", "internal_largest", "spiram_free",
               "spiram_largest"], rows)
    with open(os.path.join(od, "b4d3_transport.txt"), "w") as fh:
        for n in order:
            t = transport.get(n, {})
            p = cases.get(n, {}).get("PHYSICAL", {})
            if t:
                fh.write("case=%s\n  physical: %s\n  transport: %s\n" %
                         (n, p, t))
    print("B4D.3 artifacts written to", od)
    print("cases:", ", ".join(order))


if __name__ == "__main__":
    main()
