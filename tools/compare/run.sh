#!/bin/bash
set -u
K=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
. "$K/../env.sh"
L=$K/local
MODE=${1:-all}
shift 2>/dev/null
TOOLS=${*:-motionblade rife afmf fsr lsfg nvof svp}
WORK=$L/work
RESULTS=$K/results.txt
CLIPS=${CLIPS:-$SUITE_CLIPS}
FFMPEG=${FFMPEG:-ffmpeg}
JOBS=$(nproc)
export LD_LIBRARY_PATH=$L/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export RUSTICL_ENABLE=${RUSTICL_ENABLE:-radeonsi,iris}
export PIP_DISABLE_PIP_VERSION_CHECK=1

GLSLANG=(https://github.com/KhronosGroup/glslang.git b411a8fb8aa0f6ee727df17f3f8eafb3eb85ba9e)
AFMF=(https://github.com/serialexperimentslainnnn/afmf-linux.git v1.3.1)
FFX=(https://github.com/ychwch/FidelityFX-SDK-Linux.git 2c90b7f4b877b97e5779aa4d09d338c1e29cb75c)
LSFG=(https://git.lsfg-vk.dev/lsfg-vk.git 2.0.0)
RIFE=(https://github.com/styler00dollar/VapourSynth-RIFE-ncnn-Vulkan.git c3ec6aabc07c8fa37a4f58d7fed9e2ad1fc1b13f)
RIFE_MODEL=rife-v4.16_lite_ensembleFalse
VAPOURSYNTH=82

PERF_CLIP=$D/perf/park_joy_1080p50.mp4
PERF_FRAMES=301
PERF_RUNS=${PERF_RUNS:-3}
PERF_SHORT=$(((PERF_FRAMES + 1) / 2))
PERF_LONG=$(((3 * PERF_FRAMES - 1) / 2))
PERF_DIR=/dev/shm/motionblade-compare.$$

TO_RGB="scale=in_color_matrix=bt709:in_range=tv:out_range=pc"
TO_YUV="scale=out_color_matrix=bt709:out_range=tv:in_range=pc"
NVOF_HW=(-init_hw_device cuda=cu -filter_hw_device cu)
NVOF_UP="format=nv12,hwupload_cuda"

log() { echo "[$(date +%T)] $*" >&2; }
die() { log "error: $*"; exit 1; }

need() {
    local c missing=
    for c in "$@"; do command -v "$c" >/dev/null || missing="$missing $c"; done
    [ -z "$missing" ] && return 0
    log "missing:$missing"
    return 1
}

case $MODE in quality|perf|all) ;; *) die "usage: ./run.sh [quality|perf|all] [tool ...]" ;; esac
need "$FFMPEG" ffprobe python3 bc || exit 1
FFMPEG=$(readlink -f "$(command -v "$FFMPEG")")
mkdir -p "$WORK" "$L/logs" "$L/bin" "$L/lib" "$L/src" "$L/build" "$L/stamp"
trap 'rm -rf "$PERF_DIR" "$WORK"' EXIT

ff() { "$FFMPEG" -nostdin -v error -y "$@"; }

step() {
    local name=$1
    shift
    "$@" > "$L/logs/$name.log" 2>&1 && return 0
    log "$name failed, last lines of $L/logs/$name.log:"
    tail -15 "$L/logs/$name.log" >&2
    return 1
}

cm() { cmake -S "$1" -B "$2" -DCMAKE_BUILD_TYPE=Release "${@:3}" && cmake --build "$2" -j"$JOBS"; }

stamp() { { printf '%s\n' "$1"; shift; [ $# -eq 0 ] || cat "$@"; } | sha1sum | cut -d' ' -f1; }

built() {
    local name=$1 key=$2 files=$3 f
    shift 3
    for f in $files; do [ -e "$L/$f" ] || return 1; done
    [ "$(cat "$L/stamp/$name" 2>/dev/null)" = "$(stamp "$key" "$@")" ]
}

mark() { local name=$1; shift; stamp "$@" > "$L/stamp/$name"; }

tidy() { local n; for n in "$@"; do rm -rf "$L/src/$n" "$L/build/$n"; done; }

fetch() {
    local name=$1 url=$2 ref=$3 dir=$L/src/$1
    shift 3
    [ "$(cat "$dir/.fetched" 2>/dev/null)" = "$url $ref" ] && return 0
    need git || return 1
    log "downloading $name"
    rm -rf "$dir"
    mkdir -p "$dir"
    (
        cd "$dir" && git init -q && git remote add origin "$url" &&
            if [ $# -gt 0 ]; then git sparse-checkout set --no-cone "$@"; fi &&
            git fetch -q --depth 1 --filter=blob:none origin "$ref" && git checkout -q FETCH_HEAD
    ) > "$L/logs/fetch-$name.log" 2>&1 || { log "downloading $name failed, see $L/logs/fetch-$name.log"; return 1; }
    echo "$url $ref" > "$dir/.fetched"
}

venv() {
    [ -f "$L/venv/.ready" ] && return 0
    log "creating Python venv with VapourSynth (RIFE and SVP)"
    rm -rf "$L/venv"
    python3 -m venv "$L/venv" && "$L/venv/bin/pip" install -q "vapoursynth==$VAPOURSYNTH" numpy &&
        touch "$L/venv/.ready"
}

build_eval() {
    need meson ninja || return 1
    step eval make -C "$ROOT" && [ -x "$E" ]
}

glslang_ok() {
    printf '#version 450\nlayout(local_size_x=1) in;\nvoid main(){}\n' > "$WORK/t.comp"
    glslangValidator -V -Os "$WORK/t.comp" -o "$WORK/t.spv" >/dev/null 2>&1
}

glslang_build() {
    (cd "$L/src/glslang" && python3 update_glslang_sources.py) &&
        cm "$L/src/glslang" "$L/build/glslang" -DENABLE_OPT=ON -DGLSLANG_TESTS=OFF -DSPIRV_SKIP_TESTS=ON \
            -DCMAKE_INSTALL_PREFIX="$L/glslang" &&
        cmake --install "$L/build/glslang"
}

build_glslang() {
    GLPATH=$L/glslang/bin:
    built glslang "${GLSLANG[*]}" glslang/bin/glslangValidator && return 0
    GLPATH=
    glslang_ok && return 0
    GLPATH=$L/glslang/bin:
    need cmake c++ || return 1
    fetch glslang "${GLSLANG[@]}" || return 1
    log "building glslang"
    step glslang glslang_build && mark glslang "${GLSLANG[*]}" && tidy glslang
}

afmf_probe() {
    AFMF_WSI=$1 afmf_run 128 128 /dev/null 2 timeout 60 env -u AFMF_TEST_SOURCE 2>&1 | sed -n 's/^using //p'
}

is_cpu_device() {
    case $1 in *" (cpu)") return 0 ;; esac
    return 1
}

afmf_device() {
    local dev
    if [ -n "${AFMF_WSI:-}" ]; then
        dev=$(afmf_probe "$AFMF_WSI")
    else
        AFMF_WSI=headless
        dev=$(afmf_probe headless)
        if { [ -z "$dev" ] || is_cpu_device "$dev"; } && [ -n "${WAYLAND_DISPLAY:-}" ]; then
            local on_wayland=$(afmf_probe wayland)
            [ -n "$on_wayland" ] && { AFMF_WSI=wayland; dev=$on_wayland; }
        fi
    fi
    [ -n "$dev" ] || { log "afmf: the test program found no device (AFMF_WSI=$AFMF_WSI)"; return 1; }
    log "afmf: $dev ($AFMF_WSI surface)"
    is_cpu_device "$dev" || return 0
    [ "${ALLOW_CPU:-0}" = 1 ] && return 0
    log "afmf: that is the CPU, not the GPU (set ALLOW_CPU=1 to run it anyway)"
    return 1
}

build_afmf() {
    afmf_build && afmf_device
}

afmf_build() {
    built afmf "${AFMF[*]}" "bin/afmf_headless layer/afmf-linux.json" "$K/patches/afmf-linux.patch" && return 0
    build_glslang || return 1
    fetch afmf "${AFMF[@]}" || return 1
    local s=$L/src/afmf b=$L/build/afmf
    git -C "$s" checkout -q . && git -C "$s" apply "$K/patches/afmf-linux.patch" || return 1
    log "building afmf-linux"
    PATH=$GLPATH$PATH step afmf cm "$s" "$b" -DAFMF_WERROR=OFF || return 1
    mkdir -p "$L/layer"
    cp "$b/afmf_headless" "$L/bin/" && cp "$b/layer/libafmf-linux.so" "$L/layer/" &&
        sed 's#"library_path": ".*"#"library_path": "./libafmf-linux.so"#' "$b/layer/afmf-linux.json" > "$L/layer/afmf-linux.json" &&
        mark afmf "${AFMF[*]}" "$K/patches/afmf-linux.patch" && tidy afmf
}

build_fsr() {
    local files=("$K/harness/fsr_fi.cpp" "$K/harness/CMakeLists.txt")
    built fsr "${FFX[*]}" "bin/fsr_fi lib/libffx_backend_vk_x64.so" "${files[@]}" && return 0
    build_glslang || return 1
    need cmake c++ || return 1
    fetch ffx "${FFX[@]}" '/sdk/' || return 1
    log "building the FidelityFX SDK (FSR 3)"
    PATH=$GLPATH$PATH step ffx cm "$L/src/ffx/sdk" "$L/build/ffx" -DFFX_API_BACKEND=VK_X64 -DFFX_FI=ON \
        -DFFX_OF=ON -DFFX_AUTO_COMPILE_SHADERS=ON || return 1
    cp "$L/build/ffx/src/backends/vk/libffx_backend_vk_x64.so" "$L/lib/" || return 1
    step fsr_fi cm "$K/harness" "$L/build/fsr_fi" -DFFX_SDK_DIR="$L/src/ffx/sdk" \
        -DFFX_BACKEND_LIB="$L/lib/libffx_backend_vk_x64.so" || return 1
    cp "$L/build/fsr_fi/fsr_fi" "$L/bin/" && mark fsr "${FFX[*]}" "${files[@]}" && tidy ffx fsr_fi
}

build_lsfg() {
    local files=("$K/harness/lsfg_fi.cpp" "$K/harness/CMakeLists.txt")
    built lsfg "${LSFG[*]}" bin/lsfg_fi "${files[@]}" && return 0
    need cmake c++ || return 1
    fetch lsfg "${LSFG[@]}" || return 1
    log "building lsfg-vk"
    step lsfg cm "$K/harness" "$L/build/lsfg" -DLSFGVK_SRC="$L/src/lsfg" || return 1
    cp "$L/build/lsfg/lsfg_fi" "$L/bin/" && mark lsfg "${LSFG[*]}" "${files[@]}" && tidy lsfg
}

rife_build() {
    local s=$L/src/rife pc=$L/build/rife-pc
    local vs=$("$L/venv/bin/python" -I -c "import os, vapoursynth; print(os.path.dirname(vapoursynth.__file__))")
    mkdir -p "$pc"
    printf 'prefix=%s\nincludedir=${prefix}/include\nlibdir=${prefix}\nName: vapoursynth\nDescription: vs\nVersion: %s\nCflags: -I${includedir}\n' \
        "$vs" "$VAPOURSYNTH" > "$pc/vapoursynth.pc"
    git -C "$s" submodule update -q --init --depth 1 subprojects/ncnn &&
        git -C "$s/subprojects/ncnn" submodule update -q --init --depth 1 glslang &&
        rm -rf "$L/build/rife" && PKG_CONFIG_PATH=$pc meson setup "$L/build/rife" "$s" -Db_lto=false &&
        ninja -C "$L/build/rife"
}

build_rife() {
    built rife "${RIFE[*]} $RIFE_MODEL $VAPOURSYNTH" "lib/librife.so models/$RIFE_MODEL" && return 0
    need meson ninja c++ || return 1
    venv || return 1
    fetch rife "${RIFE[@]}" '/*' '!/models/' "/models/$RIFE_MODEL/" || return 1
    log "building RIFE"
    step rife rife_build || return 1
    mkdir -p "$L/models"
    rm -rf "$L/models/$RIFE_MODEL"
    cp "$L/build/rife/librife.so" "$L/lib/" && cp -r "$L/src/rife/models/$RIFE_MODEL" "$L/models/" &&
        mark rife "${RIFE[*]} $RIFE_MODEL $VAPOURSYNTH" && tidy rife rife-pc
}

lsfg_dll() {
    local f
    for f in "${LSFG_DLL:-}" "$K/lsfg-vk.dll" \
        "$HOME/.steam/steam/steamapps/common/Lossless Scaling/lsfg-vk.dll" \
        "$HOME/.local/share/Steam/steamapps/common/Lossless Scaling/lsfg-vk.dll"; do
        [ -n "$f" ] && [ -f "$f" ] && { echo "$f"; return 0; }
    done
    log "lsfg: copy lsfg-vk.dll (Lossless Scaling on Steam, beta branch \"lsfg-vk\") next to run.sh, or set LSFG_DLL"
    return 1
}

svp_libs() {
    [ -f "$L/lib/svp/libsvpflow1.so" ] && [ -f "$L/lib/svp/libsvpflow2.so" ] && return 0
    local run=${SVP_INSTALLER:-$(ls "$K"/svp*-linux.run 2>/dev/null | head -1)}
    [ -n "$run" ] && [ -f "$run" ] ||
        { log "svp: copy the SVP installer (svp4-linux.run) next to run.sh, or set SVP_INSTALLER"; return 1; }
    venv && "$L/venv/bin/pip" install -q py7zr && "$L/venv/bin/python" -I "$K/scripts/svp_extract.py" "$run" "$L/lib/svp"
}

nvof_filter() {
    "$FFMPEG" -hide_banner -h filter=nvinterpolate 2>/dev/null | grep -q "Filter nvinterpolate" &&
        "$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -q " hevc_nvenc " && return 0
    log "nvof: needs an FFmpeg with the nvinterpolate filter and hevc_nvenc, set FFMPEG=/path/to/ffmpeg"
    return 1
}

available() {
    case $1 in
        motionblade) return 0 ;;
        rife) build_rife ;;
        afmf) build_afmf ;;
        fsr) build_fsr ;;
        lsfg) LSFG_FILE=$(lsfg_dll) && build_lsfg ;;
        nvof) nvof_filter ;;
        svp) svp_libs && opencl_vendors && svp_gpu ;;
        *) log "unknown tool $1"; return 1 ;;
    esac
}

