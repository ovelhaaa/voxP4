"""Extract exact publication-to-first-grain lead from B1 device telemetry."""

import argparse
import csv
import math
import statistics
from pathlib import Path


def percentile(values, pct):
    values = sorted(values)
    return values[math.ceil(len(values) * pct / 100) - 1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    rows = []
    for line in args.capture.open(encoding="utf-8", errors="ignore"):
        if not line.startswith("PRED_GRAIN,"):
            continue
        fields = line.strip().split(",")
        if len(fields) != 25 or fields[3] not in ("2", "3") or fields[4] != "0":
            continue
        publication_block, use_block = int(fields[21]), int(fields[22])
        publication_us, use_us = int(fields[23]), int(fields[24])
        if not publication_block or not publication_us or use_us < publication_us:
            raise ValueError(f"invalid publication lead: {fields[:5]}")
        blocks = (use_block - publication_block) & 0xFFFFFFFF
        rows.append({
            "case": fields[1], "block": int(fields[2]),
            "class": "MC+2" if fields[3] == "2" else "MC+3+",
            "model_serial": int(fields[11]),
            "publication_audio_block": publication_block,
            "consumption_audio_block": use_block,
            "lead_blocks": blocks, "lead_samples_64frame": blocks * 64,
            "publication_time_us": publication_us,
            "consumption_time_us": use_us,
            "lead_us": use_us - publication_us,
            "cache_path": int(fields[17]),
        })
    if not rows:
        raise SystemExit("no extended first-grain records")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for klass in ("MC+2", "MC+3+"):
        subset = [r for r in rows if r["class"] == klass]
        if not subset:
            continue
        for field in ("lead_blocks", "lead_us"):
            values = [r[field] for r in subset]
            print(klass, field, "n", len(values), "min", min(values),
                  "median", statistics.median(values), "p95", percentile(values, 95),
                  "max", max(values))


if __name__ == "__main__":
    main()
