"""Flash and capture the Stage B4B.4K ESP32-P4 audit."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import time

import serial

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "artifacts" / "alpha01b" / "b4b4k_full_worker.txt"
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def flash(port: str) -> None:
    export = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export} >nul && idf.py -p {port} flash"
    completed = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if completed.returncode:
        raise SystemExit(completed.returncode)


def capture(port: str, timeout: float) -> int:
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
    OUT.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    OUT.write_text(text, encoding="utf-8")
    return 0 if re.search(r"B4B\.4K RESULT:\s*PASS", text) else 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--no-flash", action="store_true")
    args = parser.parse_args()
    if not args.no_flash:
        flash(args.port)
        time.sleep(.25)
    return capture(args.port, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
