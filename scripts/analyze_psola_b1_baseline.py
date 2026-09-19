"""Extract prediction-free LPC prewarming feasibility from Experiment A capture."""

import argparse
import csv
from collections import Counter
from pathlib import Path


def records(path):
    result = {name: {} for name in ("PRED_GRAIN", "PRED_PROJ", "PRED_AVAIL", "B4D12_GRAIN")}
    for line in path.open(encoding="utf-8", errors="ignore"):
        fields = line.strip().split(",")
        kind = fields[0]
        if kind == "PRED_GRAIN":
            result[kind][(fields[1], fields[2], fields[4])] = fields
        elif kind in ("PRED_PROJ", "PRED_AVAIL"):
            result[kind][(fields[1], fields[2], fields[4], fields[5])] = fields
        elif kind == "B4D12_GRAIN":
            result[kind][(fields[2], fields[3], fields[7])] = fields
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = records(args.capture)
    rows = []
    for (case, block, ordinal), grain in data["PRED_GRAIN"].items():
        if grain[3] not in ("2", "3") or ordinal != "0":
            continue
        prior = data["PRED_PROJ"].get((case, block, ordinal, "1"))
        available = data["PRED_AVAIL"].get((case, block, ordinal, "1"))
        cost = data["B4D12_GRAIN"].get((block, ordinal, grain[10]))
        if prior is None or available is None:
            continue
        actual_key = (grain[18], grain[19], grain[13], grain[14], grain[15], grain[20], grain[16])
        prior_key = (prior[14], prior[15], prior[16], prior[17], prior[18], prior[19], prior[20])
        selected = int(grain[11])
        newest = int(available[8])
        rank = newest - selected + 1 if selected and newest >= selected else 0
        rows.append({
            "case": case, "block": block,
            "class": "MC+2" if grain[3] == "2" else "MC+3+",
            "model_serial": selected, "n1_newest_serial": newest,
            "n1_recency_rank": rank,
            "n1_model_available": available[7],
            "n1_control_key_exact": int(actual_key == prior_key),
            "n1_shift_exact": int(actual_key[0] == prior_key[0]),
            "n1_amount_exact": int(actual_key[1] == prior_key[1]),
            "n1_lambda_exact": int(actual_key[2] == prior_key[2]),
            "n1_gamma_exact": int(actual_key[3] == prior_key[3]),
            "n1_rate_exact": int(actual_key[4] == prior_key[4]),
            "n1_mode_exact": int(actual_key[5] == prior_key[5]),
            "n1_norm_exact": int(actual_key[6] == prior_key[6]),
            "n3_model_available": data["PRED_AVAIL"].get((case, block, ordinal, "3"), [None] * 8)[7],
            "n2_model_available": data["PRED_AVAIL"].get((case, block, ordinal, "2"), [None] * 8)[7],
            "addgrain_cycles": cost[16] if cost else "",
            "select_cycles": cost[17] if cost else "",
            "model_near_cycles": cost[18] if cost else "",
            "cache_lookup_cycles": cost[19] if cost else "",
            "polynomial_cycles": cost[20] if cost else "",
            "gain_cycles": cost[21] if cost else "",
            "cache_path": grain[17],
        })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    for klass in ("MC+2", "MC+3+"):
        subset = [r for r in rows if r["class"] == klass]
        print(klass, "n", len(subset), "key_exact", sum(r["n1_control_key_exact"] for r in subset))
        print("ranks", dict(sorted(Counter(r["n1_recency_rank"] for r in subset).items())))
        print("coverage", {n: sum(0 < r["n1_recency_rank"] <= n and r["n1_model_available"] == "1" for r in subset) for n in (1, 2, 4, 8, 16)})
        print("cost_rows", sum(bool(r["addgrain_cycles"]) for r in subset))


if __name__ == "__main__":
    main()
