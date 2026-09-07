#!/usr/bin/env bash
set -euo pipefail
: "${IDF_PATH:?source ESP-IDF v5.3 export.sh before running this script}"
idf.py set-target esp32p4
idf.py -B build build
