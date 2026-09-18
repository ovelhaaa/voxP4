#!/usr/bin/env python3
"""B4D.8 artifacts from raw captures."""
from __future__ import annotations
import csv, os, re

OD = "artifacts/alpha01d"


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


def read_lines(path):
    with open(path, errors="replace") as fh:
        return [l.strip() for l in fh]


def main():
    # grain ordinal from the split probe run
    ord_rows = []
    for line in read_lines(os.path.join(OD, "b4d8_ordinal3_raw.txt")):
        if not line.startswith("B4D8_ORD"):
            continue
        d = kv(line)
        if not d.get("name", "").startswith("1v_fullfx"):
            continue
        ord_rows.append([
            inum(d, "blk"), inum(d, "mc"),
            fnum(d, "g0_sel"), fnum(d, "g0_al"), fnum(d, "g0_nr"),
            fnum(d, "g0_wp"), fnum(d, "g0_dc"),
            fnum(d, "g1_sel"), fnum(d, "g1_al"), fnum(d, "g1_nr"),
            fnum(d, "g1_wp"), fnum(d, "g1_dc"),
            fnum(d, "g2_sel"), fnum(d, "g2_nr"), fnum(d, "g2_wp")])
    write_csv(os.path.join(OD, "b4d8_grain_ordinal.csv"),
              ["blk", "mark_count", "g0_select", "g0_align", "g0_model_near",
               "g0_warp_phase", "g0_descriptor", "g1_select", "g1_align",
               "g1_model_near", "g1_warp_phase", "g1_descriptor",
               "g2_select", "g2_model_near", "g2_warp_phase"], ord_rows)

    # tail probe (B4D.8 gated production run)
    probe_rows = []
    for line in read_lines(os.path.join(OD, "b4d8_production_raw.txt")):
        if not line.startswith("B4D1_LATE_BLOCK"):
            continue
        d = kv(line)
        if not d.get("name", "").startswith("1v_fullfx"):
            continue
        probe_rows.append([inum(d, "blk"), inum(d, "dsp_us"), inum(d, "mc"),
                           inum(d, "new_grains"), inum(d, "active_desc"),
                           inum(d, "exp_warp"), inum(d, "att"),
                           fnum(d, "sch_us"), fnum(d, "ag_us"),
                           fnum(d, "df_us"), fnum(d, "mk_us"),
                           fnum(d, "wp_us"), fnum(d, "dc_us"),
                           fnum(d, "nr_us"), fnum(d, "gn_us")])
    write_csv(os.path.join(OD, "b4d8_tail_probe.csv"),
              ["blk", "dsp_us", "mc", "new_grains", "active_desc", "exp_warp",
               "attempts", "sched_us", "addgrain_us", "deferred_us",
               "mark_us", "warp_us", "descriptor_us", "model_near_us",
               "gainnorm_us"], probe_rows)

    # fullfx matrix
    rows = []
    for tag, path in (("b4d7_baseline", "artifacts/alpha01d/b4d7_production_raw.txt"),
                      ("b4d8_run1", "artifacts/alpha01d/b4d8_production_raw.txt"),
                      ("b4d8_run2", "artifacts/alpha01d/b4d8_production_raw_run2.txt")):
        if not os.path.exists(path):
            continue
        dsp = phys = mc = {}
        cls = {}
        for line in read_lines(path):
            if line.startswith("B4D1_DSP ") and "1v_fullfx" in line:
                dsp = kv(line)
            elif line.startswith("B4D1_PHYSICAL ") and "1v_fullfx" in line:
                phys = kv(line)
            elif line.startswith("B4D1_MC ") and "1v_fullfx" in line:
                mc = kv(line)
            elif line.startswith("B4D4_CLASS ") and "1v_fullfx" in line:
                c = kv(line)
                cls[c.get("cls")] = c
        rows.append([tag, fnum(dsp, "avg"), inum(dsp, "p50"), inum(dsp, "p90"),
                     inum(dsp, "p95"), inum(dsp, "p99"), inum(dsp, "p999"),
                     inum(dsp, "max"), inum(dsp, "misses"),
                     inum(mc, "mc_miss"), inum(mc, "nmc_miss"),
                     fnum(phys, "effective_fs"),
                     fnum(cls.get("D_mc2g", {}), "avg"),
                     fnum(cls.get("E_mc3pg", {}), "avg"),
                     inum(cls.get("D_mc2g", {}), "misses"),
                     inum(cls.get("E_mc3pg", {}), "misses")])
    write_csv(os.path.join(OD, "b4d8_fullfx_matrix.csv"),
              ["run", "dsp_avg_us", "p50", "p90", "p95", "p99", "p999", "max",
               "dsp_misses", "mc_misses", "nmc_misses", "effective_fs",
               "mc2_avg_us", "mc3_avg_us", "mc2_misses", "mc3_misses"], rows)

    # candidates ledger
    write_csv(os.path.join(OD, "b4d8_candidates.csv"),
              ["candidate", "hypothesis", "MC1_us", "MC2_us", "MC3_us",
               "mark_delta", "model_delta", "descriptor_delta", "deferred_delta",
               "DSP_avg_delta", "P99_delta", "P999_delta", "max_delta",
               "misses_before", "misses_after", "internal_RAM_delta",
               "bit_identical", "status", "reason"],
              [["source_grain_audit_off",
                "B4C.7 forensic 64-entry scan in production deferred path",
                "1084", "2727", "2621", "0", "0", "-80", "0", "-12", "-20",
                "-37", "0", "79", "63", "0", "yes", "ADOPTED (B4D.7)",
                "forensic telemetry; exact"],
               ["grain_alignment_audit_off",
                "record_grain_alignment soft-double division + cold flash "
                "fetch on the first grain of a burst",
                "1073", "2586", "2532", "0", "0", "-13", "0", "-13", "-22",
                "-115", "0", "63", "61", "0", "yes", "ADOPTED",
                "forensic telemetry; g0_align 113->0.1 us; exact"],
               ["model_near_reuse_in_burst",
                "same model for all burst grains -> reuse lookup",
                "1073", "2586", "2532", "0", "0", "0", "0", "0", "0", "0",
                "0", "61", "61", "0", "n/a", "REJECTED",
                "queries (centers) differ; result equal only empirically; "
                "no provable query identity without reading the ring"],
               ["select_mark_binary_search",
                "sorted marks -> O(log n) exact nearest",
                "1073", "2586", "2532", "-180", "0", "0", "0", "-4", "0",
                "0", "0", "61", "61", "0", "unverified", "NOT ADOPTED",
                "g0_select still 180-215 us for 25-64 candidates; cold-start "
                "dominated; needs a sortedness-proven fast path + millions-of-"
                "cases equivalence gate; not completed in this stage"],
               ["block_level_burst_context",
                "warm marks/model and prepare model once across the burst",
                "1073", "2586", "2532", "0", "0", "0", "0", "0", "0", "0",
                "0", "61", "61", "0", "n/a", "CANDIDATE (B4D.9)",
                "grain 0 cold-start (~200 us select + ~500 us warp) is the "
                "dominant term; requires scheduler-context refactor"]])
    print("B4D.8 artifacts written")


if __name__ == "__main__":
    main()
