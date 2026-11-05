#!/bin/bash
. "$(dirname "$(readlink -f "$0")")/env.sh"
[ $# -ge 2 ] || { echo "compare-speed.sh OLD_EVAL NEW_EVAL [rounds]" >&2; exit 1; }
OLD=$1
NEW=$2
ROUNDS=${3:-5}
N=41
STEP=2
REPEAT=8
FRAMES=$(((N - 1) / STEP * REPEAT))
CUT_CLIP=$D/film/passengers_breakfast_11_cuts_1080p24.mkv
CUT_FRAMES=320
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

need_clip "$D/suite/crowd_run_1080p50.mkv"
need_clip "$CUT_CLIP"
pgrep -x mpv >/dev/null && echo "warning: mpv is running, results will be off"
level=$(cat /sys/class/drm/card*/device/power_dpm_force_performance_level 2>/dev/null | head -1)
echo "GPU power level: ${level:-unknown}"
ffmpeg -nostdin -hide_banner -loglevel error -i "$CUT_CLIP" -frames:v $CUT_FRAMES -pix_fmt yuv420p "$work/cuts.y4m"

profile_run() {
    local eval=$1 clip=$2 frames=$3
    shift 3
    "$eval" "$clip" "$@" -nometric 2>&1 | awk '/^wall/ {print "wall", $2}'
    MOTIONBLADE_PROFILE=1 "$eval" "$clip" "$@" -nometric 2>&1 |
        awk -v f="$frames" '/motionblade profile/ {printf "pass_%s %.4f\n", $3, $4 / f}'
}

plain() { profile_run "$1" "$D/suite/crowd_run_1080p50.mkv" $FRAMES -n $N -step $STEP -repeat $REPEAT; }
cuts() { profile_run "$1" "$work/cuts.y4m" $((CUT_FRAMES / STEP)) -n $CUT_FRAMES -step $STEP; }

for r in $(seq "$ROUNDS"); do
    for side in old new; do
        eval=$OLD
        [ $side = new ] && eval=$NEW
        plain "$eval" | sed "s/^/$side plain /" >>"$work/runs"
        cuts "$eval" | sed "s/^/$side cuts /" >>"$work/runs"
    done
done

shader_stats() {
    RADV_DEBUG=shaderstats,nocache "$1" "$D/suite/crowd_run_1080p50.mkv" -n 3 -q 2>&1 |
        awk '/^VGPRs:/ {v = $2} /^Spilled VGPRs:/ {s = $3} /^Subgroups per SIMD:/ {w = $4}
             /^Instructions:/ {i = $2} /^Latency:/ {line[++n] = v " VGPRs, " s " spilled, " w " waves, " i " instr, latency " $2}
             END {for (k = n - 1; k <= n; k++) print "  pipeline " k ": " line[k]}'
}

echo "ms per interpolated frame over $ROUNDS rounds: old and new median, old and new best"
awk '{k = $2 " " $3; v[$1, k, ++n[$1, k]] = $4; keys[k] = 1}
     function sorted(side, k, a,    m, i) {
         m = n[side, k]
         for (i = 1; i <= m; i++) a[i] = v[side, k, i]
         asort(a)
         return m
     }
     function median(side, k,    m, a) {
         m = sorted(side, k, a)
         return m == 0 ? 0 : m % 2 ? a[(m + 1) / 2] : (a[m / 2] + a[m / 2 + 1]) / 2
     }
     function best(side, k,    m, a) {
         m = sorted(side, k, a)
         return m == 0 ? 0 : a[1]
     }
     END {
         for (k in keys) {
             o = median("old", k)
             w = median("new", k)
             if (o == 0 && w == 0) continue
             change = o > 0 ? (w - o) / o * 100 : 0
             printf "%-28s %8.4f %8.4f %+7.1f%%   best %8.4f %8.4f\n", k, o, w, change, best("old", k), best("new", k)
         }
     }' "$work/runs" | sort
echo "last two pipelines created (interpolate, and extrapolate when it exists)"
echo "old:"
shader_stats "$OLD"
echo "new:"
shader_stats "$NEW"
