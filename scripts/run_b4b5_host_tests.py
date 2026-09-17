"""Run the complete host suite and save the B4B.5 transcript."""

from __future__ import annotations

import pathlib
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "artifacts" / "alpha01b" / "b4b5_host_tests.txt"


def main() -> int:
    completed = subprocess.run(
        ["make", "test"], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        encoding="utf-8", errors="replace")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(completed.stdout, encoding="utf-8")
    print(completed.stdout, end="")
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
