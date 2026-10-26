#!/bin/bash
T=$(dirname "$(readlink -f "$0")")
D=${MOTIONBLADE_DATA:-$(cd "$T/../.." && pwd)/samples}
E=${EVAL:-$T/../eval_vk}
B=$T/baseline.txt
update=0
[ "$1" = --update ] && update=1
out=$(mktemp)
fail=0

pgrep -x mpv >/dev/null && echo "warning: mpv is running, speed results will be off"
lvl=$(cat /sys/class/drm/card*/device/power_dpm_force_performance_level 2>/dev/null | head -1)
[ -n "$lvl" ] && echo "GPU power level: $lvl"

put() { echo "$1 $2" >>"$out"; }
put gpu_level "${lvl:-unknown}"
mean() { tail -1 | awk -v k="$1" '{for (i = 1; i < NF; i++) if ($i == k) {print $(i + 1); exit}}'; }

echo "== quality"
s=$("$T/suite.sh")
for k in local psnr ssim cpsnr; do put "suite_$k" "$(echo "$s" | mean $k)"; done
for m in flicker fade flash; do
    s=$(LIGHT=$m "$T/flicker.sh")
    for k in local psnr cpsnr; do put "${m}_$k" "$(echo "$s" | mean $k)"; done
done

echo "== formats"
for fm in nv12 p010 yuv420p10; do
    put "fmt_${fm}_psnr" "$("$E" "$D/light/flicker_crowd_run_1080p50.mkv" -n 21 -q -fmt $fm | tail -1 | grep -o "MEAN psnr [0-9.]*" | awk '{print $3}')"
done

echo "== scene cuts"
cuts() { ffmpeg -hide_banner -loglevel error "$@" -pix_fmt yuv420p -f yuv4mpegpipe - | "$E" /dev/stdin -n 3000 | awk '/CUT/ {s = s $2 ","} END {print s == "" ? "none" : s}'; }
put cuts_passengers "$(cuts -i "$D/film/passengers_breakfast_11_cuts_1080p24.mkv")"
put cuts_bbb "$(cuts -t 30 -i "$D/film/bbb_cuts_1080p24.mp4")"
put cuts_siren "$(cuts -i "$D/film/crown_vic_siren_flicker_784p24.mp4" -vf fps=48000/1001)"
put cuts_concert "$(cuts -i "$D/film/concert_cut_backlit_422_1080p30.mp4" -vf fps=60000/1001)"
n=0
for f in "$D"/light/flash_*.mkv; do n=$((n + $("$E" "$f" -n 41 | grep -c CUT))); done
put cuts_flash "$n"

echo "== speed"
best=99999
for r in 1 2 3; do
    ms=$("$E" "$D/suite/crowd_run_1080p50.mkv" -n 41 -nometric -repeat 8 | head -1 | awk '{print $2}')
    best=$(echo "$ms $best" | awk '{print ($1 < $2) ? $1 : $2}')
done
put speed_ms_1080p "$best"
MOTIONBLADE_PROFILE=1 "$E" "$D/suite/crowd_run_1080p50.mkv" -n 41 -nometric -repeat 8 2>&1 | grep "motionblade profile" |
    awk '{printf "pass_%s %s\n", $3, $4 / 160}' >>"$out"

vg=$(RADV_DEBUG=shaderstats,nocache "$E" "$D/suite/crowd_run_1080p50.mkv" -n 3 -q 2>&1 | grep -E "^VGPRs" | tail -1 | awk '{print $2}')
[ -n "$vg" ] && put interp_vgprs "$vg"

if [ $update = 1 ]; then
    cp "$out" "$B"
    echo "baseline updated: $B"
    rm "$out"
    exit 0
fi
[ -f "$B" ] || { echo "no baseline, run with --update first"; rm "$out"; exit 1; }

echo "== result (current / baseline)"
while read -r k v; do
    b=$(awk -v k="$k" '$1 == k {print $2}' "$B")
    st=ok
    case $k in
        suite_local|flicker_local|fade_local|flash_local) tol=0.1 ;;
        *_psnr|*_cpsnr) tol=0.05 ;;
        suite_ssim) tol=0.002 ;;
        speed_ms_1080p) tol=$(echo "$b" | awk '{print $1 * 0.05}') ;;
        pass_*) tol=$(echo "$b" | awk '{print $1 * 0.10 + 0.05}') ;;
        *) tol= ;;
    esac
    if [ -z "$b" ]; then
        st=new
    elif [ $k = interp_vgprs ]; then
        [ "$v" -le 168 ] || st=FAIL
    elif [ $k = gpu_level ]; then
        [ "$v" = "$b" ] || st=warn
    elif [ -z "$tol" ]; then
        [ "$v" = "$b" ] || st=FAIL
    else
        case $k in
            speed_*|pass_*) st=$(echo "$v $b $tol" | awk '{print ($1 > $2 + $3) ? "FAIL" : ($1 < $2 - $3) ? "better" : "ok"}') ;;
            *) st=$(echo "$v $b $tol" | awk '{print ($1 < $2 - $3) ? "FAIL" : ($1 > $2 + $3) ? "better" : "ok"}') ;;
        esac
    fi
    [ "$st" = FAIL ] && fail=1
    printf "%-26s %-34s %-34s %s\n" "$k" "$v" "$b" "$st"
done <"$out"
while read -r k b; do
    grep -q "^$k " "$out" && continue
    printf "%-26s %-34s %-34s %s\n" "$k" "missing" "$b" FAIL
    fail=1
done <"$B"
rm "$out"
[ $fail = 0 ] && echo "PASS" || echo "FAIL"
exit $fail
