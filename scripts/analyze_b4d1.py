#!/usr/bin/env python3
"""B4D.1 serial parser -> artifacts/alpha01d CSVs.

Parses B4D1_* tagged serial output from the Stage B4D.1 qualification build and
generates the CPU/FX budget, MC contingency, TX-prep, F0-matrix, memory and
stack artifacts required by the stage.  The firmware suite may have been run
more than once in a capture; the last complete occurrence of each case is used.

Usage:
  python3 scripts/analyze_b4d1.py <serial.txt> [--outdir artifacts/alpha01d]
"""

import argparse
import csv
import os
import re
from collections import OrderedDict

DEADLINE_US = 1333.3333333333


def parse_kv(text):
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("serial")
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    cases = OrderedDict()
    order = []
    slow_blocks = {}
    late_blocks = {}
    voice_cat = {}
    raw_case_lines = {}
    memory_rows = []
    stack_rows = []

    with open(args.serial, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line.startswith("B4D1_"):
                continue
            tag = line.split()[0][5:]
            kv = parse_kv(line)
            name = kv.get("name")
            if tag == "MEMORY":
                memory_rows.append(kv)
                continue
            if tag == "STACK":
                stack_rows.append(kv)
                continue
            if tag == "CASE" and name:
                if name not in cases:
                    order.append(name)
                    cases[name] = {}
                    slow_blocks[name] = []
                    late_blocks[name] = []
                    voice_cat[name] = {}
                    raw_case_lines[name] = []
                # A repeated case name starts a new (later) run: reset its data.
                cases[name] = {}
                slow_blocks[name] = []
                late_blocks[name] = []
                voice_cat[name] = {}
                raw_case_lines[name] = []
                cases[name]["CASE"] = kv
            if name is None:
                continue
            raw_case_lines.setdefault(name, []).append(line)
            if tag == "SLOW_BLOCK":
                slow_blocks.setdefault(name, []).append(kv)
            elif tag == "LATE_BLOCK":
                late_blocks.setdefault(name, []).append(kv)
            elif tag == "VOICE_CAT":
                voice_cat.setdefault(name, {})[kv.get("cat")] = kv.get("cycles")
            elif tag in ("CASE", "DSP", "LOOP", "LOOPPERIOD", "DEADLINES", "MC",
                         "MC_CYCLES", "TXPREP", "SECTIONS", "QUALIFY", "PHYSICAL",
                         "TRANSPORT", "MEMORY", "STACK", "LATE", "DEFERRED"):
                cases.setdefault(name, {})[tag] = kv

    def dsp_of(n):
        return cases.get(n, {}).get("DSP", {})

    def avg_of(n):
        return fnum(dsp_of(n), "avg")

    def p99_of(n):
        return inum(dsp_of(n), "p99")

    # ── per-case timing CSV ────────────────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_timing_all.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case", "wall_s", "blocks", "expected_blocks", "effective_fs",
                    "dsp_avg_us", "dsp_p50", "dsp_p90", "dsp_p95", "dsp_p99",
                    "dsp_p999", "dsp_max", "dsp_misses", "loop_avg_us",
                    "loop_p99", "loop_p999", "loop_max", "loop_total_misses",
                    "transport_misses", "result"])
        for n in order:
            c = cases[n]
            p = c.get("PHYSICAL", {})
            d = c.get("DSP", {})
            l = c.get("LOOP", {})
            dl = c.get("DEADLINES", {})
            q = c.get("QUALIFY", {})
            w.writerow([
                n, fnum(c.get("CASE", {}), "wall_s"), inum(c.get("CASE", {}), "blocks"),
                fnum(c.get("CASE", {}), "expected_blocks"),
                fnum(p, "effective_fs"), fnum(d, "avg"), inum(d, "p50"),
                inum(d, "p90"), inum(d, "p95"), inum(d, "p99"), inum(d, "p999"),
                inum(d, "max"), inum(d, "misses"), fnum(l, "avg"), inum(l, "p99"),
                inum(l, "p999"), inum(l, "max"), inum(l, "misses"),
                inum(dl, "transport_misses"), q.get("result", "?")])

    for name, out in (("1v_core_60s", "b4d1_1v_core_timing.csv"),
                      ("1v_fullfx_60s", "b4d1_fullfx_timing.csv")):
        with open(os.path.join(args.outdir, out), "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["case", "dsp_avg_us", "dsp_p50", "dsp_p90", "dsp_p95",
                        "dsp_p99", "dsp_p999", "dsp_max", "dsp_misses",
                        "loop_avg_us", "loop_p99", "loop_p999", "loop_max",
                        "loop_total_misses"])
            c = cases.get(name, {})
            d = c.get("DSP", {})
            l = c.get("LOOP", {})
            w.writerow([name, fnum(d, "avg"), inum(d, "p50"), inum(d, "p90"),
                        inum(d, "p95"), inum(d, "p99"), inum(d, "p999"),
                        inum(d, "max"), inum(d, "misses"), fnum(l, "avg"),
                        inum(l, "p99"), inum(l, "p999"), inum(l, "max"),
                        inum(l, "misses")])

    # ── slow blocks (voice >1200 us) ───────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_1v_slow_blocks.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["idx", "dur_us", "blk", "new_grains", "active_desc",
                    "slices", "src_samples", "fir_samples", "model_change", "f0"])
        for kv in slow_blocks.get("1v_core_60s", []):
            w.writerow([kv.get("idx"), kv.get("dur_us"), kv.get("blk"),
                        kv.get("grn"), kv.get("desc"), kv.get("slc"),
                        kv.get("src"), kv.get("fir"), kv.get("model"),
                        kv.get("f0")])

    # ── MC contingency (observed 2x2 table) ────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_mc_contingency.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case", "mc_miss", "mc_nomiss", "nmc_miss", "nmc_nomiss",
                    "p_miss_given_mc", "p_miss_given_nmc", "p_mc_given_miss",
                    "mc_dsp_avg_us", "nmc_dsp_avg_us"])
        for n in order:
            m = cases[n].get("MC", {})
            if not m:
                continue
            w.writerow([n, inum(m, "mc_miss"), inum(m, "mc_nomiss"),
                        inum(m, "nmc_miss"), inum(m, "nmc_nomiss"),
                        fnum(m, "p_miss_given_mc"), fnum(m, "p_miss_given_nmc"),
                        fnum(m, "p_mc_given_miss"), fnum(m, "mc_dsp_avg"),
                        fnum(m, "nmc_dsp_avg")])

    # ── TX prep breakdown ──────────────────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_txprep_breakdown.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case", "ramp_avg_us", "sanity_encode_avg_us", "rms_avg_us",
                    "total_avg_us", "ramp_max", "sanity_encode_max", "rms_max"])
        for n in order:
            t = cases[n].get("TXPREP", {})
            if not t:
                continue
            w.writerow([n, fnum(t, "ramp_avg"), fnum(t, "sanity_encode_avg"),
                        fnum(t, "rms_avg"), fnum(t, "total_avg"),
                        inum(t, "ramp_max"), inum(t, "sanity_encode_max"),
                        inum(t, "rms_max")])

    # ── incremental FX costs vs 1V_CORE ────────────────────────────────────
    core = avg_of("1v_core_60s")
    fx_rows = [
        ("compressor", "1v_comp_60s"),
        ("delay", "1v_delay_60s"),
        ("reverb", "1v_reverb_60s"),
    ]
    with open(os.path.join(args.outdir, "b4d1_fx_incremental.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["effect", "case", "dsp_avg_us", "dsp_p99", "dsp_p999",
                    "dsp_max", "incremental_avg_us", "incremental_p99_us"])
        for label, n in fx_rows:
            d = dsp_of(n)
            w.writerow([label, n, fnum(d, "avg"), inum(d, "p99"),
                        inum(d, "p999"), inum(d, "max"),
                        fnum(d, "avg") - core, inum(d, "p99") - p99_of("1v_core_60s")])
        # Combined full-FX
        d = dsp_of("1v_fullfx_60s")
        w.writerow(["fullfx", "1v_fullfx_60s", fnum(d, "avg"), inum(d, "p99"),
                    inum(d, "p999"), inum(d, "max"), fnum(d, "avg") - core,
                    inum(d, "p99") - p99_of("1v_core_60s")])

    # Per-effect cost files.
    for label, n, fname in (("compressor", "1v_comp_60s", "b4d1_compressor_cost.csv"),
                            ("delay", "1v_delay_60s", "b4d1_delay_cost.csv"),
                            ("reverb", "1v_reverb_60s", "b4d1_reverb_cost.csv")):
        d = dsp_of(n)
        with open(os.path.join(args.outdir, fname), "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["effect", "dsp_avg_us", "dsp_p95", "dsp_p99", "dsp_p999",
                        "dsp_max", "incremental_avg_us", "incremental_p99_us",
                        "dsp_misses", "effective_fs", "psram_bytes_owned"])
            p = cases.get(n, {}).get("PHYSICAL", {})
            w.writerow([label, fnum(d, "avg"), inum(d, "p95"), inum(d, "p99"),
                        inum(d, "p999"), inum(d, "max"), fnum(d, "avg") - core,
                        inum(d, "p99") - p99_of("1v_core_60s"),
                        inum(d, "misses"), fnum(p, "effective_fs"), ""])

    # ── FX budget (§20/§21) ────────────────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_fx_budget.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["scenario", "harmonizer_avg_us", "harmonizer_p99_us",
                    "reserve_us", "fx_budget_avg_us", "fx_budget_p99_us",
                    "safe_fx_budget_us"])
        core_avg = avg_of("1v_core_60s")
        core_p99 = p99_of("1v_core_60s")
        for label, reserve in (("10pct", DEADLINE_US * 0.10),
                               ("15pct", DEADLINE_US * 0.15)):
            budget_avg = DEADLINE_US - core_avg - reserve
            budget_p99 = DEADLINE_US - core_p99 - reserve
            w.writerow([label, round(core_avg, 2), core_p99, round(reserve, 2),
                        round(budget_avg, 2), round(budget_p99, 2),
                        round(min(budget_avg, budget_p99), 2)])

    # ── CPU budget table (§37/§38) ─────────────────────────────────────────
    sec = cases.get("1v_core_60s", {}).get("SECTIONS", {})
    with open(os.path.join(args.outdir, "b4d1_cpu_budget.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["component", "avg_us", "pct_of_deadline"])
        comps = [
            ("rx_wait", fnum(sec, "rx_wait")),
            ("rx_copy_input_conversion", fnum(sec, "rx_copy")),
            ("fixture_prestage_copy", fnum(sec, "fixture")),
            ("dsp_total", fnum(sec, "dsp")),
            ("tx_prep", fnum(sec, "tx_prep")),
            ("tx_wait", fnum(sec, "tx_wait")),
            ("bookkeeping", fnum(sec, "book")),
            ("other", fnum(sec, "other")),
            ("loop_total", fnum(sec, "loop_total")),
        ]
        for label, val in comps:
            w.writerow([label, round(val, 2), round(val / DEADLINE_US * 100.0, 2)])
        w.writerow(["deadline", DEADLINE_US, 100.0])
        for label, reserve in (("reserve_10pct", DEADLINE_US * 0.10),
                               ("reserve_15pct", DEADLINE_US * 0.15)):
            w.writerow([label, round(reserve, 2),
                        round(reserve / DEADLINE_US * 100.0, 2)])

    # ── F0 matrix (§42) ────────────────────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_f0_matrix.csv"), "w",
              newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case", "f0_hz", "dsp_avg_us", "dsp_p99", "dsp_p999",
                    "dsp_max", "dsp_misses", "effective_fs", "result"])
        for n in order:
            if not n.startswith("f0_"):
                continue
            d = dsp_of(n)
            p = cases[n].get("PHYSICAL", {})
            q = cases[n].get("QUALIFY", {})
            w.writerow([n, n.replace("f0_", "").replace("_fullfx", ""),
                        fnum(d, "avg"), inum(d, "p99"), inum(d, "p999"),
                        inum(d, "max"), inum(d, "misses"),
                        fnum(p, "effective_fs"), q.get("result", "?")])

    # ── memory / stack / transport ─────────────────────────────────────────
    with open(os.path.join(args.outdir, "b4d1_memory.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["tag", "internal_free", "internal_largest", "spiram_free",
                    "spiram_largest"])
        for m in memory_rows:
            w.writerow([m.get("tag"), inum(m, "internal_free"),
                        inum(m, "internal_largest"), inum(m, "spiram_free"),
                        inum(m, "spiram_largest")])

    with open(os.path.join(args.outdir, "b4d1_stack.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["case_index", "audio_hwm_bytes", "pitch_hwm_bytes"])
        for i, s in enumerate(stack_rows):
            w.writerow([i, inum(s, "audio_hwm_bytes"), inum(s, "pitch_hwm_bytes")])

    with open(os.path.join(args.outdir, "b4d1_transport.txt"), "w") as fh:
        for n in order:
            t = cases[n].get("TRANSPORT", {})
            p = cases[n].get("PHYSICAL", {})
            if not t:
                continue
            fh.write("case=%s\n" % n)
            fh.write("  physical: " + " ".join("%s=%s" % (k, v)
                                                for k, v in p.items()) + "\n")
            fh.write("  transport: " + " ".join("%s=%s" % (k, v)
                                                 for k, v in t.items()) + "\n")

    # Raw per-case text dumps for the two headline cases.
    for name, out in (("1v_core_60s", "b4d1_1v_core_60s.txt"),
                      ("1v_fullfx_60s", "b4d1_fullfx_60s.txt")):
        with open(os.path.join(args.outdir, out), "w", encoding="utf-8") as fh:
            fh.write("\n".join(raw_case_lines.get(name, [])))
            fh.write("\n")

    print("B4D.1 artifacts written to", args.outdir)
    print("cases:", ", ".join(order))


if __name__ == "__main__":
    main()
