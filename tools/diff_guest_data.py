#!/usr/bin/env python3
"""Diff two snapshots of the same guest memory range (e.g. Xenia vs recomp).

Usage: diff_guest_data.py BASE_HEX A.bin B.bin [--all] [--symbols CSV] [--ghidra]

Prints big-endian dwords that differ. By default only "flag-like" differences
are kept: both values <= 0xFF, or a byte that is 0/1 in one and different in
the other. Those are the engine switches and enums worth chasing; pointers,
timers and counters are filtered out. --all prints every differing dword.
--ghidra annotates each address with the containing data symbol from the
headless Ghidra MCP server (http://127.0.0.1:8089).
"""
import json
import struct
import sys
import urllib.request


def ghidra_label(addr):
    try:
        url = (f"http://127.0.0.1:8089/get_xrefs_to?address=0x{addr:08X}"
               f"&program=/xbox360/default.xex&limit=5")
        refs = json.load(urllib.request.urlopen(url, timeout=5)).get("references", [])
        funcs = sorted({r.get("from_function") or "?" for r in refs})
        return f"refs:{len(refs)} {','.join(funcs[:3])}"
    except Exception:
        return ""


def main():
    base = int(sys.argv[1], 16)
    a = open(sys.argv[2], "rb").read()
    b = open(sys.argv[3], "rb").read()
    show_all = "--all" in sys.argv
    annotate = "--ghidra" in sys.argv
    n = min(len(a), len(b)) // 4
    shown = total = 0
    for i in range(n):
        va, vb = struct.unpack_from(">I", a, i * 4)[0], struct.unpack_from(">I", b, i * 4)[0]
        if va == vb:
            continue
        total += 1
        flag_like = (va <= 0xFF and vb <= 0xFF)
        if not flag_like:
            # A single differing byte that is boolean in one snapshot.
            for k in range(4):
                xa, xb = a[i * 4 + k], b[i * 4 + k]
                if xa != xb and (xa in (0, 1) or xb in (0, 1)) and max(xa, xb) <= 1:
                    flag_like = True
        if not (show_all or flag_like):
            continue
        shown += 1
        addr = base + i * 4
        extra = ghidra_label(addr) if annotate else ""
        print(f"0x{addr:08X}  A={va:08X}  B={vb:08X}  {extra}")
    print(f"# {total} differing dwords, {shown} shown", file=sys.stderr)


if __name__ == "__main__":
    main()
