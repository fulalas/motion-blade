#!/bin/sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/build/mpv-src
cd "$root"
[ -f mpv/meson.build ] || git submodule update --init mpv >&2
want="$(git -C mpv rev-parse HEAD) $(cksum <patches/mpv.patch)"
if [ "$(cat "$out/.motionblade-stamp" 2>/dev/null)" != "$want" ]; then
    rm -rf "$out"
    mkdir -p "$out"
    git -C mpv archive HEAD | tar -x -C "$out"
    patch -s -p1 -d "$out" <patches/mpv.patch
    echo "$want" >"$out/.motionblade-stamp"
fi
mkdir -p "$out/subprojects"
ln -snf "$root" "$out/subprojects/motionblade"
for file in vf_motionblade.c vf_motionblade_pulldown.c vf_motionblade_pulldown.h; do
    ln -snf "$root/adapters/mpv/$file" "$out/video/filter/$file"
done
echo "$out"
