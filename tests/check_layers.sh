#!/bin/sh
root=$(cd "$(dirname "$0")/.." && pwd)
bad=$(grep -rnE '#include *[<"](vulkan/|libav|mpv/|video/|common/msg|motionblade/vulkan\.h)' "$root/core" || true)
if [ -n "$bad" ]; then
    echo "core must not depend on GPU, ffmpeg or mpv headers:"
    echo "$bad"
    exit 1
fi
bad=$(grep -rnE '#include *"(engine|framing|estimation|selection|compensation|scene|retiming|reference)/' "$root/vulkan" "$root/adapters" "$root/tools" || true)
if [ -n "$bad" ]; then
    echo "outer layers must use only the public core headers:"
    echo "$bad"
    exit 1
fi
echo "layers ok"
