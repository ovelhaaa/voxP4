"""Flash, capture, and materialize the Stage B4B.4F hardware audit."""

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
        finishing = False
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                if finishing:
                    break
                continue
            line = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if line == "NEXT STEP:":
                finishing = True
                deadline = min(deadline, time.monotonic() + 3)
    text = "\n".join(lines) + "\n"
    (OUT / "b4b4f_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def materialize(text: str) -> None:
    isolated = re.compile(
        r"^B4B4F_ISOLATED variant=(\S+) searches=(\d+) "
        r"avg_us/search=([0-9.]+) cycles/search=([0-9.]+) "
        r"us/offset=([0-9.]+) cycles/offset=([0-9.]+) "
        r"cycles/pair=([0-9.]+) P50_us=(\d+) P95_us=(\d+) "
        r"P99_us=(\d+) max_us=(\d+) P50_cycles=(\d+) "
        r"P95_cycles=(\d+) P99_cycles=(\d+) max_cycles=(\d+) "
        r"window=(\d+) offsets=(\d+) pairs=(\d+) checksum=(\S+)", re.M)
    with (OUT / "b4b4f_pitch_mark_benchmark.csv").open(
            "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["platform", "variant", "searches", "avg_us_per_search",
                    "cycles_per_search", "us_per_offset", "cycles_per_offset",
                    "cycles_per_sample_pair", "p50_us", "p95_us", "p99_us",
                    "max_us", "p50_cycles", "p95_cycles", "p99_cycles",
                    "max_cycles", "window", "offsets", "sample_pairs", "checksum"])
        for m in isolated.finditer(text):
            w.writerow(["ESP32-P4"] + list(m.groups()))

    budget = re.search(
        r"Target-rate budget: PitchMark=([0-9.]+) ms/s "
        r"other_PitchAnalysis=([0-9.]+) ms/s LPC=([0-9.]+) ms/s "
        r"total_Core1=([0-9.]+) ms/s", text)
    with (OUT / "b4b4f_target_rate_budget.csv").open(
            "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["phase", "scope", "rate_per_second", "us_per_unit",
                    "cpu_ms_per_second"])
        before_mark_us = 2368.19
        before_pitch_us = 4587.94
        before_lpc_us = 4295.25
        before_mark = before_mark_us * 200 / 1000
        before_other = (before_pitch_us - before_mark_us) * 200 / 1000
        before_lpc = before_lpc_us * 125 / 1000
        w.writerow(["before_B4B.4F", "PitchMark", 200, before_mark_us,
                    f"{before_mark:.3f}"])
        w.writerow(["before_B4B.4F", "OtherPitchAnalysis", 200,
                    f"{before_pitch_us - before_mark_us:.3f}",
                    f"{before_other:.3f}"])
        w.writerow(["before_B4B.4F", "LPC", 125, before_lpc_us,
                    f"{before_lpc:.3f}"])
        w.writerow(["before_B4B.4F", "TotalCore1", "", "",
                    f"{before_mark + before_other + before_lpc:.3f}"])
        if budget:
            mark, other, lpc, total = map(float, budget.groups())
            w.writerow(["after_B4B.4F", "PitchMark", 200, mark * 5, mark])
            w.writerow(["after_B4B.4F", "OtherPitchAnalysis", 200,
                        other * 5, other])
            w.writerow(["after_B4B.4F", "LPC", 125, lpc * 8, lpc])
            w.writerow(["after_B4B.4F", "TotalCore1", "", "", total])

    delta = re.search(r"^B4B4F_DELTA_HIST labels=([^ ]+) counts=([^\n]+)", text, re.M)
    ages = re.compile(
        r"^B4B4F_AGE_HIST bin=(\d+) attempts=(\d+) "
        r"distance_failures=(\d+) rate=([0-9.]+)", re.M)
    diagnostics = re.compile(r"^B4B4F_GRAIN_FAILURE (.+)$", re.M)
    with (OUT / "b4b4f_grain_alignment.csv").open(
            "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["scope", "bin", "count", "attempts", "distance_failures",
                    "failure_rate", "diagnostic"])
        if delta:
            for label, count in zip(delta.group(1).split(","), delta.group(2).split(",")):
                w.writerow(["signed_delta_period", label, count, "", "", "", ""])
        for m in ages.finditer(text):
            w.writerow(["pitch_age", m.group(1), "", m.group(2), m.group(3),
                        m.group(4), ""])
        for m in diagnostics.finditer(text):
            w.writerow(["diagnostic", "", "", "", "", "", m.group(1)])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if not args.no_flash:
        flash(args.port)
        time.sleep(.25)
    text = capture(args.port, args.timeout)
    materialize(text)
    return 0 if "B4B.4F RESULT:" in text else 2


if __name__ == "__main__":
    sys.exit(main())
