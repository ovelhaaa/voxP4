#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=idf-env.sh
source "$ROOT/scripts/idf-env.sh"
cd "$ROOT"

if [[ -f sdkconfig ]]; then
  configured_target="$(sed -n 's/^CONFIG_IDF_TARGET="\([^"]*\)"/\1/p' sdkconfig | head -n1)"
  if [[ -n "$configured_target" && "$configured_target" != esp32p4 ]]; then
    printf 'Existing sdkconfig targets %s, not esp32p4; refusing to replace it.\n' \
      "$configured_target" >&2
    exit 1
  fi
fi

# set-target is destructive, so invoke it only for a new/unconfigured tree.
if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32p4"$' sdkconfig; then
  idf.py set-target esp32p4
fi
idf.py build

grep -q '^CONFIG_IDF_TARGET_ESP32P4=y$' sdkconfig || {
  echo 'Build completed without CONFIG_IDF_TARGET_ESP32P4=y.' >&2
  exit 1
}
