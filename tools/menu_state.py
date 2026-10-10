#!/usr/bin/env python3
"""Which item is highlighted in a 768x480 screenshot from tools/live_step.sh.

Usage: menu_state.py [--strict] title|main|lobby PNG
  title: 0 = START SOLO CAMPAIGN, 1 = MAIN MENU (the title screen)
  main:  0 = CAMPAIGN, 1 = MATCHMAKING, 2 = FIREFIGHT, 3 = CUSTOM GAME, 4 = FORGE,
         5 = THEATER (the main menu)
  lobby: 0 = GAME TYPE, 1 = MAP, 2 = GAME OPTIONS, 3 = START GAME (the Custom Game lobby)
The game remembers the last selection of both menus in the profile, so scripts must
read the highlight instead of counting presses (UP moves up and wraps around).
It measures the selection marker bar left of each row.
--strict compares the marker column with the background just left of it and prints -1
when no row has a clear marker (the screen is not that menu, e.g. still loading), so a
script can wait for the menu; the plain mode always names a row.
"""
import sys
from PIL import Image

ROWS = {"title": (66, [368, 387]), "main": (100, [267, 285, 303, 322, 340, 358]),
        "lobby": (100, [287, 306, 324, 343])}
# --strict: the marker's column, and how much brighter than the background left of it
# the marked row must be (about 130 levels when shown; other rows pick up background
# edges of up to ~20).
BARS = {"title": 67, "main": 102, "lobby": 102}
MIN_CONTRAST, MAX_OTHER_RATIO = 40, 0.35
# The main menu and the lobby put their markers on nearly the same rows; the lobby has the
# game type's name right of GAME TYPE, the main menu nothing there. Text measures about
# 30 in mean horizontal gradient, the menus' fog and sky under 10.
TEXT_BOX = (320, 282, 376, 292)
TEXT_THRESHOLD = 15
NEEDS_TEXT = {"main": False, "lobby": True}


def texture(im, box):
    crop = im.crop(box)
    w, h = crop.size
    d = list(crop.get_flattened_data())
    return sum(abs(d[y * w + x + 1] - d[y * w + x]) for y in range(h) for x in range(w - 1)) / (
        h * (w - 1))


def column(im, x, y):
    data = list(im.crop((x, y - 5, x + 1, y + 5)).get_flattened_data())
    return sum(data) / len(data)


def strict(menu, im):
    rows = ROWS[menu][1]
    bar = BARS[menu]
    contrast = [column(im, bar, y) - column(im, bar - 4, y) for y in rows]
    best = max(range(len(rows)), key=lambda i: contrast[i])
    others = [c for i, c in enumerate(contrast) if i != best]
    clear = contrast[best] >= MIN_CONTRAST and max(others) <= MAX_OTHER_RATIO * contrast[best]
    if clear and menu in NEEDS_TEXT:
        clear = (texture(im, TEXT_BOX) >= TEXT_THRESHOLD) == NEEDS_TEXT[menu]
    return best if clear else -1


def main():
    args = sys.argv[1:]
    if args and args[0] == "--strict":
        menu, path = args[1], args[2]
        print(strict(menu, Image.open(path).convert("L")))
        return
    menu, path = args[0], args[1]
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
