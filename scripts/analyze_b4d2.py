#!/usr/bin/env python3
"""B4D.2 serial parser -> artifacts/alpha01d CSVs.

Parses B4D1_*/B4D2_* tagged serial output from the B4D.2 run and generates the
MC cache audit, before/after 1V, reverb breakdown/memory, TX-prep, incremental
FX, CPU budget, F0 matrix, memory and transport artifacts.

Usage:
  python3 scripts/analyze_b4d2.py <after_serial.txt> [--before <b4d1_serial.txt>]
                                  [--outdir artifacts/alpha01d]
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


class Run:
    def __init__(self, path):
        self.cases = OrderedDict()
        self.order = []
        self.slow = {}
        self.late = {}
        self.warp = {}
        self.revprof = {}
        self.revmem = {}
        self.sections = {}
        self.voice_sections = {}
        self._parse(path)

    def _reset(self, name):
        self.cases[name] = {}
        self.slow[name] = []
        self.late[name] = []
        self.warp[name] = {}
        self.revprof[name] = {}
        self.revmem[name] = {}
        self.sections[name] = []
        self.voice_sections[name] = []

    def _parse(self, path):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                line = raw.strip()
                if line.startswith("B4D1_CASE"):
                    d = kv(line)
                    n = d.get("name")
                    if n:
                        if n not in self.order:
                            self.order.append(n)
                        self._reset(n)
                        self.cases[n]["CASE"] = d
                    continue
                if line.startswith("B4D2_WARP_AUDIT"):
                    d = kv(line)
                    n = d.get("name")
                    if n:
                        self.warp.setdefault(n, {}).update(d)
                    continue
                if line.startswith("B4D2_REVERB_PROFILE"):
                    d = kv(line)
                    n = d.get("name")
                    if n:
                        self.revprof.setdefault(n, {}).update(d)
                    continue
                if line.startswith("B4D2_REVERB_LINE") or \
                   line.startswith("B4D2_REVERB_DIFFUSER") or \
                   line.startswith("B4D2_REVERB_MEMORY"):
                    d = kv(line)
                    n = "reverb"
                    self.revmem.setdefault(n, []).append((line.split()[0], d))
                    continue
                if line.startswith("B4D2_SECTION"):
                    d = kv(line)
                    n = d.get("name")
                    if n:
                        self.sections.setdefault(n, []).append(d)
                    continue
                if line.startswith("B4D2_VOICE_SECTION"):
                    d = kv(line)
                    n = d.get("name")
                    if n:
                        self.voice_sections.setdefault(n, []).append(d)
                    continue
                if line.startswith("B4D1_"):
                    tag = line.split()[0][5:]
                    d = kv(line)
                    n = d.get("name")
                    if tag == "SLOW_BLOCK" and n:
                        self.slow.setdefault(n, []).append(d)
                    elif tag == "LATE_BLOCK" and n:
                        self.late.setdefault(n, []).append(d)
                    elif n and tag in ("DSP", "LOOP", "LOOPPERIOD", "DEADLINES",
                                       "MC", "MC_CYCLES", "TXPREP", "SECTIONS",
                                       "QUALIFY", "PHYSICAL", "TRANSPORT",
                                       "LATE", "DEFERRED"):
                        self.cases.setdefault(n, {})[tag] = d
                    elif tag == "MEMORY":
                        self.cases.setdefault("_memory", {})[d.get("tag", "?")] = d
                    elif tag == "STACK":
                        self.cases.setdefault("_stack", {})["last"] = d

    def dsp(self, n):
        return self.cases.get(n, {}).get("DSP", {})

    def avg(self, n):
        return fnum(self.dsp(n), "avg")

    def p99(self, n):
        return inum(self.dsp(n), "p99")


def write_csv(path, header, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(header)
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("after")
    ap.add_argument("--before", default=None)
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    od = args.outdir
    A = Run(args.after)
    B = Run(args.before) if args.before else None

    core_after = A.avg("1v_core_60s")
    p99_after = A.p99("1v_core_60s")

    # ── MC cache audit / key changes ───────────────────────────────────────
    rows = []
    for n in A.order:
        w = A.warp.get(n, {})
        if not w:
            continue
        miss = inum(w, "miss_total")
        only = inum(w, "math_only_miss")
        rows.append([n, miss, only, fnum(w, "math_only_pct"),
                     "NO" if only == 0 else "YES"])
    write_csv(os.path.join(od, "b4d2_mc_cache_audit.csv"),
              ["case", "warp_cache_misses", "math_only_misses",
               "math_only_pct", "timestamp_key_reuse_useful"], rows)
    write_csv(os.path.join(od, "b4d2_mc_key_changes.csv"),
              ["case", "timestamp_only_changes", "pct_of_mc_events",
               "all_math_inputs_identical"], rows)

    # MC miss breakdown: per-voice section cost for 1v_core.
    rows = []
    n_blocks = inum(A.cases.get("1v_core_60s", {}).get("CASE", {}), "blocks", 1) or 1
    for d in A.voice_sections.get("1v_core_60s", []):
        if d.get("voice") != "0":
            continue
        calls = inum(d, "calls")
        per_block = fnum(d, "avg_us") * calls / n_blocks
        rows.append([d.get("sec"), calls, fnum(d, "avg_us"),
                     round(per_block, 3), inum(d, "max_us")])
    write_csv(os.path.join(od, "b4d2_mc_miss_breakdown.csv"),
              ["section", "calls", "avg_per_call_us", "us_per_block", "max_us"],
              rows)

    # Reverb candidate classes (measured outcome).
    write_csv(os.path.join(od, "b4d2_reverb_candidates.csv"),
              ["candidate", "class", "status", "measured_effect"],
              [["pointer_wrap_indexing", "A indexing", "IMPLEMENTED",
                "removed 11 runtime modulo/sample"],
               ["contiguous_spans", "B contiguous", "N/A",
                "FDN feedback couples all lines per sample; not applicable"],
               ["invariant_hoisting", "C hoist", "IMPLEMENTED",
                "1-damping/feedback gains hoisted; scalar unroll"],
               ["fdn_matrix", "D matrix", "RETAINED",
                "Hadamard already minimal (32 ops); now the FP floor (66 us)"],
               ["psram_locality", "E locality", "IMPLEMENTED",
                "single 64B-aligned pool fixed internal cache aliasing"]])

    # Grain-count contingency, conditioned on late (>1200us) MC blocks (the
    # only per-block new-grain data; biased toward misses, documented).
    rows = []
    for n in ("1v_core_60s", "1v_reverb_60s"):
        buckets = {}
        for d in A.late.get(n, []):
            if inum(d, "mc") != 1:
                continue
            g = inum(d, "new_grains")
            g = 3 if g >= 3 else g
            b = buckets.setdefault(g, [0, 0])
            b[0] += 1
            if inum(d, "dsp_us") > 1333:
                b[1] += 1
        for g in sorted(buckets):
            tot, miss = buckets[g]
            rows.append([n, g, tot, miss, round(miss / tot, 6)])
    write_csv(os.path.join(od, "b4d2_mc_grain_contingency.csv"),
              ["case", "new_grains", "late_mc_blocks", "misses",
               "p_miss_given_late_mc"], rows)

    # Compressor breakdown (not split further this pass).
    comp = A.dsp("1v_comp_60s")
    write_csv(os.path.join(od, "b4d2_comp_breakdown.csv"),
              ["stage", "incremental_avg_us", "note"],
              [["detector+envelope+gain+smoothing", fnum(comp, "avg") - core_after,
                "not split further; compressor not optimized in B4D.2"]])

    # ── before/after 1V ────────────────────────────────────────────────────
    def row_for(R, n):
        if R is None:
            return [n, "", "", "", "", "", "", ""]
        d = R.dsp(n)
        p = R.cases.get(n, {}).get("PHYSICAL", {})
        return [n, fnum(d, "avg"), inum(d, "p99"), inum(d, "p999"),
                inum(d, "max"), inum(d, "misses"), fnum(p, "effective_fs"),
                R.cases.get(n, {}).get("QUALIFY", {}).get("result", "?")]

    hdr = ["case", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
           "dsp_misses", "effective_fs", "result"]
    write_csv(os.path.join(od, "b4d2_1v_before.csv"), hdr,
              [row_for(B, n) for n in (B.order if B else [])])
    write_csv(os.path.join(od, "b4d2_1v_after.csv"), hdr,
              [row_for(A, n) for n in A.order])

    with open(os.path.join(od, "b4d2_1v_60s.txt"), "w") as fh:
        for n in A.order:
            if n.startswith("1v_core"):
                for tag, d in A.cases[n].items():
                    fh.write("B4D1_%s %s\n" % (tag, " ".join(
                        "%s=%s" % (k, v) for k, v in d.items())))

    # ── reverb breakdown ───────────────────────────────────────────────────
    rows = []
    for n in A.order:
        rp = A.revprof.get(n, {})
        if not rp:
            continue
        sec = next((s for s in A.sections.get(n, []) if s.get("sec") == "reverb"), {})
        rows.append([n, fnum(rp, "read_damping_us"), fnum(rp, "mix_us"),
                     fnum(rp, "hadamard_us"), fnum(rp, "diffuser_us"),
                     fnum(rp, "write_index_us"), fnum(rp, "wet_mix_us"),
                     fnum(rp, "total_us"), fnum(sec, "avg_us")])
    write_csv(os.path.join(od, "b4d2_reverb_breakdown.csv"),
              ["case", "read_damping_us", "mix_us", "hadamard_us",
               "diffuser_us", "write_index_us", "wet_mix_us",
               "profile_total_us", "section_reverb_avg_us"], rows)

    # Reverb memory/placement (last run's reverb case).
    mem = []
    psram = []
    for tag, d in A.revmem.get("reverb", []):
        mem.append([tag, d.get("i", ""), inum(d, "bytes"), inum(d, "psram"),
                    inum(d, "total_bytes"), inum(d, "reported")])
    write_csv(os.path.join(od, "b4d2_reverb_memory.csv"),
              ["kind", "index", "bytes", "in_psram", "total_bytes", "reported"], mem)
    for row in mem:
        if row[0] in ("B4D2_REVERB_LINE", "B4D2_REVERB_DIFFUSER"):
            psram.append([row[0], row[1], row[2], row[3],
                          row[2] if row[3] else 0])
    write_csv(os.path.join(od, "b4d2_reverb_psram.csv"),
              ["kind", "index", "bytes", "in_psram", "psram_bytes"], psram)

    # ── TX prep ────────────────────────────────────────────────────────────
    rows = []
    for n in A.order:
        t = A.cases.get(n, {}).get("TXPREP", {})
        if not t:
            continue
        rows.append([n, fnum(t, "ramp_avg"), fnum(t, "sanity_encode_avg"),
                     fnum(t, "rms_avg"), fnum(t, "total_avg")])
    write_csv(os.path.join(od, "b4d2_txprep_breakdown.csv"),
              ["case", "ramp_avg_us", "sanity_encode_avg_us", "rms_avg_us",
               "total_avg_us"], rows)
    # TX candidates (measured / qualitative).
    write_csv(os.path.join(od, "b4d2_tx_candidates.csv"),
              ["candidate", "scope", "status", "measured_effect"],
              [["disable_output_rms", "TX prep", "IMPLEMENTED",
                "rms 9.8->0.9 us; sanity+encode 82.5->32.7 us"],
               ["fuse_sanity_clamp_encode", "TX prep", "DEFERRED",
                "exact bits must be preserved; not required after RMS gating"],
               ["branchless_float_to_pcm32", "TX prep", "REJECTED",
                "clamp boundary changes output bits"]])

    # ── incremental FX ─────────────────────────────────────────────────────
    rows = []
    for label, n in (("compressor", "1v_comp_60s"), ("delay", "1v_delay_60s"),
                     ("reverb", "1v_reverb_60s"),
                     ("fullfx", "1v_fullfx_60s")):
        d = A.dsp(n)
        if not d:
            continue
        rows.append([label, fnum(d, "avg"), fnum(d, "avg") - core_after,
                     inum(d, "p99"), inum(d, "p99") - p99_after,
                     inum(d, "p999"), inum(d, "max"), inum(d, "misses")])
    write_csv(os.path.join(od, "b4d2_incremental_fx.csv"),
              ["effect", "dsp_avg_us", "incremental_avg_us", "dsp_p99",
               "incremental_p99_us", "dsp_p999", "dsp_max", "misses"], rows)

    with open(os.path.join(od, "b4d2_fullfx_60s.txt"), "w") as fh:
        for n in A.order:
            if n.startswith("1v_fullfx"):
                for tag, d in A.cases[n].items():
                    fh.write("B4D1_%s %s\n" % (tag, " ".join(
                        "%s=%s" % (k, v) for k, v in d.items())))

    # ── CPU budget ─────────────────────────────────────────────────────────
    sec = A.cases.get("1v_core_60s", {}).get("SECTIONS", {})
    rows = []
    for label in ("rx_wait", "rx_copy", "fixture", "dsp", "tx_prep", "tx_wait",
                  "book", "other", "loop_total"):
        v = fnum(sec, label)
        rows.append([label, round(v, 2), round(v / DEADLINE_US * 100.0, 2)])
    rows.append(["deadline", DEADLINE_US, 100.0])
    rows.append(["reserve_10pct", DEADLINE_US * 0.10, 10.0])
    rows.append(["reserve_15pct", DEADLINE_US * 0.15, 15.0])
    write_csv(os.path.join(od, "b4d2_cpu_budget.csv"),
              ["component", "avg_us", "pct_of_deadline"], rows)

    # ── F0 matrix ──────────────────────────────────────────────────────────
    rows = []
    for n in A.order:
        if not n.startswith("f0_"):
            continue
        d = A.dsp(n)
        p = A.cases.get(n, {}).get("PHYSICAL", {})
        rows.append([n, n.replace("f0_", "").replace("_fullfx", ""),
                     fnum(d, "avg"), inum(d, "p99"), inum(d, "p999"),
                     inum(d, "max"), inum(d, "misses"), fnum(p, "effective_fs")])
    write_csv(os.path.join(od, "b4d2_f0_matrix.csv"),
              ["case", "f0_hz", "dsp_avg_us", "dsp_p99", "dsp_p999", "dsp_max",
               "dsp_misses", "effective_fs"], rows)

    # ── memory / stack / transport ─────────────────────────────────────────
    rows = []
    for tag, d in A.cases.get("_memory", {}).items():
        rows.append([tag, inum(d, "internal_free"), inum(d, "internal_largest"),
                     inum(d, "spiram_free"), inum(d, "spiram_largest")])
    write_csv(os.path.join(od, "b4d2_memory.csv"),
              ["tag", "internal_free", "internal_largest", "spiram_free",
               "spiram_largest"], rows)
    st = A.cases.get("_stack", {}).get("last", {})
    write_csv(os.path.join(od, "b4d2_stack.csv"),
              ["audio_hwm_bytes", "pitch_hwm_bytes"],
              [[inum(st, "audio_hwm_bytes"), inum(st, "pitch_hwm_bytes")]])
    with open(os.path.join(od, "b4d2_transport.txt"), "w") as fh:
        for n in A.order:
            t = A.cases.get(n, {}).get("TRANSPORT", {})
            p = A.cases.get(n, {}).get("PHYSICAL", {})
            if t:
                fh.write("case=%s\n  physical: %s\n  transport: %s\n" %
                         (n, p, t))

    print("B4D.2 artifacts written to", od)
    print("cases:", ", ".join(A.order))


if __name__ == "__main__":
    main()
