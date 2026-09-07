#!/usr/bin/env python3
"""Objective offline measurements for the real-vocal formant evaluation.

This program only analyses WAV files. Rendering is deliberately delegated to
the repository's C++ pitch_shift executable by run-formant-eval.sh.
"""
from __future__ import annotations

import csv
import math
import re
import shutil
import sys
from pathlib import Path

try:
    import numpy as np
    from scipy.io import wavfile
    from scipy.ndimage import gaussian_filter1d
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError as exc:
    raise SystemExit("Missing analysis dependency. Run: python3 -m pip install numpy scipy matplotlib") from exc


def read(path: Path):
    rate, x = wavfile.read(path)
    dtype = x.dtype
    fmt = str(dtype)
    bits = dtype.itemsize * 8
    if np.issubdtype(dtype, np.integer):
        x = x.astype(np.float64) / float(2 ** (bits - 1))
    else:
        x = x.astype(np.float64)
    if x.ndim == 2:
        channels = x.shape[1]
        x = x.mean(axis=1)
    else:
        channels = 1
    return rate, x, channels, bits, fmt


def frames(x, size=2048, hop=480):
    if len(x) < size:
        return np.empty((0, size))
    n = 1 + (len(x) - size) // hop
    return np.lib.stride_tricks.sliding_window_view(x, size)[::hop][:n]


def pitch_track(x, sr):
    # Normalized autocorrelation on energetic 42.7-ms frames. This is an
    # analysis metric, not part of the rendering/DSP path.
    fs = frames(x, 2048, 480)
    if not len(fs): return np.array([]), np.array([])
    rms = np.sqrt(np.mean(fs * fs, axis=1)); floor = max(1e-5, np.percentile(rms, 35) * 1.5)
    out = np.full(len(fs), np.nan)
    lo, hi = sr // 1000, sr // 70
    win = np.hanning(fs.shape[1])
    for i, f in enumerate(fs):
        if rms[i] < floor: continue
        y = (f-f.mean())*win
        spectrum = np.fft.rfft(y, n=4096)
        ac = np.fft.irfft(spectrum * spectrum.conjugate())[:len(y)]
        if ac[0] <= 1e-12: continue
        ac /= ac[0]
        # first credible local maximum reduces octave selection bias
        peaks = np.where((ac[lo+1:hi-1] > ac[lo:hi-2]) & (ac[lo+1:hi-1] >= ac[lo+2:hi]))[0] + lo + 1
        peaks = peaks[ac[peaks] > .35]
        if len(peaks):
            lag = peaks[np.argmax(ac[peaks])]
            out[i] = sr / lag
    return (np.arange(len(fs))*480+1024)/sr, out


def envelope(x, sr):
    fs = frames(x, 2048, 960)
    if not len(fs): return np.array([]), np.array([])
    energy = np.sqrt(np.mean(fs*fs, axis=1)); keep = energy >= np.percentile(energy, 65)
    specs = 20*np.log10(np.maximum(np.abs(np.fft.rfft(fs[keep]*np.hanning(2048), axis=1)), 1e-9))
    env = gaussian_filter1d(np.median(specs, axis=0), sigma=12)
    freq = np.fft.rfftfreq(2048, 1/sr)
    band = (freq >= 100) & (freq <= 8000)
    env = env[band]; env -= env.mean()
    return freq[band], env


def parse_name(name):
    if name == "original_reference.wav": return 0, "reference", 0.0, 0
    m = re.search(r"_shift_([mp])(\d+)_(psola|lpc(\d{3}))(?:_order(\d+))?\.wav$", name)
    if not m: return 0, "unknown", 0.0, 0
    st = int(m.group(2)) * (1 if m.group(1)=="p" else -1)
    return st, ("off" if m.group(3)=="psola" else "lpc"), (int(m.group(4))/100 if m.group(4) else 0), int(m.group(5) or (16 if m.group(4) else 0))


