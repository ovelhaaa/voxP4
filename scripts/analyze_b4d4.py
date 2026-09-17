#!/usr/bin/env python3
"""B4D.4 parser -> artifacts/alpha01d CSVs.

Usage:
  python3 scripts/analyze_b4d4.py <after_serial> [--forensic <serial>]
                                  [--outdir artifacts/alpha01d]
"""

import argparse
import csv
import os
import re
from collections import OrderedDict

SECS = ["mark_selection", "grain_scheduling", "grain_history_lookup",
        "plain_window_ola", "lpc_residual_fir", "lpc_model_lookup",
        "lpc_warp_poly", "lpc_warp_gainnorm", "lpc_window_ola",
        "lpc_synthesis_iir", "lpc_state_shift", "formant_gain_match",
        "formant_soft_clip", "formant_blend", "fallback", "articulation",
        "plosive_bridge", "telemetry", "other", "total"]


def kv(text):
    return dict(re.findall(r"(\w+)=([^\s]+)", text))


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
    pitch = {}
    analysis = {}
    fifo = {}
    cls = {}
    clss = {}
    mem = {}
    transport = {}
    stack = {}
    cfg = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("B4D1_CASE"):
                d = kv(line)
                n = d.get("name")
                if n:
                    if n not in order:
                        order.append(n)
                    cases[n] = {"CASE": d}
            if line.startswith("B4D4_ANALYSIS_CONFIG"):
                cfg.append(kv(line))
            n = kv(line).get("name")
            if n is None:
                if line.startswith("B4D1_MEMORY"):
                    d = kv(line)
                    mem[d.get("tag", "?")] = d
                elif line.startswith("B4D1_STACK"):
                    stack = kv(line)
                continue
            if line.startswith("B4D4_PITCH_AGE"):
                pitch[n] = kv(line)
            elif line.startswith("B4D4_ANALYSIS"):
                analysis[n] = kv(line)
            elif line.startswith("B4D4_FIFO"):
                fifo[n] = kv(line)
            elif line.startswith("B4D4_CLASS_SECTION"):
                d = kv(line)
                clss.setdefault(n, {})[(inum(d, "bucket"), d.get("sec"))] = d
            elif line.startswith("B4D4_CLASS"):
                cls.setdefault(n, []).append(kv(line))
            elif line.startswith("B4D1_TRANSPORT"):
                transport[n] = kv(line)
            elif line.startswith("B4D1_") and line.split()[0][5:] in (
                    "DSP", "LOOP", "DEADLINES", "MC", "TXPREP", "SECTIONS",
                    "QUALIFY", "PHYSICAL"):
                cases.setdefault(n, {})[line.split()[0][5:]] = kv(line)
    return dict(cases=cases, order=order, pitch=pitch, analysis=analysis,
                fifo=fifo, cls=cls, clss=clss, mem=mem, transport=transport,
                stack=stack, cfg=cfg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("after")
    ap.add_argument("--forensic", default=None)
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir
    os.makedirs(od, exist_ok=True)
    A = parse(args.after)
    F = parse(args.forensic) if args.forensic else A

    def pick(R, name):
        return R["cases"].get(name, {})

    # analysis config
    with open(os.path.join(od, "b4d4_analysis_config.txt"), "w") as fh:
        for d in A["cfg"]:
            fh.write(" ".join("%s=%s" % (k, v) for k, v in d.items()) + "\n")

    # analysis throughput / pitch age / lpc invalid
    rows = []
    for n in A["order"]:
        p = A["pitch"].get(n, {})
        a = A["analysis"].get(n, {})
        f = A["fifo"].get(n, {})
        if not p:
            continue
        rows.append([n, inum(p, "rate"), fnum(p, "avg_ms"), fnum(p, "p95_ms"),
                     fnum(p, "p99_ms"), fnum(p, "max_ms"), fnum(p, "latest_ms"),
                     fnum(p, "backlog_ms"), fnum(p, "backlog_max_ms"),
                     inum(a, "lpc_frames"), inum(a, "lpc_frames_per_s"),
                     fnum(a, "invalid_pct"), inum(f, "drops"),
                     inum(f, "overflow_attempts"), inum(f, "max_occ"),
                     fnum(f, "avg_occ")])
    write_csv(os.path.join(od, "b4d4_analysis_throughput.csv"),
              ["case", "rate", "pitch_age_avg_ms", "pitch_age_p95_ms",
               "pitch_age_p99_ms", "pitch_age_max_ms", "pitch_age_latest_ms",
               "backlog_ms", "backlog_max_ms", "lpc_frames", "lpc_frames_per_s",
               "lpc_invalid_pct", "fifo_drops", "fifo_overflow",
               "fifo_max_occ", "fifo_avg_occ"], rows)
    write_csv(os.path.join(od, "b4d4_pitch_age_audit.csv"),
              ["case", "rate", "pitch_age_avg_ms", "pitch_age_max_ms",
               "backlog_ms", "backlog_max_ms"], 
              [[n, inum(A["pitch"].get(n, {}), "rate"),
                fnum(A["pitch"].get(n, {}), "avg_ms"),
                fnum(A["pitch"].get(n, {}), "max_ms"),
                fnum(A["pitch"].get(n, {}), "backlog_ms"),
                fnum(A["pitch"].get(n, {}), "backlog_max_ms")]
               for n in A["order"] if n in A["pitch"]])
    write_csv(os.path.join(od, "b4d4_lpc_invalid.csv"),
              ["case", "lpc_frames", "lpc_invalid", "invalid_pct",
               "unvoiced", "solve_fail"],
              [[n, inum(A["analysis"].get(n, {}), "lpc_frames"),
                inum(A["analysis"].get(n, {}), "lpc_invalid"),
                fnum(A["analysis"].get(n, {}), "invalid_pct"),
                inum(A["analysis"].get(n, {}), "unvoiced"),
                inum(A["analysis"].get(n, {}), "solve_fail")]
               for n in A["order"] if n in A["analysis"]])

    # block classes (forensic run)
    rows = []
    for n in F["order"]:
        for d in F["cls"].get(n, []):
            rows.append([n, d.get("cls"), inum(d, "count"), fnum(d, "avg"),
                         inum(d, "p50"), inum(d, "p90"), inum(d, "p95"),
                         inum(d, "p99"), inum(d, "p999"), inum(d, "max"),
                         inum(d, "misses"), fnum(d, "miss_pct")])
    write_csv(os.path.join(od, "b4d4_block_classes.csv"),
              ["case", "class", "count", "dsp_avg_us", "p50", "p90", "p95",
               "p99", "p999", "max", "misses", "miss_pct"], rows)

    # grain marginal cost from the 1v_core class sections (bucket1 - bucket0)
    key = None
    for n in F["clss"]:
        if n.startswith("1v_core"):
            key = n
            break
    rows = []
    if key:
        b0 = F["clss"][key]
        for sec in SECS:
            d0 = b0.get((0, sec), {})
            d1 = b0.get((1, sec), {})
            a0 = fnum(d0, "avg_us")
            a1 = fnum(d1, "avg_us")
            rows.append([sec, a0, a1, round(a1 - a0, 2)])
    write_csv(os.path.join(od, "b4d4_grain_marginal_cost.csv"),
              ["section", "nmc0_us", "nmc1_us", "marginal_us"], rows)

    # per-class section breakdowns
    for bucket, name in ((0, "nmc0"), (1, "nmc1"), (2, "mc1"),
                         (3, "mc23")):
        rws = []
        for sec in SECS:
            d = F["clss"].get(key, {}).get((bucket, sec), {}) if key else {}
            rws.append([sec, fnum(d, "avg_us")])
        write_csv(os.path.join(od, "b4d4_%s_breakdown.csv" % name),
                  ["section", "avg_us"], rws)

    # before/after
    write_csv(os.path.join(od, "b4d4_before.csv"),
              ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs"],
              [["1v_fullfx_44100_60s", 1296.85, 1935, 3359, 3740, 16836,
                40131.972]])
    rows = []
    for n in A["order"]:
        c = A["cases"].get(n, {})
        d = c.get("DSP", {})
        p = c.get("PHYSICAL", {})
        rows.append([n, fnum(d, "avg"), inum(d, "p99"), inum(d, "p999"),
                     inum(d, "max"), inum(d, "misses"), fnum(p, "effective_fs"),
                     fnum(p, "err_pct"), c.get("QUALIFY", {}).get("result", "?")])
    write_csv(os.path.join(od, "b4d4_after.csv"),
              ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs", "fs_err_pct", "result"], rows)

    # renderer candidates
    write_csv(os.path.join(od, "b4d4_renderer_candidates.csv"),
              ["candidate", "section", "marginal_us", "status", "bit_identical"],
              [["ola_index_progression", "lpc_window_ola", 42.21, "ADOPTED",
                "YES"],
               ["model_lookup_cache", "lpc_model_lookup", 51.72, "CANDIDATE",
                ""],
               ["mark_selection", "mark_selection", 57.99, "CANDIDATE", ""],
               ["synthesis_state", "lpc_synthesis_iir+lpc_state_shift", 60.79,
                "CANDIDATE", ""],
               ["residual_fir", "lpc_residual_fir", 28.18, "CANDIDATE", ""]])

    # correlations placeholder (class-level conditional means)
    rows = []
    for n in F["order"]:
        for d in F["cls"].get(n, []):
            rows.append([n, d.get("cls"), inum(d, "count"), fnum(d, "avg"),
                         inum(d, "p99")])
    write_csv(os.path.join(od, "b4d4_grain_correlations.csv"),
              ["case", "class", "count", "dsp_avg_us", "dsp_p99"], rows)

    # memory / transport
    write_csv(os.path.join(od, "b4d4_memory.csv"),
              ["tag", "internal_free", "internal_largest", "spiram_free",
               "spiram_largest"],
              [[t, inum(d, "internal_free"), inum(d, "internal_largest"),
                inum(d, "spiram_free"), inum(d, "spiram_largest")]
               for t, d in A["mem"].items()])
    write_csv(os.path.join(od, "b4d4_transport.csv"),
              ["case", "rx_overruns", "tx_underruns", "dma_errors",
               "rx_dropped", "tx_dropped"],
              [[n, inum(A["transport"].get(n, {}), "rx_overruns"),
                inum(A["transport"].get(n, {}), "tx_underruns"),
                inum(A["transport"].get(n, {}), "dma_errors"),
                inum(A["transport"].get(n, {}), "rx_dropped_frames"),
                inum(A["transport"].get(n, {}), "tx_dropped_frames")]
               for n in A["order"] if n in A["transport"]])

    # raw fullfx dump
    with open(os.path.join(od, "b4d4_fullfx_44100_60s.txt"), "w") as fh:
        for n in A["order"]:
            if "fullfx" in n:
                for tag, d in A["cases"][n].items():
                    fh.write("B4D1_%s %s\n" % (tag, " ".join(
                        "%s=%s" % (k, v) for k, v in d.items())))
    print("B4D.4 artifacts written to", od)


if __name__ == "__main__":
    main()
