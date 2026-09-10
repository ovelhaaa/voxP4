import csv
import math
from pathlib import Path

csv_path = Path("artifacts/psola_recovery/csv/combined_60ms_soft.csv")
rows = list(csv.DictReader(csv_path.open()))

ep_indices = []
in_ep = False
start_i = 0
for i, r in enumerate(rows):
    st = r["pitch_tracker_state"]
    if st == "acquiring" and not in_ep:
        in_ep = True
        start_i = i
    elif st != "acquiring" and in_ep:
        in_ep = False
        ep_indices.append((start_i, i - 1))
if in_ep:
    ep_indices.append((start_i, len(rows) - 1))

print(f"Found {len(ep_indices)} episodes")
for ep_id, (s_i, e_i) in enumerate(ep_indices):
    r_start = rows[s_i]
    r_end = rows[e_i]
    dur_ms = (int(r_end["sample"]) - int(r_start["sample"]) + 1) * 1000.0 / 48000.0
    print(f"\n=== Episode {ep_id}: {r_start['time_s']}s - {r_end['time_s']}s ({dur_ms:.2f} ms) ===")
    unique_pitch_samples = []
    last_p = None
    for j in range(s_i, e_i + 1):
        p = (rows[j]["raw_pitch_hz"], rows[j]["pitch_confidence"], rows[j]["coherent_marks"], rows[j]["mark_valid"], rows[j]["voiced"])
        if p != last_p:
            unique_pitch_samples.append((rows[j]["time_s"], rows[j]["sample"], *p))
            last_p = p
    for u in unique_pitch_samples:
        print(f"  t={u[0]} raw_f0={float(u[2]):.1f}Hz conf={float(u[3]):.3f} coherent={u[4]} mark_val={u[5]} voiced={u[6]}")
    # Also check the row immediately after e_i (when it locks)
    if e_i + 1 < len(rows):
        r_lock = rows[e_i + 1]
        print(f"  -> LOCK at t={r_lock['time_s']} state={r_lock['pitch_tracker_state']} raw_f0={float(r_lock['raw_pitch_hz']):.1f}Hz conf={float(r_lock['pitch_confidence']):.3f} coherent={r_lock['coherent_marks']} mark_val={r_lock['mark_valid']}")

unl_episodes = []
in_unl = False
start_i = 0
for i, r in enumerate(rows):
    st = r["pitch_tracker_state"]
    if st == "unlocked" and not in_unl:
        in_unl = True
        start_i = i
    elif st != "unlocked" and in_unl:
        in_unl = False
        unl_episodes.append((start_i, i - 1))
if in_unl:
    unl_episodes.append((start_i, len(rows) - 1))

print(f"\nTotal UNLOCKED episodes: {len(unl_episodes)}")
for idx, (s_i, e_i) in enumerate(unl_episodes):
    r_start = rows[s_i]
    r_end = rows[e_i]
    dur_ms = (int(r_end["sample"]) - int(r_start["sample"]) + 1) * 1000.0 / 48000.0
    ep_rows = rows[s_i:e_i+1]
    confs = [float(r["pitch_confidence"]) for r in ep_rows]
    voiced_cnt = sum(r["voiced"] == "1" for r in ep_rows)
    rms_vals = [float(r["input_rms"]) for r in ep_rows]
    print(f"Unl {idx}: {r_start['time_s']}s - {r_end['time_s']}s ({dur_ms:.2f} ms) | conf: [{min(confs):.2f}, {max(confs):.2f}], voiced: {voiced_cnt}/{len(ep_rows)}, max_rms: {max(rms_vals):.4f}")

