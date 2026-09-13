"""Render and qualify compensated-F32 LPC frame energy against production B."""

from __future__ import annotations

import csv
import math
import pathlib
import re
import subprocess

import numpy as np
from scipy.io import wavfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
INPUT = ROOT / "artifacts/formant_preservation/renders/full/00_lead_dry.wav"
EXECUTABLE = ROOT / "build-host/pitch_shift.exe"
SCRATCH = ROOT / "scratch/b4b4h"
OUTPUT = ROOT / "artifacts/alpha01b/b4b4h_vocal_audio_equivalence.txt"
RESIDUAL_WAV = ROOT / "artifacts/alpha01b/b4b4h_vocal_residual.wav"
VARIANTS = ("double", "compensated")


def render(variant: str) -> tuple[pathlib.Path, pathlib.Path, str]:
    output = SCRATCH / f"vocal_{variant}.wav"
    debug = SCRATCH / f"vocal_{variant}_debug.csv"
    command = [
        str(EXECUTABLE), str(INPUT), str(output), "7",
        "--formants", "lpc", "--formant-amount", "1",
        "--lpc-order", "16", "--autocorr", "f32-double-single",
        "--lpc-energy", variant, "--render-mode", "solo",
        "--continuity-policy", "baseline", "--debug-csv", str(debug),
    ]
    completed = subprocess.run(command, cwd=ROOT, check=True,
                               capture_output=True, text=True)
    return output, debug, completed.stderr.strip()


def telemetry(text: str) -> dict[str, float | int]:
    result: dict[str, float | int] = {}
    match = re.search(
        r"grains=(\d+).*formant_frames=(\d+) formant_resets=(\d+) "
        r"max_restored=([^\s]+)", text)
    if match:
        result.update(grains=int(match.group(1)),
                      formant_frames=int(match.group(2)),
                      formant_resets=int(match.group(3)),
                      max_restored=float(match.group(4)))
    match = re.search(r"lpc_frames=(\d+) invalid=(\d+) .*max_error=([^\s]+)",
                      text)
    if match:
        result.update(lpc_frames=int(match.group(1)),
                      invalid_lpc_frames=int(match.group(2)),
                      max_prediction_error=float(match.group(3)))
    return result


def load_float(path: pathlib.Path) -> tuple[int, np.ndarray]:
    rate, data = wavfile.read(path)
    data = np.asarray(data, dtype=np.float64)
    if data.ndim == 2:
        data = np.mean(data, axis=1)
    return rate, data


def region_masks(debug_path: pathlib.Path, input_audio: np.ndarray,
                 sample_rate: int) -> dict[str, np.ndarray]:
    count = input_audio.size
    quiet = np.zeros(count, dtype=bool)
    onset = np.zeros(count, dtype=bool)
    transition = np.zeros(count, dtype=bool)
    voiced = np.zeros(count, dtype=bool)
    rows: list[dict[str, str]] = []
    with debug_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    previous_voiced = False
    previous_f0 = 0.0
    for index, row in enumerate(rows):
        start = index * 64
        if start >= count:
            break
        end = min(count, start + 64)
        block = input_audio[start:end]
        block_rms = float(np.sqrt(np.mean(block * block))) if block.size else 0.0
        is_quiet = block_rms < 1e-4
        is_voiced = row.get("pitch_voiced") == "1"
        f0 = float(row.get("source_f0", "0") or 0)
        quiet[start:end] = is_quiet
        voiced[start:end] = is_voiced and not is_quiet
        if is_voiced and not previous_voiced:
            onset[start:min(count, start + int(.050 * sample_rate))] = True
        if is_voiced and previous_voiced and f0 > 0 and previous_f0 > 0:
            cents = abs(1200.0 * math.log2(f0 / previous_f0))
            if cents >= 20.0:
                radius = int(.020 * sample_rate)
                transition[max(0, start - radius):min(count, start + radius)] = True
        previous_voiced = is_voiced
        if f0 > 0:
            previous_f0 = f0
    sustained = voiced & ~onset & ~transition
    other = ~(quiet | onset | transition | sustained)
    return {"voiced_sustained": sustained, "onsets": onset,
            "pitch_transitions": transition, "quiet": quiet,
            "other": other}


def residual_stats(residual: np.ndarray, mask: np.ndarray) -> str:
    values = np.abs(residual[mask])
    if values.size == 0:
        return "samples=0"
    rms = float(np.sqrt(np.mean(values * values)))
    return (f"samples={values.size} max_abs={np.max(values):.17g} "
            f"rms={rms:.17g} p50={np.percentile(values, 50):.17g} "
            f"p95={np.percentile(values, 95):.17g} "
            f"p99={np.percentile(values, 99):.17g} "
            f"p99_9={np.percentile(values, 99.9):.17g}")