clips() {
    local c
    for c in $CLIPS; do
        [ -f "$D/${c%%:*}.mkv" ] && continue
        need curl unzip || return 1
        log "downloading the test clips into $D"
        MOTIONBLADE_DATA=$D step clips "$ROOT/tools/fetch-data.sh"
        return
    done
}

perf_clip() {
    [ -f "$PERF_CLIP" ] && return 0
    need curl || return 1
    log "downloading the performance clip into $D/perf"
    mkdir -p "$D/perf"
    step perf_clip curl -sfL -r 0-$((PERF_FRAMES * (1920 * 1080 * 3 / 2 + 6) + 200)) -o "$D/perf/tmp.y4m" \
            https://media.xiph.org/video/derf/y4m/park_joy_1080p50.y4m &&
        step perf_clip ff -i "$D/perf/tmp.y4m" -frames:v $PERF_FRAMES -an -c:v libx264 -crf 12 \
            -preset fast -pix_fmt yuv420p "$PERF_CLIP"
    local r=$?
    rm -f "$D/perf/tmp.y4m"
    return $r
}

gpu_info() {
    if command -v nvidia-smi >/dev/null; then
        nvidia-smi --query-gpu=name,driver_version,pstate,clocks.gr,clocks.max.gr --format=csv,noheader
        echo "For steady numbers the line above should not say P8/300 MHz: keep nvidia-settings open on"
        echo "PowerMizer 'Prefer Maximum Performance' during the run, or lock clocks: sudo nvidia-smi -lgc 1800,1800"
    fi
    for f in /sys/class/drm/card*/device/power_dpm_force_performance_level; do
        [ -f "$f" ] && echo "$f: $(cat "$f") (use 'high' for steady numbers)"
    done
}

