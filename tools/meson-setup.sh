#!/bin/sh
set -e
build=$1
src=$2
shift 2
configured_for_source() {
    python3 -I -c 'import json, os, sys
source = json.load(open(sys.argv[1]))["directories"]["source"]
sys.exit(os.path.realpath(source) != os.path.realpath(sys.argv[2]))' "$build/meson-info/meson-info.json" "$src" 2>/dev/null
}
if [ -f "$build/build.ninja" ]; then
    configured_for_source && exit 0
    echo "$build was configured for another source directory, setting it up again"
    exec meson setup --wipe "$build" "$src" "$@"
fi
exec meson setup "$build" "$src" "$@"
