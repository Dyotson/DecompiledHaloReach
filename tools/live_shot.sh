#!/usr/bin/env bash
# Save what a game started by tools/live_session.sh shows, from the host's presenter
# (at the draw resolution scale, e.g. --resolution_scale=2), as DIR/shot.png.
#
# Usage: tools/live_shot.sh DIR
#   Prints the PNG path, or "no shot" if none appears within 10 s.
set -u
DIR="$1"
before=$(ls "$DIR"/reach_shot_*.ppm 2>/dev/null | wc -l)
touch "$DIR/shot.trigger"
for _ in $(seq 1 40); do
    [ "$(ls "$DIR"/reach_shot_*.ppm 2>/dev/null | wc -l)" -gt "$before" ] && break
    sleep 0.25
done
f=$(ls -t "$DIR"/reach_shot_*.ppm 2>/dev/null | head -1)
if [ "$(ls "$DIR"/reach_shot_*.ppm 2>/dev/null | wc -l)" -le "$before" ]; then
    echo "no shot"; exit 1
fi
python3 -c "from PIL import Image; Image.open('$f').save('$DIR/shot.png')" && rm -f "$f"
echo "$DIR/shot.png"
