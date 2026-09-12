"""Flash, capture, and materialize the Stage B4B.4B hardware benchmark."""

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
ARTIFACT_DIR = ROOT / "artifacts/alpha01b"
FINAL_SENTINEL = "algorithm-preserving non-float autocorrelation investigation"


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
            line = raw.decode("utf-8", errors="replace").rstrip()
            print(line, flush=True)
            lines.append(line)
            if FINAL_SENTINEL in line:
                sentinel_seen = True
                deadline = min(deadline, time.monotonic() + 1.0)
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b4b_serial_log.txt").write_text(text, encoding="utf-8")
    return text


def number(log: str, pattern: str) -> str:
    match = re.search(pattern, log, re.MULTILINE)
    return match.group(1) if match else ""


def write_csv(log: str) -> pathlib.Path:
    path = ARTIFACT_DIR / "b4b4b_autocorr_benchmark.csv"
    isolated_pattern = re.compile(
        r"^B4B4B_ISOLATED variant=(\S+) frames=(\d+) "
        r"autocorrelation_avg_us/frame=([0-9.]+) P50=(\d+) P95=(\d+) "
        r"P99=(\d+) max=(\d+) cycles/frame=([0-9.]+) "
        r"cycles/product=([0-9.]+) LPC_total_us/frame=([0-9.]+) "
        r"LPC_max_us=(\d+) valid_frames=(\d+) products/frame=(\d+)",
        re.MULTILINE,
    )
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow([
            "scope", "variant", "metric", "unit", "b4b4a", "measured",
            "speedup", "frames", "products_per_frame",
        ])
        isolated = list(isolated_pattern.finditer(log))
        reference_avg = float(isolated[0].group(3)) if isolated else 0.0
        for match in isolated:
            variant, frames = match.group(1), match.group(2)
            products = match.group(13)
            metrics = (
                ("autocorrelation_avg", "us/frame", match.group(3)),
                ("autocorrelation_p50", "us/frame", match.group(4)),
                ("autocorrelation_p95", "us/frame", match.group(5)),
                ("autocorrelation_p99", "us/frame", match.group(6)),
                ("autocorrelation_max", "us/frame", match.group(7)),
                ("cycles_per_frame", "cycles/frame", match.group(8)),
                ("cycles_per_product", "cycles/product", match.group(9)),
                ("lpc_total_avg", "us/frame", match.group(10)),
                ("lpc_total_max", "us/frame", match.group(11)),
                ("valid_frames", "frames", match.group(12)),
            )
            for metric, unit, value in metrics:
                speedup = ""
                if metric == "autocorrelation_avg" and float(value) != 0.0:
                    speedup = f"{reference_avg / float(value):.6f}"
                writer.writerow(["isolated", variant, metric, unit, "", value,
                                 speedup, frames, products])

        full_rows = (
            ("LPC window handling", "us/frame", "246.18",
             r"^LPC window handling \(us/frame\)\s+[0-9.]+\s+([0-9.]+)"),
            ("LPC autocorrelation", "us/frame", "13150.37",
             r"^LPC autocorrelation \(us/frame\)\s+[0-9.]+\s+([0-9.]+)"),
            ("LPC Levinson-Durbin", "us/frame", "181.42",
             r"^LPC Levinson-Durbin \(us/frame\)\s+[0-9.]+\s+([0-9.]+)"),
            ("LPC total", "us/frame", "14855.20",
             r"^LPC total \(us/frame\)\s+[0-9.]+\s+([0-9.]+)"),
            ("YinDifference", "us/pitch hop", "4127.86",
             r"^YinDifference \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)"),
            ("PitchAnalysis total", "us/pitch hop", "5136.14",
             r"^PitchAnalysis total \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)"),
            ("combined worker cost", "us/pitch hop", "19111.35",
             r"^Combined worker cost \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)"),
            ("Core1 utilization", "percent", "98.26",
             r"^Core 1 utilization \(percent\)\s+[0-9.]+\s+([0-9.]+)"),
            ("pitch throughput", "hops/s", "51.39",
             r"^Pitch throughput \(hops/s\)\s+[0-9.]+\s+([0-9.]+)"),
        )
        for metric, unit, before, pattern in full_rows:
            measured = number(log, pattern)
            speedup = ""
            if measured and float(measured) != 0.0:
                speedup = (float(measured) / float(before)
                           if metric == "pitch throughput"
                           else float(before) / float(measured))
                speedup = f"{speedup:.6f}"
            writer.writerow(["full_worker_retained_double",
                             "AUTOCORR_REFERENCE_DOUBLE", metric, unit,
                             before, measured, speedup, "", "17272"])
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(0.25)
    log = capture(args.port, args.timeout)
    path = write_csv(log)
    print(f"B4B.4B benchmark CSV: {path}")
    return 0 if FINAL_SENTINEL in log else 2


if __name__ == "__main__":
    sys.exit(main())
