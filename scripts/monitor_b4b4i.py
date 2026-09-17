"""Flash, capture, and materialize the Stage B4B.4I diagnostic audit."""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import subprocess
import sys
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01b"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def git_text(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=ROOT, text=True, encoding="utf-8"
    ).strip()


def flash(port: str) -> None:
    export = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export} >nul && idf.py -p {port} flash"
    done = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if done.returncode:
        raise SystemExit(done.returncode)


def capture(port: str, timeout: float) -> str:
    lines: list[str] = []
    with serial.Serial(port, 115200, timeout=.25, dsrdtr=False,
                       rtscts=False) as connection:
        connection.dtr = False
        connection.rts = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                continue
            line = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if line == "NEXT STEP:":
                deadline = min(deadline, time.monotonic() + 3)
    text = "\n".join(lines) + "\n"
    (OUT / "b4b4i_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def kv(line: str) -> dict[str, str]:
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def first_line(text: str, prefix: str) -> str:
    return next((line for line in text.splitlines() if line.startswith(prefix)),
                "")


def number(values: dict[str, str], key: str, default: float = 0.0) -> float:
    try:
        return float(values[key])
    except (KeyError, ValueError):
        return default


def integer(values: dict[str, str], key: str, default: int = 0) -> int:
    try:
        return int(values[key])
    except (KeyError, ValueError):
        return default


def materialize(text: str, checkout: dict[str, str]) -> bool:
    sections = [
        kv(line) for line in text.splitlines()
        if line.startswith("B4B4I_SECTION ")
    ]
    by_name = {row["name"]: row for row in sections if "name" in row}
    summary = kv(first_line(text, "B4B4I_SUMMARY "))
    reconciliation = kv(first_line(text, "B4B4I_RECONCILIATION "))
    audit_estimate = kv(first_line(text, "B4B4I_AUDIT_ESTIMATE "))
    overhead = [
        kv(line) for line in text.splitlines()
        if line.startswith("B4B4I_OVERHEAD ")
    ]
    sensitivity = kv(first_line(text, "B4B4I_CMND_SENSITIVITY "))
    cold = kv(first_line(text, "B4B4I_COLD "))
    iterative = kv(first_line(text, "B4B4I_ITERATIVE "))

    fifo_match = re.search(
        r"FIFO/backlog: current=(\d+) max=(\d+) drops=(\d+) bounded=(\w+).*"
        r"backlog_current_ms=([0-9.]+) backlog_max_ms=([0-9.]+) "
        r"pitch_age_current_ms=([0-9.]+) pitch_age_avg_ms=([0-9.]+) "
        r"pitch_age_P95_ms=([0-9.]+) pitch_age_P99_ms=([0-9.]+) "
        r"pitch_age_max_ms=([0-9.]+)", text)
    grains_match = re.search(
        r"GRAINS:\s*attempted=(\d+) scheduled=(\d+) rendered=(\d+)", text)
    transport_match = re.search(
        r"actual_DMA_errors=(\d+)/(\d+) read/write_failures=(\d+)/(\d+) "
        r"dropped_frames=(\d+)/(\d+)", text)
    result_match = re.search(r"B4B\.4I RESULT:\s*(PASS|FAIL)", text)

    with (OUT / "b4b4i_pitch_section_profile.csv").open(
            "w", newline="", encoding="utf-8") as output:
        fields = [
            "name", "category", "executions", "avg_us_hop", "P50_us",
            "P95_us", "P99_us", "max_us", "avg_cycles_hop", "cpu_ms_s",
            "samples", "bytes", "cycles_per_sample",
        ]
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in sections:
            writer.writerow({field: row.get(field, "") for field in fields})
        writer.writerow({
            "name": "Other", "category": "AUDIT_OR_UNACCOUNTED",
            "avg_us_hop": number(reconciliation, "other_us_hop"),
            "cpu_ms_s": number(reconciliation, "other_us_hop") * .2,
        })

    lpc_us = number(summary, "LPC_us_frame")
    with (OUT / "b4b4i_target_rate_budget.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["section", "rate_per_second", "us_per_unit",
                         "cpu_ms_per_second", "headroom_capable"])
        for row in sections:
            cpu = number(row, "cpu_ms_s")
            writer.writerow([row.get("name", ""), 200,
                             row.get("avg_us_hop", ""), f"{cpu:.3f}",
                             "yes" if cpu >= 40 else "no"])
        other_us = number(reconciliation, "other_us_hop")
        writer.writerow(["Other", 200, f"{other_us:.3f}",
                         f"{other_us * .2:.3f}",
                         "yes" if other_us * .2 >= 40 else "no"])
        writer.writerow(["LPC", 125, f"{lpc_us:.3f}",
                         f"{number(summary, 'LPC_ms_s'):.3f}", "yes"])
        writer.writerow(["TOTAL_CORE1", "", "",
                         f"{number(summary, 'target_rate_current_ms_s'):.3f}",
                         ""])

    with (OUT / "b4b4i_audit_overhead.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["component", "classification", "iterations",
                         "avg_cycles", "avg_us", "charged_us_per_hop",
                         "charged_ms_per_second"])
        for row in overhead:
            writer.writerow([
                row.get("component", ""), "AUDIT_DIAGNOSTIC_ONLY",
                row.get("iterations", ""), row.get("avg_cycles", ""),
                row.get("avg_us", ""), "", "",
            ])
        writer.writerow([
            "Measured aggregate charged overhead", "AUDIT_DIAGNOSTIC_ONLY",
            "", "", "", f"{number(audit_estimate, 'audit_us_hop'):.3f}",
            f"{number(audit_estimate, 'audit_ms_s'):.3f}",
        ])

    (OUT / "b4b4i_window_pass_inventory.md").write_text(
        """# B4B.4I window-pass inventory

The frozen 512-sample analysis window is traversed three complete times per
pitch hop:

1. LinearCopy copies 512 float samples (2048 bytes) out of the rolling ring.
2. YinEnergy accumulates sum(x*x) in double, then uses log10 for RMS dB.
3. VoicedFeatures accumulates peak, binary32 sum_sq, lag-1 correlation and
   zero crossings; it then derives HFR and a centroid proxy using division and
   acos.

YinEnergy and VoicedFeatures both calculate an energy/sum-of-squares quantity,
but with different accumulator precision. The values therefore cannot simply
be substituted without changing arithmetic semantics. A future loop-fusion
experiment could retain both accumulators and their exact update orders, but no
such change is made in B4B.4I.

The classifier converts rms_db back with pow, and the smoother derives RMS with
sqrt; these are related transformations, but not duplicate calls to the same
transcendental. CMND is a separate tau traversal, not a 512-sample pass.
""", encoding="utf-8")

    risk = {
        "FIFO": "LOW RISK", "RollingWindow": "LOW RISK",
        "LinearCopy": "LOW RISK", "YinEnergy": "LOW RISK",
        "YinDifference": "MEDIUM RISK", "YinCMND": "MEDIUM RISK",
        "YinSearch": "HIGH RISK", "YinInterpolation": "HIGH RISK",
        "VoicedFeatures": "LOW RISK", "VoicedClassifier": "HIGH RISK",
        "PitchSmoother": "HIGH RISK", "PitchMarkSearch": "MEDIUM RISK",
        "PitchPublication": "LOW RISK", "Other": "HIGH RISK",
        "AuditOverhead": "LOW RISK",
    }
    ranked = sorted(
        [(row["name"], number(row, "avg_us_hop"),
          number(row, "cpu_ms_s")) for row in sections],
        key=lambda item: item[2], reverse=True)
    low_risk = [(name, us, cpu) for name, us, cpu in ranked
                if risk.get(name) == "LOW RISK"]
    audit_cpu = number(audit_estimate, "audit_ms_s")
    audit_us = number(audit_estimate, "audit_us_hop")
    low_risk.append(("AuditOverhead", audit_us, audit_cpu))
    low_risk.sort(key=lambda item: item[2], reverse=True)
    current = number(summary, "target_rate_current_ms_s")
    largest = low_risk[0] if low_risk else ("none", 0.0, 0.0)
    second = low_risk[1] if len(low_risk) > 1 else ("none", 0.0, 0.0)
    single_capable = audit_cpu >= 36.036
    required_fraction = 36.036 / largest[2] if largest[2] else 0.0
    next_step = (
        "B4B.4J — remove or compile-gate the measured audit/profiler overhead"
        if single_capable else
        "B4B.4J — isolated YinCMND kernel audit with threshold-margin and "
        "selected-tau numerical guardrails"
    )

    result = (
        result_match is not None and result_match.group(1) == "PASS"
        and number(reconciliation, "percent") >= 97.0
        and len(sections) == 13
    )
    fifo = fifo_match.groups() if fifo_match else ("?",) * 11
    grains = grains_match.groups() if grains_match else ("?",) * 3
    transport = transport_match.groups() if transport_match else ("?",) * 6
    unit_ranking = "\n".join(
        f"{index}. {name}: {us:.3f} us/hop, {cpu:.3f} ms/s @200"
        for index, (name, us, cpu) in enumerate(ranked, 1))
    target_ranking = "\n".join(
        f"{index}. {name}: {cpu:.3f} ms/s @200 ({us:.3f} us/hop)"
        for index, (name, us, cpu) in enumerate(ranked, 1))
    residual_ranking = [
        item for item in ranked
        if item[0] not in ("YinDifference", "PitchMarkSearch")
    ]
    residual_top = "; ".join(
        f"{name} {us:.3f} us/hop ({cpu:.3f} ms/s)"
        for name, us, cpu in residual_ranking[:3])
    capable = [
        f"{name} ({cpu:.3f} ms/s, {risk.get(name, 'UNCLASSIFIED')})"
        for name, _us, cpu in ranked if cpu >= 40
    ]
    pitch = number(summary, "PitchAnalysis_us_hop")
    yin_diff = number(summary, "YinDifference_us_hop")
    mark = number(summary, "PitchMarkSearch_us_hop")
    report = f"""# Stage B4B.4I — Residual PitchAnalysis Hotspot Audit

## Result

**B4B.4I RESULT: {"PASS" if result else "FAIL"}**

The primary ten-second statistics exclude a one-second cold warmup. No DSP
algorithm, arithmetic order, threshold, cadence, scheduler setting, FIFO size,
or production default was changed.

## Checkout

- HEAD: {checkout["head"]}
- Branch: {checkout["branch"]}
- Initial status: {checkout["status"] or "(clean)"}
- Initial diff stat: {checkout["diffstat"] or "(empty)"}
- Present milestones: B4B.4E incremental-YIN audit override, B4B.4F
  pitch-mark NCC reuse, B4B.4G windowing, and B4B.4H compensated-F32 LPC
  energy.

## Frozen configuration

YIN_DIFF_INCREMENTAL_F32, rebase 64; PITCH_MARK_NCC_REUSE_AA;
LPC_ENERGY_F32_COMPENSATED; AUTOCORR_F32_DOUBLE_SINGLE; 48 kHz transport,
12 kHz analysis, 60-sample pitch hop, 512-sample window, and unchanged
task/FIFO/max-hop settings. The normal P4 YIN default remains
YIN_DIFF_FMA_8ACC.

## Direct answers

1. PitchAnalysis total: **{pitch:.3f} us/hop**.
2. YinDifference: **{yin_diff:.3f} us/hop**.
3. PitchMarkSearch: **{mark:.3f} us/hop**.
4. Residual after both: **{number(summary, "residual_us_hop"):.3f} us/hop**.
5. YinEnergy: **{number(by_name.get("YinEnergy", {}), "avg_us_hop"):.3f} us/hop**.
6. YinCMND: **{number(by_name.get("YinCMND", {}), "avg_us_hop"):.3f} us/hop**.
7. YinSearch: **{number(by_name.get("YinSearch", {}), "avg_us_hop"):.3f} us/hop**.
8. YinInterpolation: **{number(by_name.get("YinInterpolation", {}), "avg_us_hop"):.3f} us/hop**.
9. FIFO drain: **{number(by_name.get("FIFO", {}), "avg_us_hop"):.3f} us/hop**.
10. Rolling-window maintenance: **{number(by_name.get("RollingWindow", {}), "avg_us_hop"):.3f} us/hop**.
11. Linear-window copy: **{number(by_name.get("LinearCopy", {}), "avg_us_hop"):.3f} us/hop**.
12. VoicedFeatures: **{number(by_name.get("VoicedFeatures", {}), "avg_us_hop"):.3f} us/hop**.
13. VoicedClassifier: **{number(by_name.get("VoicedClassifier", {}), "avg_us_hop"):.3f} us/hop**.
14. PitchSmoother: **{number(by_name.get("PitchSmoother", {}), "avg_us_hop"):.3f} us/hop**.
15. PitchPublication: **{number(by_name.get("PitchPublication", {}), "avg_us_hop"):.3f} us/hop**.
16. Other/unaccounted: **{number(reconciliation, "other_us_hop"):.3f} us/hop**.
17. Reconciliation: **{number(reconciliation, "percent"):.3f}%**.
18. Proven audit-only cost: **{audit_us:.3f} us/hop ({audit_cpu:.3f} ms/s)**.
19. Estimated production-only PitchAnalysis: **{number(audit_estimate, "estimated_production_pitch_us_hop"):.3f} us/hop** (estimate only).
20. Full-window passes: **3 per hop**.
21. Energy/RMS/sum_sq redundancy: **yes**, but double and float accumulation semantics differ.
22. Duplicate transcendental: **no identical call**; the energy path uses
    log10 -> pow -> sqrt conversions that may admit value reuse.
23. Largest section at 200 hops/s: **{ranked[0][0] if ranked else "missing"} ({ranked[0][2] if ranked else 0:.3f} ms/s)**.
24. Largest LOW-RISK opportunity: **{largest[0]} ({largest[2]:.3f} ms/s upper bound)**.
25. One measured LOW-RISK transformation can cover 36.036 ms/s:
    **no**. The largest low-risk section is large enough in principle, but no
    concrete eliminable transformation was measured; it would require a
    {required_fraction * 100:.1f}% reduction in {largest[0]}.
26. Candidate: **none proven**; {largest[0]} would require a
    {required_fraction * 100:.1f}% reduction. No optimization was implemented.
27. Current target-rate total: **{current:.3f} ms/s**.
28. Removing only proven audit overhead: **{number(summary, "target_rate_minus_audit_ms_s"):.3f} ms/s** (100% elimination upper bound).
29. A 50% reduction in the largest LOW-RISK hotspot gives
    **{current - .5 * largest[2]:.3f} ms/s**.
30. Recommended milestone: **{next_step}**.
31. FIFO bounded: **{fifo[3]}** (current/max/drops {fifo[0]}/{fifo[1]}/{fifo[2]}).
32. Pitch age current/avg/P95/P99/max: **{fifo[6]}/{fifo[7]}/{fifo[8]}/{fifo[9]}/{fifo[10]} ms**.
33. Grains attempted/scheduled/rendered: **{grains[0]}/{grains[1]}/{grains[2]}**.
34. Transport DMA RX/TX, read/write, dropped RX/TX: **{"/".join(transport)}**.
35. Teardown: **{"clean" if "spinlock/reboot/watchdog/callback-after-free: not observed" in text else "not proven"}**.

## Hotspot rankings

### A. Unit cost (us/hop)

{unit_ranking}

### B. Target-rate budget (ms/s at 200 hops/s)

{target_ranking}

The ordering is identical because every pitch section is normalized at the
same 200 hops/s target rate.

HEADROOM-CAPABLE TARGETS (>=40 ms/s): {", ".join(capable) or "none"}.

CMND sensitivity: threshold {number(sensitivity, "threshold"):.6f}, minimum
selected CMND {number(sensitivity, "selected_min"):.6f}, closest threshold
distance {number(sensitivity, "closest_threshold_distance"):.6f}, selected tau
range {integer(sensitivity, "selected_tau_min")}–{integer(sensitivity, "selected_tau_max")}.

## Headroom scenarios

- Current: {current:.3f} ms/s.
- Minus measured audit-only overhead (100% upper bound): {current - audit_cpu:.3f} ms/s.
- Minus largest LOW-RISK target (100% upper bound): {current - largest[2]:.3f} ms/s.
- Minus top two LOW-RISK targets (100% upper bound): {current - largest[2] - second[2]:.3f} ms/s.
- 50% reduction in largest LOW-RISK target: {current - .5 * largest[2]:.3f} ms/s.

Cold PitchAnalysis was {number(cold, "PitchAnalysis_us_hop"):.3f} us/hop over
{integer(cold, "hops")} hops. The warm section distributions use a bounded
512-sample tail reservoir; percentile values are integer-microsecond
measurements. Pitch-age P95/P99 are histogram upper bounds, so they can exceed
the separately tracked exact maximum. YinDifference executed
{integer(iterative, "YinDifference_inner")} inner operations.

## Required final format

```text
B4B.4I RESULT:
{"PASS" if result else "FAIL"}

PITCH ANALYSIS:
- total: {pitch:.3f} us/hop
- YinDifference: {yin_diff:.3f} us/hop
- PitchMarkSearch: {mark:.3f} us/hop
- residual: {number(summary, "residual_us_hop"):.3f} us/hop

TOP RESIDUAL SECTIONS:
- {residual_top}

AUDIT-ONLY COST:
- {audit_us:.3f} us/hop ({audit_cpu:.3f} ms/s @200)

TARGET-RATE BUDGET:
- current: {current:.3f} ms/s
- missing to 900 ms/s: {number(summary, "missing_to_900_ms_s"):.3f} ms/s

BEST LOW-RISK OPPORTUNITY:
- {largest[0]}: {largest[2]:.3f} ms/s full-section upper bound;
  {largest[2] * .5:.3f} ms/s at a hypothetical 50% reduction

ESTIMATED SAVING:
- no measured transformation proven; {required_fraction * 100:.1f}% of
  {largest[0]} would be needed to cover the prior 36.036 ms/s gap

NEXT STEP:
- {next_step}
```
"""
    (OUT / "b4b4i_pitch_residual_report.md").write_text(
        report, encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    checkout = {
        "head": git_text("rev-parse", "HEAD"),
        "branch": git_text("branch", "--show-current"),
        "status": git_text("status", "--short"),
        "diffstat": git_text("diff", "--stat"),
    }
    if not args.no_flash:
        flash(args.port)
        time.sleep(.25)
    text = capture(args.port, args.timeout)
    return 0 if materialize(text, checkout) else 2


if __name__ == "__main__":
    sys.exit(main())
