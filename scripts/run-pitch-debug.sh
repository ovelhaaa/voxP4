#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd); src=${1:-samples/dry-acapella-leave-this-place_95bpm.wav}; [[ $src == /* ]] || src="$repo/$src"
root="$repo/artifacts/pitch_debug"; rm -rf "$root"; mkdir -p "$root"/{renders,yin,plots,stable_segments}
source_copy="$root/renders/original_44100.wav"
if [[ $(head -c4 "$src") == RIFF ]]; then cp "$src" "$source_copy"; else
  found=
  while read -r commit; do
    for candidate in "samples/$(basename "$src")" "sample/$(basename "$src")" "$(basename "$src")"; do
      if git -C "$repo" cat-file -e "$commit:$candidate" 2>/dev/null &&
         [[ $(git -C "$repo" cat-file -s "$commit:$candidate") -gt 44 ]] &&
         [[ $(git -C "$repo" show "$commit:$candidate" | head -c4) == RIFF ]]; then
        git -C "$repo" show "$commit:$candidate" > "$source_copy"; found=1; break 2
      fi
    done
  done < <(git -C "$repo" rev-list --all)
  [[ $found ]] || { echo "no valid WAV found in source or repository history" >&2; exit 2; }
fi
python3 "$repo/scripts/pitch_debug.py" prepare "$source_copy" "$root"
cmake -S "$repo/tests" -B "$repo/build-host"; cmake --build "$repo/build-host" --parallel --target pitch_shift pitch_analyze
analyze="$repo/build-host/pitch_analyze"; shift="$repo/build-host/pitch_shift"; log="$root/render.log"
for c in left right sum difference; do "$analyze" "$root/inputs/${c}_48000.wav" "$root/yin/$c.csv"; done
"$analyze" "$source_copy" "$root/yin/original_44100.csv"; python3 "$repo/scripts/pitch_debug.py" segments "$root"
render(){ echo "+ $shift $*" >> "$log"; "$shift" "$@" 2>> "$log"; }
for st in -7 -4 -3 3 4 7; do tag=p${st}; if ((st<0)); then tag=m${st#-}; fi; render "$root/inputs/sum_48000.wav" "$root/renders/full_${tag}.wav" "$st" --formants off; "$analyze" "$root/renders/full_${tag}.wav" "$root/yin/full_${tag}.csv"; done
"$analyze" "$root/inputs/synthetic_220hz.wav" "$root/yin/synthetic.csv"
for st in -7 -4 -3 3 4 7; do tag=p${st}; if ((st<0)); then tag=m${st#-}; fi; render "$root/inputs/synthetic_220hz.wav" "$root/renders/synthetic_${tag}.wav" "$st" --formants off; "$analyze" "$root/renders/synthetic_${tag}.wav" "$root/yin/synthetic_${tag}.csv"; done
for seg in 1 2 3; do
  in="$root/stable_segments/segment${seg}_original.wav"; "$analyze" "$in" "$root/yin/segment${seg}_original.csv"
  for st in -7 -4 4 7; do tag=p${st}; if ((st<0)); then tag=m${st#-}; fi; extra=(); if [[ $seg == 1 && $st == 7 ]]; then extra=(--debug-csv "$root/debug_timeline.csv"); fi; render "$in" "$root/renders/segment${seg}_${tag}.wav" "$st" --formants off "${extra[@]}"; "$analyze" "$root/renders/segment${seg}_${tag}.wav" "$root/yin/segment${seg}_${tag}.csv"; done
done
for ms in 24 32 40 48 64; do render "$root/stable_segments/segment1_original.wav" "$root/renders/history_${ms}.wav" 7 --formants off --history-offset-ms "$ms" --debug-csv "$root/history_${ms}_timeline.csv"; "$analyze" "$root/renders/history_${ms}.wav" "$root/yin/history_${ms}.csv"; done
render "$root/inputs/sum_48000.wav" "$root/renders/zero_psola.wav" 0 --formants off
render "$root/inputs/sum_48000.wav" "$root/renders/zero_lpc.wav" 0 --formants lpc --formant-amount 1 --lpc-order 16
"$analyze" "$root/renders/zero_psola.wav" "$root/yin/zero_psola.csv"
"$analyze" "$root/renders/zero_lpc.wav" "$root/yin/zero_lpc.csv"
python3 "$repo/scripts/pitch_debug.py" report "$root"
echo "Pitch debug complete: $root"
