#!/usr/bin/env bash
# Play the game: no time limit and no scripted input (tools/run_reach.sh is the test
# runner). Settings go in reach.toml next to the executable or in the F4 overlay
# (e.g. live_server, gamertag, resolution_scale); extra arguments are passed to the game:
#
#   tools/play.sh                         # windowed, keyboard/mouse and pads
#   tools/play.sh --resolution_scale=2    # render at 2304x1440
#   REACH_SERVER=example.org tools/play.sh
#
# Environment: REACH_BUILD (build dir under reach-recomp/out/build, default
# linux-nightly), REXSDK (SDK prefix; default the patched overlay that
# tools/build_rexglue_sdk.sh makes), REACH_DATA (game files, default extracted/xbox360).
# The log is ~/.local/share/reach/reach.log.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-nightly}"
DATA="${REACH_DATA:-$ROOT/extracted/xbox360}"
[ -x "$BUILD/reach" ] || { echo "no build at $BUILD (see README: Building)" >&2; exit 1; }
[ -f "$DATA/default.xex" ] || { echo "no game files at $DATA (see README: Building, step 1)" >&2; exit 1; }
SDK="${REXSDK:-$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64}"
if [ ! -d "$SDK/lib" ]; then
    echo "patched SDK overlay not found at $SDK; textures will render too dark" >&2
    SDK="$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64"
fi
LOG_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/reach"
mkdir -p "$LOG_DIR"
cd "$BUILD"
LD_LIBRARY_PATH="$SDK/lib:$BUILD" ./reach --game_data_root="$DATA" \
    --log_file="$LOG_DIR/reach.log" "$@"
status=$?
# Guest memory (/dev/shm/xenia_memory_*, 5 GB) is not unlinked when the game is killed.
for shm in /dev/shm/xenia_memory_*; do
    [ -O "$shm" ] || continue
    grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
done
exit $status