size_of() { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$1"; }

fmt_of() { case $1 in svp|nvof) echo yuv ;; *) echo rgba ;; esac; }

even() {
    local src=$1 frames=$2 out=$3 fmt=$4 vf="select='not(mod(n\,2))'"
    [ "$fmt" = rgba ] && vf="$vf,$TO_RGB"
    ff -i "$src" -frames:v "$frames" -vf "$vf" -fps_mode passthrough -pix_fmt "${fmt/yuv/yuv420p}" -f rawvideo "$out"
}

filtered() {
    local noise=$1
    shift
    "$@" 2>&1 >/dev/null | grep -v "$noise" >&2
    return "${PIPESTATUS[0]}"
}

gen_rife() {
    filtered '^\[[0-9]* .*\]  ' "$L/venv/bin/python" -I "$K/scripts/rife_gen.py" "$L/lib/librife.so" "$L/models/$RIFE_MODEL" "$@"
}

gen_fsr() { "$L/bin/fsr_fi" "$@" >/dev/null; }

gen_lsfg() { "$L/bin/lsfg_fi" "$@" --dll "$LSFG_FILE" >/dev/null; }

svp_gpu() {
    local n=$("$L/venv/bin/python" -I "$K/scripts/svp_gen.py" --gpus 2>/dev/null)
    [ "${n:-0}" -gt 0 ] && return 0
    [ "${ALLOW_CPU:-0}" = 1 ] && return 0
    log "svp: no OpenCL GPU found, it would run on the CPU (install an OpenCL driver, or set ALLOW_CPU=1 to run it anyway)"
    return 1
}

