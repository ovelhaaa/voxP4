#!/usr/bin/env python3
"""Flash the pinned-IDF prediction-audit image and capture COM11 to B4D12_END."""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
IDF_PYTHON = (Path.home() / ".espressif" / "python_env" /
              "idf5.3_py3.14_env" / "Scripts" / "python.exe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--output", type=Path,
                    default=ROOT / "artifacts" / "alpha01d" / "psola_prediction_raw.txt")
    ap.add_argument("--timeout", type=int, default=360)
    ap.add_argument("--skip-flash", action="store_true")
    args = ap.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    python = str(IDF_PYTHON if IDF_PYTHON.exists() else Path(sys.executable))

    if not args.skip_flash:
        # Existing P4 rev 1.3 board needs the same image-header limit used by
        # prior repository runners. Only generated build products are changed.
        for name, elf, extra in (
            ("bootloader/bootloader.bin", "bootloader/bootloader.elf", []),
            ("vox_p4.bin", "vox_p4.elf", ["--elf-sha256-offset", "0xb0"]),
        ):
            subprocess.run([python, "-m", "esptool", "--chip", "esp32p4",
                            "elf2image", "--flash_mode", "dio", "--flash_freq", "80m",
                            "--flash_size", "2MB", "--min-rev-full", "1",
                            "--max-rev-full", "199", *extra, "-o", str(BUILD / name),
                            str(BUILD / elf)], check=True)
        subprocess.run([python, "-m", "esptool", "--chip", "esp32p4",
                        "-p", args.port, "-b", "460800", "--before", "default_reset",
                        "--after", "hard_reset", "write_flash", "--force",
                        "@flash_args"], cwd=BUILD, check=True)

    with serial.Serial(args.port, 115200, timeout=0.25) as port, \
            args.output.open("wb") as output:
        port.dtr = False
        port.rts = True
        time.sleep(0.1)
        port.rts = False
        start = time.monotonic()
        tail = b""
        total = 0
        while time.monotonic() - start < args.timeout:
            chunk = port.read(8192)
            if chunk:
                output.write(chunk)
                total += len(chunk)
                tail = (tail + chunk)[-256:]
                if b"B4D12_END" in tail:
                    print(f"captured {total} bytes in {time.monotonic()-start:.1f}s: {args.output}")
                    return
    raise SystemExit(f"capture timed out after {args.timeout}s: {args.output}")


if __name__ == "__main__":
    main()
