#!/bin/sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/build/mpv-src
cd "$root"
[ -f mpv/meson.build ] || git submodule update --init mpv >&2
want="$(git -C mpv rev-parse HEAD) $(cksum <patches/mpv.patch)"
if [ "$(cat "$out/.memc-stamp" 2>/dev/null)" != "$want" ]; then
    rm -rf "$out"
    mkdir -p "$out"
    git -C mpv archive HEAD | tar -x -C "$out"
    patch -s -p1 -d "$out" <patches/mpv.patch
    echo "$want" >"$out/.memc-stamp"
fi
d=$out/video/filter/memc
mkdir -p "$d/shaders"
find "$d" "$out/video/filter/vf_memc.c" -type l -delete 2>/dev/null || true
ln -s "$root/src/vf_memc.c" "$out/video/filter/vf_memc.c"
ln -s "$root/src/memc.h" "$root/src/memc_vk.c" "$d/"
ln -s "$root"/src/shaders/*.comp "$root"/src/shaders/*.glsl "$root/src/shaders/meson.build" "$d/shaders/"
echo "$out"