mesa_gpu() { grep -qE '0x(1002|8086)' /sys/class/drm/card*/device/vendor 2>/dev/null; }

opencl_vendors() {
    local d=$WORK/opencl f
    [ -z "${OCL_ICD_VENDORS:-}" ] && [ -d /etc/OpenCL/vendors ] && ! mesa_gpu || return 0
    mkdir -p "$d"
    for f in /etc/OpenCL/vendors/*.icd; do
        case $f in *rusticl*) ;; *) [ -e "$f" ] && ln -sf "$f" "$d/" ;; esac
    done
    export OCL_ICD_VENDORS=$d
}

gen_svp() { "$L/venv/bin/python" -I "$K/scripts/svp_gen.py" "$L/lib/svp" "$@"; }

nvof_ff() { (cd "$WORK" && ff "$@"); }

afmf_run() {
    local w=$1 h=$2 src=$3 frames=$4
    shift 4
    env VK_ADD_LAYER_PATH="$L/layer" AFMF_TEST_WSI="${AFMF_WSI:-headless}" AFMF_ENABLE=1 AFMF_MIN_FPS=0 AFMF_GOVERNOR=0 AFMF_TEST_EXTENT="${w}x$h" \
        AFMF_TEST_SOURCE="$src" AFMF_TEST_FRAMES="$frames" AFMF_TEST_FRAME_MS=0 "$@" "$L/bin/afmf_headless"
}

gen_afmf() {
    local w=$1 h=$2 in=$3 out=$4 lead=8 d=$WORK/afmf
    rm -rf "$d"; mkdir -p "$d/dump"
    local count=$(( $(stat -c %s "$in") / (w * h * 4) ))
    python3 -I "$K/scripts/lead.py" "$in" "$d/src.rgba" "$w" "$h" $lead
    afmf_run "$w" "$h" "$d/src.rgba" $((count + lead)) AFMF_LOG=1 AFMF_DUMP_DIR="$d/dump" >/dev/null 2>&1
    for k in $(seq 1 $((count - 1))); do
        [ -f "$d/dump/afmf_generated_$((k + lead)).ppm" ] || { log "afmf: frame $k missing"; return 1; }
    done
    ff -start_number $((lead + 1)) -i "$d/dump/afmf_generated_%d.ppm" -frames:v $((count - 1)) \
        -pix_fmt rgba -f rawvideo "$out"
    rm -rf "$d"
}

gen_nvof() {
    local w=$1 h=$2 in=$3 out=$4 d=$WORK/nvof
    rm -rf "$d"; mkdir -p "$d"
    filtered "^Optical Flow Grid Size" nvof_ff "${NVOF_HW[@]}" -f rawvideo -pix_fmt yuv420p -s "${w}x$h" -r 25 -i "$in" \
        -vf "tpad=start=1:start_mode=clone,$NVOF_UP,nvinterpolate=fps=50" -fps_mode passthrough \
        -c:v hevc_nvenc -tune lossless "$d/all.mkv" || return 1
    ff -i "$d/all.mkv" -pix_fmt yuv420p -f rawvideo "$d/all.yuv" || return 1
    python3 -I "$K/scripts/nvof_pick.py" "$d/all.yuv" "$in" "$out" $((w * h * 3 / 2)) || return 1
    rm -rf "$d"
}

as_yuv() {
    case $1 in
        *.yuv) cat "$1" ;;
        *) ff -f rawvideo -pix_fmt rgba -s "$2" -i "$1" -vf "$TO_YUV" -pix_fmt yuv420p -f rawvideo - ;;
    esac
}

round_trip() {
    ff -i "$1" -frames:v "$2" -vf "$TO_RGB" -pix_fmt rgba -f rawvideo - |
        ff -f rawvideo -pix_fmt rgba -s "$3" -i - -vf "$TO_YUV" -pix_fmt yuv420p -f yuv4mpegpipe "$4"
}

score() {
    local ref=$1 n=$2 gen=$3 size=$4
    python3 -I "$K/scripts/interleave.py" <(ff -i "$ref" -frames:v "$n" -pix_fmt yuv420p -f rawvideo -) \
        <(as_yuv "$gen" "$size") /dev/stdout "${size%x*}" "${size#*x}" "$n" |
        "$E" "$ref" -n "$n" -q -mode blend -cmp /dev/stdin | tail -1
}

quality() {
    local tool=$1 fmt=$(fmt_of "$1") c line all=
    for c in $CLIPS; do
        local f=${c%%:*} k=${c##*:}
        local name=${f##*/}
        local clip=$D/$f.mkv
        if [ "$tool" = motionblade ]; then
            line=$("$E" "$clip" -n "$k" -q | tail -1)
        else
            local size=$(size_of "$clip") ref=$clip
            even "$clip" "$k" "$WORK/even.$fmt" "$fmt" || return 1
            "gen_$tool" "${size%x*}" "${size#*x}" "$WORK/even.$fmt" "$WORK/gen.$fmt" ||
                { log "$tool failed on $name"; return 1; }
            [ "$fmt" = rgba ] && { ref=$WORK/ref.y4m; round_trip "$clip" "$k" "$size" "$ref" || return 1; }
            line=$(score "$ref" "$k" "$WORK/gen.$fmt" "$size")
            rm -f "$WORK/even.$fmt" "$WORK/gen.$fmt" "$WORK/ref.y4m"
        fi
        local p=$(echo "$line" | field "MEAN psnr") s=$(echo "$line" | field ssim)
        [ -n "$p" ] || { log "$tool: no score on $name"; return 1; }
        printf "  %-32s psnr %7s ssim %7s\n" "$name" "$p" "$s" >&2
        all="$all$p $s"$'\n'
    done
    printf "%s" "$all" | awk '{p += $1; s += $2} END {printf "%.2f %.3f\n", p / NR, s / NR}'
}

