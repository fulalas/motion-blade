#!/bin/sh
set -e
root=$(cd "$(dirname "$0")" && pwd)
src=$("$root/tools/mpv-src.sh")
[ -d "$root/../deps/bin" ] && PATH="$root/../deps/bin:$PATH"
"$root/tools/meson-setup.sh" "$root/build/mpv" "$src" --buildtype=release --sysconfdir=/etc -Dvulkan=enabled "$@"
ninja -C "$root/build/mpv"
