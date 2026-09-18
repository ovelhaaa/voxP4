#!/usr/bin/env python3
"""Parse a B4D.12 burn-in serial capture into the milestone artifacts.

Usage:
    python scripts/analyze_b4d12.py RAW.txt OUTDIR

Produces:
    b4d12_qualification_matrix.csv
    b4d12_recovery_events.csv
    b4d12_lateness_histogram.csv
    b4d12_soak_windows.csv
    b4d12_memory_stability.csv
    b4d12_fifo_stability.csv
"""
import csv
import os
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
KV = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")


def kv(line):
    out = {}
    for m in KV.finditer(line):
        out[m.group(1)] = m.group(2)
    return out


def num(d, key, default=0):
    v = d.get(key)
    if v is None or v == "NA":
        return default
    try:
        if "." in v:
            return float(v)
        return int(v)
    except ValueError:
        return default


def parse(path):
    cases = []
    cur = None
    pending_stack = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = ANSI.sub("", raw).strip()
            if not line:
                continue
            tag = line.split(" ", 1)[0]
            if tag == "B4D1_STACK":
                # Printed immediately before the case's B4D12_CASE line.
                pending_stack = kv(line)
                continue
            if tag == "B4D12_CASE":
                d = kv(line)
                cur = {
                    "name": d.get("name", "?"),
                    "case": d,
                    "events": [],
                    "windows": [],
                    "memory": [],
                    "stack": [pending_stack] if pending_stack else [],
                    "qualify": {},
                    "dsp": {},
                    "late": {},
                    "hist": {},
                    "cls": {},
                    "fifo": {},
                    "backlog": {},
                    "physical": {},
                    "transport": {},
                }
                pending_stack = None
                cases.append(cur)
                continue
            if cur is None:
                continue
            d = kv(line)
            if tag == "B4D12_PHYSICAL":
                cur["physical"] = d
            elif tag == "B4D12_TRANSPORT":
                cur["transport"] = d
            elif tag == "B4D12_DSP":
                cur["dsp"] = d
            elif tag == "B4D12_LATE":
                cur["late"] = d
            elif tag == "B4D12_LATENESS_HIST":
                cur["hist"] = d
            elif tag == "B4D12_CLASS":
                cur["cls"] = d
            elif tag == "B4D12_BACKLOG":
                cur["backlog"] = d
            elif tag == "B4D12_FIFO":
                cur["fifo"] = d
            elif tag == "B4D12_QUALIFY":
                cur["qualify"] = d
            elif tag == "B4D12_LATE_EVENT":
                cur["events"].append(d)
            elif tag == "B4D12_WINDOW":
                cur["windows"].append(d)
            elif tag == "B4D1_MEMORY":
                cur["memory"].append(d)
    return cases


def event_class(e):
    """MC class from model_changed + new_grains (instrumentation-independent).

    0=NMC, 1=MC+1, 2=MC+2, 3=MC+3+, 4=MC+0.
    """
    mc = int(e.get("mc", 0))
    ng = num(e, "ng")
    if not mc:
        return 0
    if ng >= 3:
        return 3
    if ng == 0:
        return 4
    return ng


