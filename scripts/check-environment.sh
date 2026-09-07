#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=idf-env.sh
source "$ROOT/scripts/idf-env.sh"
command -v idf.py >/dev/null || { echo 'idf.py is not available.' >&2; exit 1; }

target=unconfigured
if [[ -f "$ROOT/sdkconfig" ]]; then
  target="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' "$ROOT/sdkconfig" | head -n1)"
  target="${target:-unconfigured}"
elif [[ -f "$ROOT/sdkconfig.defaults" ]] && grep -q '^CONFIG_IDF_TARGET="esp32p4"$' "$ROOT/sdkconfig.defaults"; then
  target='esp32p4 (sdkconfig.defaults)'
fi

printf 'repository root: %s\n' "$ROOT"
printf 'IDF_PATH: %s\n' "$IDF_PATH"
printf 'IDF_TOOLS_PATH: %s\n' "$IDF_TOOLS_PATH"
printf 'ESP-IDF version: %s\n' "$(idf.py --version)"
printf 'Python version: %s\n' "$(python3 --version 2>&1)"
printf 'CMake version: %s\n' "$(cmake --version | head -n1)"
printf 'Ninja version: %s\n' "$(ninja --version)"
if command -v ccache >/dev/null 2>&1; then
  printf 'ccache version: %s\n' "$(ccache --version | head -n1)"
else
  echo 'ccache version: unavailable (optional)'
fi
printf 'target: %s\n' "$target"
if command -v riscv32-esp-elf-gcc >/dev/null 2>&1; then
  printf 'ESP32-P4 toolchain: %s\n' "$(riscv32-esp-elf-gcc --version | head -n1)"
  printf 'ESP32-P4 compiler: %s\n' "$(command -v riscv32-esp-elf-gcc)"
else
  echo 'ESP32-P4 RISC-V toolchain is unavailable.' >&2
  exit 1
fi
