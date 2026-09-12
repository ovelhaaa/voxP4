"""Flash, capture, and materialize the Stage B4B.4D P4 benchmark."""

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
FINAL_SENTINELS = (
    "strictly follow the measured primary hotspot",
    "SCALAR_KERNEL_OPTIMIZATION_INSUFFICIENT",
)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def flash(port: str) -> None:
    export_bat = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export_bat} >nul && idf.py -p {port} flash"
    completed = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if completed.returncode:
        raise SystemExit(completed.returncode)


def capture(port: str, timeout_seconds: float) -> str:
    ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
    lines: list[str] = []
    with serial.Serial(port=port, baudrate=115200, timeout=0.25,
                       dsrdtr=False, rtscts=False) as connection:
        connection.dtr = False
        connection.rts = False
        deadline = time.monotonic() + timeout_seconds
        sentinel_seen = False
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                if sentinel_seen:
                    break
                continue
            line = ANSI_ESCAPE.sub(
                "", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if any(sentinel in line for sentinel in FINAL_SENTINELS):
                sentinel_seen = True
                deadline = min(deadline, time.monotonic() + 1.0)
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b4d_full_worker.txt").write_text(text, encoding="utf-8")
    return text


def value(log: str, pattern: str) -> str:
    match = re.search(pattern, log, re.MULTILINE)
    return match.group(1) if match else ""


def write_benchmark_csv(log: str) -> pathlib.Path:
    path = ARTIFACT_DIR / "b4b4d_yin_benchmark.csv"
    isolated = re.compile(
        r"^B4B4D_ISOLATED variant=(\S+) hops=(\d+) cold_us=(\d+) "
        r"cold_cycles=(\d+) avg_us/hop=([0-9.]+) P50=(\d+) P95=(\d+) "
        r"P99=(\d+) max=(\d+) cycles/hop=([0-9.]+) "
        r"cycles/product=([0-9.]+) taus=(\d+) products/hop=(\d+) "
        r"reference_products=(\d+) candidate_products=(\d+) checksum=(\S+)",
        re.MULTILINE,
    )
    matches = list(isolated.finditer(log))
    reference_us = float(matches[0].group(5)) if matches else 0.0
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow([
            "scope", "variant", "hops", "cold_us", "cold_cycles",
            "avg_us_per_hop", "p50_us", "p95_us", "p99_us", "max_us",
            "cycles_per_hop", "cycles_per_product", "speedup_vs_reference",
            "taus", "products_per_hop", "reference_products",
            "candidate_products", "checksum",
        ])
        for match in matches:
            fields = list(match.groups())
            measured_us = float(fields[4])
            speedup = reference_us / measured_us if measured_us else 0.0
            writer.writerow(["isolated", fields[0], fields[1], fields[2],
                             fields[3], fields[4], fields[5], fields[6],
                             fields[7], fields[8], fields[9], fields[10],
                             f"{speedup:.6f}", fields[11], fields[12],
                             fields[13], fields[14], fields[15]])

        full_metrics = (
            ("YinDifference", "us/pitch hop"),
            ("PitchAnalysis total", "us/pitch hop"),
            ("LPC total", "us/frame"),
            ("Combined worker cost", "us/pitch hop"),
            ("Core 1 utilization", "percent"),
            ("Pitch throughput", "hops/s"),
        )
        for metric, unit in full_metrics:
            match = re.search(
                rf"^{re.escape(metric)} \([^\n]+\)\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)x",
                log, re.MULTILINE)
            if match:
                writer.writerow(["full_worker", "YIN_DIFF_FMA_8ACC", "",
                                 "", "", match.group(2), "", "", "", "",
                                 "", "", match.group(3), "", "77515",
                                 "77515", "77515", f"{metric} [{unit}]"])
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=75.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(0.25)
    log = capture(args.port, args.timeout)
    benchmark = write_benchmark_csv(log)
    print(f"B4B.4D benchmark CSV: {benchmark}")
    return 0 if any(sentinel in log for sentinel in FINAL_SENTINELS) else 2


if __name__ == "__main__":
    sys.exit(main())
