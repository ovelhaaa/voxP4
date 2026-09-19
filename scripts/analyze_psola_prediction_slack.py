#!/usr/bin/env python3
"""Extract B4D.12 MC burst predecessor timing from a serial capture."""

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path

DEADLINE_US = 64 * 1_000_000 / 44_100


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    for line in args.capture.read_text(errors="replace").splitlines():
        pos = line.find("PRED_SLACK,")
        if pos < 0:
            continue
        parts = next(csv.reader([line[pos + len("PRED_SLACK,"):]]))
        if len(parts) != 12:
            continue
        case, block, klass, grains, *numbers = parts
        klass = int(klass)
        n3, n2, n1, n, *leads = map(int, numbers)
        prior = [n3, n2, n1]
        available = [max(0.0, DEADLINE_US - v) for v in prior]
        rows.append({
            "case": case, "block": int(block), "class": f"MC+{klass}" if klass < 3 else "MC+3+",
            "grains": int(grains), "n3_dsp_us": n3, "n2_dsp_us": n2,
            "n1_dsp_us": n1, "n_dsp_us": n,
            "n3_slack_us": round(DEADLINE_US - n3, 3),
            "n2_slack_us": round(DEADLINE_US - n2, 3),
            "n1_slack_us": round(DEADLINE_US - n1, 3),
            "n1_positive_slack_us": round(available[2], 3),
            "n2_n1_positive_slack_us": round(sum(available[1:]), 3),
            "n3_n2_n1_positive_slack_us": round(sum(available), 3),
            "fits_800_n1": int(available[2] >= 800),
            "fits_800_n2_n1": int(sum(available[1:]) >= 800),
            "fits_1000_n3_n2_n1": int(sum(available) >= 1000),
            **{f"grain_{i}_lead_samples_rounded": v for i, v in enumerate(leads)},
        })
    if not rows:
        raise SystemExit("no PRED_SLACK rows found")
    with args.output.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} sampled model-change events to {args.output}")
    groups = defaultdict(list)
    for row in rows:
        groups[(row["case"], row["class"])].append(row)
        groups[("ALL", row["class"])].append(row)

    def quantile(values, p):
        ordered = sorted(values)
        return ordered[max(0, math.ceil(p * len(ordered)) - 1)]

    summary = []
    for (case, klass), subset in sorted(groups.items()):
        item = {"case": case, "class": klass, "events": len(subset)}
        for prefix in ("n3", "n2", "n1", "n"):
            values = [r[f"{prefix}_dsp_us"] for r in subset]
            item[f"{prefix}_dsp_median_us"] = statistics.median(values)
            item[f"{prefix}_dsp_p95_us"] = quantile(values, .95)
            if prefix != "n":
                slack = [r[f"{prefix}_slack_us"] for r in subset]
                item[f"{prefix}_slack_median_us"] = statistics.median(slack)
                item[f"{prefix}_slack_p05_us"] = quantile(slack, .05)
        for key in ("n1_positive_slack_us", "n2_n1_positive_slack_us",
                    "n3_n2_n1_positive_slack_us"):
            item[f"{key}_median"] = statistics.median(r[key] for r in subset)
        for key in ("fits_800_n1", "fits_800_n2_n1", "fits_1000_n3_n2_n1"):
            item[f"{key}_count"] = sum(r[key] for r in subset)
        leads = [r[f"grain_{i}_lead_samples_rounded"] for r in subset
                 for i in range(4) if r[f"grain_{i}_lead_samples_rounded"] != -2147483648]
        item["lead_samples_min"] = min(leads)
        item["lead_samples_median"] = statistics.median(leads)
        item["lead_samples_p95"] = quantile(leads, .95)
        item["lead_samples_max"] = max(leads)
        summary.append(item)
    summary_path = args.output.with_name("psola_prediction_slack_summary.csv")
    with summary_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    print(f"wrote {len(summary)} summaries to {summary_path}")


if __name__ == "__main__":
    main()
