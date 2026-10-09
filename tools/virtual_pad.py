#!/usr/bin/env python3
"""Press buttons on a virtual Xbox 360 controller at scheduled times.

Usage: virtual_pad.py SCHEDULE [--hold SECONDS]
  SCHEDULE is "t:BUTTON,t:BUTTON,..." in seconds from start, e.g. "15:START,25:A".
  Buttons: A B X Y START BACK LB RB LS RS UP DOWN LEFT RIGHT.

Drives games running in emulators (Xenia reads the pad through SDL) the way
REACH_AUTOPRESS drives the recompiled build, so both can be brought to the same
scene for differential debugging. Needs write access to /dev/uinput and
python3-evdev. The device exists only while the script runs; it stays alive
for a few seconds after the last press.
"""
import sys
import time

from evdev import AbsInfo, UInput, ecodes as e

BUTTONS = {
    "A": e.BTN_A, "B": e.BTN_B, "X": e.BTN_X, "Y": e.BTN_Y,
    "START": e.BTN_START, "BACK": e.BTN_SELECT, "LB": e.BTN_TL, "RB": e.BTN_TR,
    "LS": e.BTN_THUMBL, "RS": e.BTN_THUMBR,
}
DPAD = {"UP": (e.ABS_HAT0Y, -1), "DOWN": (e.ABS_HAT0Y, 1),
        "LEFT": (e.ABS_HAT0X, -1), "RIGHT": (e.ABS_HAT0X, 1)}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    hold = float(sys.argv[sys.argv.index("--hold") + 1]) if "--hold" in sys.argv else 0.3
    schedule = []
    for item in sys.argv[1].split(","):
        t, button = item.split(":")
        schedule.append((float(t), button.upper()))
    schedule.sort()

    stick = AbsInfo(value=0, min=-32768, max=32767, fuzz=16, flat=128, resolution=0)
    trigger = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
    hat = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)
    caps = {
        e.EV_KEY: list(BUTTONS.values()) + [e.BTN_MODE],
        e.EV_ABS: [(e.ABS_X, stick), (e.ABS_Y, stick), (e.ABS_RX, stick), (e.ABS_RY, stick),
                   (e.ABS_Z, trigger), (e.ABS_RZ, trigger), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)],
    }
    # Vendor/product of the wired Xbox 360 pad, so SDL applies its standard mapping.
    with UInput(caps, name="Microsoft X-Box 360 pad", vendor=0x045E, product=0x028E,
                version=0x110, bustype=e.BUS_USB) as pad:
        start = time.monotonic()
        for at, button in schedule:
            time.sleep(max(0.0, at - (time.monotonic() - start)))
            if button in DPAD:
                axis, value = DPAD[button]
                pad.write(e.EV_ABS, axis, value); pad.syn()
                time.sleep(hold)
                pad.write(e.EV_ABS, axis, 0); pad.syn()
            else:
                pad.write(e.EV_KEY, BUTTONS[button], 1); pad.syn()
                time.sleep(hold)
                pad.write(e.EV_KEY, BUTTONS[button], 0); pad.syn()
            print(f"{time.monotonic() - start:7.2f}s {button}", flush=True)
        time.sleep(3)


if __name__ == "__main__":
    main()
