#!/usr/bin/env python3
"""B4D.6 parser -> artifacts/alpha01d/b4d6_* ."""
from __future__ import annotations
import argparse, csv, os, re

def kv(line): return dict(re.findall(r"(\w+)=([^\s]+)", line))

def fnum(d, k, dflt=0.0):
    try: return float(d.get(k, dflt))
    except (TypeError, ValueError): return dflt

def inum(d, k, dflt=0):
    try: return int(d.get(k, dflt))
    except (TypeError, ValueError): return dflt

def parse(path):
    cases = {}
    order = []
    memory = {}
    paired = []
    with open(path, errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if line.startswith("B4D1_CASE") and "wall_s" in line:
                n = kv(line).get("name")
                if n:
                    if n not in order: order.append(n)
                    cases.setdefault(n, {})["CASE"] = kv(line)
            elif line.startswith("B4D1_MEMORY"):
                memory[kv(line).get("tag", "?")] = kv(line)
            elif line.startswith("B4D5_PAIRED"):
                paired.append(kv(line))
            elif line.startswith("B4D1_"):
                d = kv(line); n = d.get("name")
                if not n: continue
                tag = line.split()[0][5:]
                if tag in ("DSP", "PHYSICAL", "MC", "SECTIONS", "QUALIFY",
                           "DEADLINES", "TRANSPORT"):
                    cases.setdefault(n, {})[tag] = d
    return cases, order, memory, paired

def pctl(v, q):
    if not v: return 0
    a = sorted(v); return a[min(len(a)-1, int((len(a)-1)*q))]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default="artifacts/alpha01d")
    args = ap.parse_args()
    od = args.outdir

    runs = [
        ("b4d5_baseline_2v_og", "artifacts/alpha01d/b4d5_raw.txt",
         "2V / -Og / profiling ON"),
        ("b4d6_2v_o2", "artifacts/alpha01d/b4d6_o2_audit_raw.txt",
         "2V / -O2 / profiling ON"),
        ("b4d6_1v_o2", "artifacts/alpha01d/b4d6_single_raw.txt",
         "1V / -O2 / profiling ON"),
        ("b4d6_1v_o2_noprof", "artifacts/alpha01d/b4d6_noprof_raw.txt",
         "1V / -O2 / profiling OFF"),
        ("b4d6_1v_o2_prof_run2", "artifacts/alpha01d/b4d6_single_raw_run2.txt",
         "1V / -O2 / profiling ON run2"),
        ("b4d6_1v_o2_noprof_run2", "artifacts/alpha01d/b4d6_noprof_raw_run2.txt",
         "1V / -O2 / profiling OFF run2"),
    ]

    rows = []
    for tag, path, desc in runs:
        if not os.path.exists(path): continue
        cases, order, mem, paired = parse(path)
        c = cases.get("1v_fullfx_44100_60s", {})
        d = c.get("DSP", {}); p = c.get("PHYSICAL", {}); s = c.get("SECTIONS", {})
        mc = c.get("MC", {})
        sw = fnum(s, "loop_total") - fnum(s, "rx_wait") - fnum(s, "tx_wait")
        rows.append([tag, desc, fnum(d, "avg"), inum(d, "p50"), inum(d, "p90"),
                     inum(d, "p95"), inum(d, "p99"), inum(d, "p999"),
                     inum(d, "max"), inum(d, "misses"),
                     fnum(p, "effective_fs"), fnum(p, "err_pct"),
                     round(sw, 2), fnum(mc, "nmc_dsp_avg"), fnum(mc, "mc_dsp_avg"),
                     round(fnum(mc, "mc_dsp_avg") - fnum(mc, "nmc_dsp_avg"), 2),
                     inum(mc, "nmc_miss"), inum(mc, "mc_miss"),
                     c.get("QUALIFY", {}).get("result", "?")])
    write_csv(os.path.join(od, "b4d6_fullfx_matrix.csv"),
              ["run", "config", "dsp_avg_us", "p50", "p90", "p95", "p99",
               "p999", "max", "misses", "effective_fs", "err_pct",
               "software_work_us", "nmc_dsp_avg", "mc_dsp_avg", "grain_delta_us",
               "nmc_miss", "mc_miss", "qualify"], rows)

    # active_desc/slices classification for the two 1V builds
    adrows = []
    for tag, path, _ in runs:
        if "1v" not in tag or not os.path.exists(path): continue
        _, _, _, paired = parse(path)
        buckets = {}
        for r in paired:
            if not r.get("name", "").startswith("1v_fullfx"): continue
            if inum(r, "mc") != 0 or inum(r, "ng") != 0: continue
            buckets.setdefault(min(inum(r, "ad"), 4), []).append(inum(r, "dsp_us"))
        for k in sorted(buckets):
            v = buckets[k]
            adrows.append([tag, k, len(v), round(sum(v)/len(v), 1),
                           pctl(v, 0.99), max(v)])
    write_csv(os.path.join(od, "b4d6_active_desc.csv"),
              ["run", "active_desc", "blocks", "dsp_avg_us", "dsp_p99_us",
               "dsp_max_us"], adrows)

    # CPU ledger (sequential, no overlapping sums)
    write_csv(os.path.join(od, "b4d6_ledger.csv"),
              ["candidate", "baseline_dsp_avg_us", "candidate_dsp_avg_us",
               "saving_avg_us", "baseline_misses", "candidate_misses",
               "eff_fs_delta_hz", "bit_identical", "working_set", "status"],
              [["-Og -> -O2 (compiler)", 1330.54, 903.89, 426.65, 17321, 653,
                44102.28 - 39352.33, "yes (source unchanged)",
                "none", "ADOPTED"],
               ["2V -> 1V (structural)", 903.89, 824.46, 79.43, 653, 79,
                44104.34 - 44102.28, "yes (voice1 disabled before)",
                "-64476 B internal", "ADOPTED"],
               ["profiling ON -> OFF", 824.46, 739.98, 84.48, 79, 52,
                44102.15 - 44104.34, "yes (observability only)",
                "-~24 KB .bss", "ADOPTED (production)"]])
    print("B4D.6 artifacts written to", od)

def write_csv(path, header, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh); w.writerow(header); w.writerows(rows)

if __name__ == "__main__":
    main()

