#!/usr/bin/env python3
"""Read or search the guest memory of a running `reach` process.

The guest's 4 GB address space is mapped at host 0x100000000 (guest address X
lives at host 0x100000000 + X). Reads go through /proc/PID/mem, so the game
keeps running.

Usage:
  guestmem.py search PID HEXBYTES [--max N]   e.g. 000000000000000000000000
  guestmem.py read PID GUEST_ADDR LENGTH       hex dump (big-endian dwords)
"""
import sys

ARENA = 0x100000000
ARENA_END = 0x200000000


def regions(pid):
    for line in open(f"/proc/{pid}/maps"):
        span, perms = line.split()[:2]
        lo, hi = (int(v, 16) for v in span.split("-"))
        if "r" in perms and hi > ARENA and lo < ARENA_END:
            yield max(lo, ARENA), min(hi, ARENA_END)


def search(pid, needle, limit):
    hits = []
    with open(f"/proc/{pid}/mem", "rb", buffering=0) as mem:
        for lo, hi in regions(pid):
            pos = lo
            while pos < hi and len(hits) < limit:
                size = min(16 << 20, hi - pos)
                try:
                    mem.seek(pos)
                    chunk = mem.read(size + len(needle) - 1)
                except OSError:
                    pos += size
                    continue
                start = 0
                while True:
                    i = chunk.find(needle, start)
                    if i < 0 or i >= size:
                        break
                    hits.append(pos + i - ARENA)
                    start = i + 4
                pos += size
    return hits


def read(pid, addr, length):
    with open(f"/proc/{pid}/mem", "rb", buffering=0) as mem:
        mem.seek(ARENA + addr)
        return mem.read(length)


def main():
    cmd, pid = sys.argv[1], int(sys.argv[2])
    if cmd == "search":
        needle = bytes.fromhex(sys.argv[3])
        limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 64
        for hit in search(pid, needle, limit):
            print(f"0x{hit:08X}")
    elif cmd == "read":
        addr, length = int(sys.argv[3], 16), int(sys.argv[4], 0)
        data = read(pid, addr, length)
        for off in range(0, len(data), 16):
            words = " ".join(data[off + i:off + i + 4].hex() for i in range(0, 16, 4))
            print(f"0x{addr + off:08X}: {words}")


if __name__ == "__main__":
    main()
