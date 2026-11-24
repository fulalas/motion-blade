#!/bin/sh
set -e
. "$(dirname "$(readlink -f "$0")")/env.sh"
mkdir -p "$D/suite" "$D/film" && cd "$D"
trap 'rm -f "$D"/suite/*.tmp.y4m "$D"/suite/*.part.mkv "$D"/film/*.part.mp4' EXIT
X=https://media.xiph.org/video/derf/y4m
into() {
    local out=$1 part="${1%.*}.part.${1##*.}"
    shift
    "$@" "$part" && mv "$part" "$out"
}
xiph() {
    [ -f suite/$1.mkv ] && return 0
    curl -sfL -r 0-$3 -o suite/$1.tmp.y4m $X/$2.y4m && into suite/$1.mkv ffv1 -i suite/$1.tmp.y4m -frames:v 41
    r=$?
    rm -f suite/$1.tmp.y4m
    return $r
}
for f in park_joy crowd_run in_to_tree ducks_take_off old_town_cross; do xiph ${f}_1080p50 ${f}_1080p50 130000000; done
xiph mobcal_720p50 720p50_mobcal_ter 60000000
xiph shields_720p50 720p50_shields_ter 60000000
xiph parkrun_720p50 720p50_parkrun_ter 60000000
xiph stockholm_720p60 720p5994_stockholm_ter 60000000
B=.bbb/bbb_sunflower_1080p_60fps_normal.mp4
getbbb() {
    [ -f $B ] && return 0
    mkdir -p .bbb
    curl -sf -o .bbb/bbb.zip https://download.blender.org/demo/movies/BBB/bbb_sunflower_1080p_60fps_normal.mp4.zip
    (cd .bbb && unzip -q -o bbb.zip && rm bbb.zip)
}
bbb() {
    [ -f suite/$1.mkv ] && return 0
    getbbb && into suite/$1.mkv ffv1 -ss $2 -i $B -frames:v 81 -pix_fmt yuv420p
}
bbb bbb_tree_cut_1080p60 95
bbb bbb_meadow_still_1080p60 290
bbb bbb_squirrel_branch_1080p60 460
bbb bbb_credits_scroll_1080p60 560
bbb bbb_butterfly_closeup_1080p60 80
bbb bbb_butterfly_bunny_1080p60 85
bbb bbb_apple_cut_1080p60 108
[ -f film/bbb_cuts_1080p24.mp4 ] || { getbbb && into film/bbb_cuts_1080p24.mp4 ffmpeg -hide_banner -loglevel error -ss 60 -t 60 -i $B -an -vf fps=24 -c:v libx264 -crf 18 -preset fast -y; }
[ -f film/bbb_1080p60.mp4 ] || { getbbb && into film/bbb_1080p60.mp4 ffmpeg -hide_banner -loglevel error -ss 60 -t 30 -i $B -an -c copy -y; }
[ -f film/bbb_squirrel_branch_1080p30.mp4 ] || into film/bbb_squirrel_branch_1080p30.mp4 ffmpeg -hide_banner -loglevel error -i suite/bbb_squirrel_branch_1080p60.mkv -vf "select='not(mod(n\,2))',setpts=N/30/TB" -r 30 -c:v libx264 -crf 12 -preset fast -y
rm -rf .bbb
echo "test data ready in $D"
