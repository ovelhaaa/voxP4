"""Render and qualify the B4B.4K YIN energy candidate end to end."""

from __future__ import annotations

import math
import pathlib
import re
import subprocess

import numpy as np
from scipy.io import wavfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
INPUT = ROOT / "artifacts/formant_preservation/renders/full/00_lead_dry.wav"
EXECUTABLE = ROOT / "build-host/pitch_shift.exe"
SCRATCH = ROOT / "scratch/b4b4k"
OUTPUT = ROOT / "artifacts/alpha01b/b4b4k_audio_equivalence.txt"
RESIDUAL = ROOT / "artifacts/alpha01b/b4b4k_audio_residual.wav"


def render(name: str, energy: str) -> tuple[pathlib.Path, str]:
    output = SCRATCH / f"vocal_{name}.wav"
    command = [
        str(EXECUTABLE), str(INPUT), str(output), "7",
        "--formants", "lpc", "--formant-amount", "1",
        "--lpc-order", "16", "--autocorr", "f32-double-single",
        "--lpc-energy", "compensated",
        "--yin-cmnd", "f32-compensated", "--yin-energy", energy,
        "--render-mode", "solo", "--continuity-policy", "baseline",
    ]
    completed = subprocess.run(command, cwd=ROOT, check=True,
                               capture_output=True, text=True)
    return output, completed.stderr


def telemetry(text: str) -> dict[str, int]:
    match = re.search(
        r"pitch_results=(\d+) pitch_marks=(\d+) grains_attempted=(\d+) "
        r"grains_scheduled=(\d+) grains_rendered=(\d+)", text)
    if not match:
        return {}
    keys = ("pitch_results", "pitch_marks", "grains_attempted",
            "grains_scheduled", "grains_rendered")
    return dict(zip(keys, map(int, match.groups())))


def load(path: pathlib.Path) -> tuple[int, np.ndarray]:
    rate, samples = wavfile.read(path)
    return rate, np.asarray(samples, dtype=np.float64)


def main() -> int:
    if not INPUT.exists() or not EXECUTABLE.exists():
        raise SystemExit("vocal fixture or pitch_shift executable is missing")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    reference_path, reference_log = render("reference", "reference")
    candidate_path, candidate_log = render("f32_compensated",
                                           "f32-compensated")
    rate, reference = load(reference_path)
    candidate_rate, candidate = load(candidate_path)
    same_shape = reference.shape == candidate.shape
    residual = candidate - reference if same_shape else np.array([math.inf])
    reference_rms = float(np.sqrt(np.mean(reference * reference)))
    residual_rms = float(np.sqrt(np.mean(residual * residual)))
    snr = math.inf if residual_rms == 0.0 else 20.0 * math.log10(
        reference_rms / residual_rms)
    ref_info = telemetry(reference_log)
    candidate_info = telemetry(candidate_log)
    finite = bool(np.isfinite(reference).all() and np.isfinite(candidate).all())
    reference_clipping = int(np.count_nonzero(np.abs(reference) >= 1.0))
    candidate_clipping = int(np.count_nonzero(np.abs(candidate) >= 1.0))
    counters_equal = ref_info == candidate_info and bool(ref_info)
    passed = (same_shape and candidate_rate == rate and finite and
              reference_clipping == candidate_clipping and counters_equal and
              snr >= 100.0)
    wavfile.write(RESIDUAL, rate, residual.astype(np.float32))
    lines = [
        "Stage B4B.4K — YIN Energy Vocal Audio Equivalence",
        "",
        f"input={INPUT.relative_to(ROOT)}",
        "configuration=+7 semitones, formant amount 1.0, LPC order 16, "
        "AUTOCORR_F32_DOUBLE_SINGLE, LPC_ENERGY_F32_COMPENSATED, solo "
        "harmony, baseline continuity, YIN_DIFF_INCREMENTAL_F32 rebase 64, "
        "YIN_CMND_F32_COMPENSATED, PITCH_MARK_NCC_REUSE_AA",
        "oracle=YIN_ENERGY_REFERENCE_DOUBLE",
        "candidate=YIN_ENERGY_F32_COMPENSATED",
        f"sample_rate={rate}",
        "",
        "variant,sample_count,peak,rms,dc,nan_inf,clipping,pitch_results,"
        "pitch_marks,grains_attempted,grains_scheduled,grains_rendered",
    ]
    for name, samples, info in (("reference", reference, ref_info),
                                ("f32_compensated", candidate,
                                 candidate_info)):
        lines.append(
            f"{name},{samples.size},{np.max(np.abs(samples)):.17g},"
            f"{np.sqrt(np.mean(samples * samples)):.17g},"
            f"{np.mean(samples):.17g},"
            f"{np.count_nonzero(~np.isfinite(samples))},"
            f"{np.count_nonzero(np.abs(samples) >= 1.0)},"
            f"{info.get('pitch_results', 'not_reported')},"
            f"{info.get('pitch_marks', 'not_reported')},"
            f"{info.get('grains_attempted', 'not_reported')},"
            f"{info.get('grains_scheduled', 'not_reported')},"
            f"{info.get('grains_rendered', 'not_reported')}"
        )
    lines.extend([
        "",
        f"sample_count_identical={'yes' if same_shape else 'no'}",
        f"max_sample_residual={np.max(np.abs(residual)):.17g}",
        f"rms_residual={residual_rms:.17g}",
        f"difference_snr_db={snr:.9g}",
        f"nan_inf_regression={0 if finite else 1}",
        f"clipping_difference={candidate_clipping-reference_clipping}",
        f"pitch_result_count_identical={'yes' if ref_info.get('pitch_results') == candidate_info.get('pitch_results') else 'no'}",
        f"pitch_mark_count_identical={'yes' if ref_info.get('pitch_marks') == candidate_info.get('pitch_marks') else 'no'}",
        f"grain_counts_identical={'yes' if counters_equal else 'no'}",
        "",
        "Classification: " + ("AUDIO_GUARDRAILS_PASS" if passed else
                               "AUDIO_GUARDRAILS_FAIL"),
    ])
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(OUTPUT)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
