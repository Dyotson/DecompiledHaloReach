#!/usr/bin/env bash
# Usage: tools/run_reach.sh [seconds] [extra reach flags...]
# Logs go to /tmp/reach_run.log (runtime log) and /tmp/reach_run.out (stdout/stderr).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS="${1:-60}"
shift || true
cd "$ROOT/reach-recomp/out/build/linux-release"
[ -f /tmp/reach_run.log ] && mv -f /tmp/reach_run.log /tmp/reach_run.prev.log
export LD_LIBRARY_PATH="$HOME/rexglue-sdk/linux-amd64/lib:$PWD"
timeout --signal=INT --kill-after=10 "$SECS" ./reach \
    --game_data_root="$ROOT/extracted/xbox360" \
    --log_file=/tmp/reach_run.log --log_level=debug "$@" > /tmp/reach_run.out 2>&1
echo "EXIT=$?" >> /tmp/reach_run.out
