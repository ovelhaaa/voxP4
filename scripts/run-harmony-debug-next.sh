#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
source_wav=${1:-samples/dry-acapella-leave-this-place_95bpm.wav}
[[ "$source_wav" == /* ]] || source_wav="$repo/$source_wav"
[[ -f "$source_wav" && $(head -c4 "$source_wav") == RIFF ]] || {
  echo "requires a valid RIFF WAV: $source_wav" >&2
  exit 2
}

root="$repo/artifacts/harmony_debug_next"
mkdir -p "$root"/{renders,csv,plots}
cp -f -- "$source_wav" "$root/renders/original_source.wav"
python3 - "$source_wav" "$root/renders/reference_full.wav" <<'PY'
import math
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

source, output = sys.argv[1:]
rate, x = wavfile.read(source)
if np.issubdtype(x.dtype, np.integer):
    x = x.astype(np.float64) / float(2 ** (x.dtype.itemsize * 8 - 1))
else:
    x = x.astype(np.float64)
if x.ndim == 2:
    x = x.mean(axis=1)
if rate != 48000:
    divisor = math.gcd(rate, 48000)
    x = resample_poly(x, 48000 // divisor, rate // divisor)
wavfile.write(output, 48000, x.astype(np.float32))
PY

cmake -S "$repo/tests" -B "$repo/build-host"
cmake --build "$repo/build-host" --parallel --target pitch_shift pitch_analyze
shift_tool="$repo/build-host/pitch_shift"
analyze_tool="$repo/build-host/pitch_analyze"
"$analyze_tool" "$root/renders/reference_full.wav" "$root/csv/reference_full_pitch.csv"
python3 "$repo/scripts/harmony_debug_next.py" prepare "$root" \
  "$root/renders/reference_full.wav" "$root/csv/reference_full_pitch.csv"

render() {
  local input=$1 output=$2 semitones=$3 voice=$4 csv=$5
  shift 5
  "$shift_tool" "$input" "$output" "$semitones" --voice "$voice" \
    --both-voices --apply-voice-envelope --formants off --debug-csv "$csv" \
    "$@" 2>> "$root/render.log"
}

: > "$root/render.log"
for number in 01 02 03 04 05; do
  segment="segment$number"
  render_dir="$root/renders/$segment"
  csv_dir="$root/csv/$segment"
  mkdir -p "$csv_dir"
  reference="$render_dir/reference.wav"
  render "$reference" "$render_dir/voice1_p4.wav" 4 1 "$csv_dir/voice1_p4.csv"
  render "$reference" "$render_dir/voice2_p4.wav" 4 2 "$csv_dir/voice2_p4.csv"
  render "$reference" "$render_dir/voice1_m4.wav" -4 1 "$csv_dir/voice1_m4.csv"
  render "$reference" "$render_dir/voice2_m4.wav" -4 2 "$csv_dir/voice2_m4.csv"
  render "$reference" "$render_dir/voice2_p7.wav" 7 2 "$csv_dir/voice2_p7.csv"
  render "$reference" "$render_dir/voice2_m7.wav" -7 2 "$csv_dir/voice2_m7.csv"
  render "$reference" "$render_dir/voice2_p4_no_attenuation.wav" 4 2 \
    "$csv_dir/voice2_p4_no_attenuation.csv" \
    --disable-onset-unvoiced-attenuation
  render "$reference" "$render_dir/voice2_p4_keep40.wav" 4 2 \
    "$csv_dir/voice2_p4_keep40.csv" --keep-active-ms 40

  # One-factor-at-a-time sweep around 40/40/10/40 ms. These are host-only
  # diagnostics: HarmonyArticulationConfig remains disabled by default.
  articulation_cases=(
    "grace25 25 40 10 40" "grace40 40 40 10 40" "grace60 60 40 10 40"
    "hold20 40 20 10 40" "hold40 40 40 10 40" "hold60 40 60 10 40"
    "attack5 40 40 5 40" "attack10 40 40 10 40" "attack20 40 40 20 40"
    "release20 40 40 10 20" "release40 40 40 10 40" "release60 40 40 10 60"
  )
  for specification in "${articulation_cases[@]}"; do
    read -r name grace hold attack release <<< "$specification"
    render "$reference" "$render_dir/voice2_p4_art_${name}.wav" 4 2 \
      "$csv_dir/voice2_p4_art_${name}.csv" \
      --articulation "$grace" "$hold" "$attack" "$release" .15 .10
  done
done

synthetic="$root/renders/synthetic/reference.wav"
mkdir -p "$root/csv/synthetic"
for semitones in -7 -4 0 4 7; do
  if (( semitones < 0 )); then tag="m${semitones#-}"; else tag="p$semitones"; fi
  render "$synthetic" "$root/renders/synthetic/voice2_${tag}.wav" "$semitones" 2 \
    "$root/csv/synthetic/voice2_${tag}.csv"
done

python3 "$repo/scripts/harmony_debug_next.py" report "$root"
echo "Harmony diagnostic complete: $root"
