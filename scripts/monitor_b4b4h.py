"""Flash, capture, and materialize the Stage B4B.4H hardware audit."""

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
    (OUT / "b4b4h_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def materialize(text: str) -> None:
    isolated = re.compile(
        r"^B4B4H_ISOLATED variant=(\S+) frames=(\d+) "
        r"window_avg_us/frame=([0-9.]+) P50=(\d+) P95=(\d+) "
        r"P99=(\d+) max=(\d+) cycles/frame=([0-9.]+) "
        r"P50_cycles=(\d+) P95_cycles=(\d+) P99_cycles=(\d+) "
        r"max_cycles=(\d+) LPC_total_us/frame=([0-9.]+) "
        r"LPC_P50=(\d+) LPC_P95=(\d+) LPC_P99=(\d+) LPC_max=(\d+) "
        r"valid_frames=(\d+) checksum=(\S+)", re.M)
    with (OUT / "b4b4h_lpc_energy_benchmark.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow([
            "variant", "frames", "window_avg_us_per_frame", "window_p50_us",
            "window_p95_us", "window_p99_us", "window_max_us",
            "window_cycles_per_frame", "window_p50_cycles",
            "window_p95_cycles", "window_p99_cycles", "window_max_cycles",
            "lpc_total_avg_us_per_frame", "lpc_total_p50_us",
            "lpc_total_p95_us", "lpc_total_p99_us", "lpc_total_max_us",
            "valid_frames", "checksum"])
        for match in isolated.finditer(text):
            writer.writerow(match.groups())

    budget = re.search(
        r"^Target-rate budget: PitchMark=([0-9.]+) ms/s "
        r"other_PitchAnalysis=([0-9.]+) ms/s LPC=([0-9.]+) ms/s "
        r"total_Core1=([0-9.]+) ms/s", text, re.M)
    full = re.search(
        r"^B4B4H_FULL_WORKER pitch_mark_us/hop=([0-9.]+) "
        r"other_pitch_us/hop=([0-9.]+) pitch_analysis_us/hop=([0-9.]+) "
        r"yin_difference_us/hop=([0-9.]+).*LPC_total_us/frame=([0-9.]+)",
        text, re.M)
    with (OUT / "b4b4h_target_rate_budget.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["phase", "scope", "rate_per_second", "us_per_unit",
                         "cpu_ms_per_second"])
        writer.writerow(["before_B4B.4H", "PitchMark", 200, 641.25, 128.250])
        writer.writerow(["before_B4B.4H", "OtherPitchAnalysis", 200,
                         2126.445, 425.289])
        writer.writerow(["before_B4B.4H", "LPC", 125, 3539.96, 442.495])
        writer.writerow(["before_B4B.4H", "TotalCore1", "", "", 996.033])
        if budget and full:
            mark, other, lpc, total = map(float, budget.groups())
            mark_us, other_us, _pitch_us, _yin_us, lpc_us = map(float,
                                                                 full.groups())
            writer.writerow(["after_B4B.4H", "PitchMark", 200, mark_us, mark])
            writer.writerow(["after_B4B.4H", "OtherPitchAnalysis", 200,
                             other_us, other])
            writer.writerow(["after_B4B.4H", "LPC", 125, lpc_us, lpc])
            writer.writerow(["after_B4B.4H", "TotalCore1", "", "", total])


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
    return 0 if "B4B.4H RESULT:" in text else 2


if __name__ == "__main__":
    sys.exit(main())