def main() -> int:
    if not INPUT.exists() or not EXECUTABLE.exists():
        raise SystemExit("build-host/pitch_shift.exe or vocal fixture is missing")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    renders = {variant: render(variant) for variant in VARIANTS}
    sample_rate, reference = load_float(renders["double"][0])
    candidate_rate, candidate = load_float(renders["compensated"][0])
    same_count = candidate.size == reference.size
    same_rate = candidate_rate == sample_rate
    residual = (candidate - reference if same_count
                else np.array([math.inf], dtype=np.float64))
    reference_rms = float(np.sqrt(np.mean(reference * reference)))
    residual_rms = float(np.sqrt(np.mean(residual * residual)))
    difference_snr = (math.inf if residual_rms == 0.0 else
                      20.0 * math.log10(reference_rms / residual_rms))
    max_index = int(np.argmax(np.abs(residual)))
    reference_info = telemetry(renders["double"][2])
    candidate_info = telemetry(renders["compensated"][2])
    reference_clipping = int(np.count_nonzero(np.abs(reference) >= 1.0))
    candidate_clipping = int(np.count_nonzero(np.abs(candidate) >= 1.0))
    finite = bool(np.isfinite(reference).all() and np.isfinite(candidate).all())
    counters_equal = all(candidate_info.get(key) == reference_info.get(key)
                         for key in ("lpc_frames", "invalid_lpc_frames",
                                     "formant_frames", "formant_resets"))
    guard = (same_count and same_rate and finite and
             candidate_clipping == reference_clipping and counters_equal and
             difference_snr >= 100.0)
    wavfile.write(RESIDUAL_WAV, sample_rate, residual.astype(np.float32))
    masks = (region_masks(renders["double"][1], reference, sample_rate)
             if same_count else {})
    abs_residual = np.abs(residual)
    lines = [
        "Stage B4B.4H — LPC Compensated F32 Vocal Audio Equivalence",
        "",
        f"input={INPUT.relative_to(ROOT)}",
        "configuration=+7 semitones, formant amount 1.0, LPC order 16, "
        "AUTOCORR_F32_DOUBLE_SINGLE, solo harmony, baseline continuity",
        "oracle=LPC_WINDOW_HANN_DOUBLE_ENERGY",
        "candidate=LPC_ENERGY_F32_COMPENSATED",
        f"sample_rate={sample_rate}",
        "",
        "variant,sample_count,peak,rms,dc,nan_inf,clipping_samples,"
        "formant_frames,formant_resets,max_restored,lpc_frames,invalid_lpc_frames",
    ]
    for variant in VARIANTS:
        _, data = load_float(renders[variant][0])
        info = telemetry(renders[variant][2])
        lines.append(
            f"{variant},{data.size},{np.max(np.abs(data)):.17g},"
            f"{np.sqrt(np.mean(data * data)):.17g},{np.mean(data):.17g},"
            f"{np.count_nonzero(~np.isfinite(data))},"
            f"{np.count_nonzero(np.abs(data) >= 1.0)},"
            f"{info.get('formant_frames', 'not_reported')},"
            f"{info.get('formant_resets', 'not_reported')},"
            f"{info.get('max_restored', 'not_reported')},"
            f"{info.get('lpc_frames', 'not_reported')},"
            f"{info.get('invalid_lpc_frames', 'not_reported')}"
        )
    lines.extend([
        "",
        f"sample_count_identical={'yes' if same_count else 'no'}",
        f"max_absolute_sample_difference={np.max(abs_residual):.17g}",
        f"rms_residual={residual_rms:.17g}",
        f"difference_snr_db={difference_snr:.9g}",
        f"max_residual_index={max_index}",
        f"max_residual_timestamp_s={max_index / sample_rate:.9f}",
        f"p50_abs_residual={np.percentile(abs_residual, 50):.17g}",
        f"p95_abs_residual={np.percentile(abs_residual, 95):.17g}",
        f"p99_abs_residual={np.percentile(abs_residual, 99):.17g}",
        f"p99_9_abs_residual={np.percentile(abs_residual, 99.9):.17g}",
        f"clipping_difference={candidate_clipping-reference_clipping}",
        f"lpc_frame_counts_identical={'yes' if candidate_info.get('lpc_frames') == reference_info.get('lpc_frames') else 'no'}",
        f"lpc_invalid_counts_identical={'yes' if candidate_info.get('invalid_lpc_frames') == reference_info.get('invalid_lpc_frames') else 'no'}",
        f"formant_frame_counts_identical={'yes' if candidate_info.get('formant_frames') == reference_info.get('formant_frames') else 'no'}",
        f"formant_reset_counts_identical={'yes' if candidate_info.get('formant_resets') == reference_info.get('formant_resets') else 'no'}",
        "",
        "Critical-region residuals (classification from render pitch metadata and input level):",
    ])
    for name, mask in masks.items():
        lines.append(f"{name}: {residual_stats(residual, mask)}")
    lines.extend(["", "Classification: " +
                  ("AUDIO_GUARDRAILS_PASS" if guard else
                   "AUDIO_GUARDRAILS_FAIL")])
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(OUTPUT)
    return 0 if guard else 1


if __name__ == "__main__":
    raise SystemExit(main())
