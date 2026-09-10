#!/usr/bin/env python3
"""Render and summarize the fixed Milestone 5.3 vocal listening matrix."""
from __future__ import annotations

import csv
import subprocess
import sys
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
BLOCK = 64


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


def runs(mask):
    changes = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(changes == 1), np.flatnonzero(changes == -1)))


def render(root: Path, sample: Path, executable: Path):
    renders = root / "renders"
    csv_dir = root / "csv"
    work = renders / ".work"
    renders.mkdir(parents=True, exist_ok=True)
    csv_dir.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    source = read_audio(sample)
    context = source[int(SOURCE_BEGIN * RATE):int(LISTEN_END * RATE)]
    context_path = work / "input_context.wav"
    wavfile.write(context_path, RATE, context.astype(np.float32))
    reference = source[int(LISTEN_BEGIN * RATE):int(LISTEN_END * RATE)]
    wavfile.write(renders / "reference_6p4_9p4.wav", RATE,
                  reference.astype(np.float32))

    variants = (
        ("current_articulation_old_ola", "current", []),
        ("current_articulation_fixed_ola", "hybrid", []),
        ("candidate_articulation_fixed_ola", "hybrid",
         ["--articulation", "40", "40", "10", "60", ".15", ".10"]),
    )
    for shift in (-7, -4, 4, 7):
        label = f"m{abs(shift)}" if shift < 0 else f"p{shift}"
        for variant, normalization, extra in variants:
            name = f"voice2_{label}_{variant}"
            full = work / f"{name}_full.wav"
            debug = csv_dir / f"{name}.csv"
            command = [str(executable), str(context_path), str(full), str(shift),
                       "--voice", "2", "--apply-voice-envelope", "--formants",
                       "off", "--ola-normalization", normalization, "--debug-csv",
                       str(debug), *extra]
            subprocess.run(command, check=True)
            rendered = read_audio(full)
            begin = int((LISTEN_BEGIN - SOURCE_BEGIN) * RATE)
            end = begin + int((LISTEN_END - LISTEN_BEGIN) * RATE)
            wavfile.write(renders / f"{name}.wav", RATE,
                          rendered[begin:end].astype(np.float32))
            full.unlink()

    stems_dir = renders / "stems"
    stems_dir.mkdir(exist_ok=True)
    validation = []
    for shift in (-7, -4, 4, 7):
        label = f"m{abs(shift)}" if shift < 0 else f"p{shift}"
        rendered_stems = {}
        for stem in ("psola", "fallback", "final"):
            name = f"voice2_{label}_{stem}_component" if stem != "final" \
                else f"voice2_{label}_final_mixed_harmony"
            full = work / f"{name}_full.wav"
            command = [str(executable), str(context_path), str(full), str(shift),
                       "--voice", "2", "--apply-voice-envelope", "--formants",
                       "off", "--ola-normalization", "hybrid", "--stem", stem]
            subprocess.run(command, check=True)
            audio = read_audio(full)
            begin = int((LISTEN_BEGIN - SOURCE_BEGIN) * RATE)
            end = begin + int((LISTEN_END - LISTEN_BEGIN) * RATE)
            rendered_stems[stem] = audio[begin:end].astype(np.float32)
            wavfile.write(stems_dir / f"{name}.wav", RATE,
                          rendered_stems[stem])
            full.unlink()
        residual = rendered_stems["final"].astype(np.float64) - (
            rendered_stems["psola"].astype(np.float64) +
            rendered_stems["fallback"].astype(np.float64))
        prior = read_audio(
            renders / f"voice2_{label}_current_articulation_fixed_ola.wav")
        validation.append({
            "shift_st": shift,
            "psola_rms": float(np.sqrt(np.mean(rendered_stems["psola"] ** 2))),
            "fallback_rms": float(np.sqrt(np.mean(rendered_stems["fallback"] ** 2))),
            "final_rms": float(np.sqrt(np.mean(rendered_stems["final"] ** 2))),
            "max_sum_residual": float(np.max(np.abs(residual))),
            "max_difference_from_prior_final":
                float(np.max(np.abs(rendered_stems["final"] - prior))),
        })
    with (root / "stem_validation.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=validation[0].keys())
        writer.writeheader()
        writer.writerows(validation)


def vocal_metrics(root: Path):
    rows_out = []
    csv_dir = root / "csv"
    for path in sorted(csv_dir.glob("voice2_*.csv")):
        with path.open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        selected = [row for row in rows
                    if LISTEN_BEGIN - SOURCE_BEGIN <= float(row["time_s"])
                    <= LISTEN_END - SOURCE_BEGIN]
        input_rms = np.array([float(row["input_rms"]) for row in selected])
        nominal = max(float(np.percentile(input_rms, 85)), 1e-8)
        active = input_rms >= max(nominal * .10, 1e-6)
        gain = np.array([float(row["voice2_output_gain"]) for row in selected])
        audible = gain > .01
        active_count = max(1, int(np.sum(active)))
        onset_lags, early_releases, internal_gaps = [], [], []
        for begin, end in runs(active):
            if end - begin < round(.080 * RATE / BLOCK):
                continue
            indices = np.flatnonzero(audible[begin:end])
            if not len(indices):
                continue
            first = begin + int(indices[0])
            last = begin + int(indices[-1]) + 1
            onset_lags.append((first - begin) * BLOCK * 1000 / RATE)
            early_releases.append((end - last) * BLOCK * 1000 / RATE)
            for gap_begin, gap_end in runs(~audible[begin:end]):
                if gap_begin > 0 and gap_end < end - begin:
                    internal_gaps.append((gap_end - gap_begin) * BLOCK * 1000 / RATE)
        audio = read_audio(root / "renders" / f"{path.stem}.wav")
        state_changes = sum(row["voice2_state_changed"] == "1" for row in selected)
        fallback = sum(row["fallback_active_v2"] == "1" for row in selected)
        rows_out.append({
            "case": path.stem,
            "rms": float(np.sqrt(np.mean(audio * audio))),
            "peak": float(np.max(np.abs(audio))),
            "coverage_db40": float(np.sum(active & audible) / active_count),
            "onset_lag_median_ms": float(np.median(onset_lags)) if onset_lags else 0,
            "early_release_median_ms": float(np.median(early_releases)) if early_releases else 0,
            "internal_gaps_gt20": sum(value > 20 for value in internal_gaps),
            "state_changes": state_changes,
            "fallback_blocks": fallback,
        })
    with (root / "vocal_metrics.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows_out[0].keys())
        writer.writeheader()
        writer.writerows(rows_out)


def plots(root: Path):
    plot_dir = root / "plots"
    plot_dir.mkdir(exist_ok=True)
    with (root / "synthetic_metrics.csv").open(newline="") as stream:
        metrics = list(csv.DictReader(stream))
    methods = ["current", "window_sum", "window_energy", "cola_energy_hybrid"]
    labels = ["A current", "B sum(w)", "C sqrt(sum(w^2))", "D COLA+energy"]
    shifts = [-12, -7, -4, 0, 4, 7, 12]
    for signal in ("constant", "sine", "harmonic"):
        figure, axis = plt.subplots(figsize=(9, 5))
        for method, label in zip(methods, labels):
            values = [float(next(row["rms_ratio"] for row in metrics
                                 if row["signal"] == signal and
                                 row["method"] == method and
                                 float(row["shift_st"]) == shift))
                      for shift in shifts]
            axis.plot(shifts, values, marker="o", label=label)
        axis.axhspan(.95, 1.05, color="green", alpha=.12, label="acceptance")
        axis.set(xlabel="Shift (st)", ylabel="RMS output/input",
                 title=f"Stationary {signal}: OLA normalization")
        axis.grid(True, alpha=.3)
        axis.legend()
        figure.tight_layout()
        figure.savefig(plot_dir / f"rms_methods_{signal}.png", dpi=150)
        plt.close(figure)

    with (root / "ola_norm_timeseries.csv").open(newline="") as stream:
        timeline = list(csv.DictReader(stream))
    for shift in (-7, -4, 0, 4, 7):
        selected = [row for row in timeline if float(row["shift_st"]) == shift]
        time = np.array([float(row["time_s"]) for row in selected])
        figure, axis = plt.subplots(figsize=(10, 4))
        axis.plot(time, [float(row["sum_w_mean"]) for row in selected],
                  label="sum(w)")
        axis.plot(time, [float(row["sum_w2_mean"]) for row in selected],
                  label="sum(w^2)")
        axis.set(xlabel="Time (s)", ylabel="Block mean",
                 title=f"Local COLA terms, {shift:+d} st")
        axis.grid(True, alpha=.3)
        axis.legend()
        figure.tight_layout()
        suffix = f"m{abs(shift)}" if shift < 0 else f"p{shift}"
        figure.savefig(plot_dir / f"cola_{suffix}.png", dpi=150)
        plt.close(figure)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: ola_energy_debug.py ROOT SAMPLE PITCH_SHIFT_EXE")
    root, sample, executable = map(Path, sys.argv[1:])
    render(root, sample, executable)
    vocal_metrics(root)
    plots(root)


if __name__ == "__main__":
    main()
