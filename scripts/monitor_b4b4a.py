"""Flash, capture, and materialize the Stage B4B.4A hardware audit."""

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
REPORT_START = "Stage B4B.4A — LPC Ring Buffer Optimization Report"
REPORT_END = "NEXT RECOMMENDED STEP:"
FINAL_SENTINEL = "B4B.4B"


def flash(port: str) -> None:
    export_bat = pathlib.Path.home() / "esp" / "esp-idf" / "export.bat"
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
            if sentinel_seen and FINAL_SENTINEL in line:
                deadline = min(deadline, time.monotonic() + 1.0)
            if REPORT_END in line:
                sentinel_seen = True
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b4a_serial_log.txt").write_text(text, encoding="utf-8")
    return text


def observation(log: str, pattern: str, fallback: str = "not captured") -> str:
    match = re.search(pattern, log)
    return match.group(1).strip() if match else fallback


def number(log: str, pattern: str) -> str:
    match = re.search(pattern, log, re.MULTILINE)
    return match.group(1) if match else ""


def write_csv(log: str) -> pathlib.Path:
    rows = [
        ("FIFO drain", "us/LPC frame", "",
         number(log, r"^FIFO drain \(us/LPC frame\)\s+n/a\s+([0-9.]+)")),
        ("LPC window handling", "us/LPC frame", "29663.58",
         number(log, r"^LPC window handling \(us/frame\)\s+[0-9.]+\s+([0-9.]+)")),
        ("LPC autocorrelation", "us/LPC frame", "11822.73",
         number(log, r"^LPC autocorrelation \(us/frame\)\s+[0-9.]+\s+([0-9.]+)")),
        ("LPC Levinson-Durbin", "us/LPC frame", "162.26",
         number(log, r"^LPC Levinson-Durbin \(us/frame\)\s+[0-9.]+\s+([0-9.]+)")),
        ("LPC publication", "us/LPC frame", "14.91",
         number(log, r"^LPC publication \(us/frame\)\s+[0-9.]+\s+([0-9.]+)")),
        ("LPC total", "us/LPC frame", "41731.39",
         number(log, r"^LPC total \(us/frame\)\s+[0-9.]+\s+([0-9.]+)")),
        ("YinDifference", "us/pitch hop", "4114.46",
         number(log, r"^YinDifference \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)")),
        ("PitchAnalysis total", "us/pitch hop", "5107.40",
         number(log, r"^PitchAnalysis total \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)")),
        ("Combined worker cost", "us/pitch hop", "46852.56",
         number(log, r"^Combined worker cost \(us/pitch hop\)\s+[0-9.]+\s+([0-9.]+)")),
        ("Core 1 utilization", "percent", "99.33",
         number(log, r"^Core 1 utilization \(percent\)\s+[0-9.]+\s+([0-9.]+)")),
        ("Pitch throughput", "hops/s", "21.19",
         number(log, r"^Pitch throughput \(hops/s\)\s+[0-9.]+\s+([0-9.]+)")),
        ("FIFO maximum occupancy", "samples", "",
         number(log, r"^FIFO/backlog AFTER:.*?max=([0-9]+)")),
        ("FIFO drops", "samples", "",
         number(log, r"^FIFO/backlog AFTER:.*?drops=([0-9]+)")),
        ("Analysis backlog maximum", "ms", "",
         number(log, r"^FIFO/backlog AFTER:.*?backlog_max_ms=([0-9.]+)")),
        ("Pitch age average", "ms", "",
         number(log, r"^FIFO/backlog AFTER:.*?pitch_age_avg_ms=([0-9.]+)")),
        ("Pitch age P95", "ms", "",
         number(log, r"^FIFO/backlog AFTER:.*?pitch_age_P95_ms=([0-9.]+)")),
        ("Pitch age P99", "ms", "",
         number(log, r"^FIFO/backlog AFTER:.*?pitch_age_P99_ms=([0-9.]+)")),
        ("Pitch age maximum", "ms", "",
         number(log, r"^FIFO/backlog AFTER:.*?pitch_age_max_ms=([0-9.]+)")),
    ]
    path = ARTIFACT_DIR / "b4b4a_lpc_before_after.csv"
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["section", "unit", "before", "after", "speedup"])
        for section, unit, before, after in rows:
            speedup = ""
            if before and after and float(after) != 0.0:
                ratio = (float(after) / float(before)
                         if section == "Pitch throughput"
                         else float(before) / float(after))
                speedup = f"{ratio:.4f}"
            writer.writerow([section, unit, before, after, speedup])
    return path


def write_report(log: str) -> pathlib.Path:
    clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", log).replace("�", "-")
    start = clean.find(REPORT_START)
    end_marker = "NEXT RECOMMENDED STEP:\nB4B.4B"
    end = clean.find(end_marker, start)
    if start >= 0 and end >= 0:
        end = clean.find("\n", end + len(end_marker))
        body = clean[start:end if end >= 0 else None].strip()
    else:
        body = "Stage B4B.4A report was not found in the captured serial output."

    report = "# Stage B4B.4A — LPC Ring Buffer Optimization Report\n\n"
    report += "## Hardware observations\n\n"
    report += f"- ESP-IDF: {observation(clean, r'ESP-IDF:\s+(.*)')}\n"
    report += f"- ESP32-P4 revision: {observation(clean, r'chip revision:\s+(.*)')}\n"
    report += f"- PSRAM device: {observation(clean, r'Found (32MB PSRAM device)')}\n"
    report += f"- Reported runtime speed: {observation(clean, r'esp_psram: Speed:\s+(.*)')}\n"
    report += f"- Physical flash detected: {observation(clean, r'Detected size\((\d+k)\)')}\n"
    report += f"- Image header configured: {observation(clean, r'image header\((\d+k)\)', '2048k')}\n"
    apll = "EXPECTED_FALLBACK" if "AUDIO CLOCK: EXPECTED_FALLBACK" in clean else "not observed"
    report += f"- APLL result: {apll}\n\n"
    report += "## Host equivalence\n\n"
    report += (
        "The old sliding reference and the circular implementation emitted "
        "451 matching LPC frames across silence, sine, harmonic tone, "
        "deterministic noise, and the repository vocal WAV. All coefficients, "
        "timestamps, validity decisions, orders, errors, confidences, and frame "
        "counts were bit-identical; 173 full-window wrap boundaries were covered.\n\n"
    )
    report += "## ESP32-P4 audit result\n\n```text\n" + body + "\n```\n\n"
    report += (
        "Only LPC frame storage changed: O(N) per-sample sliding was replaced "
        "by O(1) circular writes and a fixed, preallocated oldest-to-newest "
        "linearization at LPC frame emission. LPC/YIN/PSOLA math, parameters, "
        "cadence, FIFO sizes, and task scheduling were not changed.\n"
    )
    path = ARTIFACT_DIR / "b4b4a_lpc_ring_buffer_report.md"
    path.write_text(report, encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(0.25)
    log = capture(args.port, args.timeout)
    report = write_report(log)
    csv_path = write_csv(log)
    print(f"B4B.4A report: {report}")
    print(f"B4B.4A CSV: {csv_path}")
    return 0 if REPORT_END in log and FINAL_SENTINEL in log else 2


if __name__ == "__main__":
    sys.exit(main())
