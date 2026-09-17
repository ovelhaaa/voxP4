"""Create the small deterministic real-vocal replay fixture for B4B.6."""

from __future__ import annotations

import argparse
import array
import pathlib
import sys
import wave


SEGMENTS = ((0.0, 4.0), (8.0, 4.0), (16.0, 4.0),
            (25.0, 4.0), (33.0, 4.0), (41.0, 4.0))
OUTPUT_RATE = 8000


def linear_to_mulaw(sample: int) -> int:
    sign = 0x80 if sample < 0 else 0
    magnitude = min(abs(sample), 32635) + 0x84
    exponent = 7
    mask = 0x4000
    while exponent > 0 and not magnitude & mask:
        exponent -= 1
        mask >>= 1
    mantissa = (magnitude >> (exponent + 3)) & 0x0F
    return (~(sign | (exponent << 4) | mantissa)) & 0xFF


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    with wave.open(str(args.source), "rb") as stream:
        if stream.getsampwidth() != 2 or stream.getcomptype() != "NONE":
            raise SystemExit("B4B.6 fixture must be uncompressed signed PCM16")
        channels = stream.getnchannels()
        source_rate = stream.getframerate()
        frames = stream.getnframes()
        pcm = array.array("h", stream.readframes(frames))
    if sys.byteorder != "little":
        pcm.byteswap()

    output = bytearray()
    for start_seconds, duration_seconds in SEGMENTS:
        start = int(round(start_seconds * source_rate))
        output_count = int(round(duration_seconds * OUTPUT_RATE))
        for destination in range(output_count):
            position = start + destination * source_rate / OUTPUT_RATE
            whole = min(int(position), frames - 2)
            fraction = position - whole
            a = sum(pcm[whole * channels + channel]
                    for channel in range(channels)) / channels
            b = sum(pcm[(whole + 1) * channels + channel]
                    for channel in range(channels)) / channels
            output.append(linear_to_mulaw(int(round(a + (b - a) * fraction))))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="ascii", newline="\n") as header:
        header.write("#pragma once\n#include <cstddef>\n#include <cstdint>\n")
        header.write("inline constexpr uint8_t kB4b6VocalMulaw[] = {\n")
        for offset in range(0, len(output), 16):
            header.write("  " + ",".join(f"0x{value:02x}" for value in output[offset:offset + 16]) + ",\n")
        header.write("};\n")
        header.write(f"inline constexpr size_t kB4b6VocalMulawSize = {len(output)};\n")
        header.write(f"inline constexpr unsigned kB4b6VocalSourceRate = {OUTPUT_RATE};\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
