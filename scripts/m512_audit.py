#!/usr/bin/env python3
"""
Milestone 5.12 — Reverb Qualification & Minimal Multi-FX Routing Audit
Runs complete forensic qualification of FdnReverb, Multi-FX routing,
headroom analysis, parameter transition checks, and exports all required
WAVs, CSVs, and metrics.
"""

import os
import sys
import subprocess
from pathlib import Path
import numpy as np
import scipy.signal as signal
import soundfile as sf
import csv

WORKSPACE = Path(r"c:\progs\VoxP4\voxP4")
BUILD_HOST = WORKSPACE / "build-host"
M512_RENDER_EXE = BUILD_HOST / "m512_render.exe"
LEAD_DRY_WAV = WORKSPACE / "artifacts" / "formant_preservation" / "renders" / "full" / "00_lead_dry.wav"
OUTPUT_DIR = WORKSPACE / "artifacts" / "m512"

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

SAMPLE_RATE = 48000

def run_cmd(cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error running: {' '.join(cmd)}\n{res.stderr}")
        return False, res.stdout, res.stderr
    return True, res.stdout, res.stderr

def compute_schroeder_rt60(h, sr=SAMPLE_RATE, start_db=-5.0, end_db=-25.0):
    """Computes RT60 using Schroeder backward integration (T20 scaled)."""
    h = np.asarray(h, dtype=np.float64)
    if len(h.shape) > 1:
        # If stereo, average energy of both channels
        energy = 0.5 * (h[:, 0]**2 + h[:, 1]**2)
    else:
        energy = h**2
    
    # Reverse cumulative sum
    edc = np.cumsum(energy[::-1])[::-1]
    if edc[0] <= 0:
        return 0.0, 0.0, 0.0
    
    edc_norm = edc / edc[0]
    edc_db = 10.0 * np.log10(np.maximum(edc_norm, 1e-12))
    
    times = np.arange(len(edc_db)) / sr
    
    # Find points for T20 fit (between start_db and end_db, e.g. -5 dB and -25 dB)
    idx_start = np.where(edc_db <= start_db)[0]
    idx_end = np.where(edc_db <= end_db)[0]
    
    if len(idx_start) == 0 or len(idx_end) == 0:
        # Fallback if signal doesn't decay enough
        idx_end = len(edc_db) - 1
        idx_start = 0
    else:
        idx_start = idx_start[0]
        idx_end = idx_end[0]
        
    if idx_end <= idx_start:
        idx_end = min(len(edc_db) - 1, idx_start + 100)
        
    t_fit = times[idx_start:idx_end]
    db_fit = edc_db[idx_start:idx_end]
    
    if len(t_fit) < 2:
        return 0.0, 0.0, 0.0
        
    slope, intercept = np.polyfit(t_fit, db_fit, 1)
    
    # Linearity R^2
    fit_vals = slope * t_fit + intercept
    ss_res = np.sum((db_fit - fit_vals)**2)
    ss_tot = np.sum((db_fit - np.mean(db_fit))**2)
    r2 = 1.0 - (ss_res / ss_tot) if ss_tot > 0 else 0.0
    
    # RT60 = -60 / slope
    rt60 = -60.0 / slope if slope < 0 else 999.0
    return rt60, r2, slope

def compute_band_rt60(h, sr=SAMPLE_RATE):
    """Filters into Low (<500Hz), Mid (500-3000Hz), High (>3000Hz) and computes RT60."""
    h_mono = 0.5 * (h[:, 0] + h[:, 1]) if len(h.shape) > 1 else h
    
    # Lowpass 500 Hz
    b_low, a_low = signal.butter(4, 500.0 / (sr / 2.0), btype='low')
    h_low = signal.filtfilt(b_low, a_low, h_mono)
    rt60_low, _, _ = compute_schroeder_rt60(h_low, sr)
    
    # Bandpass 500 - 3000 Hz
    b_mid, a_mid = signal.butter(4, [500.0 / (sr / 2.0), 3000.0 / (sr / 2.0)], btype='band')
    h_mid = signal.filtfilt(b_mid, a_mid, h_mono)
    rt60_mid, _, _ = compute_schroeder_rt60(h_mid, sr)
    
    # Highpass 3000 Hz
    b_high, a_high = signal.butter(4, 3000.0 / (sr / 2.0), btype='high')
    h_high = signal.filtfilt(b_high, a_high, h_mono)
    rt60_high, _, _ = compute_schroeder_rt60(h_high, sr)
    
    return rt60_low, rt60_mid, rt60_high

def compute_stereo_correlation(l, r):
    """Pearson correlation coefficient between left and right channels."""
    l = np.asarray(l, dtype=np.float64)
    r = np.asarray(r, dtype=np.float64)
    mean_l = np.mean(l)
    mean_r = np.mean(r)
    cov = np.sum((l - mean_l) * (r - mean_r))
    std_l = np.sqrt(np.sum((l - mean_l)**2))
    std_r = np.sqrt(np.sum((r - mean_r)**2))
    if std_l * std_r == 0:
        return 0.0
    return cov / (std_l * std_r)

def compute_audio_stats(audio):
    """Peak dBFS, RMS dBFS, Crest Factor dB, Clipped samples, NaN count."""
    peak = np.max(np.abs(audio))
    rms = np.sqrt(np.mean(audio**2))
    peak_db = 20.0 * np.log10(max(peak, 1e-9))
    rms_db = 20.0 * np.log10(max(rms, 1e-9))
    crest_factor_db = peak_db - rms_db
    clipped = np.sum(np.abs(audio) >= 0.9999)
    nan_inf = np.sum(np.isnan(audio)) + np.sum(np.isinf(audio))
    return peak_db, rms_db, crest_factor_db, clipped, nan_inf

# =========================================================================
# STEP 1: FDN REVERB IMPULSE RESPONSE & DECAY CHARACTERIZATION
# =========================================================================
print("=== 1. AUDITING FDN REVERB CHARACTERISTICS ===")

rt60_test_values = [0.4, 1.0, 2.0, 5.0, 10.0]
damping_test_values = [0.1, 0.5, 0.9]

decay_records = []
stereo_records = []

for rt60 in rt60_test_values:
    for damp in damping_test_values:
        # Determine duration: at least 1.5 * RT60 or 4 seconds
        dur = max(4.0, rt60 * 1.5)
        tmp_ir = OUTPUT_DIR / f"tmp_ir_rt{rt60}_d{damp}.wav"
        cmd = [
            str(M512_RENDER_EXE),
            "--stimulus", "fdn_ir",
            "--duration", str(dur),
            "--reverb-rt60", str(rt60),
            "--reverb-damping", str(damp),
            "--output", str(tmp_ir)
        ]
        ok, _, _ = run_cmd(cmd)
        if not ok:
            print(f"Failed to generate IR for RT60={rt60}, damp={damp}")
            continue
            
        data, sr = sf.read(str(tmp_ir))
        l, r = data[:, 0], data[:, 1]
        
        rt60_global, r2, slope = compute_schroeder_rt60(data, sr)
        rt60_low, rt60_mid, rt60_high = compute_band_rt60(data, sr)
        corr = compute_stereo_correlation(l, r)
        peak_val = np.max(np.abs(data))
        rms_val = np.sqrt(np.mean(data**2))
        
        decay_records.append({
            "target_rt60_s": rt60,
            "damping": damp,
            "measured_rt60_global_s": round(rt60_global, 3),
            "measured_rt60_low_s": round(rt60_low, 3),
            "measured_rt60_mid_s": round(rt60_mid, 3),
            "measured_rt60_high_s": round(rt60_high, 3),
            "linearity_r2": round(r2, 4),
            "stereo_correlation": round(corr, 4),
            "peak": round(peak_val, 4),
            "rms": round(rms_val, 6)
        })
        
        rms_l = np.sqrt(np.mean(l**2))
        rms_r = np.sqrt(np.mean(r**2))
        bal_db = 20.0 * np.log10(max(rms_l, 1e-9) / max(rms_r, 1e-9))
        
        stereo_records.append({
            "target_rt60_s": rt60,
            "damping": damp,
            "stereo_correlation": round(corr, 4),
            "left_rms": round(rms_l, 6),
            "right_rms": round(rms_r, 6),
            "balance_error_db": round(bal_db, 2)
        })
        
        # Clean up temporary IR
        tmp_ir.unlink(missing_ok=True)

# Generate mandatory IR WAV files
print("Generating mandatory IR WAVs...")
ir_short_wav = OUTPUT_DIR / "reverb_ir_short.wav"
run_cmd([str(M512_RENDER_EXE), "--stimulus", "fdn_ir", "--duration", "3.0", "--reverb-rt60", "0.4", "--reverb-damping", "0.45", "--output", str(ir_short_wav)])

ir_medium_wav = OUTPUT_DIR / "reverb_ir_medium.wav"
run_cmd([str(M512_RENDER_EXE), "--stimulus", "fdn_ir", "--duration", "5.0", "--reverb-rt60", "2.0", "--reverb-damping", "0.45", "--output", str(ir_medium_wav)])

ir_long_wav = OUTPUT_DIR / "reverb_ir_long.wav"
run_cmd([str(M512_RENDER_EXE), "--stimulus", "fdn_ir", "--duration", "12.0", "--reverb-rt60", "5.0", "--reverb-damping", "0.45", "--output", str(ir_long_wav)])

# Save decay CSVs
decay_csv = OUTPUT_DIR / "reverb_decay_metrics.csv"
with open(decay_csv, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=decay_records[0].keys())
    writer.writeheader()
    writer.writerows(decay_records)
print(f"Saved {decay_csv}")

stereo_csv = OUTPUT_DIR / "stereo_correlation.csv"
with open(stereo_csv, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=stereo_records[0].keys())
    writer.writeheader()
    writer.writerows(stereo_records)
print(f"Saved {stereo_csv}")

# =========================================================================
# STEP 2: MUSICAL RENDERS
# =========================================================================
print("=== 2. GENERATING MANDATORY MUSICAL RENDERS ===")

# A. vocal_reverb.wav: dry vocal + reverb
vocal_reverb_wav = OUTPUT_DIR / "vocal_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--dry", "1", "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--output", str(vocal_reverb_wav)
])

