T=$(dirname "$(readlink -f "$0")")
D=${MOTIONBLADE_DATA:-$(cd "$T/../.." && pwd)/samples}
E=${EVAL:-$T/../eval_vk}

ffv1() { ffmpeg -nostdin -hide_banner -loglevel error "$@" -c:v ffv1 -level 3 -slices 16 -y; }

field() { grep -o "$1 [0-9.]*" | awk '{print $NF}'; }

need_eval() { [ -x "$E" ] || { echo "$E: not found or not executable, run make" >&2; exit 1; }; }

need_clip() {
    [ -f "$1" ] && return 0
    echo "missing clip $1" >&2
    case $1 in
        */suite/*|*/film/bbb_*) echo "run tools/fetch-data.sh" >&2 ;;
        */film/*) echo "copyrighted sample, must be provided by hand" >&2 ;;
        */light/*) m=${1##*/light/}; echo "run LIGHT=${m%%_*} tools/flicker.sh" >&2 ;;
    esac
    exit 1
}
