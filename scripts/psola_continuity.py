#!/usr/bin/env python3
"""Render and measure the host-only Milestone 5.5 continuity policies.

Rendering is always delegated to the repository C++ `pitch_shift` executable.
Python only prepares/crops WAVs, reduces exported telemetry, computes metrics,
and plots the measured state transitions.
"""
from __future__ import annotations

import csv
import math
import shutil
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

RATE = 48000
SOURCE_BEGIN = 4.9
LISTEN_BEGIN = 6.4
LISTEN_END = 9.4
OFFSET = LISTEN_BEGIN - SOURCE_BEGIN
LENGTH = LISTEN_END - LISTEN_BEGIN
COASTS = (10, 20, 30, 40, 60)


def read_audio(path: Path):
    rate, data = wavfile.read(path)
    if np.issubdtype(data.dtype, np.integer):
        data = data.astype(np.float64) / (2 ** (8 * data.dtype.itemsize - 1))
    else:
        data = data.astype(np.float64)
    if data.ndim == 2:
        data = data.mean(axis=1)
    if rate != RATE:
        divisor = math.gcd(rate, RATE)
        data = resample_poly(data, RATE // divisor, rate // divisor)
    return np.asarray(data, dtype=np.float64)


def write_audio(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, RATE, np.asarray(data, dtype=np.float32))


def crop_audio(data):
    begin = round(OFFSET * RATE)
    return data[begin:begin + round(LENGTH * RATE)]


def runs(mask):
    edge = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(edge == 1), np.flatnonzero(edge == -1)))


def moving_rms(data, window=960):
    return np.sqrt(np.convolve(data * data, np.ones(window) / window, "same"))


def active_mask(reference):
    rms = moving_rms(reference)
    return rms >= max(float(np.percentile(rms, 85)), 1e-8) * .10


def execute(exe: Path, context: Path, output: Path, shift: int, policy: str,
            coast_ms: int, stem="final", telemetry: Path | None = None,
            fallback="none", full=False):
    full_output = output if full else output.with_name(output.stem + "_context.wav")
    command = [str(exe), str(context), str(full_output), str(shift), "--voice", "2",
               "--formants", "off", "--fallback-policy", fallback,
               "--continuity-policy", policy, "--coast-ms", str(coast_ms),
               "--ola-normalization", "hybrid", "--stem", stem,
               "--apply-voice-envelope"]
    if telemetry:
        command += ["--continuity-csv", str(telemetry)]
    started = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(f"render failed: {' '.join(command)}\n{result.stderr}")
    elapsed = time.perf_counter() - started
    if not full:
        write_audio(output, crop_audio(read_audio(full_output)))
        full_output.unlink()
    return elapsed, result.stderr.strip()


def reduce_telemetry(source: Path, destination: Path | None):
    rows = []
    stream = destination.open("w", newline="") if destination else None
    writer = None
    with source.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if stream:
            writer = csv.DictWriter(stream, fieldnames=reader.fieldnames)
            writer.writeheader()
        for row in reader:
            context_time = float(row["time_s"])
            if context_time < OFFSET:
                continue
            if context_time >= OFFSET + LENGTH:
                break
            row["sample"] = str(int(row["sample"]) + round(SOURCE_BEGIN * RATE))
            row["time_s"] = f"{context_time + SOURCE_BEGIN:.8f}"
            rows.append(row)
            if writer:
                writer.writerow(row)
    if stream:
        stream.close()
    source.unlink()
    return rows


