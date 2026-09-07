#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
source_wav=${1:-}
if [[ -z "$source_wav" ]]; then
  mapfile -t wavs < <(find "$repo/samples" -maxdepth 1 -type f -iname '*.wav' | sort)
  [[ ${#wavs[@]} -eq 1 ]] || { echo "usage: $0 samples/vocal.wav" >&2; exit 2; }
  source_wav=${wavs[0]}
elif [[ "$source_wav" != /* ]]; then source_wav="$repo/$source_wav"; fi
[[ -f "$source_wav" ]] || { echo "not found: $source_wav" >&2; exit 2; }
name=$(basename "$source_wav" .wav); root="$repo/artifacts/formant_eval/$name"
mkdir -p "$root"/{renders,metrics,plots,report,blind}
source_copy="$root/renders/original_source.wav"
reference="$root/renders/original_reference.wav"
if head -c4 "$source_wav" | cmp -s - <(printf RIFF); then
  cp -f -- "$source_wav" "$source_copy"
else
  # The repository's current renamed sample is a two-byte placeholder, while
  # its original binary blob remains in history. Recover to artifacts only.
  rel=${source_wav#"$repo/"}; found=
  while read -r commit; do
    for candidate in "$rel" "sample/$(basename "$source_wav")" "$(basename "$source_wav")"; do
      if git -C "$repo" cat-file -e "$commit:$candidate" 2>/dev/null &&
         [[ $(git -C "$repo" cat-file -s "$commit:$candidate") -gt 44 ]] &&
         [[ $(git -C "$repo" show "$commit:$candidate" | head -c4) == RIFF ]]; then
        git -C "$repo" show "$commit:$candidate" > "$source_copy"; found=1; break 2
      fi
    done
  done < <(git -C "$repo" rev-list --all)
  [[ $found ]] || { echo "input is not RIFF and no historical WAV blob was found" >&2; exit 2; }
  echo "warning: recovered valid source blob to $source_copy; did not modify $source_wav" >&2
fi
python3 - "$source_copy" "$reference" <<'PY'
import sys
import math
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
p,out=sys.argv[1:]; rate,x=wavfile.read(p)
if x.ndim not in (1,2): raise SystemExit("unsupported WAV layout")
print(f"input: rate={rate}, channels={1 if x.ndim==1 else x.shape[1]}, format={x.dtype}, frames={len(x)}")
if rate == 48000:
 import shutil; shutil.copyfile(p,out)
else:
 g=math.gcd(rate,48000); y=resample_poly(x.astype(np.float64),48000//g,rate//g,axis=0)
 if np.issubdtype(x.dtype,np.integer): y=np.clip(np.rint(y),np.iinfo(x.dtype).min,np.iinfo(x.dtype).max).astype(x.dtype)
 else: y=y.astype(np.float32)
 wavfile.write(out,48000,y)
 print(f"working copy: 48000 Hz ({len(y)} frames), original retained at {p}")
PY
cmake -S "$repo/tests" -B "$repo/build-host"
cmake --build "$repo/build-host" --parallel --target pitch_shift
tool="$repo/build-host/pitch_shift"; log="$root/metrics/render.log"; touch "$log"
render() { [[ -s $2 ]] && return; echo "+ $tool $*" >> "$log"; "$tool" "$@" 2>> "$log"; }
render "$reference" "$root/renders/vocal_shift_p0_psola.wav" 0 --formants off
render "$reference" "$root/renders/vocal_shift_p0_lpc100.wav" 0 --formants lpc --formant-amount 1 --lpc-order 16
for st in -7 -4 -3 3 4 7; do
  sign=p; if (( st < 0 )); then sign=m; fi
  tag=${sign}${st#-}
  render "$reference" "$root/renders/vocal_shift_${tag}_psola.wav" "$st" --formants off
  for a in 25 50 75 100; do
    amount="0.$(printf %02d "$a")"; [[ $a == 100 ]] && amount=1.0
    render "$reference" "$root/renders/vocal_shift_${tag}_lpc$(printf %03d "$a").wav" "$st" --formants lpc --formant-amount "$amount" --lpc-order 16
  done
done
for st in -7 7; do
  sign=p; if (( st < 0 )); then sign=m; fi; tag=${sign}${st#-}
  for order in 12 20; do
    for a in 50 100; do
      amount="0.$(printf %02d "$a")"; [[ $a == 100 ]] && amount=1.0
      render "$reference" "$root/renders/vocal_shift_${tag}_lpc$(printf %03d "$a")_order${order}.wav" "$st" --formants lpc --formant-amount "$amount" --lpc-order "$order"
    done
  done
done
python3 "$repo/scripts/formant_eval.py" "$root" "$source_wav"
echo "evaluation complete: $root"
