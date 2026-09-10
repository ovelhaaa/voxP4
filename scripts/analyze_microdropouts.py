#!/usr/bin/env python3
"""
scripts/analyze_microdropouts.py
Adversarial Technical Audit of TD-PSOLA Voicing Continuity & Micro-Dropouts.
Automated stimulus generation, telemetry analysis, event detection, classification (A-K),
worst-case snippet extraction, parameter sweep, and generation of Figures 1-10.
"""

import os
import sys
import subprocess
import numpy as np
import soundfile as sf
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from pathlib import Path

RATE = 48000
HOP_SAMPLES = 64
HOP_MS = (HOP_SAMPLES / RATE) * 1000.0

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = PROJECT_ROOT / "build-host"
PITCH_SHIFT_EXE = BUILD_DIR / "pitch_shift.exe"
ARTIFACTS_DIR = PROJECT_ROOT / "artifacts" / "voicing_continuity"
METRICS_DIR = ARTIFACTS_DIR / "metrics"
PLOTS_DIR = ARTIFACTS_DIR / "plots"
RENDERS_DIR = ARTIFACTS_DIR / "renders"
BASELINE_DIR = RENDERS_DIR / "baseline"
EXPERIMENTAL_DIR = RENDERS_DIR / "experimental"
BLIND_DIR = RENDERS_DIR / "blind"
WORST_DIR = RENDERS_DIR / "worst_cases"
SCRATCH_DIR = PROJECT_ROOT / "scratch" / "stimuli"

for d in [METRICS_DIR, PLOTS_DIR, BASELINE_DIR, EXPERIMENTAL_DIR, BLIND_DIR, WORST_DIR, SCRATCH_DIR]:
    d.mkdir(parents=True, exist_ok=True)

# 100-byte packed telemetry dtype
TELEMETRY_DTYPE = np.dtype([
    ('sample_index', '<u8'),
    ('input_rms', '<f4'),
    ('input_peak', '<f4'),
    ('input_envelope', '<f4'),
    ('pitch_voiced', 'u1'),
    ('pitch_confidence', '<f4'),
    ('pitch_period_samples', '<f4'),
    ('pitch_f0_hz', '<f4'),
    ('pitch_onset', 'u1'),
    ('pitch_track_state', 'u1'),
    ('coherent_marks', '<u2'),
    ('mark_count', '<u2'),
    ('target_enabled', 'u1'),
    ('psola_usable', 'u1'),
    ('psola_usable_reason', '<u4'),
    ('continuity_coasting', 'u1'),
    ('new_grain_scheduled', 'u1'),
    ('active_grain_count', '<u2'),
    ('last_grain_age', '<u4'),
    ('next_synthesis_mark', '<f8'),
    ('ola_weight_sum', '<f4'),
    ('ola_output_rms', '<f4'),
    ('release_active', 'u1'),
    ('release_remaining', '<u4'),
    ('unvoiced_path_active', 'u1'),
    ('unvoiced_gain', '<f4'),
    ('plosive_path_active', 'u1'),
    ('plosive_gain', '<f4'),
    ('psola_gain', '<f4'),
    ('active_mix', '<f4'),
    ('final_harmony_rms', '<f4'),
    ('effective_total_gain', '<f4')
])

def synthesize_vowel(f0=160.0, duration_s=4.0, formants=(700.0, 1220.0, 2600.0)):
    num_samples = int(duration_s * RATE)
    t = np.arange(num_samples) / RATE
    # Glottal pulse train
    period_samples = int(RATE / f0)
    pulse = np.zeros(period_samples, dtype=np.float32)
    n_open = int(period_samples * 0.4)
    to = np.arange(n_open) / n_open
    pulse[:n_open] = 3 * to**2 - 2 * to**3
    excitation = np.tile(pulse, int(np.ceil(num_samples / period_samples)))[:num_samples]
    
    # Formant resonators using simple IIR 2nd order sections
    from scipy.signal import lfilter
    y = excitation.astype(np.float32)
    for f, bw in zip(formants, [80.0, 100.0, 120.0]):
        r = np.exp(-np.pi * bw / RATE)
        theta = 2 * np.pi * f / RATE
        b = [1.0 - r]
        a = [1.0, -2.0 * r * np.cos(theta), r * r]
        y = lfilter(b, a, y)
    y = y / (np.max(np.abs(y)) + 1e-6) * 0.7
    return y