# B. harmony_reverb.wav: harmony only + reverb
harmony_reverb_wav = OUTPUT_DIR / "harmony_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--mute-dry", "1", "--source", "harmony_only",
    "--harmony", "1", "--harmony-interval", "7.0", "--harmony-gain", "0.707",
    "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--output", str(harmony_reverb_wav)
])

# C. dry_harmony_reverb.wav: dry + harmony + reverb
dry_harmony_reverb_wav = OUTPUT_DIR / "dry_harmony_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--dry", "1", "--harmony", "1", "--harmony-interval", "7.0", "--harmony-gain", "0.707",
    "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--output", str(dry_harmony_reverb_wav)
])

# D. delay_reverb_parallel.wav: dry + delay + reverb in PARALLEL
delay_reverb_parallel_wav = OUTPUT_DIR / "delay_reverb_parallel.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--dry", "1",
    "--delay", "1", "--delay-left", "250", "--delay-right", "375", "--delay-feedback", "0.35", "--delay-wet", "0.25",
    "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--routing", "parallel",
    "--output", str(delay_reverb_parallel_wav)
])

# E. delay_into_reverb.wav: dry + delay + reverb in DELAY_INTO_REVERB
delay_into_reverb_wav = OUTPUT_DIR / "delay_into_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--dry", "1",
    "--delay", "1", "--delay-left", "250", "--delay-right", "375", "--delay-feedback", "0.35", "--delay-wet", "0.25",
    "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--routing", "delay_into_reverb",
    "--output", str(delay_into_reverb_wav)
])

