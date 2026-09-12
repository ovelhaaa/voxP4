"""Flash and capture the 15-second Stage B4B.2 hardware audit."""

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
            if "22. FINAL CLASSIFICATION:" in line:
                report_seen = True
                deadline = min(deadline, time.monotonic() + 3.0)
    text = "\n".join(lines) + "\n"
    (ARTIFACT_DIR / "b4b2_serial_log.txt").write_text(text, encoding="utf-8")
    return text


def write_report(log: str) -> pathlib.Path:
    clean_log = re.sub(r"\x1b\[[0-9;]*m", "", log).replace("�", "-")
    match = re.search(
        r"(Stage B4B\.2 . Pitch Worker Cadence & Lock Acquisition Audit Report"
        r".*?22\. FINAL CLASSIFICATION:.*?$"
        r".*?First failed arrow:.*?$"
        r".*?B4C READY:.*?$)",
        clean_log,
        flags=re.DOTALL | re.MULTILINE,
    )
    body = match.group(1).strip() if match else (
        "Stage B4B.2 report was not found in the captured serial output."
    )
    report = "# Stage B4B.2 — Pitch Worker Cadence & Lock Acquisition Audit Report\n\n"
    psram_size = re.search(r"PSRAM device: (.*)", clean_log)
    psram_speed = re.search(r"Reported runtime speed: (.*)", clean_log)
    flash_size = re.search(r"Physical flash detected: (.*)", clean_log)
    header_size = re.search(r"Image header configured: (.*)", clean_log)
    apll = ("EXPECTED_FALLBACK" if "AUDIO CLOCK: EXPECTED_FALLBACK" in clean_log
            else "not observed")
    report += "## Hardware observations\n\n"
    report += f"- PSRAM device: {psram_size.group(1) if psram_size else 'not captured'}\n"
    report += f"- Reported runtime speed: {psram_speed.group(1) if psram_speed else 'not captured'}\n"
    report += f"- Physical flash detected: {flash_size.group(1) if flash_size else 'not captured'}\n"
    report += f"- Image header configured: {header_size.group(1) if header_size else 'not captured'}\n"
    report += f"- APLL result: {apll}\n\n"
    report += "## Audit result\n\n"
    report += "```text\n" + body + "\n```\n"
    report += (
        "\nThe physical-flash/image-header mismatch is intentionally deferred to "
        "a later milestone. B4C remains not ready until pitch-lock behavior, "
        "diagnostic queue overflow, and teardown are all clean on hardware.\n"
    )
    path = ARTIFACT_DIR / "stage_b4b2_pitch_worker_lock_audit.md"
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
        time.sleep(0.75)
    log = capture(args.port, args.timeout)
    report = write_report(log)
    print(f"B4B.2 report: {report}")
    return 0 if "22. FINAL CLASSIFICATION:" in log else 2


if __name__ == "__main__":
    sys.exit(main())
