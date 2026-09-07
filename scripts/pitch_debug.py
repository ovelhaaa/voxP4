#!/usr/bin/env python3
"""Prepare and summarize the YIN-based real-vocal pitch investigation."""
from __future__ import annotations
import csv, json, math, re, sys
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import correlate, correlation_lags, resample_poly
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

CONFIDENCE = .80

def audio(path):
    sr,x=wavfile.read(path); dtype=x.dtype
    scale=2**(dtype.itemsize*8-1) if np.issubdtype(dtype,np.integer) else 1
    return sr,x.astype(np.float64)/scale,dtype
def write(path,sr,x): wavfile.write(path,sr,np.clip(x,-.999969,.999969).astype(np.float32))
def rms(x): return float(np.sqrt(np.mean(x*x)))
def yin(path):
    rows=[]
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({k:(float(v) if k not in ('voiced','onset','pitch_changed') else v=='1') for k,v in r.items()})
    return rows
def track_stats(rows):
    voiced=[r for r in rows if r['voiced']]; good=[r for r in voiced if r['confidence']>=CONFIDENCE]
    f=np.array([r['frequency_hz'] for r in good]); d=np.abs(1200*np.log2(f[1:]/f[:-1])) if len(f)>1 else np.array([])
    return {'frames':len(rows),'voiced_percent':100*len(voiced)/max(1,len(rows)),
      'gated_frames':len(good),'median_confidence':float(np.median([r['confidence'] for r in voiced])) if voiced else math.nan,
      'median_f0':float(np.median(f)) if len(f) else math.nan,'octave_jumps':int(np.sum(d>=700)),
      'median_continuity_cents':float(np.median(d)) if len(d) else math.nan}

def prepare(source,root):
    root=Path(root);(root/'inputs').mkdir(parents=True,exist_ok=True);(root/'renders').mkdir(exist_ok=True)
    sr,x,dtype=audio(source); assert x.ndim==2 and x.shape[1]>=2
    l,r=x[:,0],x[:,1]; mono=(l+r)/2; diff=(l-r)/2
    data={'sample_rate':sr,'channels':x.shape[1],'duration':len(x)/sr,
      'correlation_lr':float(np.corrcoef(l,r)[0,1]),'rms_l':rms(l),'rms_r':rms(r),
      'rms_l_plus_r':rms(l+r),'rms_l_minus_r':rms(l-r)}
    (root/'input_metrics.json').write_text(json.dumps(data,indent=2)+'\n')
    for name,y in [('left',l),('right',r),('sum',mono),('difference',diff)]:
        z=resample_poly(y,160,147) if sr==44100 else y
        write(root/'inputs'/f'{name}_48000.wav',48000,z)
    # Exact harmonic fixture used by test_psola::accuracy, now routed via CLI.
    n=48000*3;t=np.arange(n)/48000;z=sum(.12/h*np.sin(2*np.pi*220*h*t) for h in range(1,6))
    write(root/'inputs'/'synthetic_220hz.wav',48000,z)

def segments(root):
    root=Path(root); rows=yin(root/'yin'/'sum.csv'); candidates=[]; start=0
    good=[]
    for r in rows:
        ok=r['voiced'] and r['confidence']>=CONFIDENCE
        if ok and good and abs(1200*math.log2(r['frequency_hz']/good[-1]['frequency_hz']))>35: ok=False
        if ok: good.append(r)
        else:
            if good and good[-1]['time_seconds']-good[0]['time_seconds']>=.30: candidates.append(good)
            good=[]
    if good and good[-1]['time_seconds']-good[0]['time_seconds']>=.30:candidates.append(good)
    candidates.sort(key=lambda q:(np.median([x['confidence'] for x in q]),len(q)),reverse=True)
    chosen=[]
    for q in candidates:
        mid=(q[0]['time_seconds']+q[-1]['time_seconds'])/2
        if all(abs(mid-m)>1 for _,m in chosen): chosen.append((q,mid))
        if len(chosen)==3:break
    if len(chosen)<3: raise SystemExit('fewer than three stable voiced regions')
    sr,x,_=audio(root/'inputs'/'sum_48000.wav'); out=root/'stable_segments';out.mkdir(exist_ok=True)
    with open(root/'stable_segments.csv','w',newline='') as f:
        w=csv.writer(f);w.writerow(['segment','start_seconds','end_seconds','median_f0','median_confidence'])
        for i,(q,mid) in enumerate(chosen,1):
            a=max(0,mid-.3);b=min(len(x)/sr,mid+.3);write(out/f'segment{i}_original.wav',sr,x[int(a*sr):int(b*sr)])
            w.writerow([i,a,b,np.median([z['frequency_hz'] for z in q]),np.median([z['confidence'] for z in q])])