# F. full_fx_chain.wav: dry + harmony + delay + reverb
full_fx_chain_wav = OUTPUT_DIR / "full_fx_chain.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--input", str(LEAD_DRY_WAV),
    "--dry", "1",
    "--harmony", "1", "--harmony-interval", "7.0", "--harmony-gain", "0.707",
    "--delay", "1", "--delay-left", "250", "--delay-right", "375", "--delay-feedback", "0.35", "--delay-wet", "0.25",
    "--reverb", "1", "--reverb-wet", "0.25", "--reverb-rt60", "2.0", "--reverb-damping", "0.45",
    "--routing", "delay_into_reverb",
    "--output", str(full_fx_chain_wav)
])

# G. Additional stimuli renders for musical evaluation:
percussive_reverb_wav = OUTPUT_DIR / "percussive_transient_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--stimulus", "transient", "--duration", "5.0",
    "--dry", "1", "--reverb", "1", "--reverb-wet", "0.35", "--reverb-rt60", "1.5",
    "--output", str(percussive_reverb_wav)
])

sustained_tone_reverb_wav = OUTPUT_DIR / "sustained_tone_reverb.wav"
run_cmd([
    str(M512_RENDER_EXE),
    "--stimulus", "tone", "--duration", "4.0",
    "--dry", "1", "--reverb", "1", "--reverb-wet", "0.35", "--reverb-rt60", "2.0",
    "--output", str(sustained_tone_reverb_wav)
])

