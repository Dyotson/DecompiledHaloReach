#!/usr/bin/env bash
# Usage: tools/run_reach.sh [seconds] [extra reach flags...]
# Logs go to /tmp/reach_run.log (runtime log) and /tmp/reach_run.out (stdout/stderr).
# REACH_BUILD selects the build dir under reach-recomp/out/build (default linux-release),
# REXSDK the SDK install prefix its runtime libraries come from.
# By default START is pressed at 9 s and 10 s to skip the ~37 s intro video (it
# starts at ~8 s); set REACH_AUTOPRESS="" to watch it, or to a schedule of your own.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
shift || true
cd "$ROOT/reach-recomp/out/build/${REACH_BUILD:-linux-release}"
[ -f /tmp/reach_run.log ] && mv -f /tmp/reach_run.log /tmp/reach_run.prev.log
export REACH_AUTOPRESS="${REACH_AUTOPRESS-9:START,10:START}"
export LD_LIBRARY_PATH="${REXSDK:-$HOME/rexglue-sdk/linux-amd64}/lib:$PWD"
timeout --signal=INT --kill-after=10 "$SECS" ./reach \
    --game_data_root="$ROOT/extracted/xbox360" \
    --log_file=/tmp/reach_run.log --log_level=debug "$@" > /tmp/reach_run.out 2>&1
echo "EXIT=$?" >> /tmp/reach_run.out
