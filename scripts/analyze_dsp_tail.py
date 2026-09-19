#!/usr/bin/env python3
"""Extract the B4D.12 bounded flight recorder from a serial capture.

The CSV contains all retained misses and the slowest 100 blocks. It is a
selected sample: event frequencies in this file are not whole-run rates.
"""
import argparse
import csv
import pathlib
import statistics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=pathlib.Path)
    ap.add_argument("--csv", type=pathlib.Path, required=True)
    args = ap.parse_args()
    header = None
    rows = []
    summary = None
    count = None
    for line in args.log.read_text(encoding="utf-8", errors="replace").splitlines():
        if "B4D12_FORENSIC_HEADER " in line:
            header = next(csv.reader([line.split("B4D12_FORENSIC_HEADER ", 1)[1]]))
        elif "B4D12_FORENSIC_COUNT " in line:
            count = line.split("B4D12_FORENSIC_COUNT ", 1)[1]
        elif "B4D12_FORENSIC," in line:
            values = next(csv.reader([line.split("B4D12_FORENSIC,", 1)[1]]))
            if header:
                if len(values) != len(header):
                    raise SystemExit(f"forensic column mismatch: {len(values)} vs {len(header)}")
                rows.append(dict(zip(header, values)))
        elif "B4D12_DSP name=a_ref_vocal_fullfx " in line:
            summary = line.split("B4D12_DSP name=a_ref_vocal_fullfx ", 1)[1]
    if not header or not rows:
        raise SystemExit("no B4D12 forensic records found")
    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=header)
        writer.writeheader()
        writer.writerows(rows)
    misses = [r for r in rows if r["set"] == "miss"]
    top = sorted((r for r in rows if r["set"] == "top"),
                 key=lambda r: int(r["dsp_us"]), reverse=True)
    print("whole run:", summary or "missing")
    print("recorder:", count or "missing")
    print("CSV:", args.csv)
    print("top 10: block dsp_us harmony_us other_stages_us signature")
    for r in top[:10]:
        stage = [int(r[k]) for k in ("input_cycles", "comp_cycles", "harmony_cycles",
                                      "delay_cycles", "reverb_cycles", "master_cycles")]
        print(r["block"], r["dsp_us"], round(stage[2] / 360),
              round((sum(stage) - stage[2]) / 360),
              "ng=" + r["new_grains"], "source=" + r["source_grains"],
              "active=" + r["active_grains"], "mc=" + r["formant_changed"],
              "pitch=" + r["pitch_changed"], "warp=" + r["model_expensive"])
    print("retained misses:", len(misses))
    for field in ("new_grains", "source_grains", "active_grains",
                  "model_expensive", "formant_changed", "pitch_changed",
                  "fallback", "recovery"):
        if misses:
            yes = sum(int(r[field]) > 0 for r in misses)
            print(field, f"{yes}/{len(misses)} misses", "(selected sample only)")
    ids = sorted(int(r["block"]) for r in misses)
    gaps = [b - a for a, b in zip(ids, ids[1:])]
    if gaps:
        print("miss-gap median/min/max:", statistics.median(gaps), min(gaps), max(gaps))
    if misses and "psola_addgrain_cycles" in header:
        print("PSOLA miss medians (cycles; subcounters may overlap):")
        for field in ("psola_sched_cycles", "psola_addgrain_cycles",
                      "psola_deferred_cycles", "psola_mark_cycles",
                      "psola_desc_cycles", "psola_near_cycles",
                      "psola_poly_cycles", "psola_gn_cycles"):
            print(field, round(statistics.median(int(r[field]) for r in misses)))


if __name__ == "__main__":
    main()
