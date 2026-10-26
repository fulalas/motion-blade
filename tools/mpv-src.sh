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
d=$out/video/filter/motionblade
mkdir -p "$d/shaders"
find "$d" "$out/video/filter/vf_motionblade.c" -type l -delete 2>/dev/null || true
ln -s "$root/src/vf_motionblade.c" "$out/video/filter/vf_motionblade.c"
ln -s "$root/src/motionblade.h" "$root/src/motionblade_vk.c" "$d/"
ln -s "$root"/src/shaders/*.comp "$root"/src/shaders/*.glsl "$root/src/shaders/meson.build" "$d/shaders/"
echo "$out"
