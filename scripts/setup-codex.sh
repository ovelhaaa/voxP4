#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=idf-version.sh
source "$ROOT/scripts/idf-version.sh"

export IDF_PATH="${IDF_PATH:-${HOME:?HOME must be set}/esp/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-${HOME}/.espressif}"

required_commands=(git curl flex bison gperf python3 cmake ninja)
missing_commands=()
for command_name in "${required_commands[@]}"; do
  command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
done

missing_packages=()
if command -v dpkg-query >/dev/null 2>&1; then
  packages=(git curl flex bison gperf python3 python3-pip python3-venv cmake
    ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0-dev)
  for package in "${packages[@]}"; do
    dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed' || \
      missing_packages+=("$package")
  done
fi

if ((${#missing_packages[@]})); then
  if ! command -v apt-get >/dev/null 2>&1; then
    printf 'Missing required packages and apt-get is unavailable: %s\n' \
      "${missing_packages[*]}" >&2
    exit 1
  fi
  apt_prefix=()
  if ((EUID != 0)); then
    command -v sudo >/dev/null 2>&1 || {
      echo 'System packages are missing, but neither root privileges nor sudo are available.' >&2
      exit 1
    }
    apt_prefix=(sudo)
  fi
  if ! "${apt_prefix[@]}" apt-get update; then
    echo 'Warning: apt-get update failed; trying existing package indexes.' >&2
  fi
  if ! "${apt_prefix[@]}" apt-get install -y --no-install-recommends "${missing_packages[@]}"; then
    echo 'Unable to install ESP-IDF system dependencies. Check apt repositories and internet access.' >&2
    exit 1
  fi
elif ((${#missing_commands[@]})); then
  printf 'Missing required commands and no supported package check is available: %s\n' \
    "${missing_commands[*]}" >&2
  exit 1
fi

if [[ -e "$IDF_PATH" && ! -d "$IDF_PATH/.git" ]]; then
  printf 'IDF_PATH exists but is not a Git checkout: %s\n' "$IDF_PATH" >&2
  exit 1
fi

if [[ ! -d "$IDF_PATH/.git" ]]; then
  mkdir -p "$(dirname "$IDF_PATH")"
  echo "Cloning ESP-IDF $IDF_VERSION into $IDF_PATH"
  if ! git clone --depth 1 --shallow-submodules --branch "$IDF_VERSION" --recursive \
      https://github.com/espressif/esp-idf.git "$IDF_PATH"; then
    echo 'Unable to download ESP-IDF. Check internet access and run setup again.' >&2
    exit 1
  fi
else
  [[ -f "$IDF_PATH/tools/idf.py" && -f "$IDF_PATH/install.sh" ]] || {
    printf 'The checkout at IDF_PATH is not a valid ESP-IDF repository: %s\n' "$IDF_PATH" >&2
    exit 1
  }
  actual_commit="$(git -C "$IDF_PATH" rev-parse HEAD)"
  expected_commit="$(git -C "$IDF_PATH" rev-parse "$IDF_VERSION^{commit}" 2>/dev/null || true)"
  if [[ -z "$expected_commit" ]]; then
    git -C "$IDF_PATH" fetch --tags origin "$IDF_VERSION"
    expected_commit="$(git -C "$IDF_PATH" rev-parse "$IDF_VERSION^{commit}")"
  fi
  if [[ "$actual_commit" != "$expected_commit" ]]; then
    printf 'ESP-IDF at %s is on %s, not pinned %s (%s).\n' \
      "$IDF_PATH" "$actual_commit" "$IDF_VERSION" "$expected_commit" >&2
    echo 'Use a matching IDF_PATH or remove this checkout; it will not be changed silently.' >&2
    exit 1
  fi
  echo "Using existing ESP-IDF $IDF_VERSION checkout at $IDF_PATH"
fi

if git -C "$IDF_PATH" submodule status --recursive | grep -qE '^[-+]'; then
  echo 'Initializing missing ESP-IDF submodules'
  git -C "$IDF_PATH" submodule sync --recursive
  git -C "$IDF_PATH" submodule update --init --recursive
fi

mkdir -p "$IDF_TOOLS_PATH"
install_marker="$IDF_TOOLS_PATH/.vox-p4-${IDF_VERSION}-esp32p4-installed"
if [[ ! -f "$install_marker" ]]; then
  echo 'Installing ESP-IDF tools for esp32p4 only'
  (cd "$IDF_PATH" && ./install.sh esp32p4)
  touch "$install_marker"
else
  echo "ESP32-P4 tools already installed for $IDF_VERSION; skipping installation"
fi

# Validate the cached installation rather than trusting the marker alone.
# shellcheck source=idf-env.sh
source "$ROOT/scripts/idf-env.sh"
command -v idf.py >/dev/null || { echo 'idf.py is unavailable after setup.' >&2; exit 1; }
command -v riscv32-esp-elf-gcc >/dev/null || {
  rm -f "$install_marker"
  echo 'ESP32-P4 RISC-V toolchain is unavailable; rerun setup to repair it.' >&2
  exit 1
}
echo "Codex/local ESP32-P4 environment is ready ($(idf.py --version))."