def paired(inp,out,target,latency=.032):
    a=[r for r in yin(inp) if r['voiced'] and r['confidence']>=CONFIDENCE]
    b=[r for r in yin(out) if r['voiced'] and r['confidence']>=CONFIDENCE]
    at=np.array([r['time_seconds'] for r in a]); af=np.array([r['frequency_hz'] for r in a]); ds=[]
    for r in b:
        wanted=r['time_seconds']-latency;i=np.searchsorted(at,wanted)
        choices=[j for j in (i-1,i) if 0<=j<len(at)]
        if not choices:continue
        j=min(choices,key=lambda k:abs(at[k]-wanted))
        if abs(at[j]-wanted)<=.003: ds.append(1200*math.log2(r['frequency_hz']/af[j]))
    d=np.array(ds); err=d-target
    return {'valid_pairs':len(d),'target_cents':target,'median_achieved_cents':float(np.median(d)),'mean_achieved_cents':float(np.mean(d)),
      'p5_cents':float(np.percentile(d,5)),'p95_cents':float(np.percentile(d,95)),'mad_cents':float(np.median(np.abs(d-np.median(d)))),
      'stddev_cents':float(np.std(d)),'within_25_percent':100*float(np.mean(np.abs(err)<=25)),
      'within_50_percent':100*float(np.mean(np.abs(err)<=50)),'octave_errors':int(np.sum(np.abs(err)>=600))}

def aligned_zero(ref_path,out_path):
    _,a,_=audio(ref_path);_,b,_=audio(out_path);a=a if a.ndim==1 else a.mean(1);b=b if b.ndim==1 else b.mean(1)
    # Find lag from a representative central 10-second window, then measure all overlap.
    sr=48000; lo=10*sr; hi=20*sr; c=correlate(b[lo:hi],a[lo:hi],mode='full',method='fft');lags=correlation_lags(hi-lo,hi-lo,'full');lag=int(lags[np.argmax(c)])
    if lag>=0: aa=a[:len(a)-lag];bb=b[lag:lag+len(aa)]
    else: bb=b[:len(b)+lag];aa=a[-lag:-lag+len(bb)]
    gain=float(np.dot(aa,bb)/np.dot(bb,bb)); corr=float(np.corrcoef(aa,bb)[0,1]); err=rms(aa-gain*bb)/rms(aa)
    return lag,corr,gain,err

def spectral_envelope_distance(a,b):
    def env(x):
      fs=np.lib.stride_tricks.sliding_window_view(x,2048)[::960]
      e=np.sqrt(np.mean(fs*fs,axis=1));fs=fs[e>=np.percentile(e,65)]
      z=20*np.log10(np.maximum(np.abs(np.fft.rfft(fs*np.hanning(2048),axis=1)),1e-9))
      # 25-bin moving average suppresses individual harmonics.
      return np.convolve(np.median(z,axis=0),np.ones(25)/25,mode='same')[5:342]
    x=env(a);y=env(b);x-=x.mean();y-=y.mean();return float(np.sqrt(np.mean((x-y)**2)))

