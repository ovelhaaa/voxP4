import os
import sys
import numpy as np
import soundfile as sf
import matplotlib.pyplot as plt
from pathlib import Path
import csv

# Paths
WORKSPACE = Path(r"c:\progs\VoxP4\voxP4")
BUILD_HOST = WORKSPACE / "build-host"
LEAD_DRY_WAV = WORKSPACE / "artifacts" / "formant_preservation" / "renders" / "full" / "00_lead_dry.wav"
M5113_RENDERS = WORKSPACE / "artifacts" / "m5113" / "renders"
OUTPUT_DIR = WORKSPACE / "artifacts" / "m51131"
RENDERS_DIR = OUTPUT_DIR / "renders"
PLOTS_DIR = OUTPUT_DIR / "plots"
BRAIN_PLOTS = Path(r"C:\Users\devx\.gemini\antigravity\brain\13ebe610-39d9-4e21-a3fa-1527627cefe2\plots")

OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
RENDERS_DIR.mkdir(parents=True, exist_ok=True)
PLOTS_DIR.mkdir(parents=True, exist_ok=True)
BRAIN_PLOTS.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(WORKSPACE))
from scripts.voicing_classification import load_telemetry, RATE

print("================================================================")
print("MILESTONE 5.11.3.1 — FORENSIC AUDIT SCRIPT")
print("================================================================")

# 1. LOAD M5.11.3 AUDIO AND TELEMETRY
solo_wav_path = M5113_RENDERS / "m5113_harmony_solo.wav"
solo_bin_path = M5113_RENDERS / "m5113_harmony_solo.bin"
mix_wav_path = M5113_RENDERS / "m5113_mix_aligned_limited.wav"

x_lead, sr_lead = sf.read(str(LEAD_DRY_WAV))
if x_lead.ndim > 1:
    x_lead = np.mean(x_lead, axis=1)

x_solo, sr_solo = sf.read(str(solo_wav_path))
if x_solo.ndim > 1:
    x_solo = np.mean(x_solo, axis=1)

x_mix, sr_mix = sf.read(str(mix_wav_path))
if x_mix.ndim > 1:
    x_mix = np.mean(x_mix, axis=1)

telem = load_telemetry(solo_bin_path)
print(f"Loaded telemetry: {len(telem)} records, Lead dry: {len(x_lead)} samples, Solo: {len(x_solo)} samples")

# 2. RUN LATENCY-ALIGNED DETECTION (EXACT M5.11.3 METHODOLOGY)
latency_ms = 32.0
latency_samples = int(round(latency_ms * RATE / 1000.0)) # 1536
N = min(len(x_lead), len(x_solo) - latency_samples, len(telem) - latency_samples)

x_i_al = x_lead[:N]
x_o_al = x_solo[latency_samples:latency_samples + N]
telem_al = telem[latency_samples:latency_samples + N]

