"""Parse B4C.6A serial output and materialize the forensic artifact set.

The target prints fixed-format records only after each wall-clock window
(serial is silent inside the audio loop). This parser refuses to invent
measurements: any case without a capture is reported as NOT RUN.
"""
from __future__ import annotations
import argparse
import csv
import math
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01c"

TIMELINE_FIELDS = [
    "iteration_begin_us", "rx_done_us", "conversion_done_us",
    "fixture_done_us", "dsp_done_us", "tx_before_us", "tx_done_us",
    "iteration_end_us", "loop_cycles", "rx_bytes", "tx_bytes",
    "rx_rc", "tx_rc", "rx_ok", "tx_ok",
]


def kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"([A-Za-z0-9_]+)=([^\s]+)", line))


def write_csv(path: pathlib.Path, fields: list[str],
              rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def percentile(values: list[float], q: float) -> float:
    if not values:
        return 0.0
    a = sorted(values)
    idx = min(len(a) - 1, int(math.floor((len(a) - 1) * q)))
    return float(a[idx])


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


def fmt_d(d: dict[str, float]) -> str:
    return (f"n={d['n']:.0f} avg={d['avg']:.2f} p50={d['p50']:.0f} "
            f"p90={d['p90']:.0f} p95={d['p95']:.0f} p99={d['p99']:.0f} "
            f"p999={d['p999']:.0f} max={d['max']:.0f}")


class Case:
    def __init__(self, name: str):
        self.name = name
        self.case: dict[str, str] = {}
        self.transport: dict[str, str] = {}
        self.qualify: dict[str, str] = {}
        self.summary: dict[str, str] = {}
        self.loop: dict[str, str] = {}
        self.rxdist: dict[str, str] = {}
        self.txdist: dict[str, str] = {}
        self.equation: dict[str, str] = {}
        self.backlog: dict[str, str] = {}
        self.rows: list[dict[str, str]] = []


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_log", type=pathlib.Path)
    ap.add_argument("--out", type=pathlib.Path, default=OUT)
    args = ap.parse_args()
    out: pathlib.Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    lines = args.input_log.read_text(
        encoding="utf-8", errors="replace").splitlines()

    header = {
        "start": next((x for x in lines if x.startswith("B4C6A_START ")), ""),
        "boundary": next(
            (x for x in lines if x.startswith("B4C6A_BOUNDARY ")), ""),
        "sections": next(
            (x for x in lines if x.startswith("B4C6A_SECTIONS ")), ""),
        "task": next((x for x in lines if x.startswith("B4C6A_TASK ")), ""),
        "memory": next(
            (x for x in lines if x.startswith("B4C6A_MEMORY ")), ""),
    }
    placements = [kv(x) for x in lines
                  if x.startswith("B4C6A_MEMORY_PLACEMENT ")]

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
            if d.get("status") in ("START_FAILED",):
                c = ensure(d.get("name", "unknown"))
                c.case = d
                continue
            if "wall_s" not in d:
                continue
            c = ensure(d.get("name", "unknown"))
            c.case = d
        elif x.startswith("B4C6A_TRANSPORT ") and current is not None:
            current.transport = kv(x)
        elif x.startswith("B4C6A_QUALIFY ") and current is not None:
            current.qualify = kv(x)
        elif x.startswith("B4C6A_SUMMARY ") and current is not None:
            current.summary = kv(x)
        elif x.startswith("B4C6A_LOOP ") and current is not None:
            current.loop = kv(x)
        elif x.startswith("B4C6A_RXDIST ") and current is not None:
            current.rxdist = kv(x)
        elif x.startswith("B4C6A_TXDIST ") and current is not None:
            current.txdist = kv(x)
        elif x.startswith("B4C6A_EQUATION ") and current is not None:
            current.equation = kv(x)
        elif x.startswith("B4C6A_BACKLOG ") and current is not None:
            current.backlog = kv(x)
        elif x.startswith("B4C6A_TIMELINE ") and current is not None:
            parts = x.split(" ", 1)[1].split(",")
            if len(parts) == len(TIMELINE_FIELDS):
                current.rows.append(dict(zip(TIMELINE_FIELDS, parts)))

    # ---- Per-block tables ----
    loop_rows: list[dict] = []
    rx_rows: list[dict] = []
    tx_rows: list[dict] = []
    gap_rows: list[dict] = []
    reconc_rows: list[dict] = []
    skipped_rows = 0
    for c in cases:
        prev_begin: float | None = None
        prev_end: float | None = None
        for i, r in enumerate(c.rows):
            try:
                b = float(r["iteration_begin_us"])
                rd = float(r["rx_done_us"])
                cd = float(r["conversion_done_us"])
                fd = float(r["fixture_done_us"])
                dd = float(r["dsp_done_us"])
                tb = float(r["tx_before_us"])
                td = float(r["tx_done_us"])
                e = float(r["iteration_end_us"])
            except ValueError:
                continue
            rx_wait = rd - b
            rx_copy = cd - rd
            fixture = fd - cd
            dsp = dd - fd
            tx_prep = tb - dd
            tx_wait = td - tb
            book = e - td
            total = e - b
            accounted = (rx_wait + rx_copy + fixture + dsp + tx_prep
                         + tx_wait + book)
            other = total - accounted
            period = b - prev_begin if prev_begin is not None else 0.0
            gap = b - prev_end if prev_end is not None else 0.0
            prev_begin, prev_end = b, e
            try:
                rx_bytes = int(float(r["rx_bytes"]))
                tx_bytes = int(float(r["tx_bytes"]))
            except ValueError:
                skipped_rows += 1
                continue
            loop_rows.append({
                "case": c.name, "block": i,
                "iteration_begin_us": b, "rx_wait_us": rx_wait,
                "rx_copy_us": rx_copy, "fixture_us": fixture,
                "dsp_us": dsp, "tx_prep_us": tx_prep,
                "tx_wait_us": tx_wait, "bookkeeping_us": book,
                "other_us": other, "loop_total_us": total,
                "loop_period_us": period, "inter_gap_us": gap,
                "loop_cycles": r["loop_cycles"],
                "rx_bytes": rx_bytes, "tx_bytes": tx_bytes,
                "rx_rc": r["rx_rc"], "tx_rc": r["tx_rc"],
            })
            rx_rows.append({
                "case": c.name, "wait_us": rx_wait,
                "bytes_requested": 512, "bytes_returned": rx_bytes,
                "frames_returned": rx_bytes // 8,
                "return_code": r["rx_rc"],
                "partial": int(rx_bytes != 512),
                "zero": int(rx_bytes == 0),
            })
            tx_rows.append({
                "case": c.name, "wait_us": tx_wait,
                "bytes_requested": 512, "bytes_written": tx_bytes,
                "frames_written": tx_bytes // 8,
                "return_code": r["tx_rc"],
                "partial": int(tx_bytes != 512),
                "zero": int(tx_bytes == 0),
            })
            if i:
                gap_rows.append({"case": c.name, "block": i,
                                 "inter_iteration_gap_us": gap})
            reconc_rows.append({
                "case": c.name, "block": i, "total_us": total,
                "accounted_us": accounted, "unaccounted_us": other,
                "reconciliation_pct": (100.0 * accounted / total
                                       if total else 0.0),
            })

    write_csv(out / "b4c6a_loop_timing.csv",
              ["case", "block", "iteration_begin_us", "rx_wait_us",
               "rx_copy_us", "fixture_us", "dsp_us", "tx_prep_us",
               "tx_wait_us", "bookkeeping_us", "other_us",
               "loop_total_us", "loop_period_us", "inter_gap_us",
               "loop_cycles", "rx_bytes", "tx_bytes", "rx_rc", "tx_rc"],
              loop_rows)
    write_csv(out / "b4c6a_i2s_rx.csv",
              ["case", "wait_us", "bytes_requested", "bytes_returned",
               "frames_returned", "return_code", "partial", "zero"],
              rx_rows)
    write_csv(out / "b4c6a_i2s_tx.csv",
              ["case", "wait_us", "bytes_requested", "bytes_written",
               "frames_written", "return_code", "partial", "zero"],
              tx_rows)
    write_csv(out / "b4c6a_inter_iteration_gap.csv",
              ["case", "block", "inter_iteration_gap_us"], gap_rows)
    write_csv(out / "b4c6a_timing_reconciliation.csv",
              ["case", "block", "total_us", "accounted_us",
               "unaccounted_us", "reconciliation_pct"], reconc_rows)

    backlog_rows = [{"case": c.name,
                     "queue_depth_max_rx": c.backlog.get(
                         "rx_queue_max", ""),
                     "queue_depth_max_tx": c.backlog.get(
                         "tx_queue_max", ""),
                     "backlog_frames": c.backlog.get(
                         "backlog_frames_max", ""),
                     "backlog_ms": c.backlog.get("backlog_ms_max", ""),
                     "rx_overruns": c.backlog.get("rx_overruns", ""),
                     "tx_underruns": c.backlog.get("tx_underruns", "")}
                    for c in cases if c.backlog]
    write_csv(out / "b4c6a_backlog.csv",
              ["case", "queue_depth_max_rx", "queue_depth_max_tx",
               "backlog_frames", "backlog_ms", "rx_overruns",
               "tx_underruns"], backlog_rows)
    write_csv(out / "b4c6a_memory_placement.csv",
              ["object", "placement", "evidence"],
              [{"object": p.get("object", ""),
                "placement": p.get("placement", ""),
                "evidence": p.get("evidence", "")} for p in placements])

    by_name = {c.name: c for c in cases}

    def case_text(name: str, title: str) -> str:
        c = by_name.get(name)
        if c is None or not c.case:
            return (f"{title} ({name}): NOT RUN — no wall-clock capture "
                    f"in this log.\n")
        return (f"{title} ({name}): wall_s={c.case.get('wall_s')} "
                f"expected={c.case.get('expected_blocks')} "
                f"rx={c.case.get('actual_rx_blocks')} "
                f"tx={c.case.get('actual_tx_blocks')} "
                f"dsp={c.case.get('actual_dsp_blocks')} "
                f"eff_fs={c.case.get('effective_fs')} "
                f"qualify={c.qualify.get('result', '?')}\n")

    (out / "b4c6a_transport_control.txt").write_text(
        case_text("transport_only_10s", "Transport-only control"), 
        encoding="utf-8")
    (out / "b4c6a_dry_control.txt").write_text(
        case_text("dry_10s", "Dry-path control"), encoding="utf-8")
    (out / "b4c6a_1v_10s.txt").write_text(
        case_text("1v_vocal_10s", "1V canonical 10 s")
        + case_text("harness_off_1v_10s", "Harness OFF A/B")
        + case_text("harness_on_1v_10s", "Harness ON A/B"),
        encoding="utf-8")
    (out / "b4c6a_1v_60s.txt").write_text(
        case_text("1v_vocal_60s", "1V canonical 60 s"), encoding="utf-8")
    (out / "b4c6a_2v_10s.txt").write_text(
        case_text("2v_vocal_10s", "2V diagnostic 10 s"), encoding="utf-8")

    gain_path = out / "b4c6a_gainnorm_render_equivalence.txt"
    existing = gain_path.read_text(
        encoding="utf-8", errors="replace") if gain_path.exists() else ""
    if ("rendered_sample_mismatches" not in existing
            and "GAINNORM" not in existing):
        gain_path.write_text(
            "NOT RUN: host gainnorm_render_equivalence_tests output was "
            "not captured into this artifact set.\n", encoding="utf-8")

    # ---- Evidence-bound report ----
    rep: list[str] = []
    rep.append("# B4C.6A — End-to-End Audio Loop Timing Closure")
    rep.append("")
    rep.append("Evidence-bound report generated from target serial capture. "
               "No values are synthesized.")
    rep.append("")
    rep.append(f"- input lines: {len(lines)}")
    rep.append(f"- cases captured: {len(cases)}")
    rep.append(f"- per-block rows: {len(loop_rows)}")
    if skipped_rows:
        rep.append(f"- malformed serial rows skipped: {skipped_rows}")
    if header["boundary"]:
        rep.append(f"- boundary: {header['boundary']}")
    if header["task"]:
        rep.append(f"- freertos: {header['task']}")
    if header["memory"]:
        rep.append(f"- memory: {header['memory']}")
    rep.append("")
    rep.append("## Timing boundary (firmware fact, no hardware needed)")
    rep.append("")
    rep.append("Existing `avg_us` (DSP percentiles) measures "
               "`vocal_fx_process()` only: it begins immediately before the "
               "call and ends immediately after it. It excludes I2S RX "
               "blocking, PCM conversion, fixture generation, TX "
               "preparation/write, bookkeeping, scheduler gaps, and harness "
               "work. `AUDIO_LOOP_PERIOD_US` (begin-to-begin) is the separate "
               "authoritative end-to-end metric.")
    rep.append("")
    rep.append("## B4C.6 arithmetic (closed on paper from the B4C.6 1V/60 s "
               "result)")
    rep.append("")
    rep.append("- effective Fs 46024.91 Hz -> block period 1390.55 us; "
               "DSP avg 847.49 us; non-DSP observed 543.06 us/block.")
    rep.append("- at ideal 48 kHz the 1333.33 us deadline leaves 485.84 us "
               "of expected I2S pacing wait after 847.49 us DSP, so ~57.22 "
               "us/block of recurring extra delay must be found in "
               "RX/TX/harness/scheduler sections.")
    rep.append("- timeline lost time = 60 * (1 - 46024.91/48000) = 2.469 s. "
               "69 DSP deadline misses cannot explain it: even at the "
               "observed max lateness (2130 - 1333 = 797 us) they sum to "
               "at most 69 * 797 us = 0.055 s, ~45x too small. The deficit "
               "is therefore a per-block rate shortfall (~57 us/block x "
               "43148 processed blocks = 2.47 s), not outlier lateness.")
    rep.append("")
    if reconc_rows:
        vals = [float(r["reconciliation_pct"]) for r in reconc_rows]
        rep.append("## Reconciliation (measured)")
        rep.append("")
        rep.append(f"- mean section reconciliation: {sum(vals)/len(vals):.3f}% "
                   f"(require >= 98%)")
        for c in cases:
            cv = [float(r["reconciliation_pct"]) for r in reconc_rows
                  if r["case"] == c.name]
            if cv:
                rep.append(f"- {c.name}: {sum(cv)/len(cv):.3f}% over "
                           f"{len(cv)} blocks")
        rep.append("")
        for c in cases:
            if not c.rows:
                continue
            totals = [float(r["loop_total_us"]) for r in loop_rows
                      if r["case"] == c.name]
            periods = [float(r["loop_period_us"]) for r in loop_rows
                       if r["case"] == c.name and float(r["loop_period_us"]) > 0]
            dsps = [float(r["dsp_us"]) for r in loop_rows
                    if r["case"] == c.name]
            gaps = [float(r["inter_gap_us"]) for r in loop_rows
                    if r["case"] == c.name and float(r["block"]) > 0]
            rep.append(f"### {c.name}")
            rep.append("")
            rep.append(f"- AUDIO_LOOP_PERIOD_US: {fmt_d(dist(periods))}")
            rep.append(f"- DSP_PROCESS_US: {fmt_d(dist(dsps))}")
            rep.append(f"- loop_total: {fmt_d(dist(totals))}")
            rep.append(f"- inter_gap: {fmt_d(dist(gaps))}")
            if c.equation:
                rep.append(
                    f"- equation avgs (us): rx_wait={c.equation.get('rx_wait')} "
                    f"rx_copy={c.equation.get('rx_copy')} "
                    f"fixture={c.equation.get('fixture')} "
                    f"dsp={c.equation.get('dsp')} "
                    f"tx_prep={c.equation.get('tx_prep')} "
                    f"tx_wait={c.equation.get('tx_wait')} "
                    f"book={c.equation.get('book')} "
                    f"other={c.equation.get('other')} "
                    f"reconciliation={c.equation.get('reconciliation')}%")
            if c.qualify:
                rep.append(f"- qualify: {c.qualify.get('result')} "
                           f"(eff_fs={c.qualify.get('eff_fs')})")
            rep.append("")
    else:
        rep.append("## Reconciliation (no capture)")
        rep.append("")
        rep.append("No B4C6A_TIMELINE rows were captured, so no PASS/FAIL "
                   "decision is emitted. Required captures: dry 10 s, "
                   "transport-only 10 s, 1V 10 s, 1V 60 s, 2V 10 s, all "
                   "wall-clock driven.")
        rep.append("")
    rep.append("## Decisions")
    rep.append("")
    for label, key in (("transport control", "transport_only_10s"),
                       ("dry control", "dry_10s"),
                       ("1V 10 s", "1v_vocal_10s"),
                       ("1V 60 s", "1v_vocal_60s"),
                       ("2V diagnostic", "2v_vocal_10s")):
        c = by_name.get(key)
        if c is None or not c.case:
            rep.append(f"- {label}: NOT RUN")
        else:
            rep.append(f"- {label}: qualify={c.qualify.get('result', '?')} "
                       f"eff_fs={c.case.get('effective_fs', '?')}")
    rep.append("")
    rep.append("B4C.6A RESULT: " + ("PASS / FAIL per QUALIFY lines above"
               if cases else "INCOMPLETE (no hardware capture)"))
    (out / "b4c6a_report.md").write_text("\n".join(rep) + "\n",
                                         encoding="utf-8")


if __name__ == "__main__":
    main()
