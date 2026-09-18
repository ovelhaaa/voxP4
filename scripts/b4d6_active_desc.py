import re, sys

def parse(path, name):
    recs = []
    with open(path, errors='replace') as f:
        for line in f:
            if line.startswith('B4D5_PAIRED') and ('name=' + name) in line:
                recs.append(dict(re.findall(r'(\w+)=([^\s]+)', line)))
    return recs

for tag, path in [('single', 'artifacts/alpha01d/b4d6_single_raw.txt'),
                  ('noprof', 'artifacts/alpha01d/b4d6_noprof_raw.txt')]:
    recs = parse(path, '1v_fullfx_44100_60s')
    if not recs:
        print(tag, 'no paired')
        continue
    buckets = {}
    for r in recs:
        if int(r['mc']) != 0 or int(r['ng']) != 0:
            continue
        k = min(int(r['ad']), 4)
        buckets.setdefault(k, []).append(int(r['dsp_us']))
    print(f"== {tag} NMC+0 active_desc buckets ==")
    for k in sorted(buckets):
        v = sorted(buckets[k]); n = len(v)
        avg = sum(v) / n
        p99 = v[min(n - 1, int((n - 1) * 0.99))]
        print(f"  ad={k}: n={n} avg={avg:.1f} p99={p99} max={v[-1]}")
