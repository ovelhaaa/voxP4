#!/usr/bin/env python3
"""Score observational B4D.12 frozen-state projections without future leakage."""

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path


def records(path, tag, size):
    for line in path.read_text(errors="replace").splitlines():
        p = line.find(tag + ",")
        if p >= 0:
            fields = next(csv.reader([line[p + len(tag) + 1:]]))
            if len(fields) == size:
                yield fields


def write_csv(path, rows, columns):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("output_dir", type=Path)
    args = ap.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    actual = {}
    for f in records(args.capture, "PRED_GRAIN", 20):
        key = (f[0], int(f[1]), int(f[3]))
        actual[key] = dict(klass=int(f[2]), destination=int(f[4], 16),
            source=int(f[5], 16), mark=int(f[6]), period=int(f[7], 16),
            half=int(f[8]), model_serial=int(f[10]), order=int(f[11]),
            lambda_bits=int(f[12]), gamma=int(f[13]), rate=int(f[14]),
            norm=int(f[15]), lpc=int(f[16]) != 0, shift=int(f[17]),
            amount=int(f[18]), mode=int(f[19]))

    available = {}
    for f in records(args.capture, "PRED_AVAIL", 9):
        available[(f[0], int(f[1]), int(f[3]), int(f[4]))] = dict(
            mark=int(f[5]), model=int(f[6]), prior_model_serial=int(f[7]),
            actual_model_serial=int(f[8]))

    context = {}
    for f in records(args.capture, "PRED_CONTEXT", 14):
        context[(f[0], int(f[1]))] = dict(actual_pitch=int(f[2]),
            prior_pitch=[int(f[3]), int(f[4]), int(f[5])],
            onset=int(f[6]), pitch_changed=int(f[7]), track=int(f[8]),
            recovery=int(f[9]), prior_cursor=[int(f[10]), int(f[11]), int(f[12])],
            actual_cursor=int(f[13]))

    event_class = {}
    for f in records(args.capture, "PRED_SLACK", 12):
        event_class[(f[0], int(f[1]))] = int(f[2])
    context_rows = []
    for key, ctx in context.items():
        klass = event_class.get(key)
        if klass is None:
            continue
        context_rows.append(dict(case=key[0], block=key[1],
            klass=f"MC+{klass}" if klass < 3 else "MC+3+",
            actual_cursor=ctx["actual_cursor"],
            n3_cursor=ctx["prior_cursor"][0],
            n2_cursor=ctx["prior_cursor"][1],
            n1_cursor=ctx["prior_cursor"][2],
            onset=ctx["onset"], recovery=ctx["recovery"],
            pitch_changed=ctx["pitch_changed"],
            n3_pitch_changed=int(ctx["prior_pitch"][0] != ctx["actual_pitch"]),
            n2_pitch_changed=int(ctx["prior_pitch"][1] != ctx["actual_pitch"]),
            n1_pitch_changed=int(ctx["prior_pitch"][2] != ctx["actual_pitch"])))
    if context_rows:
        write_csv(args.output_dir / "psola_prediction_context.csv", context_rows,
                  list(context_rows[0]))

    detail = []
    reasons = Counter()
    for f in records(args.capture, "PRED_PROJ", 20):
        case, block, klass, ordinal, horizon, status = f[:6]
        block, klass, ordinal, horizon, status = map(int,
            (block, klass, ordinal, horizon, status))
        a = actual.get((case, block, ordinal))
        if a is None:
            continue
        p = dict(destination=int(f[6], 16), source=int(f[7], 16),
            mark=int(f[8]), period=int(f[9], 16), half=int(f[10]),
            model_serial=int(f[11]), lpc=int(f[12]), shift=int(f[13]),
            amount=int(f[14]), lambda_bits=int(f[15]), gamma=int(f[16]),
            rate=int(f[17]), mode=int(f[18]), norm=int(f[19]))
        projected = status == 1
        comparisons = {name: int(projected and p[name] == a[name]) for name in
            ("destination", "source", "mark", "period", "half", "model_serial",
             "shift", "amount", "lambda_bits", "gamma", "rate", "mode", "norm")}
        comparisons["lpc"] = int(projected and p["lpc"] == a["lpc"])
        comparisons["warp"] = int(projected and all(comparisons[n] for n in
            ("model_serial", "lambda_bits", "gamma", "rate", "norm")))
        comparisons["normalization"] = comparisons["warp"]
        comparisons["complete"] = int(projected and all(comparisons[n] for n in
            ("destination", "source", "mark", "period", "half", "model_serial",
             "lpc", "shift", "amount", "mode", "warp")))
        av = available.get((case, block, ordinal, horizon), {})
        ctx = context.get((case, block), {})
        failures = []
        if status == 0: failures.append("NO_CANDIDATE")
        if status == 2: failures.append("NOT_PREDICTABLE")
        if status == 3: failures.append("AMBIGUOUS_MODEL_SNAPSHOT")
        for field, reason in (("destination", "DESTINATION_CHANGED"),
                              ("source", "REQUESTED_SOURCE_CHANGED"),
                              ("mark", "MARK_CHANGED"),
                              ("period", "SOURCE_PERIOD_CHANGED"),
                              ("half", "HALF_WIDTH_CHANGED"),
                              ("model_serial", "MODEL_CHANGED"),
                              ("lpc", "LPC_ENABLE_CHANGED"),
                              ("lambda_bits", "LAMBDA_CHANGED"),
                              ("gamma", "GAMMA_CHANGED"),
                              ("norm", "NORMALIZATION_CHANGED"),
                              ("mode", "FORMANT_CONTROL_CHANGED")):
            if projected and not comparisons[field]: failures.append(reason)
        if av.get("mark") == 0: failures.append("MARK_NOT_YET_AVAILABLE")
        if av.get("model") == 0: failures.append("MODEL_NOT_YET_PUBLISHED")
        if ctx.get("onset"): failures.append("ONSET_BLOCK")
        if ctx.get("recovery"): failures.append("RECOVERY_BLOCK")
        if ctx.get("pitch_changed"): failures.append("PITCH_TRANSITION_BLOCK")
        prior_pitch = ctx.get("prior_pitch", [None] * 3)[3 - horizon]
        if prior_pitch is not None and prior_pitch != ctx.get("actual_pitch"):
            failures.append("PITCH_CHANGED")
        if ctx.get("prior_cursor", [1] * 3)[3 - horizon] == 0:
            failures.append("CURSOR_UNAVAILABLE")
        row = dict(case=case, block=block, klass=f"MC+{klass}" if klass < 3 else "MC+3+",
            ordinal=ordinal, horizon=horizon, status=status, **comparisons,
            mark_available=av.get("mark", ""), model_available=av.get("model", ""),
            latest_model_matches=int(av.get("prior_model_serial", -1) ==
                                     av.get("actual_model_serial", -2)),
            reasons="|".join(failures))
        detail.append(row)
        if not comparisons["complete"]:
            for reason in set(failures): reasons[(row["klass"], horizon, reason)] += 1

    if not detail:
        raise SystemExit("no matched PRED_GRAIN/PRED_PROJ records found")
    detail_columns = list(detail[0])
    write_csv(args.output_dir / "psola_prediction_detail.csv", detail, detail_columns)
    totals = defaultdict(lambda: Counter())
    for row in detail:
        key = (row["klass"], row["horizon"])
        totals[key]["grains"] += 1
        totals[key]["predicted"] += row["status"] == 1
        for name in ("destination", "source", "mark", "period", "half",
                     "model_serial", "lpc", "warp", "normalization", "complete"):
            totals[key][name] += row[name]
    accuracy = [dict(klass=k[0], horizon=k[1], **v)
                for k, v in sorted(totals.items())]
    write_csv(args.output_dir / "psola_prediction_accuracy.csv", accuracy,
              ["klass", "horizon", "grains", "predicted", "destination",
               "source", "mark", "period", "half", "model_serial", "lpc",
               "warp", "normalization", "complete"])
    status_counts = defaultdict(Counter)
    names = {0: "NO_CANDIDATE", 1: "PREDICTED", 2: "NOT_PREDICTABLE",
             3: "AMBIGUOUS"}
    for row in detail:
        status_counts[(row["klass"], row["horizon"])][names[row["status"]]] += 1
    status_rows = [dict(klass=k[0], horizon=k[1],
                        **{name: v[name] for name in names.values()})
                   for k, v in sorted(status_counts.items())]
    write_csv(args.output_dir / "psola_prediction_status.csv", status_rows,
              ["klass", "horizon", "PREDICTED", "NOT_PREDICTABLE",
               "NO_CANDIDATE", "AMBIGUOUS"])
    failure_rows = [dict(klass=k[0], horizon=k[1], reason=k[2], count=v)
                    for k, v in sorted(reasons.items())]
    write_csv(args.output_dir / "psola_prediction_failures.csv", failure_rows,
              ["klass", "horizon", "reason", "count"])
    availability = defaultdict(Counter)
    for row in detail:
        key = (row["klass"], row["horizon"])
        availability[key]["grains"] += 1
        availability[key]["mark_already_available"] += row["mark_available"] == 1
        availability[key]["model_already_published"] += row["model_available"] == 1
        availability[key]["model_not_applicable"] += row["model_available"] == 3
        availability[key]["latest_model_matches"] += row["latest_model_matches"]
    availability_rows = [dict(klass=k[0], horizon=k[1], **v)
                         for k, v in sorted(availability.items())]
    write_csv(args.output_dir / "psola_prediction_availability.csv", availability_rows,
              ["klass", "horizon", "grains", "mark_already_available",
               "model_already_published", "model_not_applicable",
               "latest_model_matches"])
    print(f"scored {len(detail)} grain/horizon records from {len(actual)} grains")


if __name__ == "__main__":
    main()