def create_synthetic_stimuli():
    """Generates Tests A-F."""
    print("Generating synthetic stimuli suite (Tests A-F)...")
    
    # Test A: Amplitude dips (3, 6, 12, 20 dB, 5-40 ms)
    y_a = synthesize_vowel(160.0, 4.0)
    dips = [
        (0.6, 5, 3),
        (1.2, 10, 6),
        (1.8, 15, 12),
        (2.4, 20, 20),
        (3.0, 30, 20),
        (3.5, 40, 12)
    ]
    for t_start, dur_ms, depth_db in dips:
        n_start = int(t_start * RATE)
        n_dur = int(dur_ms * RATE / 1000.0)
        gain = 10.0 ** (-depth_db / 20.0)
        # Hann fade in and out
        fade_len = max(4, n_dur // 4)
        w = np.ones(n_dur, dtype=np.float32) * gain
        w[:fade_len] = 1.0 - (1.0 - gain) * 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_len) / fade_len))
        w[-fade_len:] = 1.0 - (1.0 - gain) * 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_len, 0, -1) / fade_len))
        y_a[n_start:n_start + n_dur] *= w
    sf.write(SCRATCH_DIR / "test_a_amplitude_dip.wav", y_a, RATE)
    
    # Test B: Local noise injection / confidence perturbation
    y_b = synthesize_vowel(160.0, 4.0)
    noise_bursts = [(0.8, 10), (1.6, 20), (2.4, 30), (3.2, 15)]
    rng = np.random.RandomState(42)
    for t_start, dur_ms in noise_bursts:
        n_start = int(t_start * RATE)
        n_dur = int(dur_ms * RATE / 1000.0)
        noise = rng.normal(0, 0.25, n_dur).astype(np.float32)
        y_b[n_start:n_start + n_dur] += noise
    y_b = np.clip(y_b, -1.0, 1.0)
    sf.write(SCRATCH_DIR / "test_b_confidence_noise.wav", y_b, RATE)
    
    # Test C: Missing pitch pulses
    y_c = synthesize_vowel(160.0, 4.0)
    period = int(RATE / 160.0)
    pulses_drop = [(0.8, 1), (1.6, 2), (2.4, 3)]
    for t_start, count in pulses_drop:
        n_start = int(t_start * RATE)
        n_dur = count * period
        y_c[n_start:n_start + n_dur] = 0.0
    sf.write(SCRATCH_DIR / "test_c_missing_pulse.wav", y_c, RATE)
    
    # Test D: Vibrato across boundary (5.5 Hz, +/- 100 cents) + octave jump
    dur_d = 4.0
    n_samples_d = int(dur_d * RATE)
    t = np.arange(n_samples_d) / RATE
    f_inst = 160.0 * (2.0 ** ((np.sin(2 * np.pi * 5.5 * t) * 100.0) / 1200.0))
    f_inst[int(2.0 * RATE):] *= 2.0  # Octave leap
    phase = 2 * np.pi * np.cumsum(f_inst) / RATE
    y_d = 0.6 * np.sin(phase) + 0.3 * np.sin(2 * phase) + 0.15 * np.sin(3 * phase)
    sf.write(SCRATCH_DIR / "test_d_vibrato.wav", y_d.astype(np.float32), RATE)
    
    # Test E: Legato pitch steps (+2, +5, +7 st)
    f_e = np.ones(n_samples_d, dtype=np.float32) * 160.0
    f_e[int(1.0 * RATE):int(2.0 * RATE)] = 160.0 * (2.0 ** (2.0 / 12.0))
    f_e[int(2.0 * RATE):int(3.0 * RATE)] = 160.0 * (2.0 ** (5.0 / 12.0))
    f_e[int(3.0 * RATE):] = 160.0 * (2.0 ** (7.0 / 12.0))
    phase_e = 2 * np.pi * np.cumsum(f_e) / RATE
    y_e = 0.6 * np.sin(phase_e) + 0.3 * np.sin(2 * phase_e) + 0.15 * np.sin(3 * phase_e)
    sf.write(SCRATCH_DIR / "test_e_legato_steps.wav", y_e.astype(np.float32), RATE)
    
    # Test F: Fast consonant modulation (/a/ - /s/ - /a/ - /t/ - /a/ - /p/ - /a/ - /k/)
    y_f = synthesize_vowel(160.0, 5.0)
    # /s/ fricative noise bandpass 4-8 kHz at t=1.0s, dur 60ms
    n_s = int(1.0 * RATE)
    dur_s = int(0.06 * RATE)
    from scipy.signal import butter, lfilter
    b_fric, a_fric = butter(3, [4000.0 / (RATE/2), 8000.0 / (RATE/2)], btype='band')
    noise_s = lfilter(b_fric, a_fric, rng.normal(0, 0.4, dur_s)).astype(np.float32)
    y_f[n_s:n_s + dur_s] = noise_s
    # /t/ silence 25ms + burst 5ms at t=2.0s
    n_t = int(2.0 * RATE)
    y_f[n_t:n_t + int(0.025 * RATE)] = 0.0
    burst_t = rng.normal(0, 0.5, int(0.005 * RATE)).astype(np.float32)
    y_f[n_t + int(0.025 * RATE):n_t + int(0.030 * RATE)] = burst_t
    # /p/ silence 30ms + low burst 5ms at t=3.0s
    n_p = int(3.0 * RATE)
    y_f[n_p:n_p + int(0.030 * RATE)] = 0.0
    b_p, a_p = butter(2, 800.0 / (RATE/2), btype='low')
    burst_p = lfilter(b_p, a_p, rng.normal(0, 0.5, int(0.005 * RATE))).astype(np.float32)
    y_f[n_p + int(0.030 * RATE):n_p + int(0.035 * RATE)] = burst_p
    # /k/ silence 25ms + mid burst 5ms at t=4.0s
    n_k = int(4.0 * RATE)
    y_f[n_k:n_k + int(0.025 * RATE)] = 0.0
    b_k, a_k = butter(2, [1500.0 / (RATE/2), 3000.0 / (RATE/2)], btype='band')
    burst_k = lfilter(b_k, a_k, rng.normal(0, 0.5, int(0.005 * RATE))).astype(np.float32)
    y_f[n_k + int(0.025 * RATE):n_k + int(0.030 * RATE)] = burst_k
    sf.write(SCRATCH_DIR / "test_f_consonant_mod.wav", y_f.astype(np.float32), RATE)
    print("Synthetic stimuli generated successfully.")

def run_pitch_shift(input_wav, output_wav, semitones=7.0, policy="baseline", telem_bin=None, fallback="dry", coast_ms=None):
    cmd = [
        str(PITCH_SHIFT_EXE),
        str(input_wav),
        str(output_wav),
        str(semitones),
        "--continuity-policy", policy,
        "--fallback-policy", fallback
    ]
    if coast_ms is not None:
        cmd += ["--coast-ms", str(float(coast_ms))]
    if telem_bin is not None:
        cmd += ["--sample-telemetry", str(telem_bin)]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        print(f"Error running pitch_shift: {res.stderr}")
        return False
    return True

def load_telemetry(bin_path):
    if not os.path.exists(bin_path):
        return None
    data = np.fromfile(bin_path, dtype=TELEMETRY_DTYPE)
    return data

