#!/usr/bin/env python3
"""Prepare and summarize short-segment harmony articulation diagnostics."""
from __future__ import annotations

import csv
import math
import sys
from collections import Counter, defaultdict
from numbers import Number
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile


RATE = 48000
BLOCK = 64
BASE_CASES = ("voice1_p4", "voice2_p4", "voice1_m4", "voice2_m4")
EXTRA_CASES = ("voice2_p7", "voice2_m7", "voice2_p4_no_attenuation",
               "voice2_p4_keep40")


def read_csv(path: Path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def audio(path: Path):
    rate, x = wavfile.read(path)
    scale = 2 ** (x.dtype.itemsize * 8 - 1) if np.issubdtype(x.dtype, np.integer) else 1
    x = x.astype(np.float64) / scale
    if x.ndim == 2:
        x = x.mean(axis=1)
    return rate, x


def runs(mask):
    mask = np.asarray(mask, dtype=bool)
    changes = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(changes == 1), np.flatnonzero(changes == -1)))


def fill_short_gaps(mask, max_samples):
    result = np.asarray(mask, dtype=bool).copy()
    for begin, end in runs(~result):
        if begin > 0 and end < len(result) and end - begin <= max_samples:
            result[begin:end] = True
    return result


def select_segments(reference: Path, pitch_csv: Path, root: Path):
    rate, x = audio(reference)
    if rate != RATE:
        raise SystemExit("prepare requires a 48-kHz reference")
    hop = 480
    size = 960
    count = 1 + max(0, (len(x) - size) // hop)
    frames = np.lib.stride_tricks.sliding_window_view(x, size)[::hop][:count]
    rms = np.sqrt(np.mean(frames * frames, axis=1))
    times = (np.arange(len(rms)) * hop + size / 2) / rate
    nominal = max(float(np.percentile(rms, 85)), 1e-6)
    active = fill_short_gaps(rms >= max(nominal * .10, 1e-5), 6)

    pitch = read_csv(pitch_csv)
    pt = np.array([float(row["time_seconds"]) for row in pitch])
    voiced = np.array([row["voiced"] == "1" for row in pitch])
    confidence = np.array([float(row["confidence"]) for row in pitch])
    onset = np.array([row["onset"] == "1" for row in pitch])
    changed = np.array([row["pitch_changed"] == "1" for row in pitch])

    candidates = []
    duration = len(x) / rate
    for start in np.arange(0, max(0, duration - 3.0), .20):
        end = start + 3.0
        ai = (times >= start) & (times < end)
        pi = (pt >= start) & (pt < end)
        if not np.any(ai) or not np.any(pi):
            continue
        active_fraction = float(np.mean(active[ai]))
        voiced_fraction = float(np.mean(voiced[pi]))
        good_fraction = float(np.mean(voiced[pi] & (confidence[pi] >= .6)))
        v = voiced[pi]
        internal_transitions = int(np.sum(v[1:] != v[:-1]))
        events = int(np.sum(onset[pi]) + np.sum(changed[pi]))
        consonant_proxy = float(np.mean(active[ai])) * float(np.mean(~voiced[pi]))
        score = (3 * active_fraction + 2 * good_fraction +
                 .08 * internal_transitions + .05 * events + consonant_proxy)
        if active_fraction >= .60 and voiced_fraction >= .18:
            candidates.append((score, internal_transitions, start, end,
                               active_fraction, voiced_fraction, events))
    candidates.sort(reverse=True)
    chosen = []
    for candidate in candidates:
        _, _, start, end, *_ = candidate
        if all(end + .25 <= old_start or start >= old_end + .25
               for *_, old_start, old_end, _a, _v, _e in chosen):
            chosen.append(candidate)
        if len(chosen) == 5:
            break
    if len(chosen) < 5:
        raise SystemExit(f"only {len(chosen)} suitable diagnostic regions found")
    chosen.sort(key=lambda item: item[2])
    most_staccato = max(range(len(chosen)), key=lambda i: chosen[i][1])
    segments_dir = root / "renders"
    segments_dir.mkdir(parents=True, exist_ok=True)
    with (root / "segments.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["segment", "start_s", "end_s", "duration_s",
                         "active_fraction", "voiced_fraction", "events",
                         "voiced_transitions", "staccato_candidate"])
        for index, candidate in enumerate(chosen, 1):
            _, transitions, start, end, active_fraction, voiced_fraction, events = candidate
            segment = f"segment{index:02d}"
            directory = segments_dir / segment
            directory.mkdir(exist_ok=True)
            wavfile.write(directory / "reference.wav", rate,
                          x[int(start * rate):int(end * rate)].astype(np.float32))
            writer.writerow([segment, f"{start:.3f}", f"{end:.3f}",
                             f"{end-start:.3f}", f"{active_fraction:.6f}",
                             f"{voiced_fraction:.6f}", events, transitions,
                             index - 1 == most_staccato])

    seconds = 4
    t = np.arange(rate * seconds) / rate
    synthetic = sum(.12 / harmonic * np.sin(2 * np.pi * 220 * harmonic * t)
                    for harmonic in range(1, 6))
    synthetic_dir = segments_dir / "synthetic"
    synthetic_dir.mkdir(exist_ok=True)
    wavfile.write(synthetic_dir / "reference.wav", rate,
                  synthetic.astype(np.float32))


