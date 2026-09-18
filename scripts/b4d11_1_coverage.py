import re, math, csv, os, statistics

OD = "artifacts/alpha01d"

def kv(l):
    return dict(re.findall(r"(\w+)=([^\s]+)", l))

# Collect burst blocks (fullfx) with per-grain geometry.
bursts = []
for f in ["artifacts/alpha01d/b4d11_1_raw.txt",
          "artifacts/alpha01d/b4d11_1_raw2.txt"]:
    if not os.path.exists(f):
        continue
    for l in open(f, errors="replace").read().splitlines():
        if not l.startswith("B4D8_ORD"):
            continue
        d = kv(l)
        if not d.get("name", "").startswith("1v_fullfx"):
            continue
        ng = 3 if (float(d.get("g2_mk", 0)) > 0 or float(d.get("g2_wp", 0)) > 0) else 2
        grains = []
        for i in range(ng):
            m = re.search(r"g%d=(-?\d+),(\d+)" % i, l)
            if not m:
                continue
            grains.append((int(m.group(1)), int(m.group(2))))
        bursts.append((int(d.get("blk", 0)), ng, int(d.get("debt", 0)),
                       float(d.get("op", 0.0)), grains))

def hann(off, half):
    # production window: index = (off+half)*(2047)/(2*half) into a Hann table
    if half <= 0 or off < -half or off > half:
        return 0.0
    t = (off + half) / (2.0 * half)
    return 0.5 * (1.0 - math.cos(2.0 * math.pi * t))

# Unique coverage per grain + OLA weight
support_rows = []
ucov_rows = []
ola_rows = []
for blk, ng, debt, op, grains in bursts:
    # region spans first grain start to last grain end
    lo = min(c - h for c, h in grains)
    hi = max(c + h for c, h in grains)
    # weight contributions
    weights = []  # per sample: list of (grain_idx, weight)
    for n in range(lo, hi + 1):
        w = []
        for gi, (c, h) in enumerate(grains):
            ww = hann(n - c, h)
            if ww > 1e-4:
                w.append((gi, ww))
        weights.append((n, w))
    # unique support (window > -60 dB ~ 1e-3)
    THRESH = 1e-3
    for gi, (c, h) in enumerate(grains):
        uniq = 0
        wuniq = 0.0
        for n, w in weights:
            if n < c - h or n > c + h:
                continue
            own = hann(n - c, h)
            if own <= 0:
                continue
            others = [ww for gj, ww in w if gj != gi and ww > THRESH]
            if not others and own > THRESH:
                uniq += 1
                wuniq += own
        support_rows.append([blk, ng, gi, c, h, c - h, c + h, 2 * h + 1,
                             uniq, round(uniq / (2 * h + 1), 4),
                             round(wuniq, 1)])
        ucov_rows.append([blk, ng, gi, uniq, round(wuniq, 1),
                          "ESSENTIAL" if uniq > 32 else
                          ("REDUNDANT" if uniq == 0 else "NEAR_REDUNDANT")])
    # OLA weight sum curve
    ws = [sum(ww for _, ww in w) for _, w in weights]
    if ws:
        sw = sorted(ws)
        ola_rows.append([blk, ng, debt, round(op, 1), len(ws),
                         round(min(ws), 3), round(sw[int(0.01*(len(sw)-1))], 3),
                         round(statistics.mean(ws), 3), round(max(ws), 3),
                         sum(1 for x in ws if x <= 0.0)])

with open(os.path.join(OD, "b4d11_1_grain_support.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "block_grains", "grain_idx", "dest_center", "half",
                "support_start", "support_end", "support_len",
                "unique_samples", "unique_fraction", "unique_weight"])
    w.writerows(support_rows)

with open(os.path.join(OD, "b4d11_1_unique_coverage.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "block_grains", "grain_idx", "unique_samples",
                "unique_weight", "class"])
    w.writerows(ucov_rows)

with open(os.path.join(OD, "b4d11_1_ola_reference.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "block_grains", "debt", "output_period", "samples",
                "min_weight", "p1_weight", "mean_weight", "max_weight",
                "zero_support_samples"])
    w.writerows(ola_rows)

# Summary classification by grain index
print("bursts:", len(bursts))
for gi in (0, 1, 2):
    v = [r for r in ucov_rows if r[2] == gi]
    if not v:
        continue
    ess = sum(1 for r in v if r[5] == "ESSENTIAL")
    red = sum(1 for r in v if r[5] == "REDUNDANT")
    near = sum(1 for r in v if r[5] == "NEAR_REDUNDANT")
    med = statistics.median([r[3] for r in v])
    print(f"grain{gi}: n={len(v)} ESSENTIAL={ess} REDUNDANT={red} "
          f"NEAR={near} median_unique_samples={med}")

# R2 simulation: drop the middle grain(s) that are REDUNDANT; recompute OLA
r2_rows = []
for blk, ng, debt, op, grains in bursts:
    if ng < 3:
        continue
    lo = min(c - h for c, h in grains)
    hi = max(c + h for c, h in grains)
    def weight_min(idx_keep):
        mn = 1e9
        for n in range(lo, hi + 1):
            s = 0.0
            for gi in idx_keep:
                c, h = grains[gi]
                s += hann(n - c, h)
            if s < mn:
                mn = s
        return mn
    ref_min = weight_min(range(len(grains)))
    # drop grain1 (middle) only if it had zero unique samples
    uniq1 = next((r[3] for r in ucov_rows if r[0] == blk and r[2] == 1), None)
    drop_ok = uniq1 == 0
    if drop_ok:
        keep = [0, 2]
        r2_min = weight_min(keep)
        r2_rows.append([blk, ng, 2, round(ref_min, 3), round(r2_min, 3),
                        round(r2_min / ref_min, 3) if ref_min > 0 else 0.0])
with open(os.path.join(OD, "b4d11_1_ola_r2.csv"), "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["blk", "r0_grains", "r2_grains", "r0_min_weight",
                "r2_min_weight", "r2_over_r0_min"])
    w.writerows(r2_rows)
print("R2 sim rows (drop middle):", len(r2_rows))
if r2_rows:
    print("median r2/r0 min weight:",
          statistics.median([r[5] for r in r2_rows]))