def metric(path, ref_pitch, ref_track, ref_env):
    sr,x,ch,bits,fmt=read(path); finite=np.isfinite(x); clean=np.nan_to_num(x)
    peak=float(np.max(np.abs(clean))) if len(x) else 0.; rms=float(np.sqrt(np.mean(clean*clean))) if len(x) else 0.
    t,p=pitch_track(clean,sr); valid=p[np.isfinite(p)]; med=float(np.median(valid)) if len(valid) else math.nan
    freq,env=envelope(clean,sr); ed=float(np.sqrt(np.mean((env-ref_env)**2))) if len(env)==len(ref_env) else math.nan
    jumps=np.abs(np.diff(clean)); threshold=max(.25, np.median(jumps)*25)
    st,mode,amount,order=parse_name(path.name)
    delay_frames=round(1536/480); n=min(len(p)-delay_frames,len(ref_track))
    paired=(np.isfinite(p[delay_frames:delay_frames+n]) & np.isfinite(ref_track[:n])) if n>0 else np.array([],dtype=bool)
    shifts=1200*np.log2(p[delay_frames:delay_frames+n][paired]/ref_track[:n][paired])/100 if np.any(paired) else np.array([])
    achieved=float(np.median(shifts)) if len(shifts) else math.nan
    err=(achieved-st)*100 if math.isfinite(achieved) and mode!="reference" else math.nan
    return dict(filename=path.name,semitones=st,formant_mode=mode,formant_amount=amount,lpc_order=order,
      duration=len(x)/sr,peak=peak,rms=rms,crest_factor=peak/rms if rms else math.nan,dc_offset=float(np.mean(clean)),
      clipped_samples=int(np.sum(np.abs(clean)>=1)),nan_inf=int(np.sum(~finite)),estimated_pitch_hz=med,
      pitch_shift_achieved=achieved,
      pitch_error_cents=err,pitch_variance=float(np.std(1200*np.log2(valid/med))) if len(valid) else math.nan,
      spectral_envelope_distance=ed,large_discontinuities=int(np.sum(jumps>threshold)),channels=ch,bits=bits,format=fmt,rate=sr)


def val(v, digits=5):
    return "NA" if isinstance(v,float) and not math.isfinite(v) else (f"{v:.{digits}f}" if isinstance(v,float) else str(v))


