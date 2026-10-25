#!/bin/sh
set -e
root=$(cd "$(dirname "$0")" && pwd)
src=$("$root/tools/mpv-src.sh")
[ -d "$root/../deps/bin" ] && PATH="$root/../deps/bin:$PATH"
[ -f "$root/build/mpv/build.ninja" ] || meson setup "$root/build/mpv" "$src" --buildtype=release "$@"
ninja -C "$root/build/mpv"
