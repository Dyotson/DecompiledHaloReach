#!/usr/bin/env python3
"""Drop scanner hints that split a real function.

Reads `Unresolved ... target 0xT from 0xS` lines from a codegen log; any hint
address strictly inside [min(S,T), max(S,T)] cut a function in two, so it is
commented out in the given hint files.

Usage: prune_split_hints.py CODEGEN_LOG HINT_TOML [HINT_TOML ...]
"""
import re
import sys

log = open(sys.argv[1]).read()
pairs = {(int(t, 16), int(s, 16)) for t, s in re.findall(
    r"Unresolved (?:b target|conditional branch to) (0x[0-9A-F]+) from (0x[0-9A-F]+)", log)}

for path in sys.argv[2:]:
    lines = open(path).read().splitlines()
    out, dropped = [], 0
    for line in lines:
        m = re.match(r"(0x[0-9A-F]{8}) = \{ name = \"sub_", line)
        if m:
            h = int(m.group(1), 16)
            if any(min(t, s) < h <= max(t, s) for t, s in pairs):
                out.append("# split-pruned: " + line)
                dropped += 1
                continue
        out.append(line)
    open(path, "w").write("\n".join(out) + "\n")
    print(f"{path}: pruned {dropped}")