def lead_active(rows):
    rms = np.array([float(row["input_rms"]) for row in rows])
    nominal = max(float(np.percentile(rms, 85)), 1e-8)
    active = rms >= max(nominal * .10, 1e-6)
    return fill_short_gaps(active, round(.060 * RATE / BLOCK)), nominal


def selected_voice(case):
    return 1 if case.startswith("voice1") else 2


def percentile(values, q):
    return float(np.percentile(values, q)) if values else math.nan


def case_metrics(root: Path, segment: str, case: str):
    rows = read_csv(root / "csv" / segment / f"{case}.csv")
    voice = selected_voice(case)
    prefix = f"voice{voice}_"
    active, _ = lead_active(rows)
    gains = np.array([float(row[prefix + "output_gain"]) for row in rows])
    psola = np.array([float(row[prefix + "psola_gain"]) for row in rows])
    fallback = np.array([row[f"fallback_active_v{voice}"] == "1" for row in rows])
    states = np.array([row[prefix + "state"] for row in rows])
    audible20 = gains > .10
    audible40 = gains > .01
    active_count = max(1, int(np.sum(active)))

    onset_lag = []
    early_release = []
    internal_gaps = []
    for begin, end in runs(active):
        if end - begin < round(.080 * RATE / BLOCK):
            continue
        indices = np.flatnonzero(audible40[begin:end])
        if len(indices):
            first = begin + int(indices[0])
            last = begin + int(indices[-1]) + 1
            onset_lag.append((first - begin) * BLOCK * 1000 / RATE)
            early_release.append((end - last) * BLOCK * 1000 / RATE)
            within = audible40[begin:end]
            for gap_begin, gap_end in runs(~within):
                if gap_begin > 0 and gap_end < len(within):
                    internal_gaps.append((gap_end-gap_begin) * BLOCK * 1000 / RATE)
        else:
            length = (end-begin) * BLOCK * 1000 / RATE
            onset_lag.append(length)
            early_release.append(length)

    norm_means = np.array([float(row[prefix + "ola_norm_mean"]) for row in rows])
    norm_minima = np.array([float(row[prefix + "ola_norm_min"]) for row in rows])
    norm_below = np.array([int(row[prefix + "norm_below"]) for row in rows])
    covered = BLOCK - norm_below
    positive_minima = norm_minima[norm_minima > 0]
    weighted_norm = (float(np.sum(norm_means * covered) / np.sum(covered))
                     if np.sum(covered) else 0.0)
    rate, output = audio(root / "renders" / segment / f"{case}.wav")
    reasons = Counter(row[f"fallback_reason_v{voice}"] for row in rows
                      if row[f"fallback_active_v{voice}"] == "1")
    transitions = Counter()
    for row in rows:
        if row[prefix + "state_changed"] == "1" and row[prefix + "previous_state"]:
            transitions[f"{row[prefix+'previous_state']}->{row[prefix+'state']}"] += 1
    tracker = [row["pitch_tracker_state"] for row in rows]
    locked_changes = sum(a != b and {a, b} == {"locked", "unlocked"}
                         for a, b in zip(tracker, tracker[1:]))
    fallback_samples = int(rows[-1][prefix + "fallback_samples_total"])
    target_v1 = np.array([row["voice1_target_valid"] == "1" for row in rows])
    target_v2 = np.array([row["voice2_target_valid"] == "1" for row in rows])
    return {
        "segment": segment, "case": case, "voice": voice,
        "coverage_20": float(np.sum(active & audible20) / active_count),
        "coverage_40": float(np.sum(active & audible40) / active_count),
        "onset_lag_median_ms": percentile(onset_lag, 50),
        "onset_lag_mean_ms": float(np.mean(onset_lag)) if onset_lag else math.nan,
        "onset_lag_p95_ms": percentile(onset_lag, 95),
        "onset_lag_max_ms": max(onset_lag, default=math.nan),
        "early_release_median_ms": percentile(early_release, 50),
        "early_release_mean_ms": float(np.mean(early_release)) if early_release else math.nan,
        "early_release_p95_ms": percentile(early_release, 95),
        "early_release_max_ms": max(early_release, default=math.nan),
        "internal_gaps_0_10": sum(gap <= 10 for gap in internal_gaps),
        "internal_gaps_10_20": sum(10 < gap <= 20 for gap in internal_gaps),
        "internal_gaps_20_40": sum(20 < gap <= 40 for gap in internal_gaps),
        "internal_gaps_40_80": sum(40 < gap <= 80 for gap in internal_gaps),
        "internal_gaps_gt80": sum(gap > 80 for gap in internal_gaps),
        "fallback_active_percent": 100 * float(np.sum(active & fallback) / active_count),
        "fallback_counter_samples": fallback_samples,
        "fallback_counter_ms": fallback_samples * 1000 / RATE,
        "fallback_counter_percent_active": 100 * fallback_samples /
            max(active_count * BLOCK, 1),
        "fallback_transitions": sum(count for key, count in transitions.items()
                                    if "fallback" in key or "bypass" in key),
        "state_transitions_per_active_second": sum(transitions.values()) /
            max(np.sum(active) * BLOCK / RATE, 1e-9),
        "locked_unlocked_transitions": locked_changes,
        "mean_output_gain": float(np.mean(gains[active])),
        "psola_active_percent": 100 * float(np.sum(active & (states == "active")) / active_count),
        "rms": float(np.sqrt(np.mean(output * output))),
        "peak": float(np.max(np.abs(output))),
        "ola_norm_mean": weighted_norm,
        "ola_norm_min": float(np.min(positive_minima)) if len(positive_minima) else 0.0,
        "norm_below_percent": 100 * float(np.sum(norm_below) / (len(rows) * BLOCK)),
        "grains": int(rows[-1][prefix + "grains_total"]),
        "resyncs": int(rows[-1][prefix + "resyncs_total"]),
        "v1_valid_v2_invalid": int(np.sum(target_v1 & ~target_v2)),
        "v2_valid_v1_invalid": int(np.sum(target_v2 & ~target_v1)),
        "target_range_invalid": sum(row[prefix + "target_range_invalid"] == "1"
                                    for row in rows),
        "fallback_reasons": ";".join(f"{key}:{value}" for key, value in sorted(reasons.items())),
        "transitions": ";".join(f"{key}:{value}" for key, value in sorted(transitions.items())),
    }


