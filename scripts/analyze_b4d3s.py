#!/usr/bin/env python3
"""B4D.3S sample-rate sweep parser -> artifacts/alpha01d CSVs.

Usage:
  python3 scripts/analyze_b4d3s.py <serial.txt> [--outdir artifacts/alpha01d]
"""

import argparse
import csv
import os
import re
from collections import OrderedDict

RATE_DEADLINE = {48000: 1333.3333333, 44100: 1451.2471655, 40000: 1600.0}


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
    ap.add_argument("serial")
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir
    os.makedirs(od, exist_ok=True)

    cases = OrderedDict()
    order = []
    grain = {}
    geom = {}
    analysis = {}
    reconcile = {}
    mem = {}
    stack = {}

    with open(args.serial, "r", encoding="utf-8", errors="replace") as fh:
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
            n = kv(line).get("name")
            if n is None:
                if line.startswith("B4D1_MEMORY"):
                    d = kv(line)
                    mem[d.get("tag", "?")] = d
                elif line.startswith("B4D1_STACK"):
                    stack = kv(line)
                continue
            if line.startswith("B4D3_MC_GRAIN"):
                grain.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D3S_GEOMETRY"):
                geom[n] = kv(line)
            elif line.startswith("B4D3S_ANALYSIS"):
                analysis[n] = kv(line)
            elif line.startswith("B4D3S_RECONCILE"):
                reconcile[n] = kv(line)
            elif line.startswith("B4D1_") and line.split()[0][5:] in (
                    "DSP", "LOOP", "DEADLINES", "MC", "TXPREP", "SECTIONS",
                    "QUALIFY", "PHYSICAL", "TRANSPORT"):
                cases.setdefault(n, {})[line.split()[0][5:]] = kv(line)

    # ── reconciliation gate (§16) ──────────────────────────────────────────
    recon_ok = True
    rows = []
    for n in order:
        r = reconcile.get(n, {})
        if not r:
            continue
        st = r.get("status", "?")
        if st != "OK":
            recon_ok = False
        rows.append([n, inum(r, "dsp_miss"), inum(r, "grain_miss_sum"),
                     inum(r, "exp_miss_sum"), inum(r, "grain_blocks_sum"),
                     inum(r, "count"), st])
    write_csv(os.path.join(od, "b4d3s_reconcile.csv"),
              ["case", "dsp_miss", "grain_miss_sum", "exp_miss_sum",
               "grain_blocks_sum", "count", "status"], rows)

    # ── per-rate case tables ───────────────────────────────────────────────
    def row(n):
        c = cases.get(n, {})
        d = c.get("DSP", {})
        p = c.get("PHYSICAL", {})
        s = c.get("SECTIONS", {})
        rate = int(fnum(p, "effective_fs", 0))
        sw = (fnum(s, "rx_copy") + fnum(s, "fixture") + fnum(s, "dsp") +
              fnum(s, "tx_prep") + fnum(s, "book") + fnum(s, "other"))
        hw = fnum(s, "rx_wait") + fnum(s, "tx_wait")
        return [n, fnum(d, "avg"), inum(d, "p99"), inum(d, "p999"),
                inum(d, "max"), inum(d, "misses"), fnum(p, "effective_fs"),
                round(sw, 2), round(hw, 2), fnum(s, "loop_total"),
                c.get("QUALIFY", {}).get("result", "?")]

    hdr = ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max", "dsp_misses",
           "effective_fs", "software_work_us", "hardware_wait_us",
           "loop_total_us", "result"]
    for rate, tag in ((48000, "48k"), (44100, "44100"), (40000, "40k")):
        write_csv(os.path.join(od, "b4d3s_%s.csv" % tag), hdr,
                  [row(n) for n in order if n.endswith("_%s_60s" % tag)])

    # ── full-FX loop breakdown ─────────────────────────────────────────────
    rows = []
    for rate, tag in ((48000, "48k"), (44100, "44100"), (40000, "40k")):
        n = "1v_fullfx_%s_60s" % tag
        s = cases.get(n, {}).get("SECTIONS", {})
        p = cases.get(n, {}).get("PHYSICAL", {})
        d = cases.get(n, {}).get("DSP", {})
        if not s:
            continue
        eff = fnum(p, "effective_fs")
        period = 64.0 / eff * 1e6 if eff else 0.0
        rows.append([rate, RATE_DEADLINE[rate], fnum(s, "rx_wait"),
                     fnum(s, "rx_copy"), fnum(s, "fixture"), fnum(s, "dsp"),
                     fnum(s, "tx_prep"), fnum(s, "tx_wait"), fnum(s, "book"),
                     fnum(s, "other"), fnum(s, "loop_total"),
                     round(fnum(s, "rx_copy") + fnum(s, "fixture") +
                           fnum(s, "dsp") + fnum(s, "tx_prep") +
                           fnum(s, "book") + fnum(s, "other"), 2),
                     round(fnum(s, "rx_wait") + fnum(s, "tx_wait"), 2),
                     round(period, 2), eff, fnum(p, "err_pct"),
                     inum(d, "misses"), fnum(s, "recon")])
    write_csv(os.path.join(od, "b4d3s_fullfx_loop.csv"),
              ["rate", "deadline_us", "rx_wait", "rx_copy", "fixture", "dsp",
               "tx_prep", "tx_wait", "book", "other", "loop_total",
               "software_work_us", "hardware_wait_us", "physical_period_us",
               "effective_fs", "fs_err_pct", "dsp_misses", "recon"], rows)

    # ── analysis core / PSOLA geometry ─────────────────────────────────────
    rows = []
    for n in order:
        g = geom.get(n, {})
        a = analysis.get(n, {})
        if not g:
            continue
        rows.append([n, inum(g, "rate"), fnum(g, "avg_new_grains"),
                     fnum(g, "avg_active_desc"), fnum(g, "avg_slices"),
                     fnum(g, "mc_per_s"), fnum(g, "gainnorm_per_s"),
                     fnum(g, "gainnorm_us_per_call")])
    write_csv(os.path.join(od, "b4d3s_psola_geometry.csv"),
              ["case", "rate", "avg_new_grains", "avg_active_desc", "avg_slices",
               "mc_per_s", "gainnorm_per_s", "gainnorm_us_per_call"], rows)

    rows = []
    for n in order:
        a = analysis.get(n, {})
        if not a:
            continue
        rows.append([n, inum(a, "rate"), inum(a, "lpc_frames"),
                     inum(a, "lpc_invalid"), inum(a, "lpc_fallback"),
                     fnum(a, "lpc_frames_per_s"), fnum(a, "pitch_age_ms")])
    write_csv(os.path.join(od, "b4d3s_analysis_core.csv"),
              ["case", "rate", "lpc_frames", "lpc_invalid", "lpc_fallback",
               "lpc_frames_per_s", "pitch_age_ms"], rows)

    # ── MC grain contingency ───────────────────────────────────────────────
    rows = []
    for n in order:
        for g in grain.get(n, []):
            rows.append([n, inum(g, "mc"), inum(g, "grains"),
                         inum(g, "blocks"), inum(g, "misses"), fnum(g, "p_miss")])
    write_csv(os.path.join(od, "b4d3s_mc_grain_contingency.csv"),
              ["case", "mc", "new_grains", "blocks", "misses", "p_miss"], rows)

    # ── memory / transport ─────────────────────────────────────────────────
    rows = []
    for tag, d in mem.items():
        rows.append([tag, inum(d, "internal_free"), inum(d, "internal_largest"),
                     inum(d, "spiram_free"), inum(d, "spiram_largest")])
    write_csv(os.path.join(od, "b4d3s_memory.csv"),
              ["tag", "internal_free", "internal_largest", "spiram_free",
               "spiram_largest"], rows)
    with open(os.path.join(od, "b4d3s_transport.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case", "rx_overruns", "tx_underruns", "dma_errors",
                    "rx_dropped", "tx_dropped", "rx_gaps", "tx_gaps"])
        for n in order:
            t = cases.get(n, {}).get("TRANSPORT", {})
            if t:
                w.writerow([n, inum(t, "rx_overruns"), inum(t, "tx_underruns"),
                            inum(t, "dma_errors"), inum(t, "rx_dropped_frames"),
                            inum(t, "tx_dropped_frames"),
                            inum(t, "rx_seq_gaps"), inum(t, "tx_seq_gaps")])

    print("B4D.3S artifacts written to", od)
    print("reconciliation:", "OK" if recon_ok else "FAIL")
    print("cases:", ", ".join(order))


if __name__ == "__main__":
    main()
