T=$(dirname "$(readlink -f "${BASH_SOURCE:-$0}")")
ROOT=$(cd "$T/.." && pwd)
D=${MOTIONBLADE_DATA:-$(cd "$ROOT/.." && pwd)/samples}
E=${EVAL:-$ROOT/build/local/tools/eval/eval}
FLICK=$ROOT/build/local/tools/flick
SUITE_CLIPS="suite/park_joy_1080p50:41 suite/crowd_run_1080p50:41 suite/in_to_tree_1080p50:41 suite/ducks_take_off_1080p50:41 suite/old_town_cross_1080p50:41 suite/mobcal_720p50:41 suite/shields_720p50:41 suite/parkrun_720p50:41 suite/stockholm_720p60:41 suite/bbb_tree_cut_1080p60:81 suite/bbb_meadow_still_1080p60:81 suite/bbb_squirrel_branch_1080p60:81 suite/bbb_credits_scroll_1080p60:81 suite/bbb_butterfly_closeup_1080p60:81 suite/bbb_butterfly_bunny_1080p60:81 suite/bbb_apple_cut_1080p60:81"

ffv1() { ffmpeg -nostdin -hide_banner -loglevel error "$@" -c:v ffv1 -level 3 -slices 16 -y; }

field() { grep -o "$1 [0-9.]*" | awk '{print $NF}'; }

need_eval() { [ -x "$E" ] || { echo "$E: not found or not executable, run make" >&2; exit 1; }; }

need_clip() {
    [ -f "$1" ] && return 0
    echo "missing clip $1" >&2
    case $1 in
        */suite/*|*/film/bbb_*) echo "run tools/fetch-data.sh" >&2 ;;
        */film/*) echo "copyrighted sample, must be provided by hand" >&2 ;;
        */light/*) m=${1##*/light/}; echo "run LIGHT=${m%%_*} tools/light.sh" >&2 ;;
    esac
    exit 1
}
