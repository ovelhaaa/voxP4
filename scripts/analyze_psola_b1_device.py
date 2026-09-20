"""Summarize paired B1 COM11 captures without treating policy coverage as hits."""

import argparse
import csv
import math
import re
from collections import Counter, defaultdict
from pathlib import Path


def percentile(values, pct):
    if not values:
        return ""
    values = sorted(values)
    return values[math.ceil(len(values) * pct / 100) - 1]


def parse(path):
    policy = int(re.search(r"policy(\d+)", path.name).group(1))
    variant = "publication_guard" if "optimized" in path.name else "bounded_scan"
    prep = {}
    classes = {}
    class_counts = {}
    burst = defaultdict(list)
    first = defaultdict(Counter)
    for line in path.open(encoding="utf-8", errors="ignore"):
        if (line.startswith("B1_PREP ") or line.startswith("B4D12_CLASS_DIST ")
                or line.startswith("B4D12_CLASS ")):
            name, *pairs = line.strip().split()
            fields = dict(item.split("=", 1) for item in pairs if "=" in item)
            if name == "B1_PREP":
                prep[fields["case"]] = fields
            elif name == "B4D12_CLASS":
                class_counts[fields["name"]] = fields
            else:
                classes[(fields["name"], fields["class"])] = fields
        elif line.startswith("PRED_SLACK,"):
            fields = line.strip().split(",")
            if fields[3] in ("2", "3"):
                burst[(fields[1], fields[3])].append(int(fields[8]))
        elif line.startswith("PRED_GRAIN,"):
            fields = line.strip().split(",")
            if fields[3] in ("2", "3") and fields[4] == "0":
                first[(fields[1], fields[3])][fields[17]] += 1
    rows = []
    for case in sorted(prep):
        for klass in ("2", "3"):
            values = burst[(case, klass)]
            dist = classes.get((case, "mc" + klass), {})
            normal = classes.get((case, "nmc"), {})
            counts = first[(case, klass)]
            rows.append({
                "policy": policy, "case": case,
                "variant": variant,
                "class": "MC+2" if klass == "2" else "MC+3+",
                "blocks": len(values), "first_prepared_hits": counts["5"],
                "first_grains": sum(counts.values()),
                "mean_us": round(sum(values) / len(values), 3) if values else "",
                "p95_us": percentile(values, 95),
                "p99_us": percentile(values, 99),
                "max_us": max(values) if values else "",
                "misses": sum(v > 1451.247 for v in values),
                "miss_probability": round(sum(v > 1451.247 for v in values) / len(values), 6) if values else "",
                "normal_blocks": normal.get("blocks", ""),
                "normal_mean_us": normal.get("mean_us", ""),
                "normal_p95_us": normal.get("p95", ""),
                "normal_p99_us": normal.get("p99", ""),
                "normal_misses": class_counts.get(case, {}).get("miss_nmc", ""),
                "preparations_cumulative": prep[case].get("preparations", ""),
                "preparation_cycles_cumulative": prep[case].get("prep_cycles", ""),
                "polls_cumulative": prep[case].get("polls", ""),
                "poll_cycles_cumulative": prep[case].get("poll_cycles", ""),
                "preparation_blocks_cumulative": prep[case].get("prep_blocks", ""),
                "preparation_block_max_cycles": prep[case].get("prep_block_max_cycles", ""),
                "useful_preparations_cumulative": prep[case].get("useful", ""),
                "prepared_hits_cumulative": prep[case].get("hits", ""),
                "prepared_misses_cumulative": prep[case].get("misses", ""),
                "snapshot_failures_cumulative": prep[case].get("snapshot_failures", ""),
                "class_mean_firmware_us": dist.get("mean_us", ""),
            })
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("captures", nargs="+", type=Path)
    args = parser.parse_args()
    rows = sorted((row for path in args.captures for row in parse(path)),
                  key=lambda r: (r["policy"], r["case"], r["class"]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for row in rows:
        if row["case"] == "a_ref_vocal_fullfx":
            print(row["policy"], row["class"], row["blocks"],
                  row["variant"], row["first_prepared_hits"], row["mean_us"], row["misses"],
                  row["normal_mean_us"])


if __name__ == "__main__":
    main()