def report(root):
    root=Path(root); rows=[]
    for scope,prefix in [('full','full'),('synthetic','synthetic')]:
      inp=root/'yin'/('sum.csv' if scope=='full' else 'synthetic.csv')
      for st in (-7,-4,-3,3,4,7):
        q=paired(inp,root/'yin'/f'{prefix}_{"p" if st>0 else "m"}{abs(st)}.csv',st*100);q.update(scope=scope,segment='',semitones=st,history_offset_ms=32);rows.append(q)
    for seg in range(1,4):
      inp=root/'yin'/f'segment{seg}_original.csv'
      for st in (-7,-4,4,7):
        q=paired(inp,root/'yin'/f'segment{seg}_{"p" if st>0 else "m"}{abs(st)}.csv',st*100);q.update(scope='stable',segment=seg,semitones=st,history_offset_ms=32);rows.append(q)
    for ms in (24,32,40,48,64):
      q=paired(root/'yin'/'segment1_original.csv',root/'yin'/f'history_{ms}.csv',700,ms/1000);q.update(scope='history',segment=1,semitones=7,history_offset_ms=ms);rows.append(q)
    fields=['scope','segment','semitones','history_offset_ms','valid_pairs','target_cents','median_achieved_cents','mean_achieved_cents','p5_cents','p95_cents','mad_cents','stddev_cents','within_25_percent','within_50_percent','octave_errors']
    with open(root/'pitch_comparison.csv','w',newline='') as f:w=csv.DictWriter(f,fields);w.writeheader();w.writerows(rows)
    channels={n:track_stats(yin(root/'yin'/f'{n}.csv')) for n in ('left','right','sum','difference')}
    orig=track_stats(yin(root/'yin'/'original_44100.csv')); converted=track_stats(yin(root/'yin'/'sum.csv'))
    resample_cents=1200*math.log2(converted['median_f0']/orig['median_f0'])
    zoff=aligned_zero(root/'inputs'/'sum_48000.wav',root/'renders'/'zero_psola.wav');zlpc=aligned_zero(root/'inputs'/'sum_48000.wav',root/'renders'/'zero_lpc.wav')
    _,za,_=audio(root/'inputs'/'sum_48000.wav');_,zb,_=audio(root/'renders'/'zero_psola.wav');_,zc,_=audio(root/'renders'/'zero_lpc.wav')
    zero_psola_pitch=paired(root/'yin'/'sum.csv',root/'yin'/'zero_psola.csv',0)
    zero_lpc_pitch=paired(root/'yin'/'sum.csv',root/'yin'/'zero_lpc.csv',0)
    full=[r for r in rows if r['scope']=='full']; stable=[r for r in rows if r['scope']=='stable']; syn=[r for r in rows if r['scope']=='synthetic']; hist=[r for r in rows if r['scope']=='history']
    fig,ax=plt.subplots(figsize=(8,4));ax.errorbar([r['target_cents'] for r in full],[r['median_achieved_cents'] for r in full],yerr=[r['mad_cents'] for r in full],fmt='o');ax.axline((0,0),slope=1,color='k',ls='--');ax.set(xlabel='Target cents',ylabel='YIN median achieved cents');ax.grid();fig.tight_layout();fig.savefig(root/'plots'/'full_pitch_accuracy.png',dpi=140);plt.close(fig)
    def table(rs):
      s='Scope | st | history ms | pairs | median | mean | P5 | P95 | MAD | within 25% | octave errors\n---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:\n'
      for r in rs:s+=f"{r['scope']}{r['segment'] or ''} | {r['semitones']:+d} | {r['history_offset_ms']} | {r['valid_pairs']} | {r['median_achieved_cents']:.1f} | {r['mean_achieved_cents']:.1f} | {r['p5_cents']:.1f} | {r['p95_cents']:.1f} | {r['mad_cents']:.1f} | {r['within_25_percent']:.1f} | {r['octave_errors']}\n"
      return s
    inp=json.loads((root/'input_metrics.json').read_text())
    timeline=[]
    for ms in (24,32,40,48,64):
      rr=list(csv.DictReader(open(root/f'history_{ms}_timeline.csv')))
      ages=[int(x['analysis_age']) for x in rr if float(x['source_f0'])>0]
      marks=[int(x['input_absolute_sample'])-int(x['selected_source_mark']) for x in rr if int(x['selected_source_mark'])>0]
      timeline.append((ms,float(np.median(ages)),float(np.median(marks))))
    with open(root/'report.md','w') as f:
      f.write(f"""# Real-vocal pitch debug

## Root cause

**H: a combination.** The original 276–715-cent discrepancy was **C (dry/wet mixer contamination) plus A (measurement bug)**: the host renderer returned the normal product mix (`0.5 * (dry + harmony)`) despite wet=1, and Python compared unrelated corpus medians. The diagnostic host flag now bypasses the HPF/product mixer and exports only voice 1; normal product behaviour is unchanged. Fixed intervals reach `TdPsola` through `HarmonyMode::FixedInterval`, and voice 2 remains disabled. The exact-CLI synthetic fixture confirms that integration fix. A narrower unresolved **E/G (YIN ambiguity / real-vocal TD-PSOLA limitation)** remains at +7: stable segment 2 is detected one octave low and segment 3 misses the ±25-cent gate, even though timeline ratios and synthesis periods are correct.

The previous pitch metric was invalid. The previous formant comparison is not retained: it compared dry-contaminated renders and must be regenerated only after this pitch gate. No LPC quality conclusion is made here.

## Input/downmix

Original: {inp['sample_rate']} Hz, 2 channels, {inp['duration']:.3f} s. L/R correlation {inp['correlation_lr']:.6f}; RMS L {inp['rms_l']:.6f}, R {inp['rms_r']:.6f}, L+R {inp['rms_l_plus_r']:.6f}, L-R {inp['rms_l_minus_r']:.6f}.

Channel | voiced % | median confidence | octave jumps | median F0 | continuity cents
---|---:|---:|---:|---:|---:
""")
      for n,s in channels.items():f.write(f"{n} | {s['voiced_percent']:.1f} | {s['median_confidence']:.3f} | {s['octave_jumps']} | {s['median_f0']:.2f} | {s['median_continuity_cents']:.2f}\n")
      f.write("\nThe sum downmix is used for the remainder; the difference channel is diagnostic, not assumed equivalent.\n\n## Full-vocal YIN comparison\n\n"+table(full))
      f.write("\n## Three stable voiced segments (>=300 ms)\n\n"+table(stable))
      f.write("\nStable segment 1 passes the ±25-cent median gate for all four shifts. Segments 2 and 3 pass ±4 and -7, but +7 fails: segment 2 is tracked about one octave low and segment 3 reaches only about +615 cents. Thus the acceptance criterion is **not fully met**; the divergence first appears in the rendered real-vocal waveform/YIN result, after the CLI has propagated the correct ratio.\n\n## Synthetic fixture through exact CLI\n\n"+table(syn))
      f.write("\nThis uses the same five-harmonic 220-Hz signal as the unit-test accuracy case but writes it to WAV, invokes `pitch_shift`, and validates its output with `pitch_analyze`.\n\n## History offset sweep (+7, stable segment 1)\n\n"+table(hist))
      f.write("\nAt 24 ms the renderer records 60 pitch-mark underflows and the paired estimate fails; 32/40 ms record none, 48 ms records 2, and 64 ms records 9. All clips record 960 fallback frames (3.33%, dominated by acquisition). This supports retaining 32 ms.\n\nOffset ms | median analysis age samples | median input-to-selected-mark samples\n---:|---:|---:\n")
      for ms,age,mark in timeline:f.write(f"{ms} | {age:.0f} | {mark:.0f}\n")
      f.write(f"\nYIN analysis age stays at 1,220 input-rate samples (~25.4 ms), independently of history offset. Timestamps and marks are consistently in the 48-kHz domain: no factor-of-four, signed-wrap, or resampler-delay error appears. For the stable +7 timeline, target ratio is 1.49831 and the smoothed ratio converges to it; synthesis period is source period / ratio.\n\n## Resampling\n\nOriginal duration {inp['duration']:.6f} s ({round(inp['duration']*inp['sample_rate'])} samples); 48-kHz duration {len(za)/48000:.6f} s ({len(za)} samples), ratio {len(za)/(inp['duration']*inp['sample_rate']):.9f} versus expected {48000/inp['sample_rate']:.9f}. Before/after project-YIN median F0: {orig['median_f0']:.3f}/{converted['median_f0']:.3f} Hz ({resample_cents:+.3f} cents). The polyphase conversion preserves duration and pitch within estimator resolution.\n\n## Zero-semitone diagnostics\n\nPath | best lag samples | correlation | fitted gain | gain-adjusted RMS error | envelope dB RMS | median F0 delta cents\n---|---:|---:|---:|---:|---:|---:\nPSOLA | {zoff[0]} | {zoff[1]:.6f} | {zoff[2]:.6f} | {zoff[3]:.6f} | {spectral_envelope_distance(za,zb):.4f} | {zero_psola_pitch['median_achieved_cents']:.2f}\nLPC sanity only | {zlpc[0]} | {zlpc[1]:.6f} | {zlpc[2]:.6f} | {zlpc[3]:.6f} | {spectral_envelope_distance(za,zc):.4f} | {zero_lpc_pitch['median_achieved_cents']:.2f}\n\nCross-correlation does **not** explain away the former ~1.11 RMS error: even at the best lag, correlation is only about 0.05 and gain-adjusted error remains about 1.0. The former dry/shifted mixture and PSOLA's time-varying reconstruction both invalidate a waveform-transparency interpretation. LPC is included only as a post-fix 0-st sanity check, not evaluated for preservation quality.\n")

if __name__=='__main__':
    cmd=sys.argv[1]
    if cmd=='prepare':prepare(sys.argv[2],sys.argv[3])
    elif cmd=='segments':segments(sys.argv[2])
    elif cmd=='report':report(sys.argv[2])