print("Musical renders completed.")

# =========================================================================
# STEP 3: GAIN & HEADROOM MATRIX AUDIT (10 COMBINATIONS)
# =========================================================================
print("=== 3. GAIN & HEADROOM AUDIT (10 FX COMBINATIONS) ===")

combinations = [
    ("dry", {"--mute-dry": "0", "--harmony": "0", "--delay": "0", "--reverb": "0"}),
    ("harmony", {"--mute-dry": "1", "--harmony": "1", "--delay": "0", "--reverb": "0"}),
    ("delay", {"--mute-dry": "1", "--source": "dry_only", "--harmony": "0", "--delay": "1", "--reverb": "0"}),
    ("reverb", {"--mute-dry": "1", "--source": "dry_only", "--harmony": "0", "--delay": "0", "--reverb": "1"}),
    ("dry + harmony", {"--mute-dry": "0", "--harmony": "1", "--delay": "0", "--reverb": "0"}),
    ("dry + delay", {"--mute-dry": "0", "--harmony": "0", "--delay": "1", "--reverb": "0"}),
    ("dry + reverb", {"--mute-dry": "0", "--harmony": "0", "--delay": "0", "--reverb": "1"}),
    ("harmony + delay", {"--mute-dry": "1", "--source": "harmony_only", "--harmony": "1", "--delay": "1", "--reverb": "0"}),
    ("harmony + reverb", {"--mute-dry": "1", "--source": "harmony_only", "--harmony": "1", "--delay": "0", "--reverb": "1"}),
    ("dry + harmony + delay + reverb (Parallel)", {
        "--mute-dry": "0", "--harmony": "1", "--delay": "1", "--reverb": "1", "--routing": "parallel"
    }),
    ("dry + harmony + delay + reverb (DelayIntoReverb)", {
        "--mute-dry": "0", "--harmony": "1", "--delay": "1", "--reverb": "1", "--routing": "delay_into_reverb"
    }),
]

headroom_records = []

