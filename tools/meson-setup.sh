#!/bin/sh
set -e
build=$1
src=$2
shift 2
if [ -f "$build/build.ninja" ]; then
    grep -qF "\"source\": \"$src\"" "$build/meson-info/meson-info.json" 2>/dev/null && exit 0
    echo "$build was configured for another source directory, setting it up again"
    exec meson setup --wipe "$build" "$src"
fi
exec meson setup "$build" "$src" "$@"
