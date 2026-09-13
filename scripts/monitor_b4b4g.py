"""Flash, capture, and materialize the Stage B4B.4G hardware audit."""

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
                continue
            line = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if line == "NEXT STEP:":
                finishing = True
                deadline = min(deadline, time.monotonic() + 3)
    text = "\n".join(lines) + "\n"
    (OUT / "b4b4g_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def materialize(text: str) -> None:
    isolated = re.compile(
        r"^B4B4G_ISOLATED variant=(\S+) frames=(\d+) "
        r"window_avg_us/frame=([0-9.]+) P50=(\d+) P95=(\d+) "
        r"P99=(\d+) max=(\d+) cycles/frame=([0-9.]+) "
        r"P50_cycles=(\d+) P95_cycles=(\d+) P99_cycles=(\d+) "
        r"max_cycles=(\d+) LPC_total_us/frame=([0-9.]+) "
        r"valid_frames=(\d+) checksum=(\S+)", re.M)
    component = re.compile(
        r"^B4B4G_COMPONENT runtime_hann_cos_avg_us/frame=([0-9.]+) "
        r"cycles/frame=([0-9.]+) old_LPC_percent=([0-9.]+) "
        r"double_energy_avg_us/frame=([0-9.]+) "
        r"double_energy_cycles/frame=([0-9.]+) checksum=(\S+)", re.M)
    costs = re.compile(
        r"^B4B4G_COST category=(\S+) raw_cycles/frame=([0-9.]+) "
        r"normalized_cycles/frame=([0-9.]+) percent=([0-9.]+)", re.M)
    with (OUT / "b4b4g_lpc_windowing_benchmark.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow([
            "record", "variant_or_category", "frames", "avg_us_per_frame",
            "p50_us", "p95_us", "p99_us", "max_us", "cycles_per_frame",
            "p50_cycles", "p95_cycles", "p99_cycles", "max_cycles",
            "lpc_total_us_per_frame", "valid_frames", "normalized_cycles",
            "percent", "checksum"])
        for match in isolated.finditer(text):
            g = match.groups()
            writer.writerow(["isolated", g[0], g[1], g[2], g[3], g[4],
                             g[5], g[6], g[7], g[8], g[9], g[10], g[11],
                             g[12], g[13], "", "", g[14]])
        match = component.search(text)
        if match:
            (hann_us, hann_cycles, hann_percent, energy_us, energy_cycles,
             checksum) = match.groups()
            writer.writerow(["component", "runtime_hann_cos", "128", hann_us,
                             "", "", "", "", hann_cycles, "", "", "", "",
                             "", "", "", hann_percent, checksum])
            writer.writerow(["component", "double_energy", "128", energy_us,
                             "", "", "", "", energy_cycles, "", "", "", "",
                             "", "", "", "", checksum])
        for match in costs.finditer(text):
            name, raw, normalized, percent = match.groups()
            writer.writerow(["decomposition", name, "128", "", "", "", "",
                             "", raw, "", "", "", "", "", "", normalized,
                             percent, ""])

    budget = re.search(
        r"^Target-rate budget: PitchMark=([0-9.]+) ms/s "
        r"other_PitchAnalysis=([0-9.]+) ms/s LPC=([0-9.]+) ms/s "
        r"total_Core1=([0-9.]+) ms/s", text, re.M)
    full = re.search(
        r"^B4B4G_FULL_WORKER pitch_mark_us/hop=([0-9.]+) "
        r"other_pitch_us/hop=([0-9.]+) pitch_analysis_us/hop=([0-9.]+) "
        r"yin_difference_us/hop=([0-9.]+).*LPC_total_us/frame=([0-9.]+)",
        text, re.M)
    with (OUT / "b4b4g_target_rate_budget.csv").open(
            "w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["phase", "scope", "rate_per_second", "us_per_unit",
                         "cpu_ms_per_second"])
        writer.writerow(["before_B4B.4G", "PitchMark", 200, 646.71, 129.342])
        writer.writerow(["before_B4B.4G", "OtherPitchAnalysis", 200,
                         2114.445, 422.889])
        writer.writerow(["before_B4B.4G", "LPC", 125, 4090.08, 511.260])
        writer.writerow(["before_B4B.4G", "TotalCore1", "", "", 1063.491])
        if budget and full:
            mark, other, lpc, total = map(float, budget.groups())
            mark_us, other_us, _pitch_us, _yin_us, lpc_us = map(float,
                                                                 full.groups())
            writer.writerow(["after_B4B.4G", "PitchMark", 200, mark_us, mark])
            writer.writerow(["after_B4B.4G", "OtherPitchAnalysis", 200,
                             other_us, other])
            writer.writerow(["after_B4B.4G", "LPC", 125, lpc_us, lpc])
            writer.writerow(["after_B4B.4G", "TotalCore1", "", "", total])


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
    return 0 if "B4B.4G RESULT:" in text else 2


if __name__ == "__main__":
    sys.exit(main())
