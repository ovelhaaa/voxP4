#!/usr/bin/env python3
"""Render and measure Milestone 5.4 harmony fallback experiments."""
from __future__ import annotations

import csv
import math
import subprocess
import sys
from collections import Counter
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, correlate, correlation_lags, resample_poly, sosfilt, welch

RATE = 48000
SOURCE_BEGIN = 4.9
LISTEN_BEGIN = 6.4
LISTEN_END = 9.4
CONTEXT_OFFSET = LISTEN_BEGIN - SOURCE_BEGIN
SHIFTS = (-7, -4, 4, 7)


def read_audio(path: Path):
    rate, data = wavfile.read(path)
    if np.issubdtype(data.dtype, np.integer):
        data = data.astype(np.float64) / (2 ** (data.dtype.itemsize * 8 - 1))
    else:
        data = data.astype(np.float64)
    if data.ndim == 2:
        data = data.mean(axis=1)
    if rate != RATE:
        divisor = np.gcd(rate, RATE)
        data = resample_poly(data, RATE // divisor, rate // divisor)
    return data


def write_audio(path: Path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, RATE, np.asarray(data, dtype=np.float32))


def crop(data):
    begin = round(CONTEXT_OFFSET * RATE)
    return data[begin:begin + round((LISTEN_END - LISTEN_BEGIN) * RATE)]


def label(shift):
    return f"m{abs(shift)}" if shift < 0 else f"p{shift}"


def execute(executable: Path, context: Path, destination: Path, shift: int,
            policy_args, articulation_args, stem="final", reason_path=None,
            product_mix=False):
    full = destination.with_name(destination.stem + "_context.wav")
    command = [str(executable), str(context), str(full), str(shift),
               "--voice", "2", "--formants", "off", "--ola-normalization",
               "hybrid", *policy_args, *articulation_args]
    if product_mix:
        command.append("--product-mix")
    else:
        command.extend(["--apply-voice-envelope", "--stem", stem])
    if reason_path:
        command.extend(["--fallback-reason-csv", str(reason_path)])
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(f"render failed: {' '.join(command)}\n{result.stderr}")
    write_audio(destination, crop(read_audio(full)))
    full.unlink()


def policy_sets(articulation):
    base = [
        ("P0_current", ["--fallback-policy", "current"]),
        ("P1_no_fallback", ["--fallback-policy", "none"]),
        ("P2_unvoiced025", ["--fallback-policy", "unvoiced",
                            "--fallback-gains", ".25", ".15"]),
        ("P3_onset_unvoiced", ["--fallback-policy", "onset-unvoiced",
                               "--fallback-gains", ".25", ".15"]),
        ("P4_highpass_unvoiced", ["--fallback-policy", "highpass-unvoiced",
                                  "--fallback-gains", ".25", ".15",
                                  "--fallback-hpf-hz", "2000"]),
        ("P5_hold40_highpass", ["--fallback-policy", "highpass-unvoiced",
                                "--fallback-gains", ".25", ".15",
                                "--fallback-hpf-hz", "2000",
                                "--keep-active-ms", "40"]),
    ]
    if articulation == "current":
        base.extend([
            ("P2_unvoiced015", ["--fallback-policy", "unvoiced",
                                "--fallback-gains", ".15", ".15"]),
            ("P2_unvoiced035", ["--fallback-policy", "unvoiced",
                                "--fallback-gains", ".35", ".15"]),
            ("P5_hold20_highpass", ["--fallback-policy", "highpass-unvoiced",
                                    "--fallback-gains", ".25", ".15",
                                    "--fallback-hpf-hz", "2000",
                                    "--keep-active-ms", "20"]),
            ("P5_hold60_highpass", ["--fallback-policy", "highpass-unvoiced",
                                    "--fallback-gains", ".25", ".15",
                                    "--fallback-hpf-hz", "2000",
                                    "--keep-active-ms", "60"]),
        ])
    return base


def articulation_args(name):
    return [] if name == "current" else [
        "--articulation", "40", "40", "10", "60", ".15", ".10"]


def load_reason_rows(path: Path):
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    return [row for row in rows
            if CONTEXT_OFFSET <= float(row["time_s"]) <
            CONTEXT_OFFSET + (LISTEN_END - LISTEN_BEGIN)]


def moving_rms(data, window=960):
    kernel = np.ones(window, dtype=np.float64) / window
    return np.sqrt(np.convolve(np.asarray(data) ** 2, kernel, mode="same"))


def mask_runs(mask):
    changes = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(changes == 1), np.flatnonzero(changes == -1)))


