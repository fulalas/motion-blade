#!/bin/sh
set -e
cd "$(dirname "$0")"
G="glslc -O --target-env=vulkan1.1 -mfmt=c"
for s in conf down norm me_full me_iter me_refine tproj tsel illum ilmed cutdec; do $G -o $s.h $s.comp; done
$G -o interp8.h interp.comp
$G -DDEPTH16 -o interp16.h interp.comp
$G -DNV -o interp8nv.h interp.comp
$G -DNV -DDEPTH16 -o interp16nv.h interp.comp