perf_inputs() {
    local d=$PERF_DIR size=$(size_of "$PERF_CLIP")
    local w=${size%x*} h=${size#*x}
    rm -rf "$d"; mkdir -p "$d"
    log "preparing performance input in $d"
    ff -i "$PERF_CLIP" -frames:v $PERF_FRAMES -pix_fmt yuv420p -f rawvideo "$d/fwd.yuv" || return 1
    local full=
    case " $TOOLS " in *" motionblade "*) full=$d/full.y4m ;; esac
    python3 -I "$K/scripts/pingpong.py" "$d/fwd.yuv" "$w" "$h" "$d/even_long.yuv" $full || return 1
    rm -f "$d/fwd.yuv"
    ff -f rawvideo -pix_fmt yuv420p -s "$size" -i "$d/even_long.yuv" -vf "$TO_RGB" -pix_fmt rgba \
        -f rawvideo "$d/even_long.rgba" || return 1
    head -c $((PERF_SHORT * w * h * 3 / 2)) "$d/even_long.yuv" > "$d/even_short.yuv"
    head -c $((PERF_SHORT * w * h * 4)) "$d/even_long.rgba" > "$d/even_short.rgba"
}

perf_run() {
    local tool=$1 w=$2 h=$3 len=$4 d=$PERF_DIR n=$PERF_SHORT
    [ "$len" = long ] && n=$PERF_LONG
    case $tool in
        motionblade) "$E" "$d/full.y4m" -n $((2 * n - 1)) -nometric -q ;;
        afmf) afmf_run "$w" "$h" "$d/even_$len.rgba" $n ;;
        nvof) nvof_ff "${NVOF_HW[@]}" -f rawvideo -pix_fmt yuv420p -s "${w}x$h" -r 25 -i "$d/even_$len.yuv" \
                -vf "$NVOF_UP,nvinterpolate=fps=50" -f null - ;;
        *) "gen_$tool" "$w" "$h" "$d/even_$len.$(fmt_of "$tool")" /dev/null ;;
    esac
}