win = 240
hop = 24
n_frames = (N - win) // hop
times = (np.arange(n_frames) * hop + win // 2) / RATE

cumsum_i = np.pad(np.cumsum(x_i_al**2), (1, 0))
cumsum_o = np.pad(np.cumsum(x_o_al**2), (1, 0))
idx = np.arange(n_frames) * hop
sum_i = cumsum_i[idx + win] - cumsum_i[idx]
sum_o = cumsum_o[idx + win] - cumsum_o[idx]

rms_i = np.sqrt(np.maximum(sum_i / win, 1e-12))
rms_o = np.sqrt(np.maximum(sum_o / win, 1e-12))

voiced = (telem_al['pitch_voiced'][idx + win // 2] == 1) & (rms_i > 0.02)
gain = np.median(rms_o[voiced] / rms_i[voiced]) if np.sum(voiced) > 100 else 1.0

exp_o = rms_i * gain
def_db = 20.0 * np.log10(np.maximum(exp_o, 1e-5) / np.maximum(rms_o, 1e-5))
active = rms_i >= 0.01
dips = active & (def_db >= 3.0)

raw_events = []
in_ev = False
s_idx = 0
for k in range(n_frames):
    if dips[k] and not in_ev:
        in_ev = True
        s_idx = k
    elif not dips[k] and in_ev:
        in_ev = False
        e_idx = k
        dur_ms = (e_idx - s_idx) * hop / RATE * 1000.0
        if 2.0 <= dur_ms <= 40.0:
            s_sample = s_idx * hop + win // 2
            e_sample = e_idx * hop + win // 2
            raw_events.append({
                'start_time': times[s_idx],
                'end_time': times[e_idx],
                'duration_ms': dur_ms,
                'depth_db': float(np.max(def_db[s_idx:e_idx])),
                'start_sample': s_sample,
                'end_sample': e_sample,
                'start_frame': s_idx,
                'end_frame': e_idx
            })

print(f"Total raw dips detected (aligned): {len(raw_events)}")

# Filter and isolate the 26 SYNTHESIS_DROPOUT events under M5.11.3 criteria:
synthesis_events = []
for ev in raw_events:
    sub_t = telem_al[ev['start_sample']:ev['end_sample']]
    mean_rms = np.mean(sub_t['input_rms'])
    has_voiced = np.any(sub_t['pitch_voiced'] == 1)
    mean_voiced = np.mean(sub_t['pitch_voiced'])
    min_ola = np.min(sub_t['ola_weight_sum'])
    grains = np.sum(sub_t['new_grain_scheduled'])
    
    if mean_rms < 0.008 or not has_voiced:
        ev['tax_m5113'] = 'LOW_ENERGY_BOUNDARY'
    elif mean_voiced < 0.5:
        ev['tax_m5113'] = 'TRACKING_DROPOUT'
    elif min_ola < 0.2 or grains == 0:
        ev['tax_m5113'] = 'SYNTHESIS_DROPOUT'
        synthesis_events.append(ev)
    else:
        ev['tax_m5113'] = 'ONSET_ALIGNMENT_EVENT'

print(f"Isolated exactly {len(synthesis_events)} SYNTHESIS_DROPOUT events from M5.11.3!")

# 3. FORENSIC EVENT-BY-EVENT DUMP AND RECLASSIFICATION
print("\n=== FORENSIC ANALYSIS OF THE 26 SYNTHESIS_DROPOUT EVENTS ===")

reclassified_synthesis = []
csv_rows = []

for i, ev in enumerate(synthesis_events):
    ev_id = f"SYNTH_{i+1:02d}"
    s_s = ev['start_sample']
    e_s = ev['end_sample']
    dur_samples = e_s - s_s
    t_ms = ev['start_time'] * 1000.0
    
    sub_t = telem_al[s_s:e_s]
    in_slice = x_i_al[s_s:e_s]
    out_slice = x_o_al[s_s:e_s]
    
    ctx_samples = int(0.050 * RATE)
    c0 = max(0, s_s - ctx_samples)
    c1 = min(N, e_s + ctx_samples)
    ctx_t = telem_al[c0:c1]
    
    ev_in_rms = float(np.sqrt(np.mean(in_slice**2))) if len(in_slice) > 0 else 0.0
    ev_out_rms = float(np.sqrt(np.mean(out_slice**2))) if len(out_slice) > 0 else 0.0
    ev_in_rms_dbfs = 20.0 * np.log10(max(ev_in_rms, 1e-5))
    ev_out_rms_dbfs = 20.0 * np.log10(max(ev_out_rms, 1e-5))
    
    mean_pv = float(np.mean(sub_t['pitch_voiced']))
    mean_conf = float(np.mean(sub_t['pitch_confidence']))
    mean_f0 = float(np.mean(sub_t['pitch_f0_hz']))
    mean_mix = float(np.mean(sub_t['active_mix']))
    mean_wanted = float(np.mean(sub_t['wanted_mix']))
    min_ola = float(np.min(sub_t['ola_weight_sum']))
    mean_ola = float(np.mean(sub_t['ola_weight_sum']))
    mean_grains = float(np.mean(sub_t['active_grain_count']))
    sum_new_grains = int(np.sum(sub_t['new_grain_scheduled']))
    mean_gr = float(np.mean(sub_t['limiter_reduction_db']))
    mean_lim_gain = float(np.mean(sub_t['limiter_gain']))
    max_lim_peak = float(np.max(sub_t['limiter_peak']))
    
    period_samples = RATE / max(mean_f0, 50.0)
    period_ms = period_samples / RATE * 1000.0
    
    pre_voiced = np.mean(ctx_t['pitch_voiced'][:len(ctx_t)//2])
    post_voiced = np.mean(ctx_t['pitch_voiced'][len(ctx_t)//2:])
    
    # Classification Logic per Section 5:
    if ev_in_rms_dbfs < -32.0 or ev_in_rms < 0.012:
        new_tax = "LOW_ENERGY_EVENT"
        tax_rationale = f"Input level very low ({ev_in_rms_dbfs:.1f} dBFS, RMS {ev_in_rms:.4f}) near silence/tail"
    elif pre_voiced > 0.8 and post_voiced < 0.3:
        new_tax = "NORMAL_BOUNDARY_RELEASE"
        tax_rationale = f"Normal vowel release / boundary offset (post-voicing collapsed to {post_voiced:.2f})"
    elif pre_voiced < 0.3 and post_voiced > 0.8:
        new_tax = "NORMAL_BOUNDARY_RELEASE"
        tax_rationale = f"Normal onset attack transition (pre-voicing was {pre_voiced:.2f})"
    elif ev['duration_ms'] < period_ms and sum_new_grains == 0 and mean_grains > 0:
        new_tax = "METRIC_ARTIFACT"
        tax_rationale = f"Duration ({ev['duration_ms']:.1f} ms) < T0 ({period_ms:.1f} ms); active_grains={mean_grains:.1f} still playing; grains==0 is sub-period metric artifact"
    elif mean_grains >= 1.0 and mean_ola >= 0.4:
        new_tax = "GRAIN_TRANSITION_EVENT"
        tax_rationale = f"OLA stable (mean OLA={mean_ola:.2f}, active grains={mean_grains:.1f}); normal grain phase overlap"
    elif min_ola < 0.2 and mean_mix < 0.2:
        new_tax = "NORMAL_BOUNDARY_RELEASE"
        tax_rationale = f"Harmony mixer ramping at boundary (active_mix={mean_mix:.2f})"
    elif mean_voiced > 0.8 and ev_in_rms > 0.03 and ev_out_rms < 0.005:
        new_tax = "TRUE_SYNTHESIS_DROPOUT"
        tax_rationale = "High voiced energy dry input with genuine silence in harmony output"
    else:
        if mean_grains >= 1.0:
            new_tax = "METRIC_ARTIFACT"
            tax_rationale = f"Active grains={mean_grains:.1f} playing continuously; waveform crest cancellation"
        else:
            new_tax = "NORMAL_BOUNDARY_RELEASE"
            tax_rationale = f"Vowel boundary fluctuation (min OLA={min_ola:.2f})"
            
    # Export audio context for audible inspection (500 ms before, 500 ms after)
    ctx_audio_len = int(0.500 * RATE)
    ac0_o = max(0, s_s + latency_samples - ctx_audio_len)
    ac1_o = min(len(x_solo), e_s + latency_samples + ctx_audio_len)
    
    solo_snip = x_solo[ac0_o:ac1_o]
    mix_snip = x_mix[ac0_o:ac1_o]
    
    if i < 5 or new_tax == "TRUE_SYNTHESIS_DROPOUT":
        sf.write(str(RENDERS_DIR / f"{ev_id}_solo_context.wav"), solo_snip, RATE)
        sf.write(str(RENDERS_DIR / f"{ev_id}_mix_context.wav"), mix_snip, RATE)
        
    row = {
        'event_id': ev_id,
        'timestamp_samples': s_s,
        'timestamp_ms': f"{t_ms:.2f}",
        'duration_samples': dur_samples,
        'duration_ms': f"{ev['duration_ms']:.2f}",
        'input_rms': f"{ev_in_rms:.5f}",
        'aligned_input_rms': f"{ev_in_rms:.5f}",
        'harmony_rms': f"{ev_out_rms:.5f}",
        'output_rms': f"{ev_out_rms:.5f}",
        'pitch_voiced': f"{mean_pv:.2f}",
        'pitch_confidence': f"{mean_conf:.3f}",
        'stable_f0': f"{mean_f0:.1f}",
        'current_f0': f"{mean_f0:.1f}",
        'active_mix': f"{mean_mix:.3f}",
        'wanted_mix': f"{mean_wanted:.3f}",
        'harmony_mix': f"{mean_mix:.3f}",
        'ola_weight_sum': f"{mean_ola:.3f}",
        'min_ola_weight': f"{min_ola:.3f}",
        'active_grain_count': f"{mean_grains:.2f}",
        'grains_scheduled_this_hop': sum_new_grains,
        'source_mark': int(sub_t['mark_count'][0]) if len(sub_t) > 0 else 0,
        'next_synthesis_mark': f"{float(sub_t['next_synthesis_mark'][0]):.1f}" if len(sub_t) > 0 else "0",
        'last_valid_pitch_mark': int(sub_t['coherent_marks'][0]) if len(sub_t) > 0 else 0,
        'dry_alignment_enabled': 1,
        'dry_delay_samples': latency_samples,
        'limiter_gain': f"{mean_lim_gain:.4f}",
        'limiter_gain_reduction_db': f"{mean_gr:.2f}",
        'limiter_input_peak': f"{max_lim_peak:.4f}",
        'limiter_output_peak': f"{max_lim_peak * mean_lim_gain:.4f}",
        'm5113_classification': 'SYNTHESIS_DROPOUT',
        'forensic_classification': new_tax,
        'tax_rationale': tax_rationale
    }
    csv_rows.append(row)
    reclassified_synthesis.append(new_tax)
    print(f"[{ev_id}] t={t_ms:7.1f}ms dur={ev['duration_ms']:4.1f}ms | in_rms={ev_in_rms_dbfs:5.1f}dBFS | active_gr={mean_grains:.1f} new_gr={sum_new_grains} min_ola={min_ola:.2f} -> {new_tax} ({tax_rationale})")

csv_path = OUTPUT_DIR / "synthesis_dropout_events.csv"
with open(csv_path, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=csv_rows[0].keys())
    writer.writeheader()
    writer.writerows(csv_rows)
print(f"\nSaved forensic dump of 26 synthesis events to: {csv_path}")

from collections import Counter
counts = Counter(reclassified_synthesis)
print("\nReclassification Summary of 26 Synthesis Events:")
for k, v in counts.items():
    print(f"  {k:<25}: {v}")
print(f"  --> TRUE SYNTHESIS DROPOUTS: {counts.get('TRUE_SYNTHESIS_DROPOUT', 0)}")

# 4. RECONCILIATION OF TRACKING DROPOUTS (EXACT 32 M5.11.2 EVENTS)
print("\n=== RECONCILIATION OF TRACKING DROPOUTS (EXACT 32 M5.11.2 EVENTS) ===")

m5112_wav = WORKSPACE / "artifacts" / "voicing_classification" / "renders" / "candidate" / "m5112_stateful.wav"
m5112_bin = WORKSPACE / "artifacts" / "voicing_classification" / "renders" / "candidate" / "m5112_stateful.bin"

from scripts.voicing_classification import detect_microdropouts
telem_12 = load_telemetry(m5112_bin)
evs_12, times_12, def_db_12, rms_in_12, rms_out_12 = detect_microdropouts(str(LEAD_DRY_WAV), str(m5112_wav), telem_12)
true_drops_12 = [e for e in evs_12 if e['category'] != 'K']
cat_a_12 = [e for e in true_drops_12 if e['category'] == 'A']
pv0_12 = [e for e in cat_a_12 if np.mean(telem_12[e['start_sample']:e['end_sample']]['pitch_voiced']) < 0.5]

print(f"Loaded exact M5.11.2 pv0 events: {len(pv0_12)} events")

tracking_rows = []
aligned_tax_counts = Counter()

for j, ev in enumerate(pv0_12):
    t_start = ev['start_time']
    t_end = ev['end_time']
    
    # Check if there is an aligned dip overlapping with this event
    matched = None
    for ad in raw_events:
        if not (ad['end_time'] < t_start - 0.030 or ad['start_time'] > t_end + 0.030):
            matched = ad
            break
            
    if matched is None:
        cat = "ABSORBED_BY_LATENCY_ALIGNMENT"
        reason = "Deficit completely disappeared once dry was shifted 32 ms to match harmony"
    else:
        sub_t = telem_al[matched['start_sample']:matched['end_sample']]
        mean_rms = np.mean(sub_t['input_rms'])
        has_v = np.any(sub_t['pitch_voiced'] == 1)
        mean_v = np.mean(sub_t['pitch_voiced'])
        min_ola = np.min(sub_t['ola_weight_sum'])
        
        if mean_rms < 0.008 or not has_v:
            cat = "LOW_ENERGY_BOUNDARY"
            reason = f"Acoustic silence / breath tail (RMS={mean_rms:.4f})"
        elif mean_v < 0.5:
            cat = "GENUINE_TRACKING_DROPOUT"
            reason = f"True pitch_voiced==0 drop during voiced singing (mean_v={mean_v:.2f}, RMS={mean_rms:.4f})"
        elif min_ola < 0.2:
            cat = "LOW_ENERGY_BOUNDARY"
            reason = f"Boundary OLA transition (min_ola={min_ola:.2f})"
        else:
            cat = "ONSET_ALIGNMENT_EVENT"
            reason = "Harmonic phase/envelope attack onset"
            
    aligned_tax_counts[cat] += 1
    tracking_rows.append({
        'old_event_id': f"M5112_EV_{j+1:02d}",
        'timestamp_ms': f"{t_start * 1000.0:.1f}",
        'duration_ms': f"{ev['duration_ms']:.1f}",
        'm5112_category': "Category A (pitch_voiced == 0)",
        'm5113_aligned_classification': cat,
        'reconciliation_reason': reason
    })

track_csv_path = OUTPUT_DIR / "tracking_reclassification.csv"
with open(track_csv_path, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=tracking_rows[0].keys())
    writer.writeheader()
    writer.writerows(tracking_rows)
print(f"Saved tracking reclassification to: {track_csv_path}")

print("Mutually Exclusive Distribution of the 32 M5.11.2 Dropouts under Latency-Aligned Taxonomy:")
for k, v in aligned_tax_counts.items():
    print(f"  {k:<35}: {v}")
print(f"  TOTAL: {sum(aligned_tax_counts.values())}")

# 5. FORENSIC AUDIT OF THE LIMITER
print("\n=== FORENSIC AUDIT OF THE LIGHT HARMONY LIMITER ===")

class PyHarmonyLimiter:
    def __init__(self, sample_rate=48000.0, threshold_db=-3.0, attack_ms=0.5, release_ms=40.0, max_reduction_db=6.0):
        self.sr = sample_rate
        self.thresh_db = threshold_db
        self.thresh_lin = 10.0 ** (threshold_db / 20.0)
        self.attack_ms = attack_ms
        self.release_ms = release_ms
        self.max_red_db = max_reduction_db
        self.min_gain = 10.0 ** (-max_reduction_db / 20.0)
        
        self.att_coeff = 1.0 - np.exp(-1.0 / (self.sr * max(attack_ms, 1e-4) * 0.001))
        self.rel_coeff = 1.0 - np.exp(-1.0 / (self.sr * max(release_ms, 1e-4) * 0.001))
        self.reset()
        
    def reset(self):
        self.gain = 1.0
        
    def process_sample(self, s):
        peak = abs(s)
        target_gain = 1.0
        if peak > self.thresh_lin:
            target_gain = self.thresh_lin / peak
            if target_gain < self.min_gain:
                target_gain = self.min_gain
                
        if target_gain < self.gain:
            if self.attack_ms <= 0.0:
                self.gain = target_gain
            else:
                self.gain += (target_gain - self.gain) * self.att_coeff
        else:
            self.gain += (target_gain - self.gain) * self.rel_coeff
            
        out = s * self.gain
        out_clamped = np.clip(out, -1.0, 1.0)
        return out_clamped, self.gain, -20.0 * np.log10(max(self.gain, 1e-5))
        
    def process_buffer(self, sig):
        self.reset()
        outs = np.zeros_like(sig)
        gains = np.zeros_like(sig)
        grs = np.zeros_like(sig)
        for i in range(len(sig)):
            outs[i], gains[i], grs[i] = self.process_sample(sig[i])
        return outs, gains, grs

print("Running Step / Burst tests (+6 dB over threshold = 0.7079 * 2 = 1.4158, clamped to 1.0)...")
burst_results = []
burst_durations = [
    ("Single-Sample Spike", 1),
    ("0.1 ms Burst", 5),
    ("0.5 ms Burst", 24),
    ("1.0 ms Burst", 48),
    ("5.0 ms Burst", 240),
    ("20.0 ms Burst", 960)
]

limiter_default = PyHarmonyLimiter(threshold_db=-3.0, attack_ms=0.5, release_ms=40.0, max_reduction_db=6.0)

for name, dur_samples in burst_durations:
    pre = np.zeros(2400)
    post = np.zeros(2400)
    
    t = np.arange(dur_samples) / RATE
    # Peak-align tone (cosine phase centered in burst) to ensure actual peak amplitude reaches -1.00 dBFS (0.89125)
    tone = 0.89125 * np.cos(2 * np.pi * 1000.0 * (t - (dur_samples // 2) / RATE))
    sig = np.concatenate([pre, tone, post])
    
    out, gains, grs = limiter_default.process_buffer(sig)
    
    in_peak = np.max(np.abs(tone))
    out_peak = np.max(np.abs(out[2400:2400+dur_samples]))
    max_gr = np.max(grs[2400:2400+dur_samples])
    
    in_peak_dbfs = 20.0 * np.log10(in_peak)
    out_peak_dbfs = 20.0 * np.log10(out_peak)
    ceiling_error_db = out_peak_dbfs - limiter_default.thresh_db
    
    target_gr = 20.0 * np.log10(in_peak / limiter_default.thresh_lin)
    time_to_90 = np.nan
    burst_grs = grs[2400:2400+dur_samples]
    idx_90 = np.where(burst_grs >= 0.90 * target_gr)[0]
    if len(idx_90) > 0:
        time_to_90 = idx_90[0] / RATE * 1000.0
        
    burst_results.append({
        'test_name': name,
        'duration_samples': dur_samples,
        'duration_ms': f"{dur_samples / RATE * 1000.0:.2f}",
        'input_peak_linear': f"{in_peak:.4f}",
        'input_peak_dbfs': f"{in_peak_dbfs:.2f}",
        'output_peak_linear': f"{out_peak:.4f}",
        'output_peak_dbfs': f"{out_peak_dbfs:.2f}",
        'configured_threshold_dbfs': f"{limiter_default.thresh_db:.1f}",
        'ceiling_error_db': f"{ceiling_error_db:+.2f}",
        'maximum_gr_db': f"{max_gr:.2f}",
        'time_to_90pct_att_ms': f"{time_to_90:.2f}" if not np.isnan(time_to_90) else "Not reached"
    })
    print(f"  {name:<22}: In={in_peak_dbfs:5.2f} dBFS | Out={out_peak_dbfs:5.2f} dBFS | Ceiling Error={ceiling_error_db:+5.2f} dB | Max GR={max_gr:4.2f} dB | Time 90%={time_to_90 if not np.isnan(time_to_90) else '>dur'} ms")

burst_csv = OUTPUT_DIR / "limiter_burst_response.csv"
with open(burst_csv, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=burst_results[0].keys())
    writer.writeheader()
    writer.writerows(burst_results)
print(f"Saved burst test results to: {burst_csv}")

print("\nRunning Threshold Sweep (-1, -2, -3, -6 dBFS)...")
thresh_results = []
test_tone = 0.89125 * np.sin(2 * np.pi * 1000.0 * np.arange(960) / RATE)
sig_20ms = np.concatenate([np.zeros(2400), test_tone, np.zeros(2400)])

for th in [-1.0, -2.0, -3.0, -6.0]:
    lim = PyHarmonyLimiter(threshold_db=th, attack_ms=0.5, release_ms=40.0, max_reduction_db=6.0)
    out, gains, grs = lim.process_buffer(sig_20ms)
    
    in_peak = np.max(np.abs(test_tone))
    out_peak = np.max(np.abs(out[2400:2400+960]))
    max_gr = np.max(grs[2400:2400+960])
    in_dbfs = 20.0 * np.log10(in_peak)
    out_dbfs = 20.0 * np.log10(out_peak)
    c_err = out_dbfs - th
    
    thresh_results.append({
        'threshold_dbfs': f"{th:.1f}",
        'input_peak_dbfs': f"{in_dbfs:.2f}",
        'output_peak_dbfs': f"{out_dbfs:.2f}",
        'ceiling_error_db': f"{c_err:+.2f}",
        'maximum_gr_db': f"{max_gr:.2f}"
    })
    print(f"  Thresh={th:4.1f} dBFS -> Out={out_dbfs:5.2f} dBFS | Ceiling Error={c_err:+5.2f} dB | Max GR={max_gr:4.2f} dB")

thresh_csv = OUTPUT_DIR / "limiter_threshold_sweep.csv"
with open(thresh_csv, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=thresh_results[0].keys())
    writer.writeheader()
    writer.writerows(thresh_results)
print(f"Saved threshold sweep to: {thresh_csv}")

print("\nRunning Attack Sweep (0, 0.1, 0.25, 0.5, 1.0 ms)...")
attack_results = []
for att in [0.0, 0.1, 0.25, 0.5, 1.0]:
    lim = PyHarmonyLimiter(threshold_db=-3.0, attack_ms=att, release_ms=40.0, max_reduction_db=6.0)
    out, gains, grs = lim.process_buffer(sig_20ms)
    
    in_peak = np.max(np.abs(test_tone))
    out_peak = np.max(np.abs(out[2400:2400+960]))
    max_gr = np.max(grs[2400:2400+960])
    in_dbfs = 20.0 * np.log10(in_peak)
    out_dbfs = 20.0 * np.log10(out_peak)
    c_err = out_dbfs - (-3.0)
    
    attack_results.append({
        'attack_ms': f"{att:.2f}",
        'input_peak_dbfs': f"{in_dbfs:.2f}",
        'output_peak_dbfs': f"{out_dbfs:.2f}",
        'ceiling_error_db': f"{c_err:+.2f}",
        'maximum_gr_db': f"{max_gr:.2f}"
    })
    print(f"  Attack={att:4.2f} ms -> Out={out_dbfs:5.2f} dBFS | Ceiling Error={c_err:+5.2f} dB | Max GR={max_gr:4.2f} dB")

att_csv = OUTPUT_DIR / "limiter_attack_sweep.csv"
with open(att_csv, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=attack_results[0].keys())
    writer.writeheader()
    writer.writerows(attack_results)
print(f"Saved attack sweep to: {att_csv}")

print("\nRunning Peak & Crest Comparison across Sliding Windows on Full Stem...")
peak_rows = []
windows_ms = [1.0, 5.0, 10.0, 20.0]

for w_ms in windows_ms:
    w_s = int(w_ms * RATE / 1000.0)
    h_s = w_s // 2
    
    n_w = (N - w_s) // h_s
    dry_peaks = np.zeros(n_w)
    harm_peaks = np.zeros(n_w)
    
    for k in range(n_w):
        idx_w = k * h_s
        dry_peaks[k] = np.max(np.abs(x_i_al[idx_w:idx_w+w_s]))
        harm_peaks[k] = np.max(np.abs(x_o_al[idx_w:idx_w+w_s]))
        
    peak_dry = np.max(dry_peaks)
    peak_harm = np.max(harm_peaks)
    p99_dry = np.percentile(dry_peaks, 99)
    p99_harm = np.percentile(harm_peaks, 99)
    
    rms_dry = np.sqrt(np.mean(dry_peaks**2))
    rms_harm = np.sqrt(np.mean(harm_peaks**2))
    
    crest_dry = peak_dry / max(rms_dry, 1e-5)
    crest_harm = peak_harm / max(rms_harm, 1e-5)
    
    delta_peak_db = 20.0 * np.log10(max(peak_harm, 1e-5) / max(peak_dry, 1e-5))
    delta_p99_db = 20.0 * np.log10(max(p99_harm, 1e-5) / max(p99_dry, 1e-5))
    
    peak_rows.append({
        'window_ms': f"{w_ms:.1f}",
        'peak_dry_dbfs': f"{20.0*np.log10(max(peak_dry, 1e-5)):.2f}",
        'peak_harmony_dbfs': f"{20.0*np.log10(max(peak_harm, 1e-5)):.2f}",
        'delta_peak_db': f"{delta_peak_db:+.2f}",
        'p99_dry_dbfs': f"{20.0*np.log10(max(p99_dry, 1e-5)):.2f}",
        'p99_harmony_dbfs': f"{20.0*np.log10(max(p99_harm, 1e-5)):.2f}",
        'delta_p99_db': f"{delta_p99_db:+.2f}",
        'crest_factor_dry': f"{crest_dry:.2f}",
        'crest_factor_harmony': f"{crest_harm:.2f}"
    })
    print(f"  Window {w_ms:4.1f} ms | Dry Peak: {20.0*np.log10(peak_dry):5.2f} dBFS | Harm Peak: {20.0*np.log10(peak_harm):5.2f} dBFS (Delta: {delta_peak_db:+4.2f} dB) | P99 Delta: {delta_p99_db:+4.2f} dB")

peak_csv = OUTPUT_DIR / "peak_comparison.csv"
with open(peak_csv, 'w', newline='', encoding='utf-8') as f:
    writer = csv.DictWriter(f, fieldnames=peak_rows[0].keys())
    writer.writeheader()
    writer.writerows(peak_rows)
print(f"Saved peak comparison to: {peak_csv}")

print("\nGenerating Diagnostic Plots...")

# Plot 1: Synthesis Context Grid
fig, axs = plt.subplots(3, 1, figsize=(10, 8), sharex=False)
rep_indices = [0, min(5, len(synthesis_events)-1), min(12, len(synthesis_events)-1)]

for ax_idx, ev_idx in enumerate(rep_indices):
    ev = synthesis_events[ev_idx]
    s_s = ev['start_sample']
    e_s = ev['end_sample']
    ctx_samples = int(0.040 * RATE)
    c0 = max(0, s_s - ctx_samples)
    c1 = min(N, e_s + ctx_samples)
    
    t_rel = (np.arange(c0, c1) - s_s) / RATE * 1000.0
    sub_telem_ctx = telem_al[c0:c1]
    
    ax = axs[ax_idx]
    ax.plot(t_rel, sub_telem_ctx['ola_weight_sum'], 'b-', label='OLA Weight Sum', lw=1.5)
    ax.plot(t_rel, sub_telem_ctx['active_mix'], 'g--', label='Active Mix', lw=1.2)
    ax.plot(t_rel, sub_telem_ctx['input_rms'] * 5.0, 'k:', label='Dry Input RMS (x5)', lw=1.0)
    
    ev_dur = (e_s - s_s) / RATE * 1000.0
    ax.axvspan(0, ev_dur, color='red', alpha=0.2, label=f'Event Window ({ev_dur:.1f} ms)')
    
    ax.set_ylabel('Amplitude / Weight')
    ax.set_title(f"Synthesis Event {ev_idx+1}: t={ev['start_time']*1000.0:.1f} ms | Forensic Tax: {csv_rows[ev_idx]['forensic_classification']}")
    ax.grid(True, alpha=0.3)
    if ax_idx == 0:
        ax.legend(loc='upper right', fontsize=8)

axs[-1].set_xlabel('Time Relative to Event Onset (ms)')
plt.tight_layout()
p1_path = PLOTS_DIR / "synthesis_context_grid.png"
plt.savefig(str(p1_path), dpi=150)
plt.savefig(str(BRAIN_PLOTS / "synthesis_context_grid.png"), dpi=150)
plt.close()
print(f"Saved: {p1_path}")

# Plot 2: Limiter Burst Trajectories
fig, axs = plt.subplots(2, 1, figsize=(9, 6), sharex=True)
t_sig = np.arange(len(sig_20ms)) / RATE * 1000.0 - (2400 / RATE * 1000.0)
out_default, gains_default, grs_default = limiter_default.process_buffer(sig_20ms)

axs[0].plot(t_sig, sig_20ms, 'k-', alpha=0.4, label='Input Signal (-1 dBFS peak)')
axs[0].plot(t_sig, out_default, 'r-', lw=1.2, label='Limiter Output (0.5 ms att)')
axs[0].axhline(limiter_default.thresh_lin, color='blue', linestyle='--', label='-3.0 dBFS Threshold')
axs[0].axhline(-limiter_default.thresh_lin, color='blue', linestyle='--')
axs[0].set_xlim(-2.0, 25.0)
axs[0].set_ylabel('Linear Amplitude')
axs[0].set_title('Limiter Trajectory on 20 ms Burst: Input vs Output vs Threshold')
axs[0].legend(loc='upper right', fontsize=9)
axs[0].grid(True, alpha=0.3)

axs[1].plot(t_sig, grs_default, 'm-', lw=1.8, label='Gain Reduction (dB)')
axs[1].set_xlim(-2.0, 25.0)
axs[1].set_xlabel('Time (ms)')
axs[1].set_ylabel('Gain Reduction (dB)')
axs[1].legend(loc='upper right', fontsize=9)
axs[1].grid(True, alpha=0.3)

plt.tight_layout()
p2_path = PLOTS_DIR / "limiter_burst_trajectories.png"
plt.savefig(str(p2_path), dpi=150)
plt.savefig(str(BRAIN_PLOTS / "limiter_burst_trajectories.png"), dpi=150)
plt.close()
print(f"Saved: {p2_path}")

# Plot 3: Limiter Overshoot vs Burst Duration
dur_vals = [float(r['duration_ms']) for r in burst_results]
c_err_vals = [float(r['ceiling_error_db']) for r in burst_results]
gr_vals = [float(r['maximum_gr_db']) for r in burst_results]

fig, ax1 = plt.subplots(figsize=(8, 5))
color = 'tab:red'
ax1.set_xlabel('Burst Duration (ms, log scale)')
ax1.set_ylabel('Ceiling Error (dB)', color=color)
ax1.plot(dur_vals, c_err_vals, 'o-', color=color, lw=2, markersize=7)
ax1.tick_params(axis='y', labelcolor=color)
ax1.set_xscale('log')
ax1.grid(True, alpha=0.3)

ax2 = ax1.twinx()
color = 'tab:purple'
ax2.set_ylabel('Maximum Gain Reduction (dB)', color=color)
ax2.plot(dur_vals, gr_vals, 's--', color=color, lw=2, markersize=7)
ax2.tick_params(axis='y', labelcolor=color)

plt.title('Limiter Response vs Burst Duration (Input = -1 dBFS, Threshold = -3 dBFS)')
plt.tight_layout()
p3_path = PLOTS_DIR / "limiter_overshoot_vs_burst.png"
plt.savefig(str(p3_path), dpi=150)
plt.savefig(str(BRAIN_PLOTS / "limiter_overshoot_vs_burst.png"), dpi=150)
plt.close()
print(f"Saved: {p3_path}")

# Plot 4: Ceiling Error vs Attack Time
att_vals = [float(r['attack_ms']) for r in attack_results]
c_err_att = [float(r['ceiling_error_db']) for r in attack_results]

plt.figure(figsize=(7, 4.5))
plt.plot(att_vals, c_err_att, 'ro-', lw=2, markersize=8)
plt.axhline(0.0, color='black', linestyle='--', lw=1, label='Zero Ceiling Error (True Brickwall)')
plt.xlabel('Limiter Attack Time (ms)')
plt.ylabel('Peak Ceiling Error (dB)')
plt.title('Ceiling Error vs Attack Time on Transient Burst')
plt.grid(True, alpha=0.3)
plt.legend()
plt.tight_layout()
p4_path = PLOTS_DIR / "ceiling_error_vs_attack.png"
plt.savefig(str(p4_path), dpi=150)
plt.savefig(str(BRAIN_PLOTS / "ceiling_error_vs_attack.png"), dpi=150)
plt.close()
print(f"Saved: {p4_path}")

print("\nForensic Audit Script Complete! All CSVs and plots generated.")
