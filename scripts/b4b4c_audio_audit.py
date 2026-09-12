"""Render and compare numerically eligible B4B.4C autocorrelation variants."""

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
SCRATCH = ROOT / "scratch/b4b4c"
OUTPUT = ROOT / "artifacts/alpha01b/b4b4c_audio_equivalence.txt"
# Host numeric audit made only double-single eligible for an audio render.
VARIANTS = ("reference", "f32-double-single")


def render(variant: str) -> tuple[pathlib.Path, str]:
    output = SCRATCH / f"{variant.replace('-', '_')}.wav"
    command = [
        str(EXECUTABLE), str(INPUT), str(output), "7",
        "--formants", "lpc", "--formant-amount", "1",
        "--lpc-order", "16", "--autocorr", variant,
        "--render-mode", "solo", "--continuity-policy", "baseline",
    ]
    completed = subprocess.run(command, cwd=ROOT, check=True,
                               capture_output=True, text=True)
    return output, completed.stderr.strip()


def telemetry(text: str) -> dict[str, str]:
    keys = ("formant_frames", "formant_resets", "max_restored")
    result: dict[str, str] = {}
    for key in keys:
        match = re.search(rf"\b{key}=([^\s]+)", text)
        result[key] = match.group(1) if match else "not_reported"
    lpc = re.search(r"lpc_frames=(\d+) invalid=(\d+).*max_error=([^\s]+)", text)
    if lpc:
        result.update(lpc_frames=lpc.group(1), invalid_lpc_frames=lpc.group(2),
                      max_prediction_error=lpc.group(3))
    return result


def main() -> int:
    if not INPUT.exists() or not EXECUTABLE.exists():
        raise SystemExit("build-host/pitch_shift.exe or vocal fixture is missing")
    SCRATCH.mkdir(parents=True, exist_ok=True)
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    renders = {variant: render(variant) for variant in VARIANTS}
    sample_rate, reference = wavfile.read(renders["reference"][0])
    reference = np.asarray(reference, dtype=np.float64)
    reference_rms = float(np.sqrt(np.mean(reference * reference)))
    reference_info = telemetry(renders["reference"][1])
    reference_clipping = int(np.count_nonzero(np.abs(reference) >= 1.0))
    lines = [
        "Stage B4B.4C — Compensated Autocorrelation Audio Equivalence",
        "",
        f"input={INPUT.relative_to(ROOT)}",
        "configuration=+7 semitones, LPC formants amount 1.0, order 16, "
        "solo render, baseline continuity",
        "render_scope=AUTOCORR_F32_DOUBLE_SINGLE only; other compensated "
        "candidates failed host numeric guardrails",
        f"sample_rate={sample_rate}",
        f"sample_count={reference.size}",
        "",
        "variant,sample_count,peak,rms,dc,max_absolute_sample_difference,"
        "rms_residual,difference_snr_db,nan_inf,clipping_samples,"
        "clipping_difference,formant_frames,formant_resets,max_restored,"
        "lpc_frames,invalid_lpc_frames,audio_guard",
    ]
    all_pass = True
    for variant in VARIANTS:
        _, data = wavfile.read(renders[variant][0])
        data = np.asarray(data, dtype=np.float64)
        same_count = data.size == reference.size
        residual = data - reference if same_count else np.array([math.inf])
        residual_rms = float(np.sqrt(np.mean(residual * residual)))
        difference_snr = (math.inf if residual_rms == 0.0 else
                          20.0 * math.log10(reference_rms / residual_rms))
        finite = bool(np.isfinite(data).all())
        clipping = int(np.count_nonzero(np.abs(data) >= 1.0))
        info = telemetry(renders[variant][1])
        telemetry_equal = (
            info.get("formant_frames") == reference_info.get("formant_frames")
            and info.get("formant_resets") == reference_info.get("formant_resets")
        )
        guard = (same_count and finite and clipping == reference_clipping and
                 telemetry_equal and
                 (variant == "reference" or difference_snr >= 80.0))
        all_pass &= guard
        lines.append(
            f"{variant},{data.size},{np.max(np.abs(data)):.17g},"
            f"{np.sqrt(np.mean(data * data)):.17g},{np.mean(data):.17g},"
            f"{np.max(np.abs(residual)):.17g},{residual_rms:.17g},"
            f"{difference_snr:.9g},{0 if finite else 1},{clipping},"
            f"{clipping-reference_clipping},"
            f"{info.get('formant_frames', 'not_reported')},"
            f"{info.get('formant_resets', 'not_reported')},"
            f"{info.get('max_restored', 'not_reported')},"
            f"{info.get('lpc_frames', 'not_reported')},"
            f"{info.get('invalid_lpc_frames', 'not_reported')},"
            f"{'PASS' if guard else 'FAIL'}"
        )
    lines.extend([
        "",
        "Classification: " + ("AUDIO_GUARDRAILS_PASS" if all_pass
                               else "AUDIO_GUARDRAILS_FAIL"),
    ])
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(OUTPUT)
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
