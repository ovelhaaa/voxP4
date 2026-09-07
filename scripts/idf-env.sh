#!/usr/bin/env bash

# This file is intended to be sourced. Do not enable shell options here because
# doing so would unexpectedly change the caller's shell.
_VOX_P4_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=idf-version.sh
source "$_VOX_P4_ROOT/scripts/idf-version.sh"
export IDF_PATH="${IDF_PATH:-${HOME:?HOME must be set}/esp/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-${HOME}/.espressif}"

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  printf 'ESP-IDF was not found at %s. Run %s/scripts/setup-codex.sh first.\n' \
    "$IDF_PATH" "$_VOX_P4_ROOT" >&2
  return 1 2>/dev/null || exit 1
fi

# ESP-IDF normally derives its virtualenv name from whichever python3 appears
# first in PATH. Cloud login and non-login shells may resolve different Python
# versions, so reuse the environment installed by setup explicitly.
if [[ -z "${IDF_PYTHON_ENV_PATH:-}" ]]; then
  _idf_series="${IDF_VERSION#v}"
  for _idf_python_env in "$IDF_TOOLS_PATH"/python_env/idf"${_idf_series}"_py*_env; do
    if [[ -x "$_idf_python_env/bin/python" && -f "$_idf_python_env/idf_version.txt" ]]; then
      export IDF_PYTHON_ENV_PATH="$_idf_python_env"
      break
    fi
  done
  unset _idf_series _idf_python_env
fi

# ESP-IDF's export script intentionally updates the current shell environment.
# shellcheck disable=SC1091
source "$IDF_PATH/export.sh"
unset _VOX_P4_ROOT IDF_VERSION