def write_matrix(cases, outdir):
    path = os.path.join(outdir, "b4d12_qualification_matrix.csv")
    cols = ["test", "duration_s", "input_type", "f0_range", "harmony_range",
            "controls_active", "full_fx", "blocks", "dsp_avg", "dsp_p99",
            "dsp_p999", "dsp_max", "dsp_misses", "misses_per_s", "mc1_misses",
            "mc2_misses", "mc3_misses", "nmc_misses", "max_consecutive_late",
            "max_lateness_us", "backlog_avg_ms", "backlog_p95_ms",
            "backlog_p99_ms", "backlog_max_ms", "rx_overruns", "tx_underruns",
            "dma_errors", "fifo_drops", "seq_gaps", "nan_inf", "effective_fs",
            "result"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            ce = c["case"]
            d = c["dsp"]
            tr = c["transport"]
            cl = c["cls"]
            bl = c["backlog"]
            ph = c["physical"]
            q = c["qualify"]
            kind = ce.get("kind", "")
            f0 = ce.get("f0", "")
            ctrl = ce.get("control", "none")
            full = 1 if (ce.get("comp") == "1" and ce.get("delay") == "1"
                         and ce.get("reverb") == "1") else 0
            seq_gaps = (num(tr, "rx_seq_gaps") + num(tr, "tx_seq_gaps"))
            # Class populations. When the late-event ring captured every miss
            # the exact classes are recomputed from (mc, new_grains). For the
            # 4 h soak (>ring capacity) the aggregate fields are used; the
            # as-run firmware's "miss_nmc" field is the combined NMC+MC+1
            # residual and its "miss_mc1" field is MC+2 (legend in the report).
            ev = c["events"]
            misses = num(d, "misses")
            if ev and len(ev) >= misses:
                ec = [event_class(e) for e in ev]
                nmc, mc1, mc2, mc3 = (ec.count(0), ec.count(1), ec.count(2),
                                      ec.count(3))
            else:
                nmc = num(cl, "miss_nmc") + num(cl, "miss_mc0")
                mc1 = ""
                mc2 = num(cl, "miss_mc1")
                mc3 = num(cl, "miss_mc3")
            row = [c["name"], ce.get("seconds", ""), "kind=%s" % kind, f0,
                   ce.get("interval", ""), ctrl, full,
                   num(d, "blocks"), num(d, "avg_us"), num(d, "p99"),
                   num(d, "p999"), num(d, "max"), num(d, "misses"),
                   num(d, "miss_per_s"), mc1, mc2, mc3, nmc,
                   c["late"].get("max_consecutive_late",
                                 q.get("max_consecutive_late", "")),
                   c["late"].get("max_lateness_us", ""),
                   num(bl, "avg_ms"), num(bl, "p95_ms"), num(bl, "p99_ms"),
                   num(bl, "max_ms"),
                   num(tr, "rx_overruns"), num(tr, "tx_underruns"),
                   num(tr, "dma_errors") + num(tr, "actual_rx_dma_errors") +
                   num(tr, "actual_tx_dma_errors"),
                   num(c["fifo"], "drops"), seq_gaps, num(tr, "nan_inf"),
                   num(ph, "effective_fs"),
                   q.get("result", "")]
            w.writerow(row)
    return path


def write_recovery(cases, outdir):
    path = os.path.join(outdir, "b4d12_recovery_events.csv")
    cols = ["test", "block", "dsp_us", "lateness_us", "class", "new_grains",
            "next1_dsp_us", "next2_dsp_us", "next3_dsp_us", "recovery_blocks",
            "backlog_before_ds", "backlog_after_ds"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            for e in c["events"]:
                w.writerow([c["name"], e.get("blk", ""), e.get("dsp_us", ""),
                            e.get("lateness_us", ""), event_class(e),
                            e.get("ng", ""), e.get("next1", ""),
                            e.get("next2", ""), e.get("next3", ""),
                            e.get("recov", ""), e.get("bl_before_ds", ""),
                            e.get("bl_after_ds", "")])
    return path


def write_hist(cases, outdir):
    path = os.path.join(outdir, "b4d12_lateness_histogram.csv")
    cols = ["test", "le100", "b100_250", "b250_500", "b500_1000",
            "b1000_1500", "gt1500", "total", "p50_lateness_us",
            "p90_lateness_us", "p99_lateness_us", "max_lateness_us"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            h = c["hist"]
            bins = [num(h, "le100"), num(h, "b100_250"), num(h, "b250_500"),
                    num(h, "b500_1000"), num(h, "b1000_1500"),
                    num(h, "gt1500")]
            lat = sorted(num(e, "lateness_us") for e in c["events"])

            def pct(p):
                if not lat:
                    return ""
                idx = min(len(lat) - 1, int(p * len(lat)))
                return lat[idx]
            w.writerow([c["name"]] + bins + [sum(bins), pct(0.50), pct(0.90),
                                             pct(0.99),
                                             c["late"].get("max_lateness_us",
                                                           "")])
    return path


def write_windows(cases, outdir):
    path = os.path.join(outdir, "b4d12_soak_windows.csv")
    cols = ["test", "window", "partial", "elapsed_min", "blocks", "avg_us",
            "p99_us", "p999_us", "max_us", "misses", "mc2", "mc3",
            "max_consecutive_late", "max_backlog_ms", "transport_errors",
            "cumulative_lateness_us"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            for idx, win in enumerate(c["windows"]):
                blocks = num(win, "blocks")
                w.writerow([c["name"], win.get("idx", idx),
                            win.get("partial", 0),
                            round(blocks * 64.0 / 44100.0 / 60.0, 3),
                            blocks, num(win, "avg_us"),
                            win.get("p99_us", "NA"),
                            win.get("p999_us", "NA"),
                            num(win, "max_us"), num(win, "misses"),
                            num(win, "mc2"), num(win, "mc3"),
                            num(win, "max_consecutive_late"),
                            num(win, "backlog_max_ms"),
                            num(win, "transport_errors"),
                            num(win, "cumulative_lateness_us")])
    return path


def write_memory(cases, outdir):
    path = os.path.join(outdir, "b4d12_memory_stability.csv")
    cols = ["test", "phase", "internal_free", "internal_largest",
            "spiram_free", "spiram_largest", "audio_hwm_bytes",
            "pitch_hwm_bytes"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            stack = c["stack"][0] if c["stack"] else {}
            own = [m for m in c["memory"]
                   if m.get("tag", "").startswith("b4d12_" + c["name"])]
            if not own:
                own = c["memory"]
            for i, m in enumerate(own):
                w.writerow([c["name"], "snapshot%d" % i,
                            num(m, "internal_free"), num(m, "internal_largest"),
                            num(m, "spiram_free"), num(m, "spiram_largest"),
                            num(stack, "audio_hwm_bytes"),
                            num(stack, "pitch_hwm_bytes")])
    return path


def write_fifo(cases, outdir):
    path = os.path.join(outdir, "b4d12_fifo_stability.csv")
    cols = ["test", "pushes", "pops", "drops", "overflow_attempts",
            "max_occ", "avg_occ", "current_occ"]
    with open(path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for c in cases:
            f = c["fifo"]
            w.writerow([c["name"], num(f, "pushes"), num(f, "pops"),
                        num(f, "drops"), num(f, "overflow_attempts"),
                        num(f, "max_occ"), num(f, "avg_occ"),
                        num(f, "current_occ")])
    return path


CATEGORY_FILES = [
    ("A_reference", "b4d12_reference_30m_raw.txt"),
    (("C_f0_range", "D_transition"), "b4d12_pitch_stress_raw.txt"),
    ("E_interval", "b4d12_interval_stress_raw.txt"),
    ("I_control", "b4d12_control_stress_raw.txt"),
    ("J_long_soak", "b4d12_long_soak_raw.txt"),
]


def write_split_raw(raw, outdir):
    """Split the combined capture into the per-category raw artifacts."""
    with open(raw, "r", encoding="utf-8", errors="replace") as fh:
        text = ANSI.sub("", fh.read())
    parts = text.split("B4D12_CASE ")
    preamble = parts[0]
    for cats, fname in CATEGORY_FILES:
        cats = (cats,) if isinstance(cats, str) else cats
        chunks = [preamble]
        for seg in parts[1:]:
            head = seg.split("\n", 1)[0]
            d = kv("B4D12_CASE " + head)
            if d.get("category") in cats:
                chunks.append("B4D12_CASE " + seg)
        chunks.append("B4D12_SPLIT_END category=%s\n" % ",".join(cats))
        with open(os.path.join(outdir, fname), "w", encoding="utf-8") as out:
            out.write("".join(chunks))
        print("wrote", os.path.join(outdir, fname))


def main():
    if len(sys.argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    raw, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    cases = parse(raw)
    if not cases:
        sys.stderr.write("no B4D12_CASE records found in %s\n" % raw)
        return 1
    for fn in (write_matrix, write_recovery, write_hist, write_windows,
               write_memory, write_fifo):
        print("wrote", fn(cases, outdir))
    write_split_raw(raw, outdir)
    print("parsed %d cases" % len(cases))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
