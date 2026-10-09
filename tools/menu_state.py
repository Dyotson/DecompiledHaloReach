#!/usr/bin/env python3
"""Which item is highlighted in a 768x480 screenshot from tools/live_step.sh.

Usage: menu_state.py title|main PNG
  title: 0 = START SOLO CAMPAIGN, 1 = MAIN MENU (the title screen)
  main:  0 = CAMPAIGN, 1 = MATCHMAKING, 2 = FIREFIGHT, 3 = CUSTOM GAME, 4 = FORGE,
         5 = THEATER (the main menu)
The game remembers the last selection of both menus in the profile, so scripts must
read the highlight instead of counting presses (UP moves up and wraps around).
It measures the selection marker bar left of each row.
"""
import sys
from PIL import Image

ROWS = {"title": (66, [368, 387]), "main": (100, [267, 285, 303, 322, 340, 358])}


def main():
    menu, path = sys.argv[1], sys.argv[2]
    x, rows = ROWS[menu]
    im = Image.open(path).convert("L")
    scores = []
    for y in rows:
        band = im.crop((x, y - 6, x + 4, y + 6))
        data = list(band.get_flattened_data())
        scores.append(sum(data) / len(data))
    print(max(range(len(rows)), key=lambda i: scores[i]))


if __name__ == "__main__":
    main()
