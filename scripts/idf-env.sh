#!/usr/bin/env bash

# This file is intended to be sourced. Do not enable shell options here because
# doing so would unexpectedly change the caller's shell.
_VOX_P4_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=idf-version.sh
source "$_VOX_P4_ROOT/scripts/idf-version.sh"
export IDF_PATH="${IDF_PATH:-${HOME:?HOME must be set}/esp/esp-idf}"
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-${HOME}/.espressif}"

if [[ ! -f "$IDF_PATH/export.sh" || ! -f "$IDF_PATH/tools/idf.py" ]]; then
  printf 'A valid ESP-IDF checkout was not found at %s. Run %s/scripts/setup-codex.sh first.\n' \
    "$IDF_PATH" "$_VOX_P4_ROOT" >&2
  return 1 2>/dev/null || exit 1
fi

# Refuse to activate an arbitrary ESP-IDF checkout. In particular, checking
# only for export.sh would let an IDF_PATH override silently bypass the
# repository's version pin. This check deliberately happens before export.sh
# can modify the caller's environment.
_idf_actual_commit="$(git -C "$IDF_PATH" rev-parse HEAD 2>/dev/null)" || {
  printf 'Unable to identify the ESP-IDF revision at %s.\n' "$IDF_PATH" >&2
  return 1 2>/dev/null || exit 1
}
_idf_expected_commit="$(git -C "$IDF_PATH" rev-parse "$IDF_VERSION^{commit}" 2>/dev/null)" || {
  printf 'ESP-IDF checkout at %s does not contain the pinned %s tag. Run %s/scripts/setup-codex.sh.\n' \
    "$IDF_PATH" "$IDF_VERSION" "$_VOX_P4_ROOT" >&2
  return 1 2>/dev/null || exit 1
}
if [[ "$_idf_actual_commit" != "$_idf_expected_commit" ]]; then
  printf 'ESP-IDF at %s is on %s, not pinned %s (%s); refusing to activate it.\n' \
    "$IDF_PATH" "$_idf_actual_commit" "$IDF_VERSION" "$_idf_expected_commit" >&2
  return 1 2>/dev/null || exit 1
fi
unset _idf_actual_commit _idf_expected_commit

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
