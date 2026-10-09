#!/usr/bin/env bash
# Usage: tools/run_reach.sh [seconds] [extra reach flags...]
# Logs go to /tmp/reach_run.log (runtime log) and /tmp/reach_run.out (stdout/stderr).
# REACH_BUILD selects the build dir under reach-recomp/out/build (default linux-nightly),
# REXSDK the SDK install prefix its runtime libraries come from (default: the
# patched overlay of the ReXGlue 0.10.0.24 nightly from tools/build_rexglue_sdk.sh
# if present, else the plain nightly, which renders textures 256x too dark).
# By default START is pressed at 9 s to skip the ~37 s intro video (it
# starts at ~8 s); set REACH_AUTOPRESS="" to watch it, or to a schedule of your own.
# REACH_LOG_LEVEL overrides the log level (default debug; trace logs every GPU upload).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
shift || true
cd "$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-nightly}"
[ -f /tmp/reach_run.log ] && mv -f /tmp/reach_run.log /tmp/reach_run.prev.log
export REACH_AUTOPRESS="${REACH_AUTOPRESS-9:START}"
# Scripted input is injected for user 0. A connected pad (also Steam Input's or
# Sunshine's virtual ones) makes the menus behave differently and the scripted
# presses stop reaching the campaign, so SDL ignores every pad in scripted runs.
if [ -n "$REACH_AUTOPRESS" ]; then
    export SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT="${SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT-0x0000/0x0000}"
    export SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT="${SDL_JOYSTICK_IGNORE_DEVICES_EXCEPT-0x0000/0x0000}"
fi
DEFAULT_SDK="$HOME/rexglue-sdk-patched/0.10.0.24/linux-amd64"
[ -d "$DEFAULT_SDK/lib" ] || DEFAULT_SDK="$HOME/rexglue-sdk-nightly/0.10.0.24/linux-amd64"
export LD_LIBRARY_PATH="${REXSDK:-$DEFAULT_SDK}/lib:$PWD"
timeout --signal=INT --kill-after=10 "$SECS" ./reach \
    --game_data_root="$ROOT/extracted/xbox360" \
    --log_file=/tmp/reach_run.log --log_level="${REACH_LOG_LEVEL:-debug}" "$@" > /tmp/reach_run.out 2>&1
echo "EXIT=$?" >> /tmp/reach_run.out

# The runtime backs guest memory with /dev/shm/xenia_memory_* (~5 GB each) and
# does not unlink it when the process is interrupted. Remove our orphans so
# repeated test runs don't fill /dev/shm (which then fails with SIGBUS).
for shm in /dev/shm/xenia_memory_*; do
    [ -O "$shm" ] || continue
    grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
done
