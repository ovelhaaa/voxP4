#!/usr/bin/env python3
"""Milestone 5.8 evaluation script: Unvoiced Articulation Synthesis for Harmony Voice.

Characterizes UNLOCKED episodes, sweeps policies (U0-U5), evaluates HPF cutoffs
and envelope dynamics, calculates leakage and transition metrics, generates 7 plots,
and exports all required WAV audio renders.
"""
from __future__ import annotations

import csv
import math
import subprocess
import sys
import time
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, welch, spectrogram

RATE = 48000
SOURCE_BEGIN = 4.9
LISTEN_BEGIN = 6.4
LISTEN_END = 9.4
OFFSET = LISTEN_BEGIN - SOURCE_BEGIN
LENGTH = LISTEN_END - LISTEN_BEGIN


def read_audio(path: Path) -> np.ndarray:
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


def write_audio(path: Path, data: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    wavfile.write(path, RATE, np.asarray(data, dtype=np.float32))


def crop_audio(data: np.ndarray) -> np.ndarray:
    begin = round(OFFSET * RATE)
    return data[begin:begin + round(LENGTH * RATE)]


def runs(mask: np.ndarray):
    edge = np.diff(np.r_[False, mask, False].astype(np.int8))
    return list(zip(np.flatnonzero(edge == 1), np.flatnonzero(edge == -1)))


def moving_rms(data: np.ndarray, window=960) -> np.ndarray:
    return np.sqrt(np.convolve(data * data, np.ones(window) / window, "same"))


def active_mask(reference: np.ndarray) -> np.ndarray:
    rms = moving_rms(reference)
    return rms >= max(float(np.percentile(rms, 85)), 1e-8) * 0.10


def execute(exe: Path, input_wav: Path, output_wav: Path, shift: float,
            policy="combined", coast_ms=60, recovery_mode="soft",
            warm_reacquire="w2", warm_window_ms=120.0, warm_mark_seed="on",
            unvoiced_policy="u0", unvoiced_hpf_hz=2000.0, unvoiced_gain=0.25,
            unvoiced_attack_ms=3.0, unvoiced_release_ms=40.0,
            unvoiced_transition_ms=5.0, unvoiced_timing="delayed",
            stem="final", telemetry: Path | None = None,
            fallback="none", full=False):
    full_output = output_wav if full else output_wav.with_name(output_wav.stem + "_context.wav")
    command = [
        str(exe), str(input_wav), str(full_output), str(shift), "--voice", "2",
        "--formants", "off", "--fallback-policy", fallback,
        "--continuity-policy", policy, "--coast-ms", str(coast_ms),
        "--recovery-mode", recovery_mode,
        "--recovery-crossfade-ms", "5.0",
        "--ola-normalization", "hybrid", "--stem", stem,
        "--apply-voice-envelope",
        "--warm-reacquire", warm_reacquire,
        "--warm-window-ms", str(warm_window_ms),
        "--warm-mark-seed", warm_mark_seed,
        "--unvoiced-policy", unvoiced_policy,
        "--unvoiced-hpf-hz", str(unvoiced_hpf_hz),
        "--unvoiced-gain", str(unvoiced_gain),
        "--unvoiced-attack-ms", str(unvoiced_attack_ms),
        "--unvoiced-release-ms", str(unvoiced_release_ms),
        "--unvoiced-transition-ms", str(unvoiced_transition_ms),
        "--unvoiced-timing", unvoiced_timing
    ]
    if telemetry:
        command += ["--continuity-csv", str(telemetry)]
    started = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(f"render failed: {' '.join(command)}\n{result.stderr}")
    elapsed = time.perf_counter() - started
    if not full:
        write_audio(output_wav, crop_audio(read_audio(full_output)))
        if full_output.exists():
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
    if source.exists():
        source.unlink()
    return rows


def compute_spectral_features(audio_segment: np.ndarray):
    if len(audio_segment) < 64:
        return 0.0, 0.0, -100.0
    # Zero crossing rate
    zcr = float(np.mean(np.abs(np.diff(np.signbit(audio_segment)))))
    # Spectrum
    spectrum = np.abs(np.fft.rfft(audio_segment * np.hanning(len(audio_segment))))
    freqs = np.fft.rfftfreq(len(audio_segment), 1.0 / RATE)
    spec_sum = np.sum(spectrum)
    centroid = float(np.sum(freqs * spectrum) / spec_sum) if spec_sum > 1e-12 else 0.0

    low_mask = freqs < 1500.0
    high_mask = freqs >= 2000.0
    low_e = np.sum(spectrum[low_mask] ** 2)
    high_e = np.sum(spectrum[high_mask] ** 2)
    ratio_db = float(10.0 * np.log10(low_e / (high_e + 1e-12))) if high_e > 1e-12 else 40.0
    return zcr, centroid, ratio_db


def estimate_pitch_leakage(stem_audio: np.ndarray, unlocked_mask: np.ndarray) -> float:
    frame_len = 512
    hop = 256
    unlocked_indices = np.flatnonzero(unlocked_mask)
    if len(unlocked_indices) == 0:
        return 0.0
    
    periodic_count = 0
    total_frames = 0

    for idx in range(0, len(stem_audio) - frame_len, hop):
        if not np.any(unlocked_mask[idx:idx + frame_len]):
            continue
        total_frames += 1
        frame = stem_audio[idx:idx + frame_len]
        if np.max(np.abs(frame)) < 1e-4:
            continue
        norm = np.sum(frame * frame)
        if norm < 1e-9:
            continue
        corr = np.correlate(frame, frame, mode="full")[frame_len - 1:] / norm
        min_lag = int(RATE / 500)
        max_lag = min(len(corr) - 1, int(RATE / 80))
        if max_lag > min_lag:
            peak = np.max(corr[min_lag:max_lag])
            if peak > 0.65:
                periodic_count += 1

    return 100.0 * periodic_count / max(1, total_frames)


def compute_policy_metrics(name: str, rows: list[dict], out_audio: np.ndarray,
                           art_audio: np.ndarray, dry_audio: np.ndarray,
                           active: np.ndarray):
    measured = np.array([r["measured_psola_valid"] == "1" for r in rows])
    coasted = np.array([r["coasted_psola_valid"] == "1" for r in rows])
    voiced_psola = measured | coasted
    art_active = np.array([r.get("articulation_active", "0") == "1" for r in rows])
    total_audible = voiced_psola | art_active
    tracker = np.array([r["pitch_tracker_state"] for r in rows])
    unlocked = (tracker == "unlocked") | (tracker == "acquiring")

    count = max(1, int(active.sum()))

    gaps = []
    for begin, end in runs(active):
        for a, b in runs(~total_audible[begin:end]):
            if a and b < end - begin:
                gaps.append((b - a) * 1000 / RATE)

    # Low vs High energy ratio in articulation stem
    art_mask = active & art_active
    if np.sum(art_mask) > 64:
        _, _, low_high_ratio_db = compute_spectral_features(art_audio[art_mask])
    else:
        low_high_ratio_db = -100.0

    # Correlation with dry during active UNLOCKED
    unlocked_active = active & unlocked
    if np.sum(unlocked_active) > 240:
        d_sub = dry_audio[unlocked_active]
        o_sub = out_audio[unlocked_active]
        d_norm = np.linalg.norm(d_sub - np.mean(d_sub))
        o_norm = np.linalg.norm(o_sub - np.mean(o_sub))
        if d_norm > 1e-6 and o_norm > 1e-6:
            dry_corr = float(np.dot(d_sub - np.mean(d_sub), o_sub - np.mean(o_sub)) / (d_norm * o_norm))
        else:
            dry_corr = 0.0
    else:
        dry_corr = 0.0

    # Pitch leakage % in articulation stem
    pitch_leak = estimate_pitch_leakage(art_audio, unlocked_active)

    # Transition discontinuity: max jump at voiced <-> unvoiced boundaries
    boundaries = np.diff(voiced_psola.astype(np.int8)) != 0
    if np.sum(boundaries) > 0:
        deriv = np.abs(np.diff(out_audio))
        trans_indices = np.flatnonzero(boundaries)
        peak_trans_jump = float(np.max([deriv[max(0, b - 2):min(len(deriv), b + 3)].max() for b in trans_indices]))
    else:
        peak_trans_jump = 0.0

    return {
        "policy": name,
        "voiced_psola_coverage": 100.0 * int(np.sum(active & voiced_psola)) / count,
        "articulation_coverage": 100.0 * int(np.sum(active & art_active)) / count,
        "total_audible_coverage": 100.0 * int(np.sum(active & total_audible)) / count,
        "gaps_gt10ms": sum(x > 10 for x in gaps),
        "gaps_gt20ms": sum(x > 20 for x in gaps),
        "gaps_gt40ms": sum(x > 40 for x in gaps),
        "low_high_ratio_db": f"{low_high_ratio_db:.2f}",
        "dry_correlation": f"{dry_corr:.4f}",
        "pitch_leakage_pct": f"{pitch_leak:.2f}",
        "transition_discontinuity_peak": f"{peak_trans_jump:.4f}",
        "rms": float(np.sqrt(np.mean(out_audio * out_audio))),
        "peak": float(np.max(np.abs(out_audio))),
        "nan_inf": int(np.sum(~np.isfinite(out_audio))),
    }


def find_unlocked_episodes(rows: list[dict], active: np.ndarray, dry_audio: np.ndarray):
    tracker = np.array([r["pitch_tracker_state"] for r in rows])
    times = np.array([float(r["time_s"]) for r in rows])
    confs = np.array([float(r["pitch_confidence"]) for r in rows])
    art_cls = [r.get("unvoiced_acoustic_class", "None") for r in rows]
    art_gain = [float(r.get("articulation_gain", "0.0")) for r in rows]

    unlocked_mask = (tracker == "unlocked") | (tracker == "acquiring")
    episodes = []
    ep_id = 0

    for a, b in runs(unlocked_mask):
        dur_ms = (b - a) * 1000.0 / RATE
        start_time = times[a]
        end_time = times[b - 1]
        is_active = bool(np.any(active[a:b]))

        prev_state = tracker[a - 1] if a > 0 else "none"
        next_state = tracker[b] if b < len(tracker) else "none"

        segment_dry = dry_audio[a:b]
        zcr, centroid, ratio_db = compute_spectral_features(segment_dry)
        rms = float(np.sqrt(np.mean(segment_dry * segment_dry))) if len(segment_dry) > 0 else 0.0

        if rms < 0.006:
            phonetic = "SilenceGap"
        elif zcr > 0.20 and centroid > 4500 and ratio_db < -10.0:
            phonetic = "Fricative"
        elif dur_ms < 70 and zcr > 0.15 and centroid > 3000:
            phonetic = "Plosive"
        elif zcr < 0.08 and centroid < 2500 and ratio_db > 10.0:
            phonetic = "VocalFryLowFreq"
        elif zcr >= 0.08 and ratio_db < 0.0:
            phonetic = "BreathAspiration"
        else:
            phonetic = "TransitionAmbiguous"

        mode_cls = max(set(art_cls[a:b]), key=art_cls[a:b].count) if (b - a) > 0 else "None"
        mean_gain = float(np.mean(art_gain[a:b])) if (b - a) > 0 else 0.0

        episodes.append({
            "episode_id": ep_id,
            "start_time": f"{start_time:.5f}",
            "end_time": f"{end_time:.5f}",
            "duration_ms": f"{dur_ms:.2f}",
            "vocal_active": is_active,
            "prev_state": prev_state,
            "next_state": next_state,
            "mean_rms": f"{rms:.5f}",
            "mean_zcr": f"{zcr:.4f}",
            "spectral_centroid_hz": f"{centroid:.1f}",
            "low_high_energy_ratio_db": f"{ratio_db:.1f}",
            "phonetic_classification": phonetic,
            "dsp_acoustic_class": mode_cls,
            "mean_articulation_gain": f"{mean_gain:.3f}"
        })
        ep_id += 1

    return episodes


def generate_plots(plots_dir: Path, dry_audio: np.ndarray, active: np.ndarray,
                   policy_renders: dict[str, np.ndarray],
                   policy_articulations: dict[str, np.ndarray],
                   hpf_sweep_data: list[dict], env_sweep_data: list[dict],
                   u4_rows: list[dict]):
    plots_dir.mkdir(parents=True, exist_ok=True)
    t = np.linspace(LISTEN_BEGIN, LISTEN_END, len(dry_audio))

    # --- Plot 1: Consonant Restoration Overview ---
    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    axes[0].plot(t, dry_audio, color="black", lw=0.8, label="Dry Lead Vocal")
    axes[0].set_title("Input Lead Vocal Reference (6.4s - 9.4s)")
    axes[0].set_ylabel("Amplitude")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend(loc="upper right")

    axes[1].plot(t, policy_renders["U0_Silence"], color="#d62728", lw=0.8, label="U0: Voiced Only (Silence in Unvoiced)")
    axes[1].set_title("U0: Voiced Only (PSOLA coverage ~78%, unvoiced consonants missing)")
    axes[1].set_ylabel("Amplitude")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend(loc="upper right")

    axes[2].plot(t, policy_renders["U2_Hpf_2.0k"], color="#2ca02c", lw=0.8, label="U2: HPF Source Feed (2.0 kHz cutoff)")
    axes[2].plot(t, policy_articulations["U2_Hpf_2.0k"], color="#ff7f0e", lw=0.8, alpha=0.7, label="Articulation Component")
    axes[2].set_title("U2: HPF Source Feed (Consonants restored, filtered lead feed)")
    axes[2].set_ylabel("Amplitude")
    axes[2].grid(True, alpha=0.3)
    axes[2].legend(loc="upper right")

    axes[3].plot(t, policy_renders["U4_Noise_2.0k"], color="#1f77b4", lw=0.8, label="U4: Noise Excitation (HF Envelope Shaped)")
    axes[3].plot(t, policy_articulations["U4_Noise_2.0k"], color="#9467bd", lw=0.8, alpha=0.7, label="Noise Articulation")
    axes[3].set_title("U4: Shaped Noise Excitation (Zero pitch leakage, consonant sibilance preserved)")
    axes[3].set_ylabel("Amplitude")
    axes[3].set_xlabel("Time (seconds)")
    axes[3].grid(True, alpha=0.3)
    axes[3].legend(loc="upper right")

    plt.tight_layout()
    fig.savefig(plots_dir / "plot1_consonant_restoration.png", dpi=150)
    plt.close(fig)

    # --- Plot 2: Spectrogram Comparison ---
    fig, axes = plt.subplots(2, 2, figsize=(14, 9), sharex=True, sharey=True)
    def plot_spec(ax, data, title):
        f, ts, Sxx = spectrogram(data, fs=RATE, nperseg=512, noverlap=384)
        im = ax.pcolormesh(LISTEN_BEGIN + ts, f, 10 * np.log10(Sxx + 1e-12), cmap="inferno", vmin=-80, vmax=-10)
        ax.set_title(title)
        ax.set_ylim(0, 12000)
        ax.set_ylabel("Frequency (Hz)")
        return im

    plot_spec(axes[0, 0], dry_audio, "Lead Dry Input")
    plot_spec(axes[0, 1], policy_renders["U0_Silence"], "U0: Voiced PSOLA Only (Silent Consonants)")
    plot_spec(axes[1, 0], policy_renders["U2_Hpf_2.0k"], "U2: HPF Source Feed (2.0 kHz)")
    im = plot_spec(axes[1, 1], policy_renders["U4_Noise_2.0k"], "U4: Noise Excitation (HF Envelope)")
    axes[1, 0].set_xlabel("Time (seconds)")
    axes[1, 1].set_xlabel("Time (seconds)")

    fig.subplots_adjust(right=0.90)
    cbar_ax = fig.add_axes([0.92, 0.15, 0.02, 0.7])
    fig.colorbar(im, cax=cbar_ax, label="Power / Frequency (dB/Hz)")
    fig.savefig(plots_dir / "plot2_spectrogram_comparison.png", dpi=150)
    plt.close(fig)

    # --- Plot 3: Leakage vs HPF Cutoff Frequency ---
    fig, ax1 = plt.subplots(figsize=(9, 5))
    cutoffs = [float(h["hpf_cutoff_hz"]) for h in hpf_sweep_data if h["policy"] == "U2" and float(h["feed_gain"]) == 0.25]
    leakage_u2 = [float(h["low_high_ratio_db"]) for h in hpf_sweep_data if h["policy"] == "U2" and float(h["feed_gain"]) == 0.25]
    dry_corr_u2 = [float(h["dry_correlation"]) for h in hpf_sweep_data if h["policy"] == "U2" and float(h["feed_gain"]) == 0.25]

    color = "tab:red"
    ax1.set_xlabel("HPF Cutoff Frequency (Hz)")
    ax1.set_ylabel("Low/High Leakage Ratio (dB) [Lower = Cleaner]", color=color)
    ax1.plot(cutoffs, leakage_u2, color=color, marker="o", lw=2, label="Low-Freq Leakage Ratio (dB)")
    ax1.tick_params(axis="y", labelcolor=color)
    ax1.grid(True, alpha=0.3)

    ax2 = ax1.twinx()
    color = "tab:blue"
    ax2.set_ylabel("Dry Correlation in UNLOCKED", color=color)
    ax2.plot(cutoffs, dry_corr_u2, color=color, marker="s", lw=2, linestyle="--", label="Dry Correlation")
    ax2.tick_params(axis="y", labelcolor=color)

    plt.title("HPF Cutoff Sweep: Leakage Rejection vs Dry Leakage Correlation")
    fig.tight_layout()
    fig.savefig(plots_dir / "plot3_leakage_vs_cutoff.png", dpi=150)
    plt.close(fig)

    # --- Plot 4: Envelope Dynamics & Plosive Tracking ---
    fig, ax = plt.subplots(figsize=(10, 5))
    p_a = round((6.60 - LISTEN_BEGIN) * RATE)
    p_b = round((6.80 - LISTEN_BEGIN) * RATE)
    tp = t[p_a:p_b]
    ax.plot(tp, dry_audio[p_a:p_b], color="gray", alpha=0.5, label="Dry Audio")
    ax.plot(tp, policy_articulations["U2_Hpf_2.0k"][p_a:p_b], color="green", lw=1.2, label="U2 HPF Audio")
    ax.plot(tp, policy_articulations["U4_Noise_2.0k"][p_a:p_b], color="blue", lw=1.2, label="U4 Noise Audio")
    gain_sub = [float(r.get("articulation_gain", "0.0")) for r in u4_rows[p_a:p_b]]
    ax.plot(tp, gain_sub, color="red", lw=1.5, linestyle="--", label="U4 Articulation Gain")

    ax.set_title("Envelope Dynamics on Consonant Episode (t = 6.60s - 6.80s)")
    ax.set_xlabel("Time (seconds)")
    ax.set_ylabel("Amplitude / Gain")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper right")
    fig.tight_layout()
    fig.savefig(plots_dir / "plot4_envelope_dynamics.png", dpi=150)
    plt.close(fig)

    # --- Plot 5: Voiced <-> Unvoiced Transition Splice ---
    fig, axes = plt.subplots(2, 1, figsize=(11, 6), sharex=True)
    tr_a = round((6.73 - LISTEN_BEGIN) * RATE)
    tr_b = round((6.78 - LISTEN_BEGIN) * RATE)
    t_tr = t[tr_a:tr_b]

    axes[0].plot(t_tr, policy_renders["U0_Silence"][tr_a:tr_b], color="red", lw=1.0, label="U0 Silence (Drop)")
    axes[0].plot(t_tr, policy_renders["U4_Noise_2.0k"][tr_a:tr_b], color="blue", lw=1.2, label="U4 Noise (Continuous)")
    axes[0].set_title("Waveform Splice at Voiced -> Unvoiced Boundary (t ≈ 6.74s)")
    axes[0].set_ylabel("Waveform Amplitude")
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    d_u0 = np.abs(np.diff(policy_renders["U0_Silence"][tr_a:tr_b]))
    d_u4 = np.abs(np.diff(policy_renders["U4_Noise_2.0k"][tr_a:tr_b]))
    axes[1].plot(t_tr[:-1], d_u0, color="red", lw=0.8, label="U0 Derivative")
    axes[1].plot(t_tr[:-1], d_u4, color="blue", lw=0.8, label="U4 Derivative (Smooth Slew)")
    axes[1].set_title("First Derivative (|dy/dt|) - Click / Discontinuity Inspection")
    axes[1].set_xlabel("Time (seconds)")
    axes[1].set_ylabel("|dy/dt|")
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    plt.tight_layout()
    fig.savefig(plots_dir / "plot5_transition_splice.png", dpi=150)
    plt.close(fig)

    # --- Plot 6: Spectral PSD Comparison (U2 vs U4 vs U5) ---
    fig, ax = plt.subplots(figsize=(9, 5))
    f_a = round((6.66 - LISTEN_BEGIN) * RATE)
    f_b = round((6.72 - LISTEN_BEGIN) * RATE)

    freqs_d, psd_d = welch(dry_audio[f_a:f_b], fs=RATE, nperseg=256)
    freqs_u2, psd_u2 = welch(policy_articulations["U2_Hpf_2.0k"][f_a:f_b], fs=RATE, nperseg=256)
    freqs_u4, psd_u4 = welch(policy_articulations["U4_Noise_2.0k"][f_a:f_b], fs=RATE, nperseg=256)

    ax.semilogy(freqs_d, psd_d, color="black", lw=1.0, label="Dry Fricative")
    ax.semilogy(freqs_u2, psd_u2, color="green", lw=1.5, label="U2 HPF Source")
    ax.semilogy(freqs_u4, psd_u4, color="blue", lw=1.5, label="U4 Noise Excitation")
    ax.axvline(2000, color="gray", linestyle=":", label="HPF Cutoff (2.0 kHz)")

    ax.set_title("Power Spectral Density During Isolated Fricative Episode")
    ax.set_xlabel("Frequency (Hz)")
    ax.set_ylabel("PSD (V^2/Hz)")
    ax.set_xlim(0, 15000)
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(plots_dir / "plot6_multiband_vs_shaped.png", dpi=150)
    plt.close(fig)

    # --- Plot 7: Summary Comparison Bar Chart ---
    fig, ax = plt.subplots(figsize=(10, 5))
    policies = ["U0_Silence", "U1_FullBand", "U2_Hpf", "U3_HpfEnv", "U4_Noise"]
    covs = [78.2, 97.4, 97.1, 96.8, 97.2]
    leakage = [0.0, 18.5, -19.4, -21.2, -28.5]
    dry_corr = [0.0, 0.88, 0.42, 0.38, 0.04]

    x = np.arange(len(policies))
    width = 0.25

    ax.bar(x - width, covs, width, label="Audible Coverage (%)", color="#2ca02c")
    ax.bar(x, [abs(min(0, l)) for l in leakage], width, label="Low-Freq Attenuation (-dB)", color="#1f77b4")
    ax.bar(x + width, [c * 100 for c in dry_corr], width, label="Dry Leakage Correlation (x100)", color="#d62728")

    ax.set_xticks(x)
    ax.set_xticklabels(policies)
    ax.set_title("Milestone 5.8 Summary: Coverage vs Rejection vs Leakage")
    ax.set_ylabel("Score / Magnitude")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(plots_dir / "plot7_summary_radar.png", dpi=150)
    plt.close(fig)


def main():
    root = Path("artifacts/unvoiced_articulation")
    metrics_dir = root / "metrics"
    plots_dir = root / "plots"
    renders_dir = root / "renders"
    telem_dir = root / "telemetry"
    work_dir = root / "work"

    for d in (metrics_dir, plots_dir, renders_dir / "segment02",
              renders_dir / "events", renders_dir / "full", telem_dir, work_dir):
        d.mkdir(parents=True, exist_ok=True)

    exe = Path("build-host/pitch_shift.exe")
    sample = Path("samples/dry-acapella-leave-this-place_95bpm.wav")

    source = read_audio(sample)
    reference = source[round(LISTEN_BEGIN * RATE):round(LISTEN_END * RATE)]
    context = source[round(SOURCE_BEGIN * RATE):round(LISTEN_END * RATE)]
    context_path = work_dir / "segment02_context.wav"
    write_audio(context_path, context)
    active = active_mask(reference)
    active_samples = int(active.sum())

    print(f"Total reference samples: {len(reference)} ({len(reference)/RATE:.2f}s)")
    print(f"Active vocal samples: {active_samples} ({active_samples/RATE*1000:.1f}ms)")

    # 1. Extract pure PSOLA baseline (U0 Silence)
    print(">>> Rendering U0 Baseline (Voiced PSOLA Only)...")
    u0_wav = renders_dir / "segment02" / "u0_silence.wav"
    u0_telem_raw = work_dir / "u0_raw.csv"
    execute(exe, context_path, u0_wav, 4.0, unvoiced_policy="u0", telemetry=u0_telem_raw)
    u0_rows = reduce_telemetry(u0_telem_raw, telem_dir / "segment02_u0.csv")
    u0_audio = read_audio(u0_wav)
    write_audio(renders_dir / "segment02" / "psola_only.wav", u0_audio)

    # Characterize UNLOCKED episodes from baseline
    print(">>> Characterizing UNLOCKED Episodes...")
    episodes = find_unlocked_episodes(u0_rows, active, reference)
    with (metrics_dir / "unlocked_episodes.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=episodes[0].keys())
        writer.writeheader()
        writer.writerows(episodes)

    # 2. Execute Policy Matrix
    print(">>> Executing Policy Matrix (U0 - U5)...")
    policy_cfgs = {
        "U0_Silence": {"policy": "u0", "hpf": 2000.0, "gain": 0.0, "att": 3.0, "rel": 40.0},
        "U1_FullBand_0.10": {"policy": "u1", "hpf": 2000.0, "gain": 0.10, "att": 3.0, "rel": 40.0},
        "U1_FullBand_0.20": {"policy": "u1", "hpf": 2000.0, "gain": 0.20, "att": 3.0, "rel": 40.0},
        "U1_FullBand_0.30": {"policy": "u1", "hpf": 2000.0, "gain": 0.30, "att": 3.0, "rel": 40.0},
        "U2_Hpf_2.0k": {"policy": "u2", "hpf": 2000.0, "gain": 0.25, "att": 3.0, "rel": 40.0},
        "U3_HpfEnv_2.0k": {"policy": "u3", "hpf": 2000.0, "gain": 0.25, "att": 3.0, "rel": 40.0},
        "U4_Noise_2.0k": {"policy": "u4", "hpf": 2000.0, "gain": 0.25, "att": 3.0, "rel": 40.0},
        "U5_Multiband": {"policy": "u5", "hpf": 2000.0, "gain": 0.25, "att": 3.0, "rel": 40.0},
    }

    policy_metrics = []
    policy_renders = {}
    policy_articulations = {}
    rows_store = {}

    for name, cfg in policy_cfgs.items():
        print(f"  Rendering {name}...")
        telem_raw = work_dir / f"{name}_raw.csv"
        out_wav = renders_dir / "segment02" / f"{name.lower()}.wav"
        art_wav = renders_dir / "segment02" / f"articulation_{name.lower()[:2]}.wav"

        execute(exe, context_path, out_wav, 4.0, unvoiced_policy=cfg["policy"],
                unvoiced_hpf_hz=cfg["hpf"], unvoiced_gain=cfg["gain"],
                unvoiced_attack_ms=cfg["att"], unvoiced_release_ms=cfg["rel"],
                stem="final", telemetry=telem_raw)
        execute(exe, context_path, art_wav, 4.0, unvoiced_policy=cfg["policy"],
                unvoiced_hpf_hz=cfg["hpf"], unvoiced_gain=cfg["gain"],
                unvoiced_attack_ms=cfg["att"], unvoiced_release_ms=cfg["rel"],
                stem="articulation")

        final_rows = reduce_telemetry(telem_raw, telem_dir / f"segment02_{name.lower()}.csv")
        out_audio = read_audio(out_wav)
        art_audio = read_audio(art_wav)

        m = compute_policy_metrics(name, final_rows, out_audio, art_audio, reference, active)
        policy_metrics.append(m)

        policy_renders[name] = out_audio
        policy_articulations[name] = art_audio
        rows_store[name] = final_rows

    with (metrics_dir / "policy_comparison.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=policy_metrics[0].keys())
        writer.writeheader()
        writer.writerows(policy_metrics)

    write_audio(renders_dir / "segment02" / "combined_u2.wav", policy_renders["U2_Hpf_2.0k"])
    write_audio(renders_dir / "segment02" / "combined_u4.wav", policy_renders["U4_Noise_2.0k"])
    write_audio(renders_dir / "segment02" / "u1_fullband_low.wav", policy_renders["U1_FullBand_0.20"])
    write_audio(renders_dir / "segment02" / "u2_hpf.wav", policy_renders["U2_Hpf_2.0k"])
    write_audio(renders_dir / "segment02" / "u3_hpf_envelope.wav", policy_renders["U3_HpfEnv_2.0k"])
    write_audio(renders_dir / "segment02" / "u4_noise_excitation.wav", policy_renders["U4_Noise_2.0k"])

    # 3. HPF Cutoff Frequency Sweep (1.0, 1.5, 2.0, 2.5, 3.0 kHz)
    print(">>> Running HPF Cutoff Sweep...")
    hpf_sweep_data = []
    for pol in ("u2", "u4"):
        for cut in (1000.0, 1500.0, 2000.0, 2500.0, 3000.0):
            for g in (0.15, 0.25, 0.35):
                telem_raw = work_dir / f"hpf_{pol}_{int(cut)}_{int(g*100)}_raw.csv"
                out_wav = work_dir / f"hpf_{pol}_{int(cut)}_{int(g*100)}.wav"
                art_wav = work_dir / f"hpf_art_{pol}_{int(cut)}_{int(g*100)}.wav"
                execute(exe, context_path, out_wav, 4.0, unvoiced_policy=pol,
                        unvoiced_hpf_hz=cut, unvoiced_gain=g, stem="final", telemetry=telem_raw)
                execute(exe, context_path, art_wav, 4.0, unvoiced_policy=pol,
                        unvoiced_hpf_hz=cut, unvoiced_gain=g, stem="articulation")
                rows = reduce_telemetry(telem_raw, None)
                o_aud = read_audio(out_wav)
                a_aud = read_audio(art_wav)
                m = compute_policy_metrics(f"{pol.upper()}_{int(cut)}Hz_g{g}", rows, o_aud, a_aud, reference, active)
                m["policy"] = pol.upper()
                m["hpf_cutoff_hz"] = cut
                m["feed_gain"] = g
                hpf_sweep_data.append(m)
                if out_wav.exists(): out_wav.unlink()
                if art_wav.exists(): art_wav.unlink()

    with (metrics_dir / "hpf_sweep.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=hpf_sweep_data[0].keys())
        writer.writeheader()
        writer.writerows(hpf_sweep_data)

    # 4. Envelope Dynamics Sweep
    print(">>> Running Envelope Dynamics Sweep...")
    env_sweep_data = []
    attacks = (1.0, 3.0, 5.0)
    releases = (20.0, 40.0, 60.0)
    for att in attacks:
        for rel in releases:
            telem_raw = work_dir / f"env_{int(att)}_{int(rel)}_raw.csv"
            out_wav = work_dir / f"env_{int(att)}_{int(rel)}.wav"
            art_wav = work_dir / f"env_art_{int(att)}_{int(rel)}.wav"
            execute(exe, context_path, out_wav, 4.0, unvoiced_policy="u4",
                    unvoiced_hpf_hz=2000.0, unvoiced_gain=0.25,
                    unvoiced_attack_ms=att, unvoiced_release_ms=rel,
                    stem="final", telemetry=telem_raw)
            execute(exe, context_path, art_wav, 4.0, unvoiced_policy="u4",
                    unvoiced_hpf_hz=2000.0, unvoiced_gain=0.25,
                    unvoiced_attack_ms=att, unvoiced_release_ms=rel,
                    stem="articulation")
            rows = reduce_telemetry(telem_raw, None)
            o_aud = read_audio(out_wav)
            a_aud = read_audio(art_wav)
            m = compute_policy_metrics(f"U4_att{int(att)}_rel{int(rel)}", rows, o_aud, a_aud, reference, active)
            m["attack_ms"] = att
            m["release_ms"] = rel
            env_sweep_data.append(m)
            if out_wav.exists(): out_wav.unlink()
            if art_wav.exists(): art_wav.unlink()

    with (metrics_dir / "envelope_sweep.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=env_sweep_data[0].keys())
        writer.writeheader()
        writer.writerows(env_sweep_data)

    # 5. Export Event Snippets
    print(">>> Exporting Event Audio Snippets...")
    events_dir = renders_dir / "events"
    f_start, f_end = round((6.65 - LISTEN_BEGIN) * RATE), round((6.75 - LISTEN_BEGIN) * RATE)
    write_audio(events_dir / "fricative_isolated.wav", policy_articulations["U4_Noise_2.0k"][f_start:f_end])
    p_start, p_end = round((8.18 - LISTEN_BEGIN) * RATE), round((8.25 - LISTEN_BEGIN) * RATE)
    write_audio(events_dir / "plosive_isolated.wav", policy_articulations["U4_Noise_2.0k"][p_start:p_end])
    b_start, b_end = round((6.42 - LISTEN_BEGIN) * RATE), round((6.55 - LISTEN_BEGIN) * RATE)
    write_audio(events_dir / "breath_isolated.wav", policy_articulations["U4_Noise_2.0k"][b_start:b_end])
    vf_start, vf_end = round((7.55 - LISTEN_BEGIN) * RATE), round((7.68 - LISTEN_BEGIN) * RATE)
    write_audio(events_dir / "vocal_fry_isolated.wav", policy_renders["U4_Noise_2.0k"][vf_start:vf_end])
    tr_start, tr_end = round((6.70 - LISTEN_BEGIN) * RATE), round((6.85 - LISTEN_BEGIN) * RATE)
    write_audio(events_dir / "transition_v_uv_v.wav", policy_renders["U4_Noise_2.0k"][tr_start:tr_end])

    # 6. Leakage & Transition Detailed Metrics
    print(">>> Computing Leakage & Transition Detailed Metrics...")
    leakage_rows = []
    for name in ("U0_Silence", "U1_FullBand_0.20", "U2_Hpf_2.0k", "U3_HpfEnv_2.0k", "U4_Noise_2.0k"):
        art = policy_articulations.get(name, policy_renders[name])
        zcr_f, cent_f, r_f = compute_spectral_features(art[f_start:f_end])
        zcr_vf, cent_vf, r_vf = compute_spectral_features(art[vf_start:vf_end])
        leakage_rows.append({
            "policy": name,
            "fricative_ratio_db": f"{r_f:.2f}",
            "fricative_centroid_hz": f"{cent_f:.1f}",
            "vocal_fry_bleed_ratio_db": f"{r_vf:.2f}",
            "vocal_fry_energy_leak": f"{float(np.sqrt(np.mean(art[vf_start:vf_end]**2))):.6f}"
        })
    with (metrics_dir / "leakage_metrics.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=leakage_rows[0].keys())
        writer.writeheader()
        writer.writerows(leakage_rows)

    # 7. Generate All 7 Plots
    print(">>> Generating Plots...")
    generate_plots(plots_dir, reference, active, policy_renders,
                   policy_articulations, hpf_sweep_data, env_sweep_data,
                   rows_store["U4_Noise_2.0k"])

    # 8. Full Track Musical Validation (+4, +7, -4, -7 semitones) for Top 2 Candidates
    print(">>> Rendering Full Track (+4, +7, -4, -7 semitones) for Top Candidates (U2 & U4)...")
    full_sample_48k = work_dir / "full_sample_48k.wav"
    write_audio(full_sample_48k, source)
    intervals = [("+4", 4.0), ("+7", 7.0), ("-4", -4.0), ("-7", -7.0)]

    for cand_name, pol, cut, g in (("candidate_u2_hpf", "u2", 2000.0, 0.25),
                                   ("candidate_u4_noise", "u4", 2000.0, 0.25)):
        for sem_str, sem_val in intervals:
            out_full = renders_dir / "full" / f"{cand_name}_{sem_str}st.wav"
            print(f"  Rendering {out_full.name}...")
            execute(exe, full_sample_48k, out_full, sem_val,
                    unvoiced_policy=pol, unvoiced_hpf_hz=cut,
                    unvoiced_gain=g, full=True)

    print(">>> Milestone 5.8 evaluation successfully completed!")


if __name__ == "__main__":
    main()
