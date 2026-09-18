#!/usr/bin/env python3
"""B4D.12 burn-in serial capture.

Reads a serial port until a stop sentinel is seen (or a hard timeout expires)
and writes the raw stream to a file.  Used for the layered burn-in campaign;
the firmware prints B4D12_END exactly once when the campaign completes.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - tooling environment
    sys.stderr.write("pyserial is required (pip install pyserial)\n")
    raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("output")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=int, default=0,
                    help="hard timeout in seconds (0 = no hard timeout)")
    ap.add_argument("--sentinel", default="B4D12_END")
    args = ap.parse_args()

    start = time.time()
    sentinel = args.sentinel.encode()
    tail = bytearray()
    total = 0
    with serial.Serial(args.port, args.baud, timeout=0.25) as ser, \
            open(args.output, "wb") as out:
        while True:
            chunk = ser.read(8192)
            if chunk:
                out.write(chunk)
                out.flush()
                total += len(chunk)
                tail = (tail + chunk)[-256:]
                if sentinel and sentinel in tail:
                    break
            if args.timeout and (time.time() - start) > args.timeout:
                sys.stderr.write("capture hard timeout reached\n")
                break
    sys.stderr.write("captured %d bytes in %.1f s -> %s\n"
                     % (total, time.time() - start, args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
