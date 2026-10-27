#!/bin/bash
. "$(dirname "$(readlink -f "$0")")/env.sh"
clips=${CLIPS:-"suite/park_joy_1080p50:41 suite/crowd_run_1080p50:41 suite/in_to_tree_1080p50:41 suite/ducks_take_off_1080p50:41 suite/old_town_cross_1080p50:41 suite/mobcal_720p50:41 suite/shields_720p50:41 suite/parkrun_720p50:41 suite/stockholm_720p60:41 suite/bbb_tree_cut_1080p60:81 suite/bbb_meadow_still_1080p60:81 suite/bbb_squirrel_branch_1080p60:81 suite/bbb_credits_scroll_1080p60:81 suite/bbb_butterfly_closeup_1080p60:81 suite/bbb_butterfly_bunny_1080p60:81 suite/bbb_apple_cut_1080p60:81"}
need_eval
for c in $clips; do need_clip "$D/${c%%:*}.mkv"; done
tl=0; tp=0; ts=0; tc=0; n=0
for c in $clips; do f=${c%%:*}; k=${c##*:}
  r=$("$E" "$D/$f.mkv" -n $k -q "$@" | tail -1)
  case $r in *MEAN*) ;; *) echo "eval failed on $f" >&2; exit 1 ;; esac
  l=$(echo "$r" | field "MEAN local")
  p=$(echo "$r" | field "MEAN psnr"); s=$(echo "$r" | field ssim)
  cp=$(echo "$r" | field "chroma psnr")
  printf "%-32s local %7s psnr %7s ssim %7s cpsnr %7s\n" "${f##*/}" "$l" "$p" "$s" "$cp"
  tl=$(echo "$tl + $l" | bc -l); tp=$(echo "$tp + $p" | bc -l); ts=$(echo "$ts + $s" | bc -l); tc=$(echo "$tc + $cp" | bc -l); n=$((n+1))
done
printf "%-32s local %7.3f psnr %7.3f ssim %7.4f cpsnr %7.3f\n" MEAN $(echo "$tl / $n" | bc -l) $(echo "$tp / $n" | bc -l) $(echo "$ts / $n" | bc -l) $(echo "$tc / $n" | bc -l)