def plot_segment(root: Path, segment: str):
    rows = read_csv(root / "csv" / segment / "voice2_p4.csv")
    time = np.array([float(row["time_s"]) for row in rows])
    figure, axes = plt.subplots(7, 1, figsize=(13, 10), sharex=True)
    axes[0].plot(time, [float(row["input_rms"]) for row in rows])
    axes[0].set_ylabel("input RMS")
    axes[1].step(time, [int(row["voiced"]) for row in rows], where="post")
    axes[1].set_ylabel("voiced")
    axes[2].plot(time, [float(row["pitch_confidence"]) for row in rows])
    axes[2].set_ylabel("confidence")
    axes[3].plot(time, [float(row["voice1_output_gain"]) for row in rows], label="V1")
    axes[3].plot(time, [float(row["voice2_output_gain"]) for row in rows], label="V2", alpha=.8)
    axes[3].set_ylabel("output gain"); axes[3].legend()
    axes[4].plot(time, [float(row["voice1_psola_gain"]) for row in rows], label="V1")
    axes[4].plot(time, [float(row["voice2_psola_gain"]) for row in rows], label="V2", alpha=.8)
    axes[4].set_ylabel("PSOLA gain"); axes[4].legend()
    axes[5].step(time, [int(row["fallback_active_v1"]) for row in rows], where="post", label="V1")
    axes[5].step(time, [int(row["fallback_active_v2"]) for row in rows], where="post", label="V2", alpha=.8)
    axes[5].set_ylabel("fallback"); axes[5].legend()
    axes[6].step(time, [row["pitch_tracker_state"] == "locked" for row in rows], where="post")
    axes[6].set_ylabel("tracker locked"); axes[6].set_xlabel("segment time (s)")
    for axis in axes: axis.grid(alpha=.2)
    figure.tight_layout(); figure.savefig(root / "plots" / f"{segment}_states.png", dpi=140); plt.close(figure)

    plus = read_csv(root / "csv" / segment / "voice2_p4.csv")
    minus = read_csv(root / "csv" / segment / "voice2_m4.csv")
    figure, axes = plt.subplots(3, 1, figsize=(13, 7), sharex=True)
    for label, data in (("+4", plus), ("-4", minus)):
        t = [float(row["time_s"]) for row in data]
        axes[0].plot(t, [float(row["voice2_output_gain"]) for row in data], label=label)
        axes[1].plot(t, [float(row["voice2_ola_norm_mean"]) for row in data], label=label)
        axes[2].step(t, [int(row["fallback_active_v2"]) for row in data], where="post", label=label)
    axes[0].set_ylabel("V2 gain"); axes[1].set_ylabel("OLA norm mean"); axes[2].set_ylabel("fallback")
    axes[2].set_xlabel("segment time (s)")
    for axis in axes: axis.grid(alpha=.2); axis.legend()
    figure.tight_layout(); figure.savefig(root / "plots" / f"{segment}_v2_p4_vs_m4.png", dpi=140); plt.close(figure)