def metric(name, rows, audio, active):
    measured = np.array([r["measured_psola_valid"] == "1" for r in rows])
    coasted = np.array([r["coasted_psola_valid"] == "1" for r in rows])
    total = measured | coasted
    count = max(1, int(active.sum()))
    gaps = []
    for begin, end in runs(active):
        for a, b in runs(~total[begin:end]):
            if a and b < end - begin:
                gaps.append((b - a) * 1000 / RATE)
    recovery = [abs(float(r["recovery_period_error_cents"])) for r in rows
                if r["resync_event"] == "1" and r["soft_resync"] == "1"]
    tracker = np.array([r["pitch_tracker_state"] for r in rows])
    resyncs = sum(r["resync_event"] == "1" for r in rows)
    return {
        "policy": name,
        "measured_psola": 100 * int(np.sum(active & measured)) / count,
        "coasted_psola": 100 * int(np.sum(active & coasted)) / count,
        "total_coverage": 100 * int(np.sum(active & total)) / count,
        "coast_percent_vocal_active": 100 * int(np.sum(active & coasted)) / count,
        "coast_percent_total_psola": 100 * int(np.sum(active & coasted)) /
                                      max(1, int(np.sum(active & total))),
        "gaps_gt10ms": sum(x > 10 for x in gaps),
        "gaps_gt20ms": sum(x > 20 for x in gaps),
        "gaps_gt40ms": sum(x > 40 for x in gaps),
        "acquiring_ms": 1000 * int(np.sum(active & (tracker == "acquiring"))) / RATE,
        "resyncs": resyncs,
        "pitch_discontinuity_median_cents": float(np.median(recovery)) if recovery else 0,
        "pitch_discontinuity_p95_cents": float(np.percentile(recovery, 95)) if recovery else 0,
        "pitch_discontinuity_max_cents": max(recovery, default=0),
        "rms": float(np.sqrt(np.mean(audio * audio))),
        "peak": float(np.max(np.abs(audio))),
        "nan_inf": int(np.sum(~np.isfinite(audio))),
        "clipped_samples": int(np.sum(np.abs(audio) >= 1)),
    }


def acquiring_causes(rows, output: Path):
    states = np.array([r["pitch_tracker_state"] for r in rows])
    acquiring = states == "acquiring"
    records = []
    for begin, end in runs(acquiring):
        look = rows[max(0, begin - round(.080 * RATE)):begin + 1]
        if any(r["onset"] == "1" for r in look):
            cause = "ONSET"
        elif any(float(r["pitch_confidence"]) < .60 for r in look):
            cause = "LOW_CONFIDENCE"
        elif any(r["pitch_changed"] == "1" for r in look):
            cause = "PITCH_CHANGED"
        elif any(int(r["mark_failure_count"]) > 0 for r in look):
            cause = "MARK_FAILURE"
        elif not any(r["voiced"] == "1" for r in look):
            cause = "UNVOICED"
        else:
            cause = "OTHER"
        records.append({"cause": cause, "start_s": rows[begin]["time_s"],
                        "end_s": rows[end - 1]["time_s"],
                        "duration_ms": (end - begin) * 1000 / RATE})
    summary = []
    for cause in ("ONSET", "LOW_CONFIDENCE", "UNVOICED", "PITCH_CHANGED",
                  "MARK_FAILURE", "MARK_CONFIDENCE", "TARGET_INVALID",
                  "HISTORY_INVALID", "PERIOD_INVALID", "OTHER"):
        selected = [r for r in records if r["cause"] == cause]
        summary.append({"cause": cause, "count": len(selected),
                        "total_duration_ms": sum(r["duration_ms"] for r in selected),
                        "percent_acquiring_episodes":
                            100 * len(selected) / max(1, len(records))})
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=summary[0].keys())
        writer.writeheader(); writer.writerows(summary)
    durations = [r["duration_ms"] for r in records]
    return records, {
        "episodes": len(records), "mean_ms": float(np.mean(durations)) if durations else 0,
        "median_ms": float(np.median(durations)) if durations else 0,
        "p95_ms": float(np.percentile(durations, 95)) if durations else 0,
        "max_ms": max(durations, default=0),
    }


