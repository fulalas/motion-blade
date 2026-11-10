#!/bin/bash
. "$(dirname "$(readlink -f "$0")")/env.sh"
B=$T/baseline.txt
N=41
REPEAT=8
STEP=2
RUNS=$(((N - 1) / STEP * REPEAT))
update=0
speed=1
for a in "$@"; do
    case $a in
        --update) update=1 ;;
        --quality) speed=0 ;;
        *) echo "regress.sh [--update] [--quality]" >&2; exit 1 ;;
    esac
done
need_eval
out=$(mktemp)
trap 'rm -f "$out"' EXIT
fail=0

put() { echo "$1 $2" >>"$out"; }
mean() { tail -1 | awk -v k="$1" '{for (i = 1; i < NF; i++) if ($i == k) {print $(i + 1); exit}}'; }
num() { case $1 in ''|.|*[!0-9.]*|*.*.*) return 1 ;; esac; }

quality() {
    echo "== quality"
    local s m k
    s=$("$T/suite.sh") || exit 1
    for k in local psnr ssim cpsnr; do put "suite_$k" "$(echo "$s" | mean $k)"; done
    for m in flicker fade flash; do
        s=$(LIGHT=$m "$T/light.sh") || exit 1
        for k in local psnr cpsnr; do put "${m}_$k" "$(echo "$s" | mean $k)"; done
    done
    s=$("$T/suite.sh" -p half_resolution=1) || exit 1
    for k in local psnr ssim cpsnr; do put "half_$k" "$(echo "$s" | mean $k)"; done
}

formats() {
    echo "== formats"
    local clip=$D/light/flicker_crowd_run_1080p50.mkv fm
    need_clip "$clip"
    for fm in nv12 p010 p016 yuv420p10; do
        put "fmt_${fm}_psnr" "$("$E" "$clip" -n 21 -q -fmt $fm | tail -1 | field "MEAN psnr")"
    done
}

cut_list() {
    local key=$1 p= a r
    shift
    for a in "$@"; do [ "$p" = -i ] && need_clip "$a"; p=$a; done
    r=$(ffmpeg -hide_banner -loglevel error "$@" -pix_fmt yuv420p -f yuv4mpegpipe - | "$E" /dev/stdin -n 3000 |
        awk '/CUT/ {s = s $2 ","} END {print s == "" ? "none" : s}'
        exit ${PIPESTATUS[1]}) || { echo "scene cut run failed: $key" >&2; exit 1; }
    put "$key" "$r"
}

cuts() {
    echo "== scene cuts"
    cut_list cuts_passengers -i "$D/film/passengers_breakfast_11_cuts_1080p24.mkv"
    cut_list cuts_bbb -t 30 -i "$D/film/bbb_cuts_1080p24.mp4"
    cut_list cuts_siren -i "$D/film/crown_vic_siren_flicker_784p24.mp4" -vf fps=48000/1001
    cut_list cuts_concert -i "$D/film/concert_cut_backlit_422_1080p30.mp4" -vf fps=60000/1001
    cut_list cuts_concert_native -i "$D/film/concert_cut_backlit_422_1080p30.mp4"
    cut_list cuts_concert_480i -i "$D/film/concert_pulldown_cut_480i30.mkv"
    cut_list cuts_squirrel -i "$D/film/bbb_squirrel_branch_1080p30.mp4"
    set -- "$D"/light/flash_*.mkv
    [ -f "$1" ] || { echo "no $D/light/flash_*.mkv, run LIGHT=flash tools/light.sh" >&2; exit 1; }
    local n=0 f
    for f in "$@"; do n=$((n + $("$E" "$f" -n 41 | grep -c CUT))); done
    put cuts_flash "$n"
}

speed_run() { "$E" "$D/suite/crowd_run_1080p50.mkv" -n $N -step $STEP -nometric -repeat $REPEAT "$@"; }
speed_ms() { speed_run "$@" | head -1 | awk '{print $2}'; }

