#!/usr/bin/env python3
"""Extract the bounded B4D.12 per-grain flight recorder from a serial log."""

import argparse
import csv
import pathlib
import statistics


FIELDS = (
    "set,block,ordinal,source_center,destination_center,half,model_timestamp,"
    "model_order,lambda_bits,gamma_bits,sample_rate_bits,norm_strategy,"
    "cache_path,local_difference,shared_difference,addgrain_cycles,"
    "select_cycles,model_near_cycles,cache_lookup_cycles,polynomial_cycles,"
    "gain_cycles"
).split(",")


def percentile(values, percent):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, (len(ordered) * percent + 99) // 100 - 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", type=pathlib.Path)
    ap.add_argument("--prefix", type=pathlib.Path, required=True)
    args = ap.parse_args()
    rows = []
    cache_rows = []
    class_rows = []
    for line in args.log.open(encoding="utf-8", errors="replace"):
        if line.startswith("B4D12_GRAIN,"):
            values = next(csv.reader([line.removeprefix("B4D12_GRAIN,")]))
            if len(values) == len(FIELDS):
                rows.append(dict(zip(FIELDS, values)))
        elif line.startswith("B4D12_GRAIN_CACHE "):
            cache_rows.append(dict(piece.split("=", 1) for piece in line.split()[1:]))
        elif line.startswith("B4D12_GRAIN_CLASS "):
            class_rows.append(dict(piece.split("=", 1) for piece in line.split()[1:]))

    args.prefix.parent.mkdir(parents=True, exist_ok=True)
    with args.prefix.with_name(args.prefix.name + "_grains.csv").open("w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    if cache_rows:
        with args.prefix.with_name(args.prefix.name + "_cache.csv").open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=cache_rows[0].keys())
            writer.writeheader()
            writer.writerows(cache_rows)
    if class_rows:
        with args.prefix.with_name(args.prefix.name + "_classes.csv").open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=class_rows[0].keys())
            writer.writeheader()
            writer.writerows(class_rows)

    # A block can occur in both top and miss sets. Use miss once for timing.
    unique = {(r["block"], r["ordinal"]): r for r in rows if r["set"] == "miss"}
    for ordinal in range(4):
        subset = [r for r in unique.values() if int(r["ordinal"]) == ordinal]
        if not subset:
            continue
        print(f"grain={ordinal + 1} miss_records={len(subset)}")
        for field in ("addgrain_cycles", "select_cycles", "model_near_cycles",
                      "cache_lookup_cycles", "polynomial_cycles", "gain_cycles"):
            values = [int(r[field]) for r in subset]
            print(f"  {field}: median={statistics.median(values):.0f} "
                  f"p95={percentile(values, 95)} max={max(values)}")
        outcomes = {path: sum(int(r["cache_path"]) == path for r in subset)
                    for path in range(5)}
        print("  cache_path:", outcomes)


if __name__ == "__main__":
    main()