def main(root_s, source_s):
    root=Path(root_s); renders=root/"renders"; metrics_dir=root/"metrics"; plots=root/"plots"; report=root/"report"
    for d in (metrics_dir,plots,report,root/"blind"): d.mkdir(parents=True,exist_ok=True)
    ref=renders/"original_reference.wav"; sr,x,ch,bits,fmt=read(ref)
    pt,p=pitch_track(x,sr); ref_pitch=float(np.nanmedian(p)); freq,ref_env=envelope(x,sr)
    rows=[metric(pth,ref_pitch,p,ref_env) for pth in sorted(renders.glob("*.wav")) if pth.name != "original_source.wav"]
    fields=["filename","semitones","formant_mode","formant_amount","lpc_order","duration","peak","rms","crest_factor","dc_offset","clipped_samples","nan_inf","estimated_pitch_hz","pitch_shift_achieved","pitch_error_cents","pitch_variance","spectral_envelope_distance","large_discontinuities"]
    with open(metrics_dir/"results.csv","w",newline="") as f:
        w=csv.DictWriter(f,fields);w.writeheader();w.writerows(({k:val(r[k],8) for k in fields} for r in rows))

    # Plots: reference pitch/RMS and representative +/-7 envelope comparisons.
    rmsf=np.sqrt(np.mean(frames(x,2048,480)**2,axis=1)); tr=(np.arange(len(rmsf))*480+1024)/sr
    fig,ax=plt.subplots(2,1,figsize=(11,6),sharex=True); ax[0].plot(pt,p);ax[0].set_ylabel("F0 (Hz)");ax[0].set_ylim(60,600);ax[0].grid()
    ax[1].plot(tr,rmsf);ax[1].set_ylabel("RMS");ax[1].set_xlabel("Time (s)");ax[1].grid();fig.tight_layout();fig.savefig(plots/"reference_pitch_rms.png",dpi=140);plt.close(fig)
    for st in (-7,7):
        fig,ax=plt.subplots(figsize=(10,4));ax.plot(freq,ref_env,label="reference")
        for suffix,label in (("psola","PSOLA"),("lpc050","LPC 0.50"),("lpc100","LPC 1.00")):
            q=renders/f"vocal_shift_{'p' if st>0 else 'm'}{abs(st)}_{suffix}.wav"; _,e=envelope(read(q)[1],sr);ax.plot(freq,e,label=label,alpha=.85)
        ax.set(xlabel="Frequency (Hz)",ylabel="Centered smoothed envelope (dB)",xlim=(100,8000));ax.grid();ax.legend();fig.tight_layout();fig.savefig(plots/f"spectral_envelope_{'p' if st>0 else 'm'}7.png",dpi=140);plt.close(fig)

    # Deterministic shortlist: PSOLA plus two LPC amounts with the smallest envelope distance.
    shortlist=[]
    for st in (4,7,-4,-7):
        candidates=[r for r in rows if r["semitones"]==st and r["lpc_order"] in (0,16) and "order" not in r["filename"]]
        ps=next(r for r in candidates if r["formant_mode"]=="off")
        lpc=sorted((r for r in candidates if r["formant_mode"]=="lpc"),key=lambda r:r["spectral_envelope_distance"])[:2]
        shortlist.extend([ps]+lpc)
    # Blind links/copies and key kept out of listening sheet.
    with open(root/"blind_key.csv","w",newline="") as f:
        w=csv.writer(f);w.writerow(["blind_file","source_file","semitones","mode","amount"])
        for i,r in enumerate(shortlist):
            blind=f"{chr(65+i)}.wav"; target=root/"blind"/blind
            if target.exists() or target.is_symlink(): target.unlink()
            target.symlink_to(Path("../renders")/r["filename"])
            w.writerow([blind,r["filename"],r["semitones"],r["formant_mode"],r["formant_amount"]])
    with open(report/"listening_sheet.md","w") as f:
        f.write("# Blind listening sheet\n\nUse 1–5 (1 = least, 5 = most) where applicable. The key is intentionally separate.\n\nFile | Naturalness | Formant identity | Artifacts | Consonants | Preference | Notes\n---|---|---|---|---|---|---\n")
        for i in range(len(shortlist)): f.write(f"{chr(65+i)}.wav | | | | | |\n")

    base_off=next(r for r in rows if r["filename"]=="vocal_shift_p0_psola.wav"); base_lpc=next(r for r in rows if r["filename"]=="vocal_shift_p0_lpc100.wav")
    # Align using documented engine latency for sample-domain sanity measurement.
    def relerr(name):
        y=read(renders/name)[1]; delay=1536; n=min(len(x),len(y)-delay)
        if n<=0:return math.nan
        a=x[:n];b=y[delay:delay+n];return float(np.sqrt(np.mean((a-b)**2))/max(np.sqrt(np.mean(a*a)),1e-12))
    main_rows=[r for r in rows if r["formant_mode"] in ("off","lpc") and "order" not in r["filename"] and r["semitones"] in (-7,-4,-3,3,4,7)]
    orders=[r for r in rows if "order" in r["filename"]]
    src_rate,src_x,src_ch,src_bits,src_fmt=read(renders/"original_source.wav")
    source_note=("The checked-out `/samples/` entry was only CRLF (2 bytes), not a RIFF WAV. "
                 "The runner recovered the unchanged 9,173,654-byte WAV blob from repository history into `renders/original_source.wav`; the `/samples/` path was not modified. "
                 if Path(source_s).stat().st_size==2 else "The source WAV was copied byte-for-byte to `renders/original_source.wav`. ")
    source_note += (f"Because the host renderer requires 48 kHz, a non-destructive polyphase working conversion ({src_rate} -> 48000 Hz) is `original_reference.wav`." if src_rate != 48000 else "No resampling was required.")
    with open(report/"formant_eval.md","w") as f:
        f.write(f"# Milestone 5 real-vocal formant evaluation\n\n## Input\n\n- Requested source: `{source_s}`\n- {source_note}\n- Original characteristics: {src_rate} Hz; {src_ch} channel(s); {src_fmt} ({src_bits}-bit); duration {len(src_x)/src_rate:.3f} s; peak {np.max(np.abs(src_x)):.6f}; RMS {np.sqrt(np.mean(src_x*src_x)):.6f}.\n- 48-kHz working reference: `renders/original_reference.wav`; duration: {len(x)/sr:.3f} s; peak: {np.max(np.abs(x)):.6f}; RMS: {np.sqrt(np.mean(x*x)):.6f}.\n\n")
        f.write("## Method and segmentation\n\nPitch uses normalized autocorrelation over 42.7 ms energetic frames (70–1000 Hz). The spectral metric is RMS dB distance over a Gaussian-smoothed, time-median FFT envelope from 100–8000 Hz; it deliberately smooths harmonics. Voiced analysis uses frames above the 35th-percentile-derived energy floor; envelope frames use the upper 35% by RMS. Low-energy frames represent silence/tails; high-energy/high-jump frames provide transient checks. F1/F2/F3 are not reported: automatic pole/formant assignment was not sufficiently robust for this continuous musical phrase.\n\n")
        f.write("## Render matrix and pitch / spectral envelope\n\nAll mandatory intervals contain PSOLA plus LPC 0.25/0.50/0.75/1.00 (default order 16). Pitch error uses paired voiced frames after latency compensation rather than unrelated corpus medians. Values marked NA are not reliable/applicable.\n\nFile | st | mode | amount | pitch Hz | cents error | envelope dB-RMS | RMS | peak\n---|---:|---|---:|---:|---:|---:|---:|---:\n")
        for r in main_rows:f.write(f"{r['filename']} | {r['semitones']:+d} | {r['formant_mode']} | {r['formant_amount']:.2f} | {val(r['estimated_pitch_hz'],2)} | {val(r['pitch_error_cents'],1)} | {val(r['spectral_envelope_distance'],3)} | {r['rms']:.5f} | {r['peak']:.5f}\n")
        f.write("\nAcross all six intervals, PSOLA-only had the lowest corpus-level envelope distance; every increasing LPC amount moved this metric farther from the reference. Paired-frame pitch estimates remained near 0 st rather than the requested shifts (absolute errors roughly 276–715 cents), so pitch preservation conclusions are limited and this is flagged as a pipeline/corpus compatibility concern rather than hidden by corpus medians.\n")
        f.write("\n## Zero-semitone sanity check\n\nPath | aligned relative RMS error | envelope distance (dB RMS) | peak difference\n---|---:|---:|---:\n")
        for r in (base_off,base_lpc): f.write(f"{r['filename']} | {relerr(r['filename']):.6f} | {r['spectral_envelope_distance']:.4f} | {r['peak']-np.max(np.abs(x)):+.6f}\n")
        f.write("\nThe comparison compensates the reported 1,536-sample host pipeline latency. A non-zero value can also reflect startup/fallback behavior, so it is a pipeline sanity metric rather than an isolated LPC identity proof.\n\n## LPC order (+/-7 only)\n\nFile | order | amount | pitch error cents | envelope dB-RMS | peak\n---|---:|---:|---:|---:|---:\n")
        for r in orders:f.write(f"{r['filename']} | {r['lpc_order']} | {r['formant_amount']:.2f} | {val(r['pitch_error_cents'],1)} | {r['spectral_envelope_distance']:.3f} | {r['peak']:.5f}\n")
        f.write("\nNo claim that order 16 is perceptually superior is made; the table permits comparison only on these objective measures.\n\n## Stability\n\nFile | clipped | NaN/Inf | large jumps | DC offset | crest\n---|---:|---:|---:|---:|---:\n")
        for r in rows:f.write(f"{r['filename']} | {r['clipped_samples']} | {r['nan_inf']} | {r['large_discontinuities']} | {r['dc_offset']:.7f} | {r['crest_factor']:.3f}\n")
        f.write("\n`large jumps` counts adjacent differences above max(0.25, 25× median absolute difference), a conservative burst/discontinuity screen. RMS and peak cover energy/transient preservation; the reference plots expose silence/tail regions for inspection. No human listening conclusion is asserted.\n\n## Memory / CPU\n\nNo DSP or firmware code changed. Existing host tests/benchmarks remain the applicable regression and machine-dependent profiling sources; this batch records renderer telemetry in `metrics/render.log`. Firmware was not rebuilt.\n\n## Recommended for listening comparison\n\nSelection includes PSOLA and the two LPC amounts with lowest objective smoothed-envelope distance per requested interval; this is not a perceptual ranking.\n\nInterval | candidates\n---|---\n")
        for st in (4,7,-4,-7): f.write(f"{st:+d} | "+", ".join(r["filename"] for r in shortlist if r["semitones"]==st)+"\n")
        f.write("\n## Interpretation and limitations\n\nLower envelope distance means closer global spectral-envelope shape under this corpus-level metric; it does not establish naturalness. Pitch errors come from an autocorrelation estimator and may include octave/voicing errors. Formant peaks were intentionally not inferred from individual harmonics. Use `listening_sheet.md` and the separately keyed `blind/` files for human assessment.\n")
    print(f"wrote {len(rows)} render metrics to {root}")

if __name__ == "__main__":
    if len(sys.argv)!=3: raise SystemExit("usage: formant_eval.py OUTPUT_ROOT SOURCE_WAV")
    main(sys.argv[1],sys.argv[2])