def event_tables(baseline, combined, metrics: Path):
    onset_records = []
    onset_mask = np.array([r["onset"] == "1" for r in baseline])
    total = np.array([r["psola_valid"] == "1" for r in baseline])
    for begin, _ in runs(onset_mask):
        before = baseline[max(0, begin - 1)]
        after_index = min(len(baseline) - 1, begin + round(.020 * RATE))
        after = baseline[after_index]
        future_valid = np.flatnonzero(total[begin:])
        first_valid = begin + future_valid[0] if len(future_valid) else len(baseline) - 1
        future_locked = next((i for i in range(begin, len(baseline))
                              if baseline[i]["pitch_tracker_state"] == "locked"),
                             len(baseline) - 1)
        onset_records.append({
            "onset_time_s": baseline[begin]["time_s"],
            "pitch_before_hz": before["pitch_hz"],
            "confidence_before": before["pitch_confidence"],
            "pitch_after_hz": after["pitch_hz"],
            "confidence_after": after["pitch_confidence"],
            "tracker_before": before["pitch_tracker_state"],
            "tracker_after": after["pitch_tracker_state"],
            "mark_valid_before": before["mark_valid"],
            "first_valid_mark_after_s": baseline[first_valid]["time_s"],
            "psola_interruption_ms": (first_valid - begin) * 1000 / RATE,
            "reacquisition_ms": (future_locked - begin) * 1000 / RATE,
        })
    with (metrics / "onset_events.csv").open("w", newline="") as stream:
        fields = list(onset_records[0].keys()) if onset_records else ["onset_time_s"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(onset_records)

    recovery = []
    for row in combined:
        if row["resync_event"] == "1":
            recovery.append({k: row[k] for k in (
                "sample", "time_s", "state_transition", "soft_resync",
                "hard_resync", "recovery_mark_error_samples",
                "recovery_period_error_cents")})
    with (metrics / "recovery_events.csv").open("w", newline="") as stream:
        fields = list(recovery[0].keys()) if recovery else ["sample"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(recovery)
    return onset_records, recovery


def make_plots(rows, plots: Path):
    plots.mkdir(parents=True, exist_ok=True)
    times = np.array([float(r["time_s"]) for r in rows])
    confidence = np.array([float(r["pitch_confidence"]) for r in rows])
    pitch = np.array([float(r["pitch_hz"]) for r in rows])
    onset = np.array([r["onset"] == "1" for r in rows])
    measured = np.array([r["measured_psola_valid"] == "1" for r in rows])
    coasted = np.array([r["coasted_psola_valid"] == "1" for r in rows])
    gain = np.array([float(r["psola_gain"]) for r in rows])
    marks = np.array([r["mark_valid"] == "1" for r in rows])
    state_map = {"unlocked": 0, "acquiring": 1, "coasting": 2, "locked": 3}
    state = np.array([state_map[r["pitch_tracker_state"]] for r in rows])
    transitions = [(i, r["state_transition"]) for i, r in enumerate(rows)
                   if r["state_transition"]]
    selectors = {
        "representative_onset": next((i for i, value in enumerate(onset) if value), 0),
        "low_confidence": next((i for i, value in enumerate(confidence)
                                if value < .6 and state[i] >= 2), 0),
        "coast_recovery": next((i for i, text in transitions
                                if text == "coasting->locked"), 0),
        "coast_timeout": next((i for i, text in transitions
                               if text == "coasting->unlocked"), 0),
        "note_transition": next((i for i, r in enumerate(rows)
                                 if r["pitch_changed"] == "1"), 0),
    }
    radius = round(.100 * RATE)
    for name, center in selectors.items():
        lo, hi = max(0, center - radius), min(len(rows), center + radius)
        t = times[lo:hi]
        fig, axes = plt.subplots(4, 1, figsize=(10, 8), sharex=True)
        axes[0].plot(t, pitch[lo:hi]); axes[0].set_ylabel("F0 Hz")
        axes[1].plot(t, confidence[lo:hi], label="confidence")
        axes[1].plot(t, onset[lo:hi], label="onset"); axes[1].legend(loc="upper right")
        axes[2].step(t, state[lo:hi], where="post", label="tracker")
        axes[2].plot(t, measured[lo:hi], label="measured")
        axes[2].plot(t, coasted[lo:hi], label="coasted"); axes[2].set_yticks(range(4))
        axes[2].set_yticklabels(["unlocked", "acquiring", "coasting", "locked"])
        axes[2].legend(loc="upper right")
        axes[3].plot(t, gain[lo:hi], label="gain")
        axes[3].plot(t, marks[lo:hi], label="mark valid")
        axes[3].legend(loc="upper right"); axes[3].set_xlabel("original time (s)")
        for axis in axes: axis.grid(True, alpha=.25)
        fig.suptitle(name.replace("_", " "))
        fig.tight_layout(); fig.savefig(plots / f"{name}.png", dpi=150)
        plt.close(fig)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: psala_continuity.py ROOT SAMPLE PITCH_SHIFT_EXE")
    root, sample, exe = map(Path, sys.argv[1:])
    metrics = root / "metrics"; telemetry_dir = root / "telemetry"
    segment = root / "renders" / "segment02"; full = root / "renders" / "full"
    work = root / "work"
    for directory in (metrics, telemetry_dir, segment, full, root / "plots", work):
        directory.mkdir(parents=True, exist_ok=True)
    source = read_audio(sample)
    reference = source[round(LISTEN_BEGIN * RATE):round(LISTEN_END * RATE)]
    context = source[round(SOURCE_BEGIN * RATE):round(LISTEN_END * RATE)]
    reference_48k = work / "source_48k.wav"
    context_path = work / "segment02_context.wav"
    write_audio(reference_48k, source); write_audio(context_path, context)
    write_audio(segment / "reference.wav", reference)
    active = active_mask(reference)

    specifications = [
        ("baseline", "baseline", 0, "baseline_no_fallback.wav", "segment02_baseline.csv"),
        ("onset_only", "onset", 0, "onset_continuity.wav", "segment02_onset.csv"),
    ]
    all_results = {}; rows_by_name = {}; timings = []
    for name, policy, coast, wav_name, csv_name in specifications:
        temporary = work / f"{name}.csv"
        elapsed, log = execute(exe, context_path, segment / wav_name, 4, policy,
                               coast, telemetry=temporary)
        rows = reduce_telemetry(temporary, telemetry_dir / csv_name)
        rows_by_name[name] = rows
        all_results[name] = metric(name, rows, read_audio(segment / wav_name), active)
        timings.append((name, elapsed, log))

    sweep = []
    for coast in COASTS:
        name = f"combined_{coast}ms"
        temporary = work / f"{name}.csv"
        destination = segment / f"_combined_{coast}ms.wav"
        elapsed, log = execute(exe, context_path, destination, 4, "combined", coast,
                               telemetry=temporary)
        rows = reduce_telemetry(temporary, None)
        result = metric(name, rows, read_audio(destination), active)
        result["coast_ms"] = coast
        sweep.append(result); rows_by_name[name] = rows
        all_results[name] = result; timings.append((name, elapsed, log))
    safe = [r for r in sweep if r["coast_percent_vocal_active"] <= 15]
    best = max(safe or sweep, key=lambda r: r["total_coverage"])
    best_ms = int(best["coast_ms"])

    # Preserve the selected combined telemetry and promote its render.
    selected_name = f"combined_{best_ms}ms"
    shutil.copyfile(segment / f"_combined_{best_ms}ms.wav",
                    segment / "onset_continuity_coasting.wav")
    with (telemetry_dir / "segment02_combined.csv").open("w", newline="") as stream:
        fields = list(rows_by_name[selected_name][0].keys())
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows_by_name[selected_name])

    # Coasting-only at the same duration.
    temporary = work / "coasting.csv"
    elapsed, log = execute(exe, context_path, segment / "coasting_best.wav", 4,
                           "coasting", best_ms, telemetry=temporary)
    coast_rows = reduce_telemetry(temporary, telemetry_dir / "segment02_coasting.csv")
    rows_by_name["coasting_only"] = coast_rows
    all_results["coasting_only"] = metric(
        "coasting_only", coast_rows, read_audio(segment / "coasting_best.wav"), active)
    timings.append(("coasting_only", elapsed, log))

    # Current delayed-dry reference is intentionally excluded from metrics.
    execute(exe, context_path, segment / "old_dry_fallback_reference.wav", 4,
            "baseline", 0, fallback="current")

    # Exact measured/coasted component attribution for the selected candidate.
    for stem_name, wav_name in (("measured-psola", "measured_psola.wav"),
                                ("coasted-psola", "coasted_psola.wav"),
                                ("combined-psola", "combined_psola.wav")):
        execute(exe, context_path, segment / wav_name, 4, "combined", best_ms,
                stem=stem_name)
    measured_audio = read_audio(segment / "measured_psola.wav")
    coasted_audio = read_audio(segment / "coasted_psola.wav")
    combined_audio = read_audio(segment / "combined_psola.wav")
    residual = measured_audio + coasted_audio - combined_audio
    reconstruction = {
        "relative_rms_error": float(np.sqrt(np.mean(residual ** 2)) /
                                    max(np.sqrt(np.mean(combined_audio ** 2)), 1e-15)),
        "max_abs_error": float(np.max(np.abs(residual))),
    }

    # Full harmony stems for the selected diagnostic policy.
    for shift in (4, 7, -4, -7):
        tag = f"p{shift}" if shift > 0 else f"m{abs(shift)}"
        execute(exe, reference_48k, full / f"voice2_{tag}_continuity.wav", shift,
                "combined", best_ms, full=True)

    policy_rows = [all_results["baseline"], all_results["onset_only"],
                   all_results["coasting_only"], all_results[selected_name]]
    for row, coast in zip(policy_rows, (0, 0, best_ms, best_ms)):
        row["coast_ms"] = coast
    with (metrics / "policy_comparison.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=policy_rows[0].keys())
        writer.writeheader(); writer.writerows(policy_rows)
    with (metrics / "coast_sweep.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=sweep[0].keys())
        writer.writeheader(); writer.writerows(sweep)
    acquiring_records, acquiring_stats = acquiring_causes(
        rows_by_name["baseline"], metrics / "acquiring_causes.csv")
    onset_records, recovery_records = event_tables(
        rows_by_name["baseline"], rows_by_name[selected_name], metrics)
    make_plots(rows_by_name[selected_name], root / "plots")

    with (metrics / "render_timing.csv").open("w", newline="") as stream:
        writer = csv.writer(stream); writer.writerow(["policy", "wall_seconds", "renderer_log"])
        writer.writerows(timings)

    # Machine-readable facts consumed by report generation and final review.
    with (metrics / "summary.txt").open("w") as stream:
        stream.write(f"best_coast_ms={best_ms}\n")
        stream.write(f"reconstruction_relative_rms={reconstruction['relative_rms_error']:.12g}\n")
        stream.write(f"reconstruction_max_abs={reconstruction['max_abs_error']:.12g}\n")
        for key, value in acquiring_stats.items(): stream.write(f"acquiring_{key}={value}\n")
        stream.write(f"onset_events={len(onset_records)}\n")
        stream.write(f"recovery_events={len(recovery_records)}\n")

    for temporary in segment.glob("_combined_*ms.wav"):
        temporary.unlink()
    print(f"best_coast_ms={best_ms}")
    print(f"combined_total_coverage={best['total_coverage']:.3f}")
    print(f"combined_coasted_vocal_active={best['coast_percent_vocal_active']:.3f}")
    print(f"reconstruction_relative_rms={reconstruction['relative_rms_error']:.3e}")


if __name__ == "__main__":
    main()