def timing_metrics(reference, output):
    reference_rms = moving_rms(reference)
    output_rms = moving_rms(output)
    nominal = max(float(np.percentile(reference_rms, 85)), 1e-8)
    active = reference_rms >= nominal * .10
    audible = output_rms >= nominal * .01
    active_count = max(1, int(np.sum(active)))
    onset, release, gaps = [], [], []
    for begin, end in mask_runs(active):
        if end - begin < round(.080 * RATE):
            continue
        indices = np.flatnonzero(audible[begin:end])
        if not len(indices):
            onset.append((end - begin) * 1000 / RATE)
            release.append((end - begin) * 1000 / RATE)
            continue
        first, last = begin + indices[0], begin + indices[-1] + 1
        onset.append((first - begin) * 1000 / RATE)
        release.append((end - last) * 1000 / RATE)
        for gap_begin, gap_end in mask_runs(~audible[begin:end]):
            if gap_begin and gap_end < end - begin:
                gaps.append((gap_end - gap_begin) * 1000 / RATE)
    return {
        "coverage": float(np.sum(active & audible) / active_count),
        "onset_lag_ms": float(np.median(onset)) if onset else 0.0,
        "early_release_ms": float(np.median(release)) if release else 0.0,
        "gaps_gt20": sum(gap > 20 for gap in gaps),
        "active_samples": int(np.sum(active)),
    }


def band_energy(data, low, high):
    sos = butter(4, [low, high], btype="bandpass", fs=RATE, output="sos")
    filtered = sosfilt(sos, data)
    return float(np.mean(filtered * filtered))


def tonal_leakage(data):
    low = band_energy(data, 80, 1000)
    high = band_energy(data, 2000, 8000)
    return 10 * math.log10((low + 1e-15) / (high + 1e-15))


def spectral_pitch(data):
    frequencies, power = welch(data, RATE, nperseg=min(8192, len(data)))
    selected = (frequencies >= 80) & (frequencies <= 500)
    return float(frequencies[selected][np.argmax(power[selected])])


