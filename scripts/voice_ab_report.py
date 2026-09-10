#!/usr/bin/env python3
"""Summarize host-exported voice envelopes and state transitions."""
from __future__ import annotations

import csv
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile


CASES = (
    "voice1_p4_attenuation_on",
    "voice2_p7_attenuation_on",
    "voice2_p7_attenuation_off",
)
ENVELOPES = ("psola_gain", "active_mix", "harmony_mix", "formant_mix")


def load_csv(path: Path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def write_transitions(path: Path, rows):
    selected = [row for row in rows if row["state_changed"] == "1"]
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(selected)
    return len(selected)


def wav_stats(path: Path):
    rate, samples = wavfile.read(path)
    samples = samples.astype(np.float64)
    return {
        "sample_rate": rate,
        "frames": len(samples),
        "rms": float(np.sqrt(np.mean(samples * samples))),
        "peak": float(np.max(np.abs(samples))),
        "finite": bool(np.all(np.isfinite(samples))),
    }


def main(root_string: str):
    root = Path(root_string)
    traces = {name: load_csv(root / "envelopes" / f"{name}.csv")
              for name in CASES}
    transition_counts = {
        name: write_transitions(root / "transitions" / f"{name}.csv", rows)
        for name, rows in traces.items()
    }

    figure, axes = plt.subplots(len(CASES), 1, figsize=(14, 9), sharex=True)
    for axis, name in zip(axes, CASES):
        rows = traces[name]
        time = np.array([float(row["time"]) for row in rows])
        for envelope in ENVELOPES:
            axis.plot(time, [float(row[envelope]) for row in rows],
                      label=envelope, linewidth=.8)
        for row in rows:
            if row["state_changed"] == "1":
                axis.axvline(float(row["time"]), color="black", alpha=.08,
                             linewidth=.5)
        axis.set_ylabel(name.replace("_", "\n"))
        axis.set_ylim(-.05, 1.05)
        axis.grid(alpha=.2)
    axes[0].legend(ncol=4, loc="upper right")
    axes[-1].set_xlabel("Time (s); vertical lines are voice-state transitions")
    figure.tight_layout()
    figure.savefig(root / "plots" / "voice_envelopes_and_states.png", dpi=150)
    plt.close(figure)

    summaries = []
    for name, rows in traces.items():
        stats = wav_stats(root / "renders" / f"{name}.wav")
        summaries.append({
            "case": name,
            **stats,
            "state_transitions": transition_counts[name],
            **{f"mean_{field}": float(np.mean([float(r[field]) for r in rows]))
               for field in ENVELOPES},
        })
    with (root / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=summaries[0].keys())
        writer.writeheader()
        writer.writerows(summaries)

    on_rate, on = wavfile.read(root / "renders" /
                               "voice2_p7_attenuation_on.wav")
    off_rate, off = wavfile.read(root / "renders" /
                                 "voice2_p7_attenuation_off.wav")
    if on_rate != off_rate or on.shape != off.shape:
        raise SystemExit("voice-2 A/B render shapes differ")
    delta = off.astype(np.float64) - on.astype(np.float64)
    delta_rms = float(np.sqrt(np.mean(delta * delta)))

    with (root / "report" / "voice_ab.md").open("w") as stream:
        stream.write(
            "# Isolated harmony voice transition A/B\n\n"
            "The host renderer uses the project C++ DSP. Voice 1 is rendered "
            "at +4 semitones and Voice 2 at +7 semitones. LPC is off so the "
            "only Voice-2 A/B variable is onset/unvoiced transition "
            "attenuation. Product/default behaviour is `attenuation_on`.\n\n"
            "Case | RMS | peak | finite | state transitions | mean PSOLA gain "
            "| mean harmony envelope\n"
            "---|---:|---:|---|---:|---:|---:\n")
        for row in summaries:
            stream.write(
                f"{row['case']} | {row['rms']:.7f} | {row['peak']:.7f} | "
                f"{row['finite']} | {row['state_transitions']} | "
                f"{row['mean_psola_gain']:.7f} | "
                f"{row['mean_harmony_mix']:.7f}\n")
        stream.write(
            f"\nVoice-2 off-minus-on RMS difference: {delta_rms:.7f}.\n\n"
            "Per-block envelopes are in `envelopes/`; rows where the TD-PSOLA "
            "state changes are copied to `transitions/`. The plot overlays "
            "PSOLA acquisition, active, harmony-mixer, and formant envelopes.\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: voice_ab_report.py ARTIFACT_ROOT")
    main(sys.argv[1])
