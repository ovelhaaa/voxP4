"""Flash, capture, and materialize Stage B4B.6A low-F0 decomposition artifacts."""

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
    export_bat = pathlib.Path.home() / "esp/esp-idf/export.bat"
    command = f"call {export_bat} >nul && idf.py -p {port} flash"
    print(f"Flashing firmware to {port}...")
    done = subprocess.run(["cmd", "/d", "/c", command], cwd=ROOT)
    if done.returncode:
        print(f"Flashing failed with return code {done.returncode}")
        raise SystemExit(done.returncode)
    print("Flashing successful.")


def capture(port: str, timeout: float, decomp_only: bool = True) -> list[str]:
    lines: list[str] = []
    print(f"Opening {port} at 115200 baud (timeout={timeout}s)...")
    with serial.Serial(port, 115200, timeout=0.25, dsrdtr=False, rtscts=False) as conn:
        conn.dtr = False
        conn.rts = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = conn.readline()
            if not raw:
                continue
            line = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip()
            print(line, flush=True)
            lines.append(line)
            if decomp_only and "B4B6A_DECOMPOSITION_COMPLETE" in line:
                print("Detected B4B6A_DECOMPOSITION_COMPLETE. Stopping capture.")
                time.sleep(1)
                break
    return lines


def materialize(lines: list[str]) -> bool:
    OUT.mkdir(parents=True, exist_ok=True)
    csv_path = OUT / "b4b6a_pitch_mark_decomposition.csv"

    # Regex patterns
    method_re = re.compile(
        r"^B4B6A_DECOMP_METHOD f0_hz=([0-9.]+) period=(\d+) radius=(\d+) "
        r"window=(\d+) offsets=(\d+) pairs=(\d+) ref_cycles=([0-9.]+) "
        r"ref_us=([0-9.]+) reuse_aa_cycles=([0-9.]+) reuse_aa_us=([0-9.]+) "
        r"measured_raw_sum=(\d+) scale=([0-9.]+) other_cycles=([0-9.]+)"
    )
    cost_re = re.compile(
        r"^B4B6A_DECOMP_COST f0_hz=([0-9.]+) category=(\S+) raw_cycles=(\d+) "
        r"normalized_cycles=([0-9.]+) percent=([0-9.]+)"
    )

    metadata: dict[float, dict[str, str]] = {}
    rows: list[dict[str, str]] = []

    for line in lines:
        m = method_re.match(line)
        if m:
            hz = float(m.group(1))
            metadata[hz] = {
                "f0_hz": m.group(1),
                "period": m.group(2),
                "radius": m.group(3),
                "window": m.group(4),
                "offsets": m.group(5),
                "pairs": m.group(6),
                "ref_cycles": m.group(7),
                "ref_us": m.group(8),
                "reuse_aa_cycles": m.group(9),
                "reuse_aa_us": m.group(10),
                "measured_raw_sum": m.group(11),
                "scale": m.group(12),
                "other_cycles": m.group(13),
            }
            continue

        c = cost_re.match(line)
        if c:
            hz = float(c.group(1))
            cat = c.group(2)
            raw = c.group(3)
            norm = c.group(4)
            pct = c.group(5)
            meta = metadata.get(hz, {})
            rows.append({
                "f0_hz": meta.get("f0_hz", str(hz)),
                "period": meta.get("period", ""),
                "radius": meta.get("radius", ""),
                "window": meta.get("window", ""),
                "offsets": meta.get("offsets", ""),
                "sample_pairs": meta.get("pairs", ""),
                "reference_cycles": meta.get("ref_cycles", ""),
                "reference_us": meta.get("ref_us", ""),
                "reuse_aa_cycles": meta.get("reuse_aa_cycles", ""),
                "reuse_aa_us": meta.get("reuse_aa_us", ""),
                "category": cat,
                "raw_cycles": raw,
                "normalized_cycles": norm,
                "percent": pct,
            })

    if not rows:
        print("Warning: No B4B6A_DECOMP_COST lines found in log.")
        return False

    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "f0_hz", "period", "radius", "window", "offsets", "sample_pairs",
            "reference_cycles", "reference_us", "reuse_aa_cycles", "reuse_aa_us",
            "category", "raw_cycles", "normalized_cycles", "percent"
        ])
        writer.writeheader()
        writer.writerows(rows)

    print(f"Materialized: {csv_path} ({len(rows)} rows)")
    return True


def main():
    parser = argparse.ArgumentParser(description="B4B.6A Low-F0 Monitor")
    parser.add_argument("--port", default="COM11", help="Serial port (default: COM11)")
    parser.add_argument("--flash", action="store_true", help="Flash before capturing")
    parser.add_argument("--timeout", type=float, default=60.0, help="Capture timeout (seconds)")
    parser.add_argument("--full", action="store_true", help="Wait for full RC validation rather than just decomp")
    args = parser.parse_args()

    if args.flash:
        flash(args.port)

    lines = capture(args.port, args.timeout, decomp_only=not args.full)
    log_path = OUT / "b4b6a_serial_log.txt"
    log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Saved raw log: {log_path}")

    ok = materialize(lines)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
