import re, statistics

def kv(l):
    return dict(re.findall(r"(\w+)=([^\s]+)", l))

rows = []
for f in ["artifacts/alpha01d/b4d11_raw.txt", "artifacts/alpha01d/b4d11_raw2.txt"]:
    try:
        lines = open(f, errors="replace").read().splitlines()
    except FileNotFoundError:
        continue
    for l in lines:
        if not l.startswith("B4D8_ORD"):
            continue
        d = kv(l)
        if not d.get("name", "").startswith("b4d11_s0_fullfx_44100_r0"):
            continue
        debt = int(d.get("debt", 0))
        op = float(d.get("op", 0.0))
        ng = 3 if float(d.get("g2_wp", 0.0)) > 0 or float(d.get("g2_sel", 0.0)) > 0 else 2
        rows.append((ng, debt, op, int(d.get("mc", 0))))

print("burst rows:", len(rows))
for ng in (2, 3):
    v = [r for r in rows if r[0] == ng]
    if not v:
        continue
    debts = [r[1] for r in v]
    ops = [r[2] for r in v]
    print(f"grains={ng}: n={len(v)} debt_min={min(debts)} debt_max={max(debts)} "
          f"debt_med={statistics.median(debts):.0f} op_med={statistics.median(ops):.0f} "
          f"debt/op_med={statistics.median([r[1]/r[2] for r in v if r[2]>0]):.2f}")
print("sample (grains, debt, op, mark_count):")
for r in rows[:15]:
    print(" ", r)
