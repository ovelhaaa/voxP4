"""Flash and capture the 10-second Stage B4B.3 hardware audit."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import time

import serial


ROOT = pathlib.Path(__file__).resolve().parents[1]
ARTIFACT_DIR = ROOT / "artifacts" / "alpha01b"
REPORT_START = "Stage B4B.3 — Pitch Analysis Compute Hotspot Audit Report"
REPORT_END = "B4B.4 implemented: no"


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
        report_seen = False
        while time.monotonic() < deadline:
            raw = connection.readline()
            if not raw:
                if report_seen:
                    break
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            print(line, flush=True)
            lines.append(line)
            if REPORT_END in line:
                report_seen = True
                deadline = min(deadline, time.monotonic() + 1.0)
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b3_serial_log.txt").write_text(text, encoding="utf-8")
    return text


def observation(log: str, pattern: str, fallback: str = "not captured") -> str:
    match = re.search(pattern, log)
    return match.group(1).strip() if match else fallback


def write_report(log: str) -> pathlib.Path:
    clean_log = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", log).replace("�", "-")
    start = clean_log.find(REPORT_START)
    end = clean_log.find(REPORT_END, start)
    if start >= 0 and end >= 0:
        end = clean_log.find("\n", end)
        body = clean_log[start:end if end >= 0 else None].strip()
    else:
        body = "Stage B4B.3 report was not found in the captured serial output."

    report = "# Stage B4B.3 — Pitch Analysis Compute Hotspot Audit Report\n\n"
    report += "## Hardware observations\n\n"
    report += f"- ESP-IDF: {observation(clean_log, r'ESP-IDF:\s+(.*)')}\n"
    report += f"- ESP32-P4 revision: {observation(clean_log, r'chip revision:\s+(.*)')}\n"
    report += f"- PSRAM device: {observation(clean_log, r'Found (32MB PSRAM device)')}\n"
    report += f"- Reported runtime speed: {observation(clean_log, r'esp_psram: Speed:\s+(.*)')}\n"
    report += f"- Physical flash detected: {observation(clean_log, r'Detected size\((\d+k)\)', 'not captured')}\n"
    report += f"- Image header configured: {observation(clean_log, r'image header\((\d+k)\)', '2048k if boot warning was captured')}\n"
    apll = "EXPECTED_FALLBACK" if "AUDIO CLOCK: EXPECTED_FALLBACK" in clean_log else "not observed"
    report += f"- APLL result: {apll}\n\n"
    report += "## Audit result\n\n```text\n" + body + "\n```\n\n"
    report += (
        "B4B.3 is diagnostic only. No DSP algorithm, analysis parameter, FIFO "
        "size, task priority, PSOLA setting, or continuity policy was changed. "
        "B4B.4 is intentionally not implemented here.\n"
    )
    path = ARTIFACT_DIR / "stage_b4b3_pitch_analysis_hotspot_audit.md"
    path.write_text(report, encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(0.25)
    log = capture(args.port, args.timeout)
    report = write_report(log)
    print(f"B4B.3 report: {report}")
    return 0 if REPORT_END in log else 2


if __name__ == "__main__":
    sys.exit(main())
