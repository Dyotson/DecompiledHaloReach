#!/usr/bin/env bash
# Start the game for interactive, scripted stepping (see tools/live_step.sh).
#
# Usage: tools/live_session.sh DIR [seconds] [extra reach flags...]
#   DIR receives the input FIFO (in.fifo), frame dumps, run.out and the log (reach.log).
#   Inputs written to DIR/in.fifo, one per line ("A", "UP", "START:0.5"), are pressed as
#   they arrive; touching DIR/dump.trigger dumps the next frame.
#
# Environment:
#   REACH_INSTANCE=2  a second instance on the same machine: its own profile and saves
#                     (XDG_DATA_HOME=DIR/data), XUID and gamertag (override with
#                     REACH_XUID / REACH_GAMERTAG). Use REACH_NET=1 on both for System Link.
#   REXSDK            SDK prefix (default: the patched overlay, as tools/run_reach.sh).
# The game runs in the background; stop it with kill -INT on its PID (never pkill -f).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$(mkdir -p "$1" && cd "$1" && pwd)"
SECS="${2:-1800}"
shift 2 2>/dev/null || shift $#
rm -f "$DIR/in.fifo" "$DIR/dump.trigger" "$DIR"/reach_frame_*.bin "$DIR"/reach_frame_*.json
mkfifo "$DIR/in.fifo"

if [ "${REACH_INSTANCE:-1}" = 2 ]; then
    export XDG_DATA_HOME="$DIR/data"
    export REACH_XUID="${REACH_XUID:-E00000000000B002}"
    export REACH_GAMERTAG="${REACH_GAMERTAG:-Spartan2}"
fi
DEFAULT_SDK="$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64"
[ -d "$DEFAULT_SDK/lib" ] || DEFAULT_SDK="$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64"
cd "$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-nightly}"
SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT=0x0000/0x0000 \
REACH_AUTOPRESS="${REACH_AUTOPRESS-9:START}" REACH_AUTOPRESS_FIFO="$DIR/in.fifo" \
REACH_FRAMEDUMP_TRIGGER="$DIR/dump.trigger" REACH_FRAMEDUMP_DIR="$DIR" \
LD_LIBRARY_PATH="${REXSDK:-$DEFAULT_SDK}/lib:$PWD" nohup timeout --signal=INT "$SECS" \
    ./reach --game_data_root="$ROOT/extracted/xbox360" --log_file="$DIR/reach.log" \
    --log_level="${REACH_LOG_LEVEL:-info}" --vulkan_readback_resolve=true "$@" > "$DIR/run.out" 2>&1 &
echo "started pid $! in $DIR"
