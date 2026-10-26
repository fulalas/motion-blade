#!/bin/bash
D=${MOTIONBLADE_DATA:-$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)/samples}; f=$1; n=$2; crop=$3; out=$4; shift 4
ins=(-i "$D/$f.mkv"); for x in "$@"; do ins+=(-i "$x"); done
k=$(( $# + 1 )); fl=""; st=""
for i in $(seq 0 $((k-1))); do fl+="[$i:v]select=eq(n\,$n),crop=$crop,scale=iw*2:ih*2:flags=neighbor[v$i];"; st+="[v$i]"; done
ffmpeg -hide_banner -loglevel error "${ins[@]}" -filter_complex "${fl}${st}hstack=inputs=$k" -frames:v 1 -y "$out"