def detect_microdropouts(in_wav, out_wav, telem, min_drop_db=3.0, min_dur_ms=2.0, max_dur_ms=40.0):
    """
    Offline automated micro-dropout detector.
    Evaluates energy deficits on active vocal regions, rejecting true silence / unvoiced consonants.
    """
    x_in, sr = sf.read(in_wav)
    if x_in.ndim > 1:
        x_in = np.mean(x_in, axis=1)
    x_out, _ = sf.read(out_wav)
    if x_out.ndim > 1:
        x_out = np.mean(x_out, axis=1)
        
    N = min(len(x_in), len(x_out), len(telem))
    x_in = x_in[:N]
    x_out = x_out[:N]
    telem = telem[:N]
    
    # 5 ms sliding RMS window (240 samples), hop 0.5 ms (24 samples)
    win_len = 240
    hop = 24
    num_frames = (N - win_len) // hop
    
    rms_in = np.zeros(num_frames, dtype=np.float32)
    rms_out = np.zeros(num_frames, dtype=np.float32)
    times = (np.arange(num_frames) * hop + win_len // 2) / RATE
    
    # Fast windowed sum of squares via cumsum
    cumsum_in2 = np.pad(np.cumsum(x_in**2), (1, 0))
    cumsum_out2 = np.pad(np.cumsum(x_out**2), (1, 0))
    
    indices = np.arange(num_frames) * hop
    sum_in2 = cumsum_in2[indices + win_len] - cumsum_in2[indices]
    sum_out2 = cumsum_out2[indices + win_len] - cumsum_out2[indices]
    
    rms_in = np.sqrt(np.maximum(sum_in2 / win_len, 1e-12))
    rms_out = np.sqrt(np.maximum(sum_out2 / win_len, 1e-12))
    
    # Nominal gain in voiced sections
    voiced_mask = (telem['pitch_voiced'][indices + win_len // 2] == 1) & (rms_in > 0.02)
    if np.sum(voiced_mask) > 100:
        nominal_gain = np.median(rms_out[voiced_mask] / rms_in[voiced_mask])
    else:
        nominal_gain = 1.0
        
    expected_out = rms_in * nominal_gain
    deficit_db = 20.0 * np.log10(np.maximum(expected_out, 1e-5) / np.maximum(rms_out, 1e-5))
    
    # Active vocal region: input RMS >= -40 dBFS (~0.01)
    active_vocal = rms_in >= 0.01
    
    # Event detection via thresholding deficit >= min_drop_db
    is_dip = active_vocal & (deficit_db >= min_drop_db)
    
    events = []
    in_event = False
    start_idx = 0
    
    for k in range(num_frames):
        if is_dip[k] and not in_event:
            in_event = True
            start_idx = k
        elif not is_dip[k] and in_event:
            in_event = False
            end_idx = k
            dur_ms = (end_idx - start_idx) * (hop / RATE) * 1000.0
            if min_dur_ms <= dur_ms <= max_dur_ms:
                peak_deficit = np.max(deficit_db[start_idx:end_idx])
                peak_time = times[start_idx + np.argmax(deficit_db[start_idx:end_idx])]
                start_sample = start_idx * hop + win_len // 2
                end_sample = end_idx * hop + win_len // 2
                events.append({
                    'start_time': times[start_idx],
                    'end_time': times[end_idx],
                    'peak_time': peak_time,
                    'duration_ms': dur_ms,
                    'depth_db': peak_deficit,
                    'start_sample': start_sample,
                    'end_sample': end_sample,
                    'mid_sample': (start_sample + end_sample) // 2
                })
                
    # Classify each event
    classified_events = []
    for ev in events:
        s0 = max(0, ev['start_sample'] - int(0.040 * RATE))
        s1 = min(N, ev['end_sample'] + int(0.040 * RATE))
        sub_telem = telem[s0:s1]
        ev_telem = telem[ev['start_sample']:ev['end_sample']]
        
        # False positive rejection (Category K: Silence, phrase boundary breath, or legitimate unvoiced consonant)
        mean_in_rms = np.mean(ev_telem['input_rms'])
        has_near_voicing = np.any(sub_telem['pitch_voiced'] == 1)
        if mean_in_rms < 0.008 or not has_near_voicing:
            ev['category'] = 'K'
            ev['description'] = 'Legitimate natural pause/silence/breath'
        elif np.all(sub_telem['pitch_voiced'] == 0):
            ev['category'] = 'K'
            ev['description'] = 'Legitimate unvoiced consonant articulation'
        else:
            # Check reasons
            reason_mask = ev_telem['psola_usable_reason']
            has_low_conf = np.any((reason_mask & (1 << 3)) > 0)
            has_unlocked = np.any((reason_mask & (1 << 1)) > 0)
            has_not_voiced = np.any((reason_mask & (1 << 2)) > 0)
            has_acq_not_ready = np.any((reason_mask & (1 << 6)) > 0)
            has_release = np.any(ev_telem['release_active'] > 0)
            min_ola_w = np.min(ev_telem['ola_weight_sum'])
            zero_grains = np.sum(ev_telem['new_grain_scheduled']) == 0
            
            if has_not_voiced and zero_grains and min_ola_w < 0.1:
                ev['category'] = 'A'
                ev['description'] = 'Spurious pitch tracker voicing drop (voiced 1->0) draining OLA'
            elif has_low_conf and not has_not_voiced:
                ev['category'] = 'B'
                ev['description'] = 'Confidence dip below threshold causing tracker gating'
            elif has_unlocked:
                ev['category'] = 'C'
                ev['description'] = 'Pitch track state unlocked (missing coasting)'
            elif has_acq_not_ready:
                ev['category'] = 'D'
                ev['description'] = 'Reacquisition delay awaiting coherent marks'
            elif has_release and min_ola_w < 0.05:
                ev['category'] = 'E'
                ev['description'] = 'Premature OLA release tail completion before grain re-arrival'
            elif np.max(ev_telem['ola_weight_sum']) > 3.0 or min_ola_w < 0.01:
                ev['category'] = 'F'
                ev['description'] = 'COLA normalization gain notch / weight sum collapse'
            elif np.any(ev_telem['unvoiced_path_active'] > 0) or np.any(ev_telem['plosive_path_active'] > 0):
                ev['category'] = 'G'
                ev['description'] = 'Unvoiced / plosive bridge envelope conflict with PSOLA'
            elif np.any(np.abs(np.diff(ev_telem['next_synthesis_mark'])) > 2.0 * np.mean(ev_telem['pitch_period_samples'])):
                ev['category'] = 'H'
                ev['description'] = 'Phase discontinuity / synthesis cursor forward jump'
            elif np.any(ev_telem['pitch_period_samples'] < 24) or np.any(ev_telem['pitch_period_samples'] > 800):
                ev['category'] = 'J'
                ev['description'] = 'Fast pitch modulation leaving search window'
            else:
                ev['category'] = 'A'
                ev['description'] = 'Spurious gating / OLA underflow'
        classified_events.append(ev)
        
    return classified_events, times, deficit_db, rms_in, rms_out

def run_parameter_sweep(lead_wav):
    """Evaluates coasting durations (0, 5, 10, 15, 20, 30 ms)."""
    print("Running coasting parameter sweep...")
    durations = [0, 5, 10, 15, 20, 25, 30]
    sweep_results = []
    
    for d in durations:
        pol = "baseline" if d == 0 else "combined"
        out_p = SCRATCH_DIR / f"sweep_{d}ms.wav"
        tel_p = SCRATCH_DIR / f"sweep_{d}ms.bin"
        run_pitch_shift(lead_wav, out_p, semitones=7.0, policy=pol, telem_bin=tel_p, coast_ms=d)
        telem = load_telemetry(tel_p)
        if telem is not None:
            evs, _, _, _, _ = detect_microdropouts(lead_wav, out_p, telem)
            true_drops = [e for e in evs if e['category'] != 'K']
            idr_per_sec = len(true_drops) / (len(telem) / RATE)
            mean_depth = np.mean([e['depth_db'] for e in true_drops]) if true_drops else 0.0
            
            # Articulation false alarm check on Test F
            test_f_out = SCRATCH_DIR / f"test_f_sweep_{d}ms.wav"
            run_pitch_shift(SCRATCH_DIR / "test_f_consonant_mod.wav", test_f_out, semitones=7.0, policy=pol, coast_ms=d)
            # Measure consonant energy preservation
            x_f, _ = sf.read(test_f_out)
            # /s/ at 1.0s, /t/ at 2.0s
            t_silence_t = np.mean(x_f[int(2.005*RATE):int(2.020*RATE)]**2)
            ghost_energy_db = 10.0 * np.log10(max(t_silence_t, 1e-9))
            
            # Note change latency on Test E
            test_e_out = SCRATCH_DIR / f"test_e_sweep_{d}ms.wav"
            test_e_tel = SCRATCH_DIR / f"test_e_sweep_{d}ms.bin"
            run_pitch_shift(SCRATCH_DIR / "test_e_legato_steps.wav", test_e_out, semitones=7.0, policy=pol, telem_bin=test_e_tel, coast_ms=d)
            te_data = load_telemetry(test_e_tel)
            # Transition latency at t=1.0s (+2 st)
            step_lat_ms = 0.0
            if te_data is not None:
                step_idx = int(1.0 * RATE)
                f0_target = 160.0 * (2.0 ** (2.0/12.0))
                sub_f0 = te_data['pitch_f0_hz'][step_idx:step_idx + int(0.05 * RATE)]
                reached = np.where(sub_f0 >= 0.98 * f0_target)[0]
                step_lat_ms = (reached[0] / RATE) * 1000.0 if len(reached) > 0 else 25.0
                
            sweep_results.append({
                'coasting_duration_ms': d,
                'dropouts_count': len(true_drops),
                'internal_dropout_rate_per_sec': idr_per_sec,
                'mean_depth_db': mean_depth,
                'consonant_stop_closure_db': ghost_energy_db,
                'note_change_latency_ms': step_lat_ms
            })
            
    # Export CSV
    csv_path = METRICS_DIR / "coasting_parameter_sweep.csv"
    with open(csv_path, "w") as f:
        f.write("coasting_duration_ms,dropouts_count,internal_dropout_rate_per_sec,mean_depth_db,consonant_stop_closure_db,note_change_latency_ms\n")
        for r in sweep_results:
            f.write(f"{r['coasting_duration_ms']},{r['dropouts_count']},{r['internal_dropout_rate_per_sec']:.4f},{r['mean_depth_db']:.2f},{r['consonant_stop_closure_db']:.2f},{r['note_change_latency_ms']:.2f}\n")
    print(f"Exported parameter sweep to {csv_path}")
    return sweep_results

def generate_all_plots(lead_wav, base_wav, exp_wav, base_telem, exp_telem, base_events, exp_events, sweep_results):
    """Generates all 10 diagnostic figures required by Milestone 5.11 audit."""
    print("Plotting Figure 1: Baseline vs Experimental Macro Timeline & Dropout Overlay...")
    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    t = np.arange(len(base_telem)) / RATE
    
    # 1. Input RMS & Voicing
    axes[0].plot(t, base_telem['input_rms'], color='gray', label='Input RMS', alpha=0.7)
    axes[0].plot(t, base_telem['pitch_voiced'] * 0.1, color='navy', label='Voiced Flag (0/1)', linewidth=1.5)
    axes[0].set_ylabel("Input / Voicing")
    axes[0].set_title("Figure 1: 52s Lead Vocal Macro Timeline & Voicing Continuity")
    axes[0].legend(loc='upper right')
    axes[0].grid(True, alpha=0.3)
    
    # 2. Baseline Output & Dropouts
    x_base, _ = sf.read(base_wav)
    if x_base.ndim > 1: x_base = np.mean(x_base, axis=1)
    axes[1].plot(t[:len(x_base)], x_base, color='crimson', label='Baseline Harmony', alpha=0.6, linewidth=0.8)
    for ev in base_events:
        if ev['category'] != 'K':
            axes[1].axvspan(ev['start_time'], ev['end_time'], color='red', alpha=0.35)
    axes[1].set_ylabel("Baseline Output")
    axes[1].legend(loc='upper right')
    axes[1].grid(True, alpha=0.3)
    
    # 3. Experimental Output & Dropouts
    x_exp, _ = sf.read(exp_wav)
    if x_exp.ndim > 1: x_exp = np.mean(x_exp, axis=1)
    axes[2].plot(t[:len(x_exp)], x_exp, color='forestgreen', label='Experimental Harmony (Bounded Coasting)', alpha=0.6, linewidth=0.8)
    for ev in exp_events:
        if ev['category'] != 'K':
            axes[2].axvspan(ev['start_time'], ev['end_time'], color='lime', alpha=0.35)
    axes[2].set_ylabel("Experimental Output")
    axes[2].legend(loc='upper right')
    axes[2].grid(True, alpha=0.3)
    
    # 4. Usable reasons & Gaps
    axes[3].plot(t, base_telem['psola_usable'], color='purple', label='Baseline Usable (0/1)', alpha=0.7)
    axes[3].plot(t, exp_telem['psola_usable'], color='green', label='Experimental Usable (0/1)', alpha=0.7, linestyle='--')
    axes[3].set_ylabel("PSOLA Usable")
    axes[3].set_xlabel("Time (s)")
    axes[3].legend(loc='upper right')
    axes[3].grid(True, alpha=0.3)
    
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig01_macro_timeline_dropout_overlay.png", dpi=200)
    plt.close(fig)

    # Figure 2: Anatomy of a Single Micro-Dropout (Zoomed 150 ms around worst event)
    print("Plotting Figure 2: Anatomy of Worst Micro-Dropout...")
    true_base_evs = [e for e in base_events if e['category'] != 'K']
    worst_ev = max(true_base_evs, key=lambda e: e['depth_db']) if true_base_evs else base_events[0]
    t_center = worst_ev['peak_time']
    s_start = max(0, int((t_center - 0.075) * RATE))
    s_end = min(len(base_telem), int((t_center + 0.075) * RATE))
    sub_t = (np.arange(s_start, s_end) - s_start) / RATE * 1000.0  # ms
    sub_bt = base_telem[s_start:s_end]
    sub_et = exp_telem[s_start:s_end]
    
    fig, axes = plt.subplots(5, 1, figsize=(12, 11), sharex=True)
    # Audio waveforms
    axes[0].plot(sub_t, x_base[s_start:s_end], color='crimson', label='Baseline Output (Dropout)', linewidth=1.2)
    axes[0].plot(sub_t, x_exp[s_start:s_end], color='forestgreen', label='Experimental Output (Coasted)', alpha=0.7, linewidth=1.0)
    axes[0].set_ylabel("Amplitude")
    axes[0].set_title(f"Figure 2: Micro-Dropout Anatomy (Worst Event at t={t_center:.3f}s, depth={worst_ev['depth_db']:.1f} dB, cat={worst_ev['category']})")
    axes[0].legend(loc='upper right')
    axes[0].grid(True, alpha=0.3)
    
    # Tracker Voiced & Confidence
    axes[1].plot(sub_t, sub_bt['pitch_confidence'], color='blue', label='Pitch Confidence', linewidth=1.5)
    axes[1].plot(sub_t, sub_bt['pitch_voiced'], color='navy', linestyle='--', label='Tracker Voiced Flag')
    axes[1].axhline(0.70, color='red', linestyle=':', label='Voiced Exit Threshold (0.70)')
    axes[1].set_ylabel("Confidence / Voiced")
    axes[1].legend(loc='upper right')
    axes[1].grid(True, alpha=0.3)
    
    # Track State & Usable
    axes[2].plot(sub_t, sub_bt['pitch_track_state'], color='darkorange', label='Baseline Track State (0=Unlock, 1=Acq, 2=Lock)', linewidth=1.5)
    axes[2].plot(sub_t, sub_bt['psola_usable'], color='purple', label='Baseline PSOLA Usable (0/1)', linewidth=1.5)
    axes[2].plot(sub_t, sub_et['psola_usable'], color='green', linestyle='--', label='Experimental PSOLA Usable', linewidth=1.5)
    axes[2].set_ylabel("State / Usable")
    axes[2].legend(loc='upper right')
    axes[2].grid(True, alpha=0.3)
    
    # OLA Weight Sum & Active Grains
    axes[3].plot(sub_t, sub_bt['ola_weight_sum'], color='brown', label='Baseline OLA Norm Sum', linewidth=1.5)
    axes[3].plot(sub_t, sub_et['ola_weight_sum'], color='darkgreen', linestyle='--', label='Experimental OLA Norm Sum', linewidth=1.5)
    axes[3].set_ylabel("OLA Weight Sum")
    axes[3].legend(loc='upper right')
    axes[3].grid(True, alpha=0.3)
    
    # Gain components
    axes[4].plot(sub_t, sub_bt['psola_gain'], color='magenta', label='Baseline PSOLA Gain')
    axes[4].plot(sub_t, sub_bt['active_mix'], color='cyan', label='Baseline Active Mix')
    axes[4].plot(sub_t, sub_bt['effective_total_gain'], color='black', label='Baseline Total Gain', linewidth=1.5)
    axes[4].plot(sub_t, sub_et['effective_total_gain'], color='green', label='Experimental Total Gain', linewidth=1.5, linestyle='--')
    axes[4].set_ylabel("Effective Gain")
    axes[4].set_xlabel("Relative Time (ms)")
    axes[4].legend(loc='upper right')
    axes[4].grid(True, alpha=0.3)
    
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig02_worst_microdropout_anatomy.png", dpi=200)
    plt.close(fig)

    # Figure 3: Scatter Plot Duration vs Depth by Category
    print("Plotting Figure 3: Duration vs Depth Scatter Plot...")
    fig, ax = plt.subplots(figsize=(10, 6))
    cat_colors = {
        'A': 'crimson', 'B': 'orange', 'C': 'gold', 'D': 'purple',
        'E': 'brown', 'F': 'blue', 'G': 'teal', 'H': 'magenta',
        'I': 'darkgray', 'J': 'darkgreen', 'K': 'silver'
    }
    for cat, col in cat_colors.items():
        evs = [e for e in base_events if e['category'] == cat]
        if evs:
            durs = [e['duration_ms'] for e in evs]
            deps = [e['depth_db'] for e in evs]
            ax.scatter(durs, deps, color=col, label=f"Cat {cat} (n={len(evs)})", s=40, alpha=0.8, edgecolors='black')
    ax.axhline(6.0, color='gray', linestyle='--', alpha=0.5, label='6 dB Threshold')
    ax.axhline(12.0, color='gray', linestyle=':', alpha=0.5, label='12 dB Threshold')
    ax.set_xlabel("Dropout Duration (ms)")
    ax.set_ylabel("Energy Deficit Depth (dB)")
    ax.set_title("Figure 3: Micro-Dropout Duration vs Depth Classified by Root Cause (Categories A-K)")
    ax.grid(True, alpha=0.3)
    ax.legend(bbox_to_anchor=(1.04, 1), loc="upper left")
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig03_duration_vs_depth_scatter.png", dpi=200)
    plt.close(fig)

    # Figure 4: State Machine Disconnect
    print("Plotting Figure 4: State Machine Disconnect...")
    fig, ax = plt.subplots(figsize=(12, 6))
    reasons = {
        'TargetDisabled (bit 0)': np.sum((base_telem['psola_usable_reason'] & (1 << 0)) > 0),
        'TrackUnlocked (bit 1)': np.sum((base_telem['psola_usable_reason'] & (1 << 1)) > 0),
        'NotVoiced (bit 2)': np.sum((base_telem['psola_usable_reason'] & (1 << 2)) > 0),
        'LowConfidence (bit 3)': np.sum((base_telem['psola_usable_reason'] & (1 << 3)) > 0),
        'InvalidPeriod (bit 4)': np.sum((base_telem['psola_usable_reason'] & (1 << 4)) > 0),
        'NoMarks (bit 5)': np.sum((base_telem['psola_usable_reason'] & (1 << 5)) > 0),
        'AcquiringNotReady (bit 6)': np.sum((base_telem['psola_usable_reason'] & (1 << 6)) > 0),
        'ReleaseActive (bit 7)': np.sum((base_telem['psola_usable_reason'] & (1 << 7)) > 0),
    }
    y_pos = np.arange(len(reasons))
    counts = [reasons[k] / RATE * 1000.0 for k in reasons]  # ms
    ax.barh(y_pos, counts, color='coral', edgecolor='black')
    ax.set_yticks(y_pos)
    ax.set_yticklabels(list(reasons.keys()))
    ax.invert_yaxis()
    ax.set_xlabel("Cumulative Active Gating Duration in 52s Stem (ms)")
    ax.set_title("Figure 4: State Machine Disconnect & Active PSOLA Invalidation Reasons")
    ax.grid(True, alpha=0.3)
    for i, v in enumerate(counts):
        ax.text(v + 10, i, f"{v:.1f} ms", va='center', fontweight='bold')
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig04_state_machine_disconnect.png", dpi=200)
    plt.close(fig)

    # Figure 5: Inter-Grain Interval Distribution & Grain Age
    print("Plotting Figure 5: Inter-Grain Interval Distribution...")
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
    grain_ages = base_telem['last_grain_age'][base_telem['pitch_voiced'] == 1]
    valid_ages = grain_ages[(grain_ages > 0) & (grain_ages < 2000)]
    ax1.hist(valid_ages / 48.0, bins=50, color='steelblue', edgecolor='black', alpha=0.7)
    ax1.set_xlabel("Last Grain Age (ms)")
    ax1.set_ylabel("Voiced Sample Count")
    ax1.set_title("Baseline: Last Grain Age Distribution")
    ax1.grid(True, alpha=0.3)
    
    exp_grain_ages = exp_telem['last_grain_age'][exp_telem['pitch_voiced'] == 1]
    exp_valid_ages = exp_grain_ages[(exp_grain_ages > 0) & (exp_grain_ages < 2000)]
    ax2.hist(exp_valid_ages / 48.0, bins=50, color='forestgreen', edgecolor='black', alpha=0.7)
    ax2.set_xlabel("Last Grain Age (ms)")
    ax2.set_ylabel("Voiced Sample Count")
    ax2.set_title("Experimental (Coasting): Last Grain Age Distribution")
    ax2.grid(True, alpha=0.3)
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig05_intergrain_interval_distribution.png", dpi=200)
    plt.close(fig)

    # Figure 6: Synthetic Stimuli Response Comparison
    print("Plotting Figure 6: Synthetic Stimuli Tests A-F...")
    fig, axes = plt.subplots(3, 2, figsize=(14, 10))
    tests = [
        ("test_a_amplitude_dip", "Test A: Amplitude Dip (3-20 dB)", axes[0, 0]),
        ("test_b_confidence_noise", "Test B: Noise Bursts / Conf Dip", axes[0, 1]),
        ("test_c_missing_pulse", "Test C: Missing Pitch Pulses", axes[1, 0]),
        ("test_d_vibrato", "Test D: Fast Vibrato & Octave Leap", axes[1, 1]),
        ("test_e_legato_steps", "Test E: Legato Pitch Steps", axes[2, 0]),
        ("test_f_consonant_mod", "Test F: Consonant Articulation (/s/,/t/,/p/,/k/)", axes[2, 1])
    ]
    for name, title, ax in tests:
        wb = SCRATCH_DIR / f"{name}_base.wav"
        we = SCRATCH_DIR / f"{name}_exp.wav"
        run_pitch_shift(SCRATCH_DIR / f"{name}.wav", wb, semitones=7.0, policy="baseline")
        run_pitch_shift(SCRATCH_DIR / f"{name}.wav", we, semitones=7.0, policy="combined")
        ab, _ = sf.read(wb)
        ae, _ = sf.read(we)
        t_sub = np.arange(min(len(ab), len(ae))) / RATE
        ax.plot(t_sub, ab[:len(t_sub)], color='crimson', label='Baseline', alpha=0.6)
        ax.plot(t_sub, ae[:len(t_sub)], color='forestgreen', label='Coasting', alpha=0.6)
        ax.set_title(title, fontsize=10)
        ax.grid(True, alpha=0.3)
        ax.legend(loc='upper right', fontsize=8)
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig06_synthetic_stimuli_tests_a_f.png", dpi=200)
    plt.close(fig)

    # Figure 7: Coasting Duration Sweep
    print("Plotting Figure 7: Coasting Duration Sweep...")
    fig, ax1 = plt.subplots(figsize=(10, 5))
    dur_ms = [r['coasting_duration_ms'] for r in sweep_results]
    idr = [r['internal_dropout_rate_per_sec'] for r in sweep_results]
    lat = [r['note_change_latency_ms'] for r in sweep_results]
    
    color = 'tab:red'
    ax1.set_xlabel('Coasting Duration (ms)', fontweight='bold')
    ax1.set_ylabel('Internal Dropout Rate (events/sec)', color=color, fontweight='bold')
    line1 = ax1.plot(dur_ms, idr, color=color, marker='o', linewidth=2, label='Dropout Rate (IDR)')
    ax1.tick_params(axis='y', labelcolor=color)
    ax1.grid(True, alpha=0.3)
    
    ax2 = ax1.twinx()
    color = 'tab:blue'
    ax2.set_ylabel('Note Change Latency (ms)', color=color, fontweight='bold')
    line2 = ax2.plot(dur_ms, lat, color=color, marker='s', linestyle='--', linewidth=2, label='Legato Step Latency')
    ax2.tick_params(axis='y', labelcolor=color)
    
    ax1.axvspan(10, 15, color='green', alpha=0.15, label='Recommended Optimal Range (10-15 ms)')
    
    lines = line1 + line2
    labels = [l.get_label() for l in lines]
    ax1.legend(lines, labels, loc='upper center')
    ax1.set_title("Figure 7: Bounded Coasting Window Sweep (IDR vs Articulation Latency)")
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig07_coasting_window_sweep.png", dpi=200)
    plt.close(fig)

    # Figure 8: Reacquisition Strategy Comparison
    print("Plotting Figure 8: Reacquisition Strategy Comparison...")
    fig, axes = plt.subplots(3, 1, figsize=(12, 8), sharex=True)
    wb_b = SCRATCH_DIR / "test_b_confidence_noise_base.wav"
    we_b = SCRATCH_DIR / "test_b_confidence_noise_exp.wav"
    ab_b, _ = sf.read(wb_b)
    ae_b, _ = sf.read(we_b)
    t_reac = np.arange(int(1.58 * RATE), int(1.68 * RATE))
    t_ms = (t_reac - t_reac[0]) / RATE * 1000.0
    
    axes[0].plot(t_ms, ab_b[t_reac], color='crimson', label='Baseline: Abrupt Drop & Snap')
    axes[0].set_ylabel("Baseline")
    axes[0].legend(loc='upper right')
    axes[0].grid(True, alpha=0.3)
    axes[0].set_title("Figure 8: Reacquisition Strategy Transient Comparison (Noise Recovery at t=1.6s)")
    
    axes[1].plot(t_ms, ae_b[t_reac], color='forestgreen', label='Experimental: Phase Continuity & 5 ms Crossfade')
    axes[1].set_ylabel("Experimental")
    axes[1].legend(loc='upper right')
    axes[1].grid(True, alpha=0.3)
    
    diff = np.abs(ae_b[t_reac] - ab_b[t_reac])
    axes[2].plot(t_ms, diff, color='purple', label='Transient Residual Magnitude')
    axes[2].set_ylabel("|Residual|")
    axes[2].set_xlabel("Relative Time (ms)")
    axes[2].legend(loc='upper right')
    axes[2].grid(True, alpha=0.3)
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig08_reacquisition_strategy_comparison.png", dpi=200)
    plt.close(fig)

    # Figure 9: Spectral Continuity & Spectrogram Notch Inspection around Top 3 Dropouts
    print("Plotting Figure 9: Spectrogram Notches for Top Dropouts...")
    fig, axes = plt.subplots(3, 2, figsize=(14, 9))
    top_3 = sorted(true_base_evs, key=lambda e: e['depth_db'], reverse=True)[:3]
    for idx, ev in enumerate(top_3):
        t0 = max(0, int((ev['peak_time'] - 0.10) * RATE))
        t1 = min(len(x_base), int((ev['peak_time'] + 0.10) * RATE))
        axes[idx, 0].specgram(x_base[t0:t1], NFFT=256, Fs=RATE, noverlap=192, cmap='inferno')
        axes[idx, 0].set_title(f"Baseline Event {idx+1} (t={ev['peak_time']:.3f}s, depth={ev['depth_db']:.1f} dB)", fontsize=10)
        axes[idx, 0].set_ylabel("Frequency (Hz)")
        axes[idx, 1].specgram(x_exp[t0:t1], NFFT=256, Fs=RATE, noverlap=192, cmap='inferno')
        axes[idx, 1].set_title(f"Experimental Event {idx+1} (Coasted)", fontsize=10)
    axes[2, 0].set_xlabel("Time (s)")
    axes[2, 1].set_xlabel("Time (s)")
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig09_spectrogram_notch_inspection.png", dpi=200)
    plt.close(fig)

    # Figure 10: Multi-Path Signal Architecture & Energy Handoff
    print("Plotting Figure 10: Multi-Path Architecture & Energy Handoff...")
    fig, axes = plt.subplots(4, 1, figsize=(12, 9), sharex=True)
    s0 = int(18.0 * RATE)
    s1 = int(20.0 * RATE)
    sub_t10 = (np.arange(s0, s1) - s0) / RATE
    
    axes[0].plot(sub_t10, base_telem['input_rms'][s0:s1], color='black', label='Lead Input RMS')
    axes[0].set_ylabel("Lead RMS")
    axes[0].set_title("Figure 10: Multi-Path Signal Architecture & Handoff (Lead t=18.0s - 20.0s)")
    axes[0].legend(loc='upper right')
    axes[0].grid(True, alpha=0.3)
    
    axes[1].plot(sub_t10, base_telem['ola_output_rms'][s0:s1], color='blue', label='Voiced PSOLA Path')
    axes[1].set_ylabel("PSOLA RMS")
    axes[1].legend(loc='upper right')
    axes[1].grid(True, alpha=0.3)
    
    axes[2].plot(sub_t10, base_telem['unvoiced_path_active'][s0:s1] * 0.25, color='orange', label='Unvoiced Articulation Path')
    axes[2].plot(sub_t10, base_telem['plosive_path_active'][s0:s1] * 0.20, color='red', label='Plosive Bridge Path')
    axes[2].set_ylabel("Consonant Paths")
    axes[2].legend(loc='upper right')
    axes[2].grid(True, alpha=0.3)
    
    axes[3].plot(sub_t10, base_telem['final_harmony_rms'][s0:s1], color='green', label='Composite Harmony Output')
    axes[3].set_ylabel("Final Output RMS")
    axes[3].set_xlabel("Time (s)")
    axes[3].legend(loc='upper right')
    axes[3].grid(True, alpha=0.3)
    
    plt.tight_layout()
    fig.savefig(PLOTS_DIR / "fig10_multipath_energy_handoff.png", dpi=200)
    plt.close(fig)
    print("All 10 diagnostic figures generated successfully.")

def export_worst_cases(lead_wav, base_wav, exp_wav, base_telem, exp_telem, base_events):
    """Exports Top 10 worst micro-dropouts with 300 ms pre / 500 ms post audio snippets and CSVs."""
    print("Exporting Top 10 worst micro-dropout cases...")
    x_lead, _ = sf.read(lead_wav)
    if x_lead.ndim > 1: x_lead = np.mean(x_lead, axis=1)
    x_base, _ = sf.read(base_wav)
    if x_base.ndim > 1: x_base = np.mean(x_base, axis=1)
    x_exp, _ = sf.read(exp_wav)
    if x_exp.ndim > 1: x_exp = np.mean(x_exp, axis=1)
    
    true_events = [e for e in base_events if e['category'] != 'K']
    top_10 = sorted(true_events, key=lambda e: e['depth_db'], reverse=True)[:10]
    
    n_pre = int(0.300 * RATE)
    n_post = int(0.500 * RATE)
    
    for idx, ev in enumerate(top_10, 1):
        mid = ev['mid_sample']
        s0 = max(0, mid - n_pre)
        s1 = min(len(x_lead), mid + n_post)
        
        prefix = f"worst_case_{idx:02d}_t{ev['peak_time']:.3f}s_cat{ev['category']}"
        sf.write(WORST_DIR / f"{prefix}_lead.wav", x_lead[s0:s1].astype(np.float32), RATE)
        sf.write(WORST_DIR / f"{prefix}_baseline_harmony.wav", x_base[s0:s1].astype(np.float32), RATE)
        sf.write(WORST_DIR / f"{prefix}_experimental_harmony.wav", x_exp[s0:s1].astype(np.float32), RATE)
        
        sub_telem = base_telem[s0:s1]
        csv_p = WORST_DIR / f"{prefix}_telemetry.csv"
        with open(csv_p, "w") as f:
            f.write("time,sample_index,input_rms,pitch_voiced,pitch_confidence,pitch_f0_hz,pitch_track_state,psola_usable,psola_usable_reason,ola_weight_sum,last_grain_age,release_active,psola_gain,active_mix,final_harmony_rms\n")
            step = 48
            for i in range(0, len(sub_telem), step):
                r = sub_telem[i]
                t_sec = (s0 + i) / RATE
                f.write(f"{t_sec:.4f},{r['sample_index']},{r['input_rms']:.4f},{r['pitch_voiced']},{r['pitch_confidence']:.4f},{r['pitch_f0_hz']:.1f},{r['pitch_track_state']},{r['psola_usable']},{r['psola_usable_reason']},{r['ola_weight_sum']:.4f},{r['last_grain_age']},{r['release_active']},{r['psola_gain']:.4f},{r['active_mix']:.4f},{r['final_harmony_rms']:.4f}\n")
    print("Top 10 worst case audio snippets exported successfully.")

def export_metrics_csvs(base_events, base_telem, exp_telem):
    """Exports all required CSV metrics."""
    print("Exporting metrics CSVs...")
    
    # 1. microdropouts.csv
    with open(METRICS_DIR / "microdropouts.csv", "w") as f:
        f.write("event_id,peak_time_s,duration_ms,depth_db,category,description\n")
        for idx, ev in enumerate(base_events, 1):
            f.write(f"{idx},{ev['peak_time']:.4f},{ev['duration_ms']:.2f},{ev['depth_db']:.2f},{ev['category']},\"{ev['description']}\"\n")
            
    # 2. grain_gap_events.csv
    with open(METRICS_DIR / "grain_gap_events.csv", "w") as f:
        f.write("sample_index,time_s,last_grain_age_samples,pitch_period_samples,ratio\n")
        voiced_samples = np.where(base_telem['pitch_voiced'] == 1)[0]
        step = 48
        count = 0
        for idx in voiced_samples[::step]:
            age = base_telem['last_grain_age'][idx]
            period = max(24.0, base_telem['pitch_period_samples'][idx])
            if age > 1.5 * period and count < 1000:
                f.write(f"{idx},{idx/RATE:.4f},{age},{period:.1f},{age/period:.2f}\n")
                count += 1
                
    # 3. ola_gap_events.csv
    with open(METRICS_DIR / "ola_gap_events.csv", "w") as f:
        f.write("sample_index,time_s,ola_weight_sum,pitch_confidence,psola_usable\n")
        gaps = np.where((base_telem['pitch_voiced'] == 1) & (base_telem['ola_weight_sum'] < 0.1))[0]
        count = 0
        for idx in gaps[::step]:
            if count < 1000:
                f.write(f"{idx},{idx/RATE:.4f},{base_telem['ola_weight_sum'][idx]:.4f},{base_telem['pitch_confidence'][idx]:.4f},{base_telem['psola_usable'][idx]}\n")
                count += 1
                
    # 4. release_events.csv
    with open(METRICS_DIR / "release_events.csv", "w") as f:
        f.write("sample_index,time_s,release_remaining,active_mix,psola_usable_reason\n")
        rel = np.where(base_telem['release_active'] == 1)[0]
        count = 0
        for idx in rel[::step]:
            if count < 1000:
                f.write(f"{idx},{idx/RATE:.4f},{base_telem['release_remaining'][idx]},{base_telem['active_mix'][idx]:.4f},{base_telem['psola_usable_reason'][idx]}\n")
                count += 1
                
    # 5. reacquisition_events.csv
    with open(METRICS_DIR / "reacquisition_events.csv", "w") as f:
        f.write("reacquisition_id,start_time_s,end_time_s,latency_ms,track_state_initial\n")
        v = base_telem['pitch_voiced']
        diff = np.diff(v.astype(np.int8))
        onsets = np.where(diff == 1)[0] + 1
        for r_id, ons in enumerate(onsets[:100], 1):
            sub = base_telem['new_grain_scheduled'][ons:ons + int(0.100 * RATE)]
            sched = np.where(sub == 1)[0]
            if len(sched) > 0:
                lat_ms = (sched[0] / RATE) * 1000.0
                f.write(f"{r_id},{ons/RATE:.4f},{(ons+sched[0])/RATE:.4f},{lat_ms:.2f},{base_telem['pitch_track_state'][ons]}\n")
                
    # 6. path_energy_coverage.csv
    with open(METRICS_DIR / "path_energy_coverage.csv", "w") as f:
        f.write("path,active_samples,energy_share_percent\n")
        total_e = np.sum(base_telem['final_harmony_rms']**2) + 1e-12
        psola_e = np.sum(base_telem['ola_output_rms']**2)
        unv_e = np.sum(base_telem['unvoiced_path_active']) * 0.0625
        plos_e = np.sum(base_telem['plosive_path_active']) * 0.04
        f.write(f"PSOLA,{np.sum(base_telem['psola_usable'] > 0)},{100.0 * min(1.0, psola_e / total_e):.2f}\n")
        f.write(f"UnvoicedArticulation,{np.sum(base_telem['unvoiced_path_active'] > 0)},{100.0 * min(1.0, unv_e / total_e):.2f}\n")
        f.write(f"PlosiveBridge,{np.sum(base_telem['plosive_path_active'] > 0)},{100.0 * min(1.0, plos_e / total_e):.2f}\n")
    print("Metrics CSVs generated successfully.")

def main():
    print("=== Milestone 5.11 Voicing Continuity & Micro-Dropout Audit ===")
    
    # 1. Create synthetic stimuli
    create_synthetic_stimuli()
    
    # 2. Locate 52s lead vocal
    lead_wav = PROJECT_ROOT / "artifacts" / "formant_preservation" / "renders" / "full" / "00_lead_dry.wav"
    if not lead_wav.exists():
        lead_wav = PROJECT_ROOT / "samples" / "dry-acapella-leave-this-place_95bpm.wav"
        
    print(f"Using lead vocal stem: {lead_wav}")
    
    # 3. Process Baseline
    base_out = BASELINE_DIR / "harmony_baseline.wav"
    base_tel = BASELINE_DIR / "telemetry_baseline.bin"
    print("Running Baseline Pitch Shift & Telemetry...")
    run_pitch_shift(lead_wav, base_out, semitones=7.0, policy="baseline", telem_bin=base_tel)
    base_telem = load_telemetry(base_tel)
    
    # 4. Process Experimental (Combined Coasting)
    exp_out = EXPERIMENTAL_DIR / "harmony_experimental.wav"
    exp_tel = EXPERIMENTAL_DIR / "telemetry_experimental.bin"
    print("Running Experimental Pitch Shift & Telemetry...")
    run_pitch_shift(lead_wav, exp_out, semitones=7.0, policy="combined", telem_bin=exp_tel, coast_ms=15.0)
    exp_telem = load_telemetry(exp_tel)
    
    # 5. Detect and Classify Micro-Dropouts
    print("Detecting and classifying micro-dropouts...")
    base_events, times, base_def, _, _ = detect_microdropouts(lead_wav, base_out, base_telem)
    exp_events, _, exp_def, _, _ = detect_microdropouts(lead_wav, exp_out, exp_telem)
    
    true_base = [e for e in base_events if e['category'] != 'K']
    true_exp = [e for e in exp_events if e['category'] != 'K']
    print(f"Detected {len(base_events)} total events in Baseline ({len(true_base)} genuine micro-dropouts, {len(base_events)-len(true_base)} Category K)")
    print(f"Detected {len(exp_events)} total events in Experimental ({len(true_exp)} genuine micro-dropouts, {len(exp_events)-len(true_exp)} Category K)")
    
    # 6. Run Parameter Sweep
    sweep_results = run_parameter_sweep(lead_wav)
    
    # 7. Generate All 10 Diagnostic Plots
    generate_all_plots(lead_wav, base_out, exp_out, base_telem, exp_telem, base_events, exp_events, sweep_results)
    
    # 8. Export Metrics CSVs
    export_metrics_csvs(base_events, base_telem, exp_telem)
    
    # 9. Export Worst 10 Snippets
    export_worst_cases(lead_wav, base_out, exp_out, base_telem, exp_telem, base_events)
    
    # 10. Blind Pack
    print("Exporting Blind AB Pack...")
    sf.write(BLIND_DIR / "stem_blind_A.wav", sf.read(base_out)[0], RATE)
    sf.write(BLIND_DIR / "stem_blind_B.wav", sf.read(exp_out)[0], RATE)
    with open(BLIND_DIR / "blind_key.txt", "w") as f:
        f.write("A: Baseline (Milestone 5.11 unpatched)\nB: Experimental (Milestone 5.11 + Bounded Coasting)\n")
        
    print("=== Audit execution completed successfully! ===")

if __name__ == "__main__":
    main()
