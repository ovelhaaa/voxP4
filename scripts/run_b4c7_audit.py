"""Parse B4C.7 serial output and materialize the artifact set.

Refuses to invent measurements: cases without captures are NOT RUN.
Reuses the B4C.6A loop-timing record layout where applicable.
"""
from __future__ import annotations
import argparse
import csv
import math
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01c"


def kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line))


def write_csv(path: pathlib.Path, fields: list[str], rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    a = sorted(values)
    return float(a[min(len(a) - 1, int(math.floor((len(a) - 1) * q)))])


def dist(values: list[float]) -> dict[str, float]:
    return {
        "n": float(len(values)),
        "avg": sum(values) / len(values) if values else 0.0,
        "p50": percentile(values, 0.50),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "p999": percentile(values, 0.999),
        "max": max(values) if values else 0.0,
    }


GLOBAL_SECTIONS = ["pipeline", "tap", "param_q", "input", "hpf", "gate",
                   "comp", "pitch_sync", "harmony", "dry_align", "slew_pan",
                   "harm_limiter", "bus_mix", "delay_prep", "delay",
                   "reverb_prep", "reverb", "master", "master_mix",
                   "master_limiter"]
VOICE_SECTIONS = ["mark_sel", "sched", "hist_lookup", "plain_ola",
                  "resid_fir", "model_lookup", "warp_poly", "gain_norm",
                  "lpc_win_ola", "synth_iir", "state_shift", "gain_match",
                  "soft_clip", "blend", "fallback", "articul", "plosive",
                  "telem", "other", "voice_total", "desc_setup",
                  "hist_fetch", "hann_win", "ola_norm", "recov_xfade"]
CLASSES = ["BOTH_ACTIVE", "V0_ONLY", "V1_ONLY", "NEITHER_ACTIVE"]


class Case:
    def __init__(self, name: str):
        self.name = name
        self.case: dict[str, str] = {}
        self.transport: dict[str, str] = {}
        self.qualify: dict[str, str] = {}
        self.summary: dict[str, str] = {}
        self.loop: dict[str, str] = {}
        self.equation: dict[str, str] = {}
        self.backlog: dict[str, str] = {}
        self.cls: dict[str, str] = {}
        self.both: dict[str, str] = {}
        self.decomp: dict[str, dict[str, str]] = {}
        self.decomp_g: dict[str, dict[str, float]] = {}
        self.decomp_v: dict[str, dict[str, dict[str, float]]] = {}
        self.txprep: dict[str, str] = {}
        self.lpc: dict[str, str] = {}
        self.reuse_desc: dict[str, str] = {}
        self.reuse_slice: dict[str, str] = {}
        self.fircache: dict[str, dict[str, str]] = {}
        self.blocks: list[dict[str, str]] = []


def parse_section_values(tail: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for m in re.finditer(r"(\w+)=([\d.]+)", tail):
        try:
            out[m.group(1)] = float(m.group(2))
        except ValueError:
            pass
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_log", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    out: pathlib.Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    lines = args.input_log.read_text(
        encoding="utf-8", errors="replace").splitlines()

    bw = [kv(x) for x in lines if x.startswith("B4C7_PSRAM_BW ")]
    lpc_gate = next((kv(x) for x in lines
                     if x.startswith("B4C7_LPC_GATE ")), {})
    placements = [kv(x) for x in lines
                  if x.startswith("B4C7_MEMORY_PLACEMENT ")]
    prestages = [kv(x) for x in lines if x.startswith("B4C7_PRESTAGE ")]
    stability = next((kv(x) for x in lines
                      if x.startswith("B4C7_STABILITY ")), {})

    cases: list[Case] = []
    current: Case | None = None

    def ensure(name: str) -> Case:
        nonlocal current
        if current is None or current.name != name:
            current = Case(name)
            cases.append(current)
        return current

    for x in lines:
        if x.startswith("B4C6A_CASE "):
            d = kv(x)
            if "wall_s" not in d:
                continue
            ensure(d.get("name", "unknown")).case = d
        elif x.startswith("B4C6A_TRANSPORT ") and current is not None:
            current.transport = kv(x)
        elif x.startswith("B4C6A_QUALIFY ") and current is not None:
            current.qualify = kv(x)
        elif x.startswith("B4C6A_SUMMARY ") and current is not None:
            current.summary = kv(x)
        elif x.startswith("B4C6A_LOOP ") and current is not None:
            current.loop = kv(x)
        elif x.startswith("B4C6A_EQUATION ") and current is not None:
            current.equation = kv(x)
        elif x.startswith("B4C6A_BACKLOG ") and current is not None:
            current.backlog = kv(x)
        elif x.startswith("B4C7_CLASS ") and current is not None:
            current.cls = kv(x)
        elif x.startswith("B4C7_BOTH ") and current is not None:
            current.both = kv(x)
        elif x.startswith("B4C7_DECOMP ") and current is not None:
            d = kv(x)
            current.decomp[d.get("class", "?")] = d
        elif x.startswith("B4C7_DECOMP_G ") and current is not None:
            d = kv(x)
            current.decomp_g[d.get("class", "?")] = parse_section_values(
                x.split(" ", 1)[1])
        elif x.startswith("B4C7_DECOMP_V ") and current is not None:
            d = kv(x)
            current.decomp_v.setdefault(d.get("class", "?"), {})[
                d.get("voice", "?")] = parse_section_values(x.split(" ", 1)[1])
        elif x.startswith("B4C7_TXPREP ") and current is not None:
            current.txprep = kv(x)
        elif x.startswith("B4C7_LPC ") and current is not None:
            current.lpc = kv(x)
        elif x.startswith("B4C7_REUSE_DESC ") and current is not None:
            current.reuse_desc = kv(x)
        elif x.startswith("B4C7_REUSE_SLICE ") and current is not None:
            current.reuse_slice = kv(x)
        elif x.startswith("B4C7_FIRCACHE ") and current is not None:
            d = kv(x)
            current.fircache[d.get("voice", "?")] = d
        elif x.startswith("B4C7_BLOCK ") and current is not None:
            if x.startswith("B4C7_BLOCK_HEADER"):
                continue
            parts = x.split(" ", 1)[1].split(",")
            if len(parts) == 10:
                current.blocks.append(dict(zip(
                    ["block", "class", "dsp_us", "v0_us", "v1_us",
                     "global_us", "slices_v0", "slices_v1", "sched_v0",
                     "sched_v1"], parts)))

    by_name = {c.name: c for c in cases}

    # ---- bandwidth ----
    write_csv(out / "b4c7_psram_bandwidth.csv",
              ["size_kb", "write_MBps", "read_MBps", "copy_MBps", "verify"],
              [{"size_kb": b.get("size_kb", ""),
                "write_MBps": b.get("write_MBps", ""),
                "read_MBps": b.get("read_MBps", ""),
                "copy_MBps": b.get("copy_MBps", ""),
                "verify": b.get("verify", "")} for b in bw])

    # ---- stability ----
    (out / "b4c7_psram_stability.txt").write_text(
        f"total_audio_s={stability.get('total_audio_s', '?')} "
        f"dma_errors={stability.get('dma_errors', '?')} "
        f"panics={stability.get('panics', '?')}\n"
        f"note={stability.get('note', '')}\n"
        "Requirement: >=300 s audio at 200 MHz with 0 PSRAM errors, "
        "0 LoadProhibited/StoreProhibited, 0 cache faults, 0 corruption, "
        "0 memory-subsystem WDT. A panic or reset would abort the campaign "
        "before B4C7_END.\n", encoding="utf-8")

    # ---- memory placement ----
    write_csv(out / "b4c7_memory_placement.csv",
              ["object", "bytes", "placement"],
              [{"object": p.get("object", ""), "bytes": p.get("bytes", ""),
                "placement": p.get("placement", "")} for p in placements])

    # ---- LPC order audit ----
    with (out / "b4c7_lpc_order_audit.txt").open("w",
                                                 encoding="utf-8") as f:
        f.write(f"compile_default_order="
                f"{lpc_gate.get('compile_default_order', '?')}\n")
        f.write(f"analyzer_order={lpc_gate.get('analyzer_order', '?')}\n")
        for c in cases:
            if c.lpc:
                f.write(f"{c.name} synth_v0={c.lpc.get('synth_v0', '?')} "
                        f"synth_v1={c.lpc.get('synth_v1', '?')}\n")
        f.write("verdict= "
                "PASS (order 16 everywhere active; V1=0 only where voice 1 "
                "never ran) if all active voices show 16\n")

    # ---- both-active profile ----
    prof_rows: list[dict] = []
    for c in cases:
        row = {"case": c.name,
               "both_count": c.cls.get("both", ""),
               "both_pct": c.cls.get("both_pct", ""),
               "v0only": c.cls.get("v0only", ""),
               "v1only": c.cls.get("v1only", ""),
               "neither": c.cls.get("neither", ""),
               "both_dsp_avg": c.both.get("dsp_avg", ""),
               "both_dsp_p50": c.both.get("dsp_p50", ""),
               "both_dsp_p90": c.both.get("dsp_p90", ""),
               "both_dsp_p95": c.both.get("dsp_p95", ""),
               "both_dsp_p99": c.both.get("dsp_p99", ""),
               "both_dsp_p999": c.both.get("dsp_p999", ""),
               "both_dsp_max": c.both.get("dsp_max", ""),
               "both_samples": c.both.get("samples", "")}
        for cl in CLASSES:
            d = c.decomp.get(cl, {})
            row[f"{cl}_count"] = d.get("count", "")
            row[f"{cl}_loop_avg"] = d.get("loop_avg", "")
            row[f"{cl}_acct_avg"] = d.get("acct_avg", "")
            row[f"{cl}_recon"] = d.get("recon", "")
            g = c.decomp_g.get(cl, {})
            for s in GLOBAL_SECTIONS:
                row[f"{cl}_g_{s}"] = g.get(s, "")
            for v in ("0", "1"):
                vv = c.decomp_v.get(cl, {}).get(v, {})
                for s in VOICE_SECTIONS:
                    row[f"{cl}_v{v}_{s}"] = vv.get(s, "")
        prof_rows.append(row)
    prof_fields = (["case", "both_count", "both_pct", "v0only", "v1only",
                    "neither", "both_dsp_avg", "both_dsp_p50",
                    "both_dsp_p90", "both_dsp_p95", "both_dsp_p99",
                    "both_dsp_p999", "both_dsp_max", "both_samples"]
                   + [f"{cl}_{k}" for cl in CLASSES
                      for k in ("count", "loop_avg", "acct_avg", "recon")]
                   + [f"{cl}_g_{s}" for cl in CLASSES
                      for s in GLOBAL_SECTIONS]
                   + [f"{cl}_v{v}_{s}" for cl in CLASSES for v in ("0", "1")
                      for s in VOICE_SECTIONS])
    write_csv(out / "b4c7_both_active_profile.csv", prof_fields, prof_rows)

    # ---- reuse ----
    reuse_rows = [{
        "case": c.name,
        "desc_requested": c.reuse_desc.get("requested", ""),
        "desc_unique": c.reuse_desc.get("unique", ""),
        "desc_duplicate": c.reuse_desc.get("duplicate", ""),
        "desc_cross": c.reuse_desc.get("cross", ""),
        "desc_same": c.reuse_desc.get("same", ""),
        "desc_req_pct": c.reuse_desc.get("req_pct", ""),
        "desc_sample_pct": c.reuse_desc.get("sample_pct", ""),
        "slice_requested": c.reuse_slice.get("requested", ""),
        "slice_duplicate": c.reuse_slice.get("duplicate", ""),
        "slice_cross": c.reuse_slice.get("cross", ""),
        "slice_same": c.reuse_slice.get("same", ""),
        "slice_entry_drops": c.reuse_slice.get("entry_drops", ""),
        "slice_req_pct": c.reuse_slice.get("req_pct", ""),
        "slice_sample_pct": c.reuse_slice.get("sample_pct", ""),
        "slice_tap_pct": c.reuse_slice.get("tap_pct", ""),
        "fir0_lookups": c.fircache.get("0", {}).get("lookups", ""),
        "fir0_hits": c.fircache.get("0", {}).get("hits", ""),
        "fir0_misses": c.fircache.get("0", {}).get("misses", ""),
        "fir0_evictions": c.fircache.get("0", {}).get("evictions", ""),
        "fir0_computed": c.fircache.get("0", {}).get("computed", ""),
        "fir0_reused": c.fircache.get("0", {}).get("reused", ""),
        "fir1_lookups": c.fircache.get("1", {}).get("lookups", ""),
        "fir1_hits": c.fircache.get("1", {}).get("hits", ""),
        "fir1_misses": c.fircache.get("1", {}).get("misses", ""),
        "fir1_evictions": c.fircache.get("1", {}).get("evictions", ""),
        "fir1_computed": c.fircache.get("1", {}).get("computed", ""),
        "fir1_reused": c.fircache.get("1", {}).get("reused", ""),
    } for c in cases]
    write_csv(out / "b4c7_source_slice_reuse.csv", list(reuse_rows[0])
              if reuse_rows else [], reuse_rows)
    samp_rows = [{
        "case": c.name,
        "desc_samples": c.reuse_desc.get("samples", ""),
        "desc_reusable_samples": c.reuse_desc.get("reusable_samples", ""),
        "slice_samples": c.reuse_slice.get("samples", ""),
        "slice_reusable_samples": c.reuse_slice.get("reusable_samples",
                                                    ""),
        "slice_taps": c.reuse_slice.get("taps", ""),
        "slice_reusable_taps": c.reuse_slice.get("reusable_taps", ""),
    } for c in cases]
    write_csv(out / "b4c7_source_slice_samples.csv", list(samp_rows[0])
              if samp_rows else [], samp_rows)

    # ---- hotspot ranking (BOTH_ACTIVE section avgs) ----
    rank_rows: list[dict] = []
    for c in cases:
        n = max(1.0, float(c.decomp.get("BOTH_ACTIVE", {}).get("count",
                                                               0) or 0))
        secs: list[tuple[str, float]] = []
        g = c.decomp_g.get("BOTH_ACTIVE", {})
        for s in GLOBAL_SECTIONS:
            secs.append((f"global/{s}", g.get(s, 0.0)))
        for v in ("0", "1"):
            vv = c.decomp_v.get("BOTH_ACTIVE", {}).get(v, {})
            for s in VOICE_SECTIONS:
                secs.append((f"v{v}/{s}", vv.get(s, 0.0)))
        secs.sort(key=lambda kv: kv[1], reverse=True)
        for rank, (name, avg) in enumerate(secs[:12]):
            rank_rows.append({"case": c.name, "rank": rank + 1,
                              "section": name,
                              "avg_us_per_block": round(avg, 2)})
    write_csv(out / "b4c7_hotspot_ranking.csv",
              ["case", "rank", "section", "avg_us_per_block"], rank_rows)

    # ---- high-F0 matrix ----
    f0_rows = [{
        "case": c.name,
        "dsp_avg": (c.blocks and sum(float(r["dsp_us"]) for r in c.blocks) /
                    len(c.blocks)) or "",
        "both_pct": c.cls.get("both_pct", ""),
        "eff_fs": c.case.get("effective_fs", ""),
        "misses": c.qualify.get("result", ""),
    } for c in cases if c.name.startswith("f0_")]
    for r in f0_rows:
        c = by_name[r["case"]]
        dsps = sorted(float(x["dsp_us"]) for x in c.blocks)
        r["dsp_p50"] = percentile(dsps, 0.50)
        r["dsp_p90"] = percentile(dsps, 0.90)
        r["dsp_p95"] = percentile(dsps, 0.95)
        r["dsp_p99"] = percentile(dsps, 0.99)
        r["dsp_p999"] = percentile(dsps, 0.999)
        r["dsp_max"] = max(dsps) if dsps else 0
        r["late_pct"] = ""
    write_csv(out / "b4c7_high_f0_matrix.csv",
              ["case", "dsp_avg", "dsp_p50", "dsp_p90", "dsp_p95", "dsp_p99",
               "dsp_p999", "dsp_max", "late_pct", "both_pct", "eff_fs",
               "misses"], f0_rows)

    # ---- 2V before ----
    def case_detail(name: str) -> str:
        c = by_name.get(name)
        if c is None or not c.case:
            return f"{name}: NOT RUN\n"
        e = c.equation
        return (f"{name}: wall_s={c.case.get('wall_s')} "
                f"eff_fs={c.case.get('effective_fs')} "
                f"qualify={c.qualify.get('result')} "
                f"dsp_eq_avg={e.get('dsp')} txprep={c.txprep} "
                f"both={c.cls.get('both')}/{c.cls.get('both_pct')}% "
                f"reuse_tap_pct={c.reuse_slice.get('tap_pct')}\n")

    (out / "b4c7_2v_before.csv").write_text(
        "case,eff_fs,dsp_eq_avg,both_pct,tap_pct,qualify\n" + "".join(
            f"{n},{by_name[n].case.get('effective_fs', '')},"
            f"{by_name[n].equation.get('dsp', '')},"
            f"{by_name[n].cls.get('both_pct', '')},"
            f"{by_name[n].reuse_slice.get('tap_pct', '')},"
            f"{by_name[n].qualify.get('result', '')}\n"
            for n in ("2v_diag_10s", "2v_prod_60s") if n in by_name),
        encoding="utf-8")
    (out / "b4c7_2v_after.csv").write_text(
        "NOT RUN: no B4C.7 DSP optimization selected yet; rerun the "
        "production campaign after implementing the winner and re-parse.\n",
        encoding="utf-8")
    (out / "b4c7_2v_production_60s.txt").write_text(
        case_detail("2v_prod_60s"), encoding="utf-8")
    (out / "b4c7_1v_production_60s.txt").write_text(
        case_detail("1v_prod_60s"), encoding="utf-8")
    (out / "b4c7_transport.txt").write_text(
        "".join(case_detail(n) for n in by_name), encoding="utf-8")

    gain_path = out / "b4c7_host_equivalence.txt"
    if not gain_path.exists():
        gain_path.write_text(
            "NOT RUN: fill with host gainnorm_render_equivalence_tests + "
            "b4c7_reuse_tests + bit-identity outputs.\n", encoding="utf-8")

    # ---- report ----
    rep = ["# B4C.7 — 2-Voice Throughput, Cross-Voice Reuse & PSRAM 200 MHz",
           "",
           "Evidence-bound report from target serial capture.",
           f"- input lines: {len(lines)}",
           f"- cases: {len(cases)}",
           f"- B4C7_BLOCK rows: "
           f"{sum(len(c.blocks) for c in cases)}",
           ""]
    rep += ["## PSRAM 200 MHz",
            f"- bandwidth: " + "; ".join(
                f"{b.get('size_kb')}KB w={b.get('write_MBps')} "
                f"r={b.get('read_MBps')} c={b.get('copy_MBps')}MB/s "
                f"{b.get('verify')}" for b in bw),
            f"- stability: audio_s={stability.get('total_audio_s')} "
            f"dma_errors={stability.get('dma_errors')} "
            f"panics={stability.get('panics')}",
            f"- LPC gate: compile={lpc_gate.get('compile_default_order')} "
            f"analyzer={lpc_gate.get('analyzer_order')}", ""]
    rep.append("## Cases")
    for c in cases:
        rep.append(f"### {c.name}")
        rep.append(f"- eff_fs={c.case.get('effective_fs')} "
                   f"qualify={c.qualify.get('result')} "
                   f"eq_dsp_avg={c.equation.get('dsp')} "
                   f"eq_loop_avg={c.loop.get('loop_avg')}")
        rep.append(f"- class both={c.cls.get('both')} "
                   f"({c.cls.get('both_pct')}%) v0={c.cls.get('v0only')} "
                   f"v1={c.cls.get('v1only')} neither={c.cls.get('neither')}")
        rep.append(f"- both_dsp avg={c.both.get('dsp_avg')} "
                   f"p99={c.both.get('dsp_p99')} "
                   f"max={c.both.get('dsp_max')}")
        rep.append(f"- reuse tap_pct={c.reuse_slice.get('tap_pct')} "
                   f"sample_pct={c.reuse_slice.get('sample_pct')} "
                   f"drops={c.reuse_slice.get('entry_drops')}")
        rep.append(f"- txprep {c.txprep}")
        rep.append(f"- lpc v0={c.lpc.get('synth_v0')} "
                   f"v1={c.lpc.get('synth_v1')}")
        rep.append("")
    rep += ["B4C.7 RESULT: see per-case QUALIFY lines above; 2V optimization "
            "decision requires the reuse/hotspot tables.", ""]
    (out / "b4c7_report.md").write_text("\n".join(rep), encoding="utf-8")


if __name__ == "__main__":
    main()
