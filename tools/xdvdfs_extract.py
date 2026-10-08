#!/usr/bin/env python3
"""Read-only XDVDFS (Xbox 360 game partition) lister/extractor.

Usage:
  xdvdfs_extract.py ISO list
  xdvdfs_extract.py ISO extract OUTDIR [PATH ...]   (no PATH = everything)
"""
import os
import struct
import sys

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0x0, 0x2080000, 0xFD90000, 0x18300000)


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return base
    raise SystemExit("XDVDFS partition not found")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        path = f"{prefix}/{name}" if prefix else name
        if attr & 0x10:
            if length:
                yield from walk(f, base, start, length, path)
        else:
            yield path, base + start * SECTOR, length
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + len(MAGIC))
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = sorted(walk(f, base, root_sector, root_size))
        if cmd == "list":
            print(f"partition offset: {base:#x}")
            for path, _, length in entries:
                print(f"{length:>12}  {path}")
            return
        outdir, wanted = sys.argv[3], set(sys.argv[4:])
        for path, offset, length in entries:
            if wanted and path not in wanted:
                continue
            dest = os.path.join(outdir, path)
            if os.path.exists(dest):
                print(f"skip (exists): {dest}")
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(offset)
            remaining = length
            with open(dest, "wb") as out:
                while remaining:
                    chunk = f.read(min(remaining, 1 << 24))
                    out.write(chunk)
                    remaining -= len(chunk)
            print(f"extracted {path} ({length} bytes)")


if __name__ == "__main__":
    main()
