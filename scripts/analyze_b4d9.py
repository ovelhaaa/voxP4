#!/usr/bin/env python3
"""B4D.9 artifacts: prewarm matrix, grain0 breakdown, ledger, feasibility."""
from __future__ import annotations
import csv, os, re

OD = "artifacts/alpha01d"
FILES = ["artifacts/alpha01d/b4d9_prewarm_raw.txt",
         "artifacts/alpha01d/b4d9_prewarm_raw2.txt"]


def kv(l):
    return dict(re.findall(r"(\w+)=([^\s]+)", l))


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


def write_csv(p, h, r):
    with open(p, "w", newline="") as fh:
        w = csv.writer(fh); w.writerow(h); w.writerows(r)


def pctl(v, q):
    if not v:
        return 0.0
    a = sorted(v); return a[min(len(a)-1, int((len(a)-1)*q))]


lines = []
for f in FILES:
    if os.path.exists(f):
        with open(f, errors="replace") as fh:
            lines += [l.strip() for l in fh]

VAR = {"p0": 0, "p1": 1, "p2": 2, "p3": 4, "p4": 3, "p7": 7}
DESC = {"p0": "P0 off", "p1": "P1 marks", "p2": "P2 model",
        "p3": "P3 warp tables", "p4": "P4 marks+model", "p7": "P5 all"}

# prewarm matrix
rows = []
for v, bits in VAR.items():
    name = f"b4d9_{v}_fullfx_44100"
    dsp = cls = {}
    pw = []
    for l in lines:
        if l.startswith("B4D1_DSP ") and f"name={name}" in l:
            dsp = kv(l)
        elif l.startswith("B4D4_CLASS ") and f"name={name}" in l:
            c = kv(l); cls[c.get("cls")] = c
        elif l.startswith("B4D8_ORD ") and f"name={name}" in l:
            pw.append(fnum(kv(l), "pw"))
    rows.append([DESC[v], bits, round(sum(pw)/len(pw), 1) if pw else 0.0,
                 fnum(cls.get("D_mc2g", {}), "avg"),
                 fnum(cls.get("D_mc2g", {}), "p99"),
                 fnum(cls.get("E_mc3pg", {}), "avg"),
                 fnum(cls.get("E_mc3pg", {}), "p99"),
                 fnum(cls.get("E_mc3pg", {}), "max"),
                 fnum(dsp, "avg"), inum(dsp, "misses"), "n/a", "DIAGNOSTIC"])
write_csv(os.path.join(OD, "b4d9_prewarm_matrix.csv"),
          ["candidate", "variant_bits", "prewarm_us", "MC2_avg_us", "MC2_p99_us",
           "MC3_avg_us", "MC3_p99_us", "MC3_max_us", "DSP_avg_us", "misses",
           "PCM_identical", "status"], rows)

# grain0 breakdown (P0 burst blocks)
g0 = []
for l in lines:
    if not l.startswith("B4D8_ORD ") or "b4d9_p0_" not in l:
        continue
    d = kv(l)
    g0.append([inum(d, "blk"), inum(d, "mc"), fnum(d, "g0_sel"),
               fnum(d, "g0_al"), fnum(d, "g0_nr"), fnum(d, "g0_wp"),
               fnum(d, "g0_dc"), fnum(d, "g1_sel"), fnum(d, "g1_nr"),
               fnum(d, "g1_wp"), fnum(d, "g2_sel"), fnum(d, "g2_nr"),
               fnum(d, "g2_wp")])
write_csv(os.path.join(OD, "b4d9_grain0_breakdown.csv"),
          ["blk", "mark_count", "g0_select", "g0_align", "g0_model_near",
           "g0_warp_phase", "g0_descriptor", "g1_select", "g1_model_near",
           "g1_warp_phase", "g2_select", "g2_model_near", "g2_warp_phase"], g0)

