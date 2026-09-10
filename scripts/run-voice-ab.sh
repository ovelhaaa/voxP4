#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
source_wav=${1:-samples/dry-acapella-leave-this-place_95bpm.wav}
[[ "$source_wav" == /* ]] || source_wav="$repo/$source_wav"
[[ -f "$source_wav" ]] || { echo "not found: $source_wav" >&2; exit 2; }

name=$(basename "$source_wav" .wav)
root="$repo/artifacts/voice_ab/$name"
mkdir -p "$root"/{renders,envelopes,transitions,plots,report}
source_copy="$root/renders/original_source.wav"
reference="$root/renders/original_reference.wav"

[[ $(head -c4 "$source_wav") == RIFF ]] || {
  echo "input must be a valid RIFF WAV: $source_wav" >&2
  exit 2
}
cp -f -- "$source_wav" "$source_copy"
python3 - "$source_copy" "$reference" <<'PY'
import math
import shutil
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

source, output = sys.argv[1:]
rate, x = wavfile.read(source)
if x.ndim not in (1, 2):
    raise SystemExit("unsupported WAV layout")
if rate == 48000:
    shutil.copyfile(source, output)
else:
    divisor = math.gcd(rate, 48000)
    y = resample_poly(x.astype(np.float64), 48000 // divisor,
                      rate // divisor, axis=0)
    if np.issubdtype(x.dtype, np.integer):
        limits = np.iinfo(x.dtype)
        y = np.clip(np.rint(y), limits.min, limits.max).astype(x.dtype)
    else:
        y = y.astype(np.float32)
    wavfile.write(output, 48000, y)
PY

cmake -S "$repo/tests" -B "$repo/build-host"
cmake --build "$repo/build-host" --parallel --target pitch_shift
tool="$repo/build-host/pitch_shift"
log="$root/render.log"
: > "$log"

render() {
  echo "+ $tool $*" >> "$log"
  "$tool" "$@" 2>> "$log"
}

render "$reference" "$root/renders/voice1_p4_attenuation_on.wav" 4 \
  --voice 1 --apply-voice-envelope --formants off \
  --debug-csv "$root/envelopes/voice1_p4_attenuation_on.csv"
render "$reference" "$root/renders/voice2_p7_attenuation_on.wav" 7 \
  --voice 2 --apply-voice-envelope --formants off \
  --debug-csv "$root/envelopes/voice2_p7_attenuation_on.csv"
render "$reference" "$root/renders/voice2_p7_attenuation_off.wav" 7 \
  --voice 2 --apply-voice-envelope --disable-onset-unvoiced-attenuation \
  --formants off \
  --debug-csv "$root/envelopes/voice2_p7_attenuation_off.csv"

python3 "$repo/scripts/voice_ab_report.py" "$root"
echo "Voice A/B complete: $root"