speed() {
    echo "== speed"
    pgrep -x mpv >/dev/null && echo "warning: mpv is running, speed results will be off"
    local level best=99999 r ms vg
    level=$(cat /sys/class/drm/card*/device/power_dpm_force_performance_level 2>/dev/null | head -1)
    [ -n "$level" ] && echo "GPU power level: $level"
    put gpu_level "${level:-unknown}"
    need_clip "$D/suite/crowd_run_1080p50.mkv"
    for r in 1 2 3; do
        ms=$(speed_ms)
        best=$(echo "$ms $best" | awk '{print ($1 < $2) ? $1 : $2}')
    done
    put speed_ms_1080p "$best"
    put half_ms "$(speed_ms -p half_resolution=1)"
    MOTIONBLADE_PROFILE=1 speed_run 2>&1 | grep "motionblade profile" |
        awk -v runs=$RUNS '{printf "pass_%s %s\n", $3, $4 / runs}' >>"$out"
    vg=$(RADV_DEBUG=shaderstats,nocache "$E" "$D/suite/crowd_run_1080p50.mkv" -n 3 -q 2>&1 | grep -E "^VGPRs" | tail -1 |
         awk '{print $2}')
    [ -n "$vg" ] && put interpolate_vgprs "$vg"
}

rule() {
    case $1 in
        pass_*) echo lower "$(echo "$2" | awk '{print $1 * 0.10 + 0.05}')" ;;
        *_local) echo higher 0.1 ;;
        *_psnr|*_cpsnr) echo higher 0.05 ;;
        *_ssim) echo higher 0.002 ;;
        speed_ms_1080p|half_ms) echo lower "$(echo "$2" | awk '{print $1 * 0.05}')" ;;
        interpolate_vgprs) echo max 128 ;;
        gpu_level) echo warn ;;
        *) echo exact ;;
    esac
}

judge() {
    local v=$1 b=$2 mode=$3 tol=$4
    if [ -z "$v" ]; then echo FAIL; return; fi
    if [ -n "$tol" ] && ! num "$v"; then echo FAIL; return; fi
    if [ -z "$b" ]; then echo new; return; fi
    case $mode in
        max) [ "$v" -le "$tol" ] && echo ok || echo FAIL ;;
        warn) [ "$v" = "$b" ] && echo ok || echo warn ;;
        exact) [ "$v" = "$b" ] && echo ok || echo FAIL ;;
        lower) echo "$v $b $tol" | awk '{print ($1 > $2 + $3) ? "FAIL" : ($1 < $2 - $3) ? "better" : "ok"}' ;;
        higher) echo "$v $b $tol" | awk '{print ($1 < $2 - $3) ? "FAIL" : ($1 > $2 + $3) ? "better" : "ok"}' ;;
    esac
}

report() {
    echo "== result (current / baseline)"
    local k v b mode tol st
    while read -r k v; do
        b=$(awk -v k="$k" '$1 == k {print $2}' "$B")
        read -r mode tol <<<"$(rule "$k" "$b")"
        st=$(judge "$v" "$b" "$mode" "$tol")
        [ "$st" = FAIL ] && fail=1
        printf "%-26s %-34s %-34s %s\n" "$k" "${v:-(empty)}" "$b" "$st"
    done <"$out"
    while read -r k b; do
        grep -q "^$k " "$out" && continue
        case $k in pass_*|speed_ms_1080p|half_ms|interpolate_vgprs|gpu_level) [ $speed = 0 ] && continue ;; esac
        printf "%-26s %-34s %-34s %s\n" "$k" "missing" "$b" FAIL
        fail=1
    done <"$B"
}

quality
formats
cuts
[ $speed = 1 ] && speed
if [ $update = 1 ]; then
    cp "$out" "$B"
    echo "baseline updated: $B"
    exit 0
fi
[ -f "$B" ] || { echo "no baseline, run with --update first"; exit 1; }
report
[ $fail = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
