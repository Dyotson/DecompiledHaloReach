#!/usr/bin/env python3
"""Show the Credits/Armory state saved in a Halo: Reach profile (the recompiled build's
XPROFILE_TITLE_SPECIFIC1-3 files, or any 0xAD0-byte title blob).

Usage: reach_profile.py [PROFILE_DIR]
  PROFILE_DIR defaults to ~/.local/share/reach/4D53085B/profile/User, where the ReXGlue
  runtime keeps 63E83FFF, 63E83FFE and 63E83FFD (1000 + 1000 + 768 bytes).

Layout (docs/progression_re.md 1.2): u32 version 0x27; rewards block at +0x1A0 (0x548
bytes: current totals A, online delta B, offline delta C, purchase log, xuid, hashes,
flags); SHA-1 of the whole blob at +0xAB8, computed with that field set to 0x99 bytes.
"""
import hashlib
import os
import struct
import sys

SETTINGS = ("63E83FFF", "63E83FFE", "63E83FFD")


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        "~/.local/share/reach/4D53085B/profile/User")
    blob = b"".join(open(os.path.join(directory, name), "rb").read() for name in SETTINGS)
    print(f"blob: {len(blob):#x} bytes, version {struct.unpack_from('>I', blob)[0]:#x}")
    scratch = bytearray(blob)
    scratch[0xAB8:0xAB8 + 20] = b"\x99" * 20
    print("sha-1:", "ok" if hashlib.sha1(bytes(scratch)).digest() == blob[0xAB8:0xAB8 + 20] else "MISMATCH")

    r = blob[0x1A0:0x1A0 + 0x548]
    for name, off in (("A current", 0x000), ("B online delta", 0x208)):
        cookies, count = struct.unpack_from(">ii", r, off)
        flags = r[off + 0x108:off + 0x108 + 256]
        owned = [i for i, f in enumerate(flags) if f & 1]
        print(f"{name:15s} cR={cookies:8d} awards={count:5d} owned items={len(owned)} {owned[:24]}")
    cookies, count = struct.unpack_from(">ii", r, 0x410)
    print(f"{'C offline delta':15s} cR={cookies:8d} awards={count:5d}")
    xuid, = struct.unpack_from(">Q", r, 0x520)
    flags, = struct.unpack_from(">I", r, 0x540)
    print(f"xuid {xuid:#018x}  flags {flags:#x} (bit0 synced with server, bit1 block valid)")
    print("lifetime cR drives rank; balance = lifetime - 5000 (until the initial grant is shown)"
          " - Armory spending")


if __name__ == "__main__":
    main()