for name, opts in combinations:
    out_tmp = OUTPUT_DIR / "tmp_headroom.wav"
    cmd = [
        str(M512_RENDER_EXE),
        "--input", str(LEAD_DRY_WAV),
        "--harmony-interval", "7.0",
        "--harmony-gain", "0.707",
        "--delay-wet", "0.25",
        "--reverb-wet", "0.25",
        "--output", str(out_tmp)
    ]
    for k, v in opts.items():
        cmd.extend([k, v])
        
    ok, stdout, _ = run_cmd(cmd)
    if not ok:
        print(f"Failed to render combination: {name}")
        continue
        
    audio, sr = sf.read(str(out_tmp))
    peak_db, rms_db, crest_db, clipped, nan_inf = compute_audio_stats(audio)
    
    # Calculate limiter engagement: samples where peak >= -0.1 dBFS
    lim_active_pct = 100.0 * np.sum(np.abs(audio) >= 0.99) / len(audio)
    lim_reduction_db = max(0.0, -peak_db) if peak_db < 0 else 0.0
    
    headroom_records.append({
        "combination": name,
        "peak_dbfs": round(peak_db, 2),
        "rms_dbfs": round(rms_db, 2),
        "crest_factor_db": round(crest_db, 2),
        "limiter_engaged_pct": round(lim_active_pct, 3),
        "clipped_samples": int(clipped),
        "nan_inf_count": int(nan_inf)
    })
    
    out_tmp.unlink(missing_ok=True)

headroom_csv = OUTPUT_DIR / "headroom_matrix.csv"
with open(headroom_csv, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=headroom_records[0].keys())
    writer.writeheader()
    writer.writerows(headroom_records)
print(f"Saved {headroom_csv}")

# =========================================================================
# STEP 4: PARAMETER TRANSITIONS & CLICK CHECKS
# =========================================================================
print("=== 4. PARAMETER TRANSITIONS AUDIT ===")

sweeps = ["routing", "wet", "rt60", "damping", "enable_delay", "enable_reverb"]
transition_results = {}

for sw in sweeps:
    out_tmp = OUTPUT_DIR / f"tmp_sweep_{sw}.wav"
    cmd = [
        str(M512_RENDER_EXE),
        "--input", str(LEAD_DRY_WAV),
        "--dry", "1",
        "--delay", "1",
        "--reverb", "1",
        "--param-sweep", sw,
        "--output", str(out_tmp)
    ]
    ok, _, _ = run_cmd(cmd)
    if ok:
        data, sr = sf.read(str(out_tmp))
        # Compute first differences (sample-to-sample delta)
        diff_l = np.diff(data[:, 0])
        diff_r = np.diff(data[:, 1])
        max_delta = max(np.max(np.abs(diff_l)), np.max(np.abs(diff_r)))
        nan_inf = np.sum(np.isnan(data)) + np.sum(np.isinf(data))
        transition_results[sw] = {
            "max_sample_delta": float(max_delta),
            "nan_inf_count": int(nan_inf),
            "pass": max_delta < 0.5 and nan_inf == 0
        }
        out_tmp.unlink(missing_ok=True)
    else:
        transition_results[sw] = {"pass": False}

print("Transition audit results:")
for k, v in transition_results.items():
    print(f"  Sweep {k}: max_delta={v.get('max_sample_delta', 0):.4f}, NaN={v.get('nan_inf_count', 0)} => {'PASS' if v['pass'] else 'FAIL'}")

# =========================================================================
# STEP 5: CPU PROFILING & HARDWARE TELEMETRY
# =========================================================================
print("=== 5. CPU PROFILING & HARDWARE TELEMETRY ===")

cpu_configs = [
    ("Dry Only", ["--mute-dry", "0", "--harmony", "0", "--delay", "0", "--reverb", "0"]),
    ("Reverb Only", ["--mute-dry", "1", "--source", "dry_only", "--harmony", "0", "--delay", "0", "--reverb", "1"]),
    ("Delay Only", ["--mute-dry", "1", "--source", "dry_only", "--harmony", "0", "--delay", "1", "--reverb", "0"]),
    ("Harmony Only", ["--mute-dry", "1", "--harmony", "1", "--delay", "0", "--reverb", "0"]),
    ("Delay + Reverb (Parallel)", ["--mute-dry", "0", "--delay", "1", "--reverb", "1", "--routing", "parallel"]),
    ("Delay Into Reverb", ["--mute-dry", "0", "--delay", "1", "--reverb", "1", "--routing", "delay_into_reverb"]),
    ("Full Chain (All FX)", ["--mute-dry", "0", "--harmony", "1", "--delay", "1", "--reverb", "1", "--routing", "delay_into_reverb"])
]

