#!/bin/bash
set -o pipefail
. "$(dirname "$(readlink -f "$0")")/env.sh"
m=${LIGHT:-flicker}
[ -x "$FLICK" ] || { echo "$FLICK: not found, run make" >&2; exit 1; }
mkdir -p "$D/light"
clips=""
for f in crowd_run_1080p50 park_joy_1080p50 shields_720p50 bbb_meadow_still_1080p60 bbb_butterfly_bunny_1080p60; do
  o=$D/light/${m}_$f.mkv
  [ -f "$o" ] || { need_clip "$D/suite/$f.mkv"
    ffmpeg -nostdin -v error -i "$D/suite/$f.mkv" -frames:v 41 -f yuv4mpegpipe - | "$FLICK" /dev/stdin /dev/stdout $m |
    ffv1 -f yuv4mpegpipe -i - "$o.part.mkv" && mv "$o.part.mkv" "$o" ||
    { rm -f "$o.part.mkv"; echo "failed to generate $o" >&2; exit 1; }; }
  clips="$clips light/${m}_$f:41"
done
CLIPS=$clips exec "$T/suite.sh" "$@"
