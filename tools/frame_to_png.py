#!/usr/bin/env python3
"""Convert a REACH_FRAMEDUMP front-buffer dump (.bin + .json) to PNG.

Usage: frame_to_png.py DUMP.bin [OUT.png] [--gain N]

Detiles Xenos 2D-tiled 32bpp surfaces (k_8_8_8_8 / k_2_10_10_10, read as 8-bit
channels) and writes an RGB PNG. --gain multiplies channel values, which helps
when checking whether an almost-black frame has any structure. Needs Pillow.
"""
import json
import sys

from PIL import Image


def tiled_offset_2d(x, y, width, log2_bpp):
    """Texel index of (x, y) in a Xenos 2D tiled surface (Xenia's TiledOffset2D)."""
    aligned_width = (width + 31) & ~31
    macro = ((y >> 5) * (aligned_width >> 5) + (x >> 5)) << (log2_bpp + 7)
    micro = (((y & 6) << 2) + (x & 7)) << log2_bpp
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4)
    offset = (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F)
              + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))
    return offset >> log2_bpp


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    gain = 1.0
    if "--gain" in sys.argv:
        gain = float(sys.argv[sys.argv.index("--gain") + 1])
        args = [a for a in args if a != sys.argv[sys.argv.index("--gain") + 1]]
    src = args[0]
    out = args[1] if len(args) > 1 else src.rsplit(".", 1)[0] + ".png"
    meta = json.load(open(src.rsplit(".", 1)[0] + ".json"))
    data = open(src, "rb").read()
    w, h, aw = meta["width"], meta["height"], meta["aligned_width"]
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            i = tiled_offset_2d(x, y, aw, 2) if meta["tiled"] else y * aw + x
            o = i * 4
            # k_8_8_8_8 front buffers hold B,G,R,A bytes (checked against a
            # screenshot of the intro video).
            b, g, r = data[o], data[o + 1], data[o + 2]
            px[x, y] = (min(255, int(r * gain)), min(255, int(g * gain)), min(255, int(b * gain)))
    img.save(out)
    print(out, f"{w}x{h}", "tiled" if meta["tiled"] else "linear")


if __name__ == "__main__":
    main()
