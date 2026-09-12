"""Flash, capture, and materialize the Stage B4B.4E hardware audit."""

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
ARTIFACT_DIR = ROOT / "artifacts" / "alpha01b"
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def flash(port: str) -> None:
    export_bat = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export_bat} >nul && idf.py -p {port} flash"
    completed = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if completed.returncode:
        raise SystemExit(completed.returncode)


def capture(port: str, timeout_seconds: float) -> str:
    lines: list[str] = []
    with serial.Serial(port=port, baudrate=115200, timeout=0.25,
                       dsrdtr=False, rtscts=False) as connection:
        connection.dtr = False
        connection.rts = False
        deadline = time.monotonic() + timeout_seconds
        finished = False
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                if finished:
                    break
                continue
            line = ANSI_ESCAPE.sub(
                "", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if "NEXT STEP:" in line:
                finished = True
                deadline = min(deadline, time.monotonic() + 2.0)
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b4e_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def write_benchmark(log: str) -> None:
    legacy = re.compile(
        r"^B4B4D_ISOLATED variant=(\S+) hops=(\d+) cold_us=(\d+) "
        r"cold_cycles=(\d+) avg_us/hop=([0-9.]+) P50=(\d+) P95=(\d+) "
        r"P99=(\d+) max=(\d+) cycles/hop=([0-9.]+).*products/hop=(\d+)",
        re.MULTILINE)
    incremental = re.compile(
        r"^B4B4E_ISOLATED variant=(\S+) rebase_hops=(\d+) hops=(\d+) "
        r"cold_us=(\d+) cold_cycles=(\d+) avg_us/hop=([0-9.]+) "
        r"P50=(\d+) P95=(\d+) P99=(\d+) max=(\d+) cycles/hop=([0-9.]+) "
        r"operations/hop=([0-9.]+) periodic_rebases=(\d+) checksum=(\S+)",
        re.MULTILINE)
    full_worker = re.search(
        r"^YinDifference\s+([0-9.]+)\s+[0-9.]+\s+(\d+)\s+"
        r"[0-9.]+\s+(\d+)\s+(\d+)\s+([0-9.]+)$", log, re.MULTILINE)
    telemetry = re.search(
        r"^YIN update telemetry: executions=(\d+) update_terms=(\d+) "
        r"full_rebase_products=(\d+) rebases=(\d+) gap_rebases=(\d+) "
        r"periodic_rebases=(\d+)$", log, re.MULTILINE)
    path = ARTIFACT_DIR / "b4b4e_yin_benchmark.csv"
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["scope", "variant", "rebase_hops", "hops", "cold_us",
                         "cold_cycles", "avg_us_per_hop", "p50_us", "p95_us",
                         "p99_us", "max_us", "cycles_per_hop",
                         "operations_per_hop", "periodic_rebases", "checksum"])
        for match in legacy.finditer(log):
            g = match.groups()
            writer.writerow(["isolated", g[0], "", g[1], g[2], g[3], g[4],
                             g[5], g[6], g[7], g[8], g[9], g[10], g[1], ""])
        for match in incremental.finditer(log):
            g = match.groups()
            writer.writerow(["isolated", g[0], g[1], g[2], g[3], g[4], g[5],
                             g[6], g[7], g[8], g[9], g[10], g[11], g[12], g[13]])
        if full_worker and telemetry:
            f = full_worker.groups()
            t = telemetry.groups()
            executions = int(t[0])
            operations = ((int(t[1]) + int(t[2])) / executions
                          if executions else 0.0)
            writer.writerow(["full_worker", "YIN_DIFF_INCREMENTAL_F32", 64,
                             executions, "", "", f[0], "", "", "", f[2],
                             f[4], f"{operations:.5f}", t[5], ""])


def write_grain(log: str) -> None:
    summary = re.search(
        r"^Grain rejection: source_negative=(\d+) select_total=(\d+) "
        r"no_marks=(\d+) low_confidence=(\d+) invalid_period=(\d+) "
        r"distance_too_large=(\d+) history_total=(\d+) "
        r"center_before_half=(\d+) history_too_old=(\d+) future_end=(\d+)",
        log, re.MULTILINE)
    diagnostic = re.compile(
        r"^B4B4E_GRAIN_FAILURE index=(\d+) reason=(\d+) input_end=(\d+) "
        r"oldest=(\d+) pitch_timestamp=(\d+) pitch_age_samples=(\d+) "
        r"source=([-0-9.]+) destination=([-0-9.]+) center=(\d+) "
        r"mark_age=(\d+) half=(\d+) required_first=(\d+) required_last=(\d+) "
        r"distance=([0-9.]+) allowed=([0-9.]+)", re.MULTILINE)
    path = ARTIFACT_DIR / "b4b4e_grain_rejection.csv"
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["scope", "index", "reason", "input_end", "oldest",
                         "pitch_timestamp", "pitch_age_samples", "source",
                         "destination", "center", "mark_age", "half_window",
                         "required_first", "required_last", "distance", "allowed",
                         "source_negative", "select_total", "no_marks",
                         "low_confidence", "invalid_period", "distance_too_large",
                         "history_total", "center_before_half", "history_too_old",
                         "future_end"])
        if summary:
            writer.writerow(["summary"] + [""] * 15 + list(summary.groups()))
        for match in diagnostic.finditer(log):
            writer.writerow(["diagnostic"] + list(match.groups()) + [""] * 10)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.no_flash:
        flash(args.port)
        time.sleep(0.25)
    log = capture(args.port, args.timeout)
    write_benchmark(log)
    write_grain(log)
    return 0 if "B4B.4E RESULT:" in log else 2


if __name__ == "__main__":
    sys.exit(main())
