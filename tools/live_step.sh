#!/usr/bin/env bash
# Press inputs in a game started by tools/live_session.sh, wait, and dump a frame.
#
# Usage: tools/live_step.sh DIR WAIT [INPUT...]
#   Sends each INPUT 0.8 s apart, waits WAIT seconds, dumps a frame and writes
#   DIR/last_full.png and DIR/last.png (768x480). Prints the dump's file name.
#   With no new dump within 10 s (the game stopped presenting) it prints "no frame".
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$1"; WAIT="$2"; shift 2
for input in "$@"; do echo "$input" > "$DIR/in.fifo"; sleep 0.8; done
sleep "$WAIT"
before=$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)
touch "$DIR/dump.trigger"
for _ in $(seq 1 40); do
    [ "$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)" -gt "$before" ] && break
    sleep 0.25
done
if [ "$(ls "$DIR"/reach_frame_*.bin 2>/dev/null | wc -l)" -le "$before" ]; then
    echo "no frame"; exit 1
fi
f=$(ls -t "$DIR"/reach_frame_*.bin | head -1); sleep 0.3
python3 "$ROOT/tools/frame_to_png.py" "$f" "$DIR/last_full.png" > /dev/null
python3 -c "from PIL import Image; Image.open('$DIR/last_full.png').convert('RGB').resize((768,480)).save('$DIR/last.png')"
basename "$f"
