#!/usr/bin/env bash
# Two instances on this machine in System Link: A hosts a Firefight lobby, B sits in its
# main menu with A's party selected ("Join User" is then X on B).
#
# Usage: tools/system_link_pair.sh DIR_A DIR_B [extra reach flags...]
#   Stops the instances a previous run started in DIR_A / DIR_B first. Needs about 12 GB
#   of RAM and 10 GB of /dev/shm.
#   With REACH_SERVER=host[:port] both connect to that Reach Live server instead
#   (profiles in DIR_A/data and DIR_B/data, gamertags REACH_GAMERTAG_A / _B).
#   Then: echo X:0.3 > DIR_B/in.fifo   to join; tools/live_step.sh DIR 0.3 for screenshots.
#   REACH_NETTRACE=packets (or 1) is passed through for network logging.
#   B keeps its profile in DIR_B/data. A fresh profile starts with the game's first-run
#   Armory screens instead of the main menu: get past them once by hand (or point
#   DIR_B/data at an existing second profile) before using this script.
set -u
TOOLS="$(cd "$(dirname "$0")" && pwd)"
A="$1"; B="$2"; shift 2

stop() {  # the instance a previous run started in DIR, if it still runs
    local pid game; pid=$(cat "$1/pid" 2>/dev/null) || return 0  # its `timeout` wrapper
    [ "$(ps -o comm= -p "$pid")" = timeout ] || return 0
    game=$(ps -o pid= --ppid "$pid")
    [ -n "$game" ] || return 0
    kill -INT $game; sleep 10; kill -KILL $game 2>/dev/null; sleep 2
}
stop "$A"; stop "$B"
for shm in /dev/shm/xenia_memory_*; do
    [ -O "$shm" ] || continue
    grep -qs "$(basename "$shm")" /proc/[0-9]*/maps || rm -f -- "$shm"
done

if [ -n "${REACH_SERVER:-}" ]; then  # both on a Reach Live server, each with its own profile
    REACH_INSTANCE="${REACH_INSTANCE_A:-2}" REACH_GAMERTAG="${REACH_GAMERTAG_A:-Noble Six}" \
        "$TOOLS/live_session.sh" "$A" 1800 --headless=true "$@"
    REACH_INSTANCE="${REACH_INSTANCE_B:-3}" REACH_GAMERTAG="${REACH_GAMERTAG_B:-Kat}" \
        "$TOOLS/live_session.sh" "$B" 1800 --headless=true "$@"
else
    REACH_NET=1 "$TOOLS/live_session.sh" "$A" 1800 --headless=true "$@"
    REACH_NET=1 REACH_INSTANCE=2 "$TOOLS/live_session.sh" "$B" 1800 --headless=true "$@"
fi
sleep 55

step() { "$TOOLS/live_step.sh" "$@" > /dev/null; }
to_main_menu() {  # title screen -> main menu, whatever the remembered selection
    step "$1" 0.5
    [ "$(python3 "$TOOLS/menu_state.py" title "$1/last.png")" = 0 ] && step "$1" 1.2 UP
    step "$1" 6 A
}
system_link() { step "$1" 3 Y; step "$1" 1 UP; step "$1" 8 A; }
steer() {  # main menu: highlight row $2
    for _ in $(seq 1 10); do
        [ "$(python3 "$TOOLS/menu_state.py" main "$1/last.png")" = "$2" ] && return 0
        step "$1" 1.2 UP
    done
    echo "could not reach main menu row $2 in $1" >&2; return 1
}

to_main_menu "$A"; system_link "$A"; steer "$A" 2; step "$A" 8 A
to_main_menu "$B"; system_link "$B"
step "$B" 1.5 RIGHT; step "$B" 1.5 DOWN; step "$B" 1.5 DOWN
echo "A: $A/last.png  B: $B/last.png"