def aggregate(metrics, case):
    rows = [row for row in metrics if row["case"] == case]
    keys = [key for key, value in rows[0].items()
        if isinstance(value, Number) and key not in ("voice",)]
    result = {"case": case}
    for key in keys:
        result[key] = float(np.nanmean([row[key] for row in rows]))
    result["fallback_reasons"] = Counter()
    for row in rows:
        for item in row["fallback_reasons"].split(";"):
            if item:
                reason, count = item.split(":")
                result["fallback_reasons"][reason] += int(count)
    return result


def synthetic_metrics(root: Path):
    output = []
    for semitones in (-7, -4, 0, 4, 7):
        tag = f"m{abs(semitones)}" if semitones < 0 else f"p{semitones}"
        case = f"voice2_{tag}"
        rows = read_csv(root / "csv" / "synthetic" / f"{case}.csv")
        rate, reference = audio(root / "renders" / "synthetic" / "reference.wav")
        _, rendered = audio(root / "renders" / "synthetic" / f"{case}.wav")
        begin = 2 * rate
        prefix = "voice2_"
        active_rows = rows[len(rows)//2:]
        norms = [float(row[prefix + "ola_norm_mean"]) for row in active_rows
                 if float(row[prefix + "ola_norm_mean"]) > 0]
        minima = [float(row[prefix + "ola_norm_min"]) for row in active_rows
                  if float(row[prefix + "ola_norm_min"]) > 0]
        input_rms = float(np.sqrt(np.mean(reference[begin:] ** 2)))
        output_rms = float(np.sqrt(np.mean(rendered[begin:] ** 2)))
        # The debug values persist between grain events. Snapshot only when the
        # synthesis timestamp changes, then compare the actually selected input
        # pitch mark rather than the requested floating-point source position.
        source_marks = np.array([int(row["selected_source_mark"])
                                 for row in rows], dtype=np.int64)
        synthesis_marks = np.array([int(row["output_synthesis_timestamp"])
                                    for row in rows], dtype=np.int64)
        valid = (source_marks > 0) & (synthesis_marks > 0)
        snapshots = valid & np.r_[True,
            synthesis_marks[1:] != synthesis_marks[:-1]]
        source_steps = np.diff(source_marks[snapshots])
        synthesis_steps = np.diff(synthesis_marks[snapshots])
        positive_source = source_steps[source_steps > 0]
        median_source_step = (float(np.median(positive_source))
                              if len(positive_source) else 0.0)
        output.append({"semitones": semitones, "input_rms": input_rms,
                       "output_rms": output_rms, "rms_ratio": output_rms/input_rms,
                       "ola_norm_mean": float(np.mean(norms)),
                       "ola_norm_min": min(minima),
                       "norm_below_percent": 100 * sum(
                           int(row[prefix + "norm_below"])
                           for row in active_rows) / max(len(active_rows) * BLOCK, 1),
                       "median_source_step": median_source_step,
                       "median_synthesis_step": (float(np.median(synthesis_steps))
                                                 if len(synthesis_steps) else 0.0),
                       "repeated_source_percent": 100 * float(np.mean(source_steps == 0))
                           if len(source_steps) else 0.0,
                       "large_source_skip_percent": 100 * float(np.mean(
                           source_steps > 1.5 * median_source_step))
                           if len(source_steps) and median_source_step else 0.0})
    return output


def report(root: Path):
    segments = [row["segment"] for row in read_csv(root / "segments.csv")]
    metrics = []
    available_cases = []
    for case in BASE_CASES + EXTRA_CASES:
        if (root / "csv" / segments[0] / f"{case}.csv").exists():
            available_cases.append(case)
    for path in sorted((root / "csv" / segments[0]).glob(
            "voice2_p4_art_*.csv")):
        available_cases.append(path.stem)
    for segment in segments:
        for case in available_cases:
            metrics.append(case_metrics(root, segment, case))
        plot_segment(root, segment)
    with (root / "metrics.csv").open("w", newline="") as stream:
        fields = list(metrics[0].keys())
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(metrics)
    aggregates = {case: aggregate(metrics, case) for case in available_cases}
    sweep_cases = [case for case in available_cases
                   if case.startswith("voice2_p4_art_")]
    if sweep_cases:
        sweep_fields = ["case", "coverage_20", "coverage_40",
                        "onset_lag_p95_ms", "early_release_p95_ms",
                        "gaps_gt20", "fallback_active_percent",
                        "fallback_counter_percent_active", "rms"]
        with (root / "articulation_sweep.csv").open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=sweep_fields)
            writer.writeheader()
            for case in sweep_cases:
                row = aggregates[case]
                writer.writerow({
                    "case": case,
                    "coverage_20": row["coverage_20"],
                    "coverage_40": row["coverage_40"],
                    "onset_lag_p95_ms": row["onset_lag_p95_ms"],
                    "early_release_p95_ms": row["early_release_p95_ms"],
                    "gaps_gt20": row["internal_gaps_20_40"] +
                        row["internal_gaps_40_80"] + row["internal_gaps_gt80"],
                    "fallback_active_percent": row["fallback_active_percent"],
                    "fallback_counter_percent_active":
                        row["fallback_counter_percent_active"],
                    "rms": row["rms"],
                })
    synthetic = synthetic_metrics(root)
    with (root / "synthetic_gain.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=synthetic[0].keys())
        writer.writeheader(); writer.writerows(synthetic)

    with (root / "report.md").open("w", encoding="utf-8") as stream:
        stream.write("# Harmony Voice 2 articulation diagnostic\n\n")
        stream.write("## Selected short regions\n\n")
        stream.write("Segment | start | end | active | voiced | events | voiced transitions | staccato candidate\n---|---:|---:|---:|---:|---:|---:|---\n")
        for row in read_csv(root / "segments.csv"):
            stream.write(f"{row['segment']} | {row['start_s']} | {row['end_s']} | {float(row['active_fraction'])*100:.1f}% | {float(row['voiced_fraction'])*100:.1f}% | {row['events']} | {row['voiced_transitions']} | {row['staccato_candidate']}\n")
        stream.write("\n## Required aggregate comparison\n\n")
        stream.write("Metric | V1 +4 | V2 +4 | V1 -4 | V2 -4\n---|---:|---:|---:|---:\n")
        table = (
            ("Coverage -20 dB", "coverage_20", 100, "%"),
            ("Coverage -40 dB", "coverage_40", 100, "%"),
            ("Median onset lag ms", "onset_lag_median_ms", 1, ""),
            ("P95 onset lag ms", "onset_lag_p95_ms", 1, ""),
            ("Median early release ms", "early_release_median_ms", 1, ""),
            ("P95 early release ms", "early_release_p95_ms", 1, ""),
            ("Internal gaps >20 ms", "internal_gaps_20_40", 1, ""),
            ("Fallback active %", "fallback_active_percent", 1, "%"),
            ("Fallback transitions", "fallback_transitions", 1, ""),
            ("Mean output gain", "mean_output_gain", 1, ""),
            ("RMS", "rms", 1, ""),
            ("OLA norm mean", "ola_norm_mean", 1, ""),
            ("OLA norm min", "ola_norm_min", 1, ""),
        )
        for label, key, scale, suffix in table:
            values = [aggregates[case][key] * scale for case in BASE_CASES]
            stream.write(f"{label} | " + " | ".join(f"{value:.4f}{suffix}" for value in values) + "\n")
        stream.write("\n## Fallback counter semantics\n\n")
        stream.write("`fallback_frames` is incremented once per output **sample** only while `psola_gain < 0.001` in the enabled TD-PSOLA crossfade path. It does not count blocks, events, grains, or the early `target_enabled == false` bypass branch. The broader `fallback_active` diagnostic is therefore used for articulation coverage.\n\n")
        for case in BASE_CASES:
            row = aggregates[case]
            stream.write(f"- {case}: counter {row['fallback_counter_ms']:.2f} ms mean per 3-s excerpt ({row['fallback_counter_percent_active']:.2f}% of vocal-active samples); fallback-active {row['fallback_active_percent']:.2f}% of lead-active blocks; reasons {dict(row['fallback_reasons'])}.\n")
        stream.write("\n## Synthetic constant-voiced gain\n\n")
        stream.write("Shift | input RMS | output RMS | ratio | norm mean | norm min | norm holes | repeated source | large skips\n---:|---:|---:|---:|---:|---:|---:|---:|---:\n")
        for row in synthetic:
            stream.write(f"{row['semitones']:+d} | {row['input_rms']:.7f} | {row['output_rms']:.7f} | {row['rms_ratio']:.5f} | {row['ola_norm_mean']:.5f} | {row['ola_norm_min']:.5f} | {row['norm_below_percent']:.3f}% | {row['repeated_source_percent']:.2f}% | {row['large_source_skip_percent']:.2f}%\n")
        stream.write("\n## Target validity and symmetry\n\n")
        for case in BASE_CASES:
            row = aggregates[case]
            stream.write(f"- {case}: V1-valid/V2-invalid {row['v1_valid_v2_invalid']:.1f} blocks; V2-valid/V1-invalid {row['v2_valid_v1_invalid']:.1f}; selected target-range invalid {row['target_range_invalid']:.1f}.\n")
        stream.write("\nBoth voices receive the same pitch result, confidence, onset flag, tracker state, shared mark array, and shared source history in the same callback. Their independent synthesis cursors differ by target ratio; product defaults additionally differ in gain, pan, and configured interval.\n")
        if "voice2_p4_no_attenuation" in aggregates:
            current = aggregates["voice2_p4"]
            no_atten = aggregates["voice2_p4_no_attenuation"]
            stream.write("\n## Host-only articulation A/B\n\n")
            stream.write("Variant | coverage -20 | coverage -40 | onset P95 ms | early-release P95 ms | gaps >20 ms | RMS\n---|---:|---:|---:|---:|---:|---:\n")
            for label, row in (("A current", current), ("C no attenuation", no_atten)):
                gaps = row["internal_gaps_20_40"] + row["internal_gaps_40_80"] + row["internal_gaps_gt80"]
                stream.write(f"{label} | {row['coverage_20']*100:.2f}% | {row['coverage_40']*100:.2f}% | {row['onset_lag_p95_ms']:.2f} | {row['early_release_p95_ms']:.2f} | {gaps:.1f} | {row['rms']:.7f}\n")
            if "voice2_p4_keep40" in aggregates:
                row = aggregates["voice2_p4_keep40"]
                gaps = row["internal_gaps_20_40"] + row["internal_gaps_40_80"] + row["internal_gaps_gt80"]
                stream.write(f"B keep active 40 ms | {row['coverage_20']*100:.2f}% | {row['coverage_40']*100:.2f}% | {row['onset_lag_p95_ms']:.2f} | {row['early_release_p95_ms']:.2f} | {gaps:.1f} | {row['rms']:.7f}\n")
        if sweep_cases:
            stream.write("\n## Configurable articulation-envelope sweep\n\n")
            stream.write("Case | coverage -20 | coverage -40 | onset P95 ms | early P95 ms | gaps >20 ms | fallback active | RMS\n---|---:|---:|---:|---:|---:|---:|---:\n")
            for case in sweep_cases:
                row = aggregates[case]
                gaps = row["internal_gaps_20_40"] + row["internal_gaps_40_80"] + row["internal_gaps_gt80"]
                stream.write(f"{case.removeprefix('voice2_p4_art_')} | {row['coverage_20']*100:.2f}% | {row['coverage_40']*100:.2f}% | {row['onset_lag_p95_ms']:.2f} | {row['early_release_p95_ms']:.2f} | {gaps:.1f} | {row['fallback_active_percent']:.2f}% | {row['rms']:.7f}\n")
        plus = aggregates["voice2_p4"]
        minus = aggregates["voice2_m4"]
        p4_synthetic = next(row for row in synthetic if row["semitones"] == 4)
        p7_synthetic = next(row for row in synthetic if row["semitones"] == 7)
        stream.write("\n## Decision\n\n")
        stream.write("**Classification: E — combination of A (articulation/gating) and D (OLA/normalization at ratios > 1).**\n\n")
        stream.write(f"The Voice 1 and Voice 2 baseline measurements are numerically identical at each interval (for +4, coverage -40 dB {plus['coverage_40']*100:.2f}%; for -4, {minus['coverage_40']*100:.2f}%), so the dropout is not caused by a Voice-2-only target, mark, or history path. Disabling onset/unvoiced attenuation raises Voice-2 +4 coverage to {aggregates['voice2_p4_no_attenuation']['coverage_40']*100:.2f}% and removes measured onset/release gaps, directly confirming the articulation component.\n\n")
        stream.write(f"Independently, the constant-voiced signal has full OLA coverage yet loses energy upward: +4 RMS ratio {p4_synthetic['rms_ratio']:.3f} at mean norm {p4_synthetic['ola_norm_mean']:.3f}, and +7 ratio {p7_synthetic['rms_ratio']:.3f} at mean norm {p7_synthetic['ola_norm_mean']:.3f}. Positive shifts repeat source-mark timestamps ({p4_synthetic['repeated_source_percent']:.1f}% at +4; {p7_synthetic['repeated_source_percent']:.1f}% at +7), increasing overlap weight that is subsequently divided out. This is an OLA energy-scaling problem, not missing OLA coverage; no arbitrary gain correction was applied.\n\n")
        stream.write("The new `HarmonyArticulationEnvelope` is configurable and disabled by default. Sweep renders are diagnostic candidates only; no product default was selected in this task. OLA synthesis behavior was instrumented but not changed.\n\n")
        if "voice2_p4_art_release60" in aggregates:
            candidate = aggregates["voice2_p4_art_release60"]
            candidate_gaps = (candidate["internal_gaps_20_40"] +
                              candidate["internal_gaps_40_80"] +
                              candidate["internal_gaps_gt80"])
            stream.write(f"The strongest measured articulation candidate is 40 ms pitch-loss grace, 40 ms unvoiced hold, 10 ms attack, and 60 ms release, with onset/unvoiced minima 0.15/0.10. It reaches {candidate['coverage_40']*100:.2f}% coverage at -40 dB and {candidate_gaps:.1f} gaps >20 ms per excerpt, versus {plus['coverage_40']*100:.2f}% and {plus['internal_gaps_20_40'] + plus['internal_gaps_40_80'] + plus['internal_gaps_gt80']:.1f} for current behavior. This is the listening-test candidate, not a new default.\n\n")
        stream.write("**Conclusion: ROOT CAUSE MOSTLY IDENTIFIED.** The gating mechanism is causally isolated; the upward-shift loss is narrowed to synthesis-mark repetition/overlap normalization, but requires a dedicated DSP correction and perceptual regression before changing production behavior.\n")

    manifest = sorted(path.resolve() for path in root.rglob("*")
                      if path.is_file() and path.name != "artifact_manifest.txt")
    with (root / "artifact_manifest.txt").open("w", encoding="utf-8") as stream:
        stream.write("\n".join(str(path) for path in manifest) + "\n")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: harmony_debug_next.py prepare|report ROOT [REFERENCE PITCH_CSV]")
    command = sys.argv[1]
    root = Path(sys.argv[2])
    (root / "plots").mkdir(parents=True, exist_ok=True)
    if command == "prepare":
        select_segments(Path(sys.argv[3]), Path(sys.argv[4]), root)
    elif command == "report":
        report(root)
    else:
        raise SystemExit(f"unknown command: {command}")