def render_all(root: Path, sample: Path, executable: Path):
    renders = root / "renders"
    work = root / "work"
    reasons = root / "fallback_reasons"
    policies_root = renders / "policies"
    for directory in (renders, work, reasons, policies_root):
        directory.mkdir(parents=True, exist_ok=True)
    source = read_audio(sample)
    context = source[round(SOURCE_BEGIN * RATE):round(LISTEN_END * RATE)]
    context_path = work / "input_context.wav"
    write_audio(context_path, context)
    reference = source[round(LISTEN_BEGIN * RATE):round(LISTEN_END * RATE)]
    write_audio(renders / "reference.wav", reference)

    policy_records = []
    reference_metrics = timing_metrics(reference, reference)
    for articulation in ("current", "candidate"):
        group = policies_root / f"{articulation}_articulation"
        group.mkdir(parents=True, exist_ok=True)
        art_args = articulation_args(articulation)
        for shift in SHIFTS:
            shift_label = label(shift)
            for policy, policy_args in policy_sets(articulation):
                stem = f"voice2_{shift_label}_{policy}"
                final_path = group / f"{stem}.wav"
                fallback_path = work / f"{articulation}_{stem}_fallback.wav"
                reason_path = reasons / f"{articulation}_{stem}.csv"
                execute(executable, context_path, final_path, shift, policy_args,
                        art_args, "final", reason_path)
                execute(executable, context_path, fallback_path, shift, policy_args,
                        art_args, "fallback")
                final = read_audio(final_path)
                fallback = read_audio(fallback_path)
                reason_rows = load_reason_rows(reason_path)
                fallback_count = sum(row["active"] == "1" for row in reason_rows)
                timing = timing_metrics(reference, final)
                policy_records.append({
                    "articulation": articulation,
                    "shift_st": shift,
                    "policy": policy,
                    "coverage": timing["coverage"],
                    "rms": float(np.sqrt(np.mean(final * final))),
                    "fallback_samples": fallback_count,
                    "fallback_percent": 100 * fallback_count / len(reason_rows),
                    "fallback_percent_vocal_active":
                        100 * fallback_count / max(1, reference_metrics["active_samples"]),
                    "tonal_leakage_db": tonal_leakage(fallback),
                    "gaps_gt20": timing["gaps_gt20"],
                    "onset_lag_ms": timing["onset_lag_ms"],
                    "early_release_ms": timing["early_release_ms"],
                    "peak": float(np.max(np.abs(final))),
                })

                if articulation == "current" and policy in {
                        "P0_current", "P1_no_fallback", "P2_unvoiced025",
                        "P3_onset_unvoiced", "P4_highpass_unvoiced",
                        "P5_hold40_highpass"}:
                    aliases = {
                        "P0_current": "current",
                        "P1_no_fallback": "no_fallback",
                        "P2_unvoiced025": "unvoiced025",
                        "P3_onset_unvoiced": "onset_unvoiced",
                        "P4_highpass_unvoiced": "highpass_unvoiced",
                        "P5_hold40_highpass": "hold40_highpass",
                    }
                    write_audio(renders / f"voice2_{shift_label}_{aliases[policy]}.wav",
                                final)
                fallback_path.unlink()

    with (root / "policy_metrics.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=policy_records[0].keys())
        writer.writeheader()
        writer.writerows(policy_records)

    reconstruction = []
    reason_metrics = []
    for shift in SHIFTS:
        shift_label = label(shift)
        policy_args = ["--fallback-policy", "current"]
        paths = {}
        reason_path = reasons / f"stems_voice2_{shift_label}.csv"
        for stem in ("psola", "fallback", "final", "reference"):
            output_name = {
                "psola": "psola_only",
                "fallback": "fallback_only",
                "final": "final_harmony",
                "reference": "reference_delayed",
            }[stem]
            path = renders / f"voice2_{shift_label}_{output_name}.wav"
            execute(executable, context_path, path, shift, policy_args, [], stem,
                    reason_path if stem == "final" else None)
            paths[stem] = path
        audio = {name: read_audio(path) for name, path in paths.items()}
        residual = audio["final"] - audio["psola"] - audio["fallback"]
        relative = (np.sqrt(np.mean(residual * residual)) /
                    max(np.sqrt(np.mean(audio["final"] ** 2)), 1e-15))
        reason_rows = load_reason_rows(reason_path)
        active = np.array([row["active"] == "1" for row in reason_rows])
        fallback_active = audio["fallback"][active]
        delayed_active = audio["reference"][active]
        correlation = (float(np.corrcoef(fallback_active, delayed_active)[0, 1])
                       if len(fallback_active) > 2 else 0.0)
        reconstruction.append({
            "shift_st": shift,
            "relative_rms_error": relative,
            "max_abs_error": float(np.max(np.abs(residual))),
            "fallback_delayed_correlation_active": correlation,
            "fallback_pitch_hz": spectral_pitch(fallback_active),
            "delayed_reference_pitch_hz": spectral_pitch(delayed_active),
            "fallback_rms": float(np.sqrt(np.mean(audio["fallback"] ** 2))),
            "delayed_reference_rms": float(np.sqrt(np.mean(audio["reference"] ** 2))),
        })
        counts = Counter(row["reason"] for row in reason_rows if row["active"] == "1")
        total_fallback = max(1, sum(counts.values()))
        active_vocal = reference_metrics["active_samples"]
        all_reasons = ("LOW_CONFIDENCE", "UNVOICED", "ONSET", "ACQUIRING",
                       "TARGET_INVALID", "MARK_INVALID", "HISTORY_INVALID",
                       "RESYNC", "OTHER")
        for reason in all_reasons:
            count = counts[reason]
            reason_metrics.append({
                "shift_st": shift,
                "reason": reason,
                "sample_count": count,
                "duration_ms": count * 1000 / RATE,
                "percentage_of_fallback": 100 * count / total_fallback,
                "percentage_of_vocal_active_time": 100 * count / max(1, active_vocal),
            })

        if shift in (4, 7):
            write_audio(renders / f"voice2_{shift_label}_psola_component.wav",
                        audio["psola"])
            write_audio(renders / f"voice2_{shift_label}_fallback_component.wav",
                        audio["fallback"])
            write_audio(renders / f"voice2_{shift_label}_combined.wav",
                        audio["final"])

    with (root / "stem_reconstruction.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=reconstruction[0].keys())
        writer.writeheader()
        writer.writerows(reconstruction)
    with (root / "fallback_reason_metrics.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=reason_metrics[0].keys())
        writer.writeheader()
        writer.writerows(reason_metrics)

    # Measure the exact history delay against the processed context input.
    delayed = read_audio(renders / "voice2_p4_reference_delayed.wav")
    lead = reference
    corr = correlate(delayed - np.mean(delayed), lead - np.mean(lead), mode="full",
                     method="fft")
    lags = correlation_lags(len(delayed), len(lead), mode="full")
    region = np.abs(lags) <= round(.080 * RATE)
    best_lag = int(lags[region][np.argmax(corr[region])])
    with (root / "delay_correlation.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["measured_lag_samples", "measured_lag_ms",
                         "configured_history_samples", "configured_history_ms"])
        writer.writerow([best_lag, best_lag * 1000 / RATE, 1536, 32.0])

    mix_policies = {
        "current": ["--fallback-policy", "current"],
        "no_fallback": ["--fallback-policy", "none"],
        "unvoiced025": ["--fallback-policy", "unvoiced", "--fallback-gains", ".25", ".15"],
        "highpass_unvoiced": ["--fallback-policy", "highpass-unvoiced", "--fallback-gains", ".25", ".15", "--fallback-hpf-hz", "2000"],
        "hold40_highpass": ["--fallback-policy", "highpass-unvoiced", "--fallback-gains", ".25", ".15", "--fallback-hpf-hz", "2000", "--keep-active-ms", "40"],
    }
    for name, args in mix_policies.items():
        execute(executable, context_path, renders / f"mix_{name}.wav", 4,
                args, [], product_mix=True)


def make_plots(root: Path):
    with (root / "policy_metrics.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    selected = [row for row in rows if row["shift_st"] == "4" and
                row["policy"] in {"P0_current", "P1_no_fallback",
                                  "P2_unvoiced025", "P3_onset_unvoiced",
                                  "P4_highpass_unvoiced", "P5_hold40_highpass"}]
    plots = root / "plots"
    plots.mkdir(exist_ok=True)
    for articulation in ("current", "candidate"):
        group = [row for row in selected if row["articulation"] == articulation]
        labels = [row["policy"].split("_", 1)[0] for row in group]
        figure, axes = plt.subplots(1, 3, figsize=(13, 4))
        axes[0].bar(labels, [float(row["coverage"]) for row in group])
        axes[0].set_title("Coverage"); axes[0].set_ylim(0, 1.05)
        axes[1].bar(labels, [float(row["fallback_percent"]) for row in group])
        axes[1].set_title("Fallback samples (%)")
        axes[2].bar(labels, [float(row["tonal_leakage_db"]) for row in group])
        axes[2].set_title("Low/high leakage (dB)")
        for axis in axes:
            axis.grid(True, axis="y", alpha=.3)
        figure.suptitle(f"Voice 2 +4 — {articulation} articulation")
        figure.tight_layout()
        figure.savefig(plots / f"policies_p4_{articulation}.png", dpi=150)
        plt.close(figure)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: fallback_debug.py ROOT SAMPLE PITCH_SHIFT_EXE")
    root, sample, executable = map(Path, sys.argv[1:])
    render_all(root, sample, executable)
    make_plots(root)


if __name__ == "__main__":
    main()