# additive ledger for a representative MC+3 block (P0)
ledger = []
if g0:
    r = g0[0]
    g0_tot = r[2] + r[3] + r[4] + r[5] + r[6]
    g1_tot = r[7] + r[8] + r[9]
    g2_tot = r[10] + r[11] + r[12]
    # deferred + global from the tail probe of the same run
    deferred = 320.0
    for l in lines:
        if l.startswith("B4D1_LATE_BLOCK") and "b4d9_p0_" in l and "new_grains=3" in l:
            deferred = fnum(kv(l), "df_us")
            break
    ledger = [
        ["global_pre", 127.0, "MANDATORY", 0, 0, "frozen"],
        ["grain0_select", r[2], "EXACT_OPTIMIZABLE", 0, 0,
         "integer scan; not data-cold (prewarm negative)"],
        ["grain0_align", r[3], "FORENSIC", 0, 0, "gated out in B4D.8"],
        ["grain0_model_near", r[4], "EXACT_OPTIMIZABLE", 0, 0,
         "query differs per grain"],
        ["grain0_warp_phase", r[5], "MANDATORY", 0, 0, "real warp miss"],
        ["grain0_descriptor", r[6], "MANDATORY", 0, 0, "commit"],
        ["grain1", g1_tot, "MANDATORY", 0, 0, "warm grain"],
        ["grain2", g2_tot, "MANDATORY", 0, 0, "warm grain"],
        ["deferred_render", deferred, "EXACT_OPTIMIZABLE", 0, 150,
         "kernel fusion upper bound"],
        ["compressor", 69.0, "MANDATORY", 0, 0, "frozen"],
        ["delay", 83.0, "MANDATORY", 0, 0, "frozen"],
        ["reverb", 227.0, "MANDATORY", 0, 0, "frozen"],
        ["global_post", 45.0, "MANDATORY", 0, 0, "frozen"],
        ["other_reconciled", 400.0, "UNKNOWN", 0, 0, "cache/base inflation"],
    ]
write_csv(os.path.join(OD, "b4d9_additive_ledger.csv"),
          ["term", "measured_us", "classification", "demonstrated_saving_us",
           "plausible_saving_us", "notes"], ledger)

# feasibility budget
baseline_mc3 = 2586.82
deadline = 1451.247
required = baseline_mc3 - deadline
write_csv(os.path.join(OD, "b4d9_feasibility_budget.csv"),
          ["term", "measured_us", "classification", "optimizable_exactly",
           "demonstrated_saving_us", "plausible_saving_us", "notes"],
          [["MC3_baseline", baseline_mc3, "MEASURED", "no", 0, 0, "P0 run"],
           ["deadline", deadline, "TARGET", "no", 0, 0, "44.1 kHz / 64"],
           ["required_saving", round(required, 1), "GAP", "no", 0, 0, ""],
           ["first_touch_prewarm", 0.0, "DEMONSTRATED", "yes", 0.0, 0.0,
            "P0..P5: prewarm did not reduce grain0; net ~0 (negative)"],
           ["forensic_audit_removals", 220.0, "DEMONSTRATED", "yes", 220.0,
            0.0, "B4D.7+B4D.8 adopted"],
           ["deferred_render_50pct", 150.0, "PLAUSIBLE", "maybe", 0.0, 150.0,
            "upper-bound estimate only"],
           ["remaining_gap", round(required - 220.0, 1), "GAP", "no", 0.0,
            150.0, "after demonstrated exact savings"]])

# candidates
write_csv(os.path.join(OD, "b4d9_candidates.csv"),
          ["candidate", "hypothesis", "result", "status", "reason"],
          [["P1 marks prewarm", "marks array first-touch", "MC3 2721 vs 2587",
            "REJECTED", "no gain; prewarm cost 78 us"],
           ["P2 model prewarm", "model ring first-touch", "MC3 2576 vs 2587",
            "REJECTED", "no gain; prewarm cost 37 us"],
           ["P3 tables prewarm", "warp/GainNorm tables first-touch",
            "MC3 2783 vs 2587", "REJECTED", "worse; prewarm cost 100 us"],
           ["P4/P5 combined", "marks+model+tables", "MC3 2674/2686",
            "REJECTED", "worse; prewarm cost 50/133 us"],
           ["cold-code / flash", "I-cache first-touch of the grain path",
            "not isolated", "UNRESOLVED", "data prewarm negative; code "
            "first-touch not safely measurable without state mutation"],
           ["deferred kernel fusion", "reduce deferred render", "estimate",
            "NOT IMPLEMENTED", "even 50% saving leaves MC3 > 2400 us"],
           ["BurstContext locality", "warm marks/model across burst",
            "superseded", "NOT JUSTIFIED", "prewarm shows no removable "
            "data first-touch"],
           ["scheduler semantics", "avoid the 2/3-grain burst", "analysis",
            "OUT OF SCOPE", "changes grain timing; product decision"]])
print("B4D.9 artifacts written")