perf_step() {
    local tool=$1 w=$2 h=$3 len=$4 plog=$5 what=$6
    : > "$plog"
    perf_run "$tool" "$w" "$h" "$len" >> "$plog" 2>&1 && return 0
    log "$tool failed ($what, $len input, exit $?):"
    tail -5 "$plog" >&2
    return 1
}

perf() {
    local tool=$1 size=$(size_of "$PERF_CLIP") plog=$WORK/perf_$1.log i a b c t all=
    local w=${size%x*} h=${size#*x}
    [ -f "$PERF_DIR/even_short.rgba" ] || perf_inputs || return 1
    perf_step "$tool" "$w" "$h" short "$plog" warm-up || return 1
    for i in $(seq "$PERF_RUNS"); do
        a=$(date +%s%N); perf_step "$tool" "$w" "$h" short "$plog" "run $i" || return 1
        b=$(date +%s%N); perf_step "$tool" "$w" "$h" long "$plog" "run $i" || return 1
        c=$(date +%s%N)
        t=$(echo "(($c - $b) - ($b - $a)) / 1000000 / ($PERF_LONG - $PERF_SHORT)" | bc -l)
        printf "  run %d: %.2f ms\n" "$i" "$t" >&2
        all="$all $t"
    done
    printf "%s\n" $all | sort -g | awk '{v[NR] = $1} END {printf "%.2f\n", v[int((NR + 1) / 2)]}'
}

build_eval || die "could not build eval (needs the Motion Blade build dependencies, see the main README)"
if [ "$MODE" != perf ]; then clips || die "could not download the test clips"; fi
if [ "$MODE" != quality ]; then perf_clip || die "could not download the performance clip"; fi

[ -s "$RESULTS" ] && echo >> "$RESULTS"
{
    echo "== $(date)"
    gpu_info
} | tee -a "$RESULTS" >&2

for tool in $TOOLS; do
    LSFG_FILE=
    if ! available "$tool"; then
        log "$tool: not available here, skipped"
        echo "$tool: skipped (not available)" >> "$RESULTS"
        continue
    fi
    p=- s=- ms=-
    if [ "$MODE" != perf ]; then
        log "$tool: quality ($(echo $CLIPS | wc -w) clips)"
        q=$(quality "$tool") && read -r p s <<< "$q"
    fi
    if [ "$MODE" != quality ]; then
        log "$tool: performance (1080p)"
        ms=$(perf "$tool") || ms=-
    fi
    printf "| %-12s | %8s ms | %6s | %6s |\n" "$tool" "$ms" "$p" "$s" | tee -a "$RESULTS"
done
echo "Columns: performance cost (total ms per generated 1080p frame, median of $PERF_RUNS), fidelity (PSNR), detail (SSIM). Saved in $RESULTS"