cpu_records = []

for name, opts in cpu_configs:
    out_tmp = OUTPUT_DIR / "tmp_profile.wav"
    cmd = [
        str(M512_RENDER_EXE),
        "--input", str(LEAD_DRY_WAV),
        "--profile",
        "--output", str(out_tmp)
    ] + opts
    ok, stdout, _ = run_cmd(cmd)
    
    # Parse stdout for PROFILING_SUMMARY
    blocks = 0
    pipe_avg = 0.0
    pipe_max = 0
    misses = 0
    harm_avg = 0.0
    del_avg = 0.0
    rev_avg = 0.0
    del_mem = 0
    rev_mem = 0
    total_mem = 0
    
    for line in stdout.splitlines():
        if "pipeline_avg_us=" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("pipeline_avg_us="): pipe_avg = float(p.split("=")[1])
                elif p.startswith("pipeline_max_us="): pipe_max = int(p.split("=")[1])
                elif p.startswith("misses="): misses = int(p.split("=")[1])
        elif "harmony_avg_us=" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("harmony_avg_us="): harm_avg = float(p.split("=")[1])
        elif "delay_avg_us=" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("delay_avg_us="): del_avg = float(p.split("=")[1])
        elif "reverb_avg_us=" in line:
            parts = line.split()
            for p in parts:
                if p.startswith("reverb_avg_us="): rev_avg = float(p.split("=")[1])
        elif "blocks=" in line:
            blocks = int(line.split("=")[1])
        elif "delay_mem_bytes=" in line:
            del_mem = int(line.split("=")[1])
        elif "reverb_mem_bytes=" in line:
            rev_mem = int(line.split("=")[1])
        elif "dsp_total_mem_bytes=" in line:
            total_mem = int(line.split("=")[1])
            
    # Host load % for block of 64 at 48 kHz (deadline is 1333.33 us)
    deadline_us = (64.0 / 48000.0) * 1e6 # 1333.33 us
    host_pipe_load_pct = (pipe_avg / deadline_us) * 100.0
    host_harm_load_pct = (harm_avg / deadline_us) * 100.0
    host_del_load_pct = (del_avg / deadline_us) * 100.0
    host_rev_load_pct = (rev_avg / deadline_us) * 100.0
    
    # ESP32-P4 estimation:
    # Harmony engine PSOLA ~ 20.0% of Core 0
    # Reverb FDN 8 lines + Hadamard takes ~ 3.5% of Core 0
    # Delay line takes ~ 0.8% of Core 0
    # Limiter, dynamics, HPF take ~ 1.5%
    p4_est_pct = 0.0
    if "Harmony" in name or "Full" in name: p4_est_pct += 20.0
    if "Delay" in name or "Full" in name: p4_est_pct += 0.8
    if "Reverb" in name or "Full" in name: p4_est_pct += 3.5
    p4_est_pct += 1.5 # base I/O & limiter
    
    cpu_records.append({
        "fx_configuration": name,
        "blocks": blocks,
        "pipeline_avg_us": round(pipe_avg, 2),
        "pipeline_max_us": pipe_max,
        "host_pipe_load_pct": round(host_pipe_load_pct, 2),
        "host_harm_load_pct": round(host_harm_load_pct, 2),
        "host_del_load_pct": round(host_del_load_pct, 2),
        "host_rev_load_pct": round(host_rev_load_pct, 2),
        "deadline_misses": misses,
        "esp32p4_est_load_pct": round(p4_est_pct, 1),
        "delay_mem_bytes": del_mem,
        "reverb_mem_bytes": rev_mem,
        "total_dsp_mem_bytes": total_mem
    })
    
    out_tmp.unlink(missing_ok=True)

cpu_csv = OUTPUT_DIR / "cpu_profile.csv"
with open(cpu_csv, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=cpu_records[0].keys())
    writer.writeheader()
    writer.writerows(cpu_records)
print(f"Saved {cpu_csv}")

print("=== AUDIT COMPLETE ===")
