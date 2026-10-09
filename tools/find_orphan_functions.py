#!/usr/bin/env python3
"""Find guest functions that codegen never compiled because nothing static reaches them.

Some functions are only reached through pointers the game assembles at run time (for
example the per-type callback table that sub_82874A20 indexes), so neither codegen nor
find_missing_entries.py (which looks for pointers in data and lis/addi pairs) sees them,
and calling one dies with "Call to invalid or unregistered function". This scans .text
for code that no generated function covers:

1. Each generated function covers [start, start + 4 * instructions), where instructions
   is the number of `// <insn>` comments in its body. Functions with an embedded jump
   table make this estimate end too early; step 3 compensates.
2. Uncovered runs are split into blocks at blr / bctr / b.
3. A block is kept when no static branch targets it, its address does not appear as a
   word in .text (jump-table case blocks), it does not start with a pointer-like word,
   and it ends in blr / bctr or a tail call to a known function.

Writes a [functions] hint table. Then build: codegen reports "Unresolved conditional
branch" for a listed function whose code continues past an early return; give that entry
`end = <address after its last block>` (or drop it), and run prune_split_hints.py on the
log for entries that split another function. Verify that no warning comes from a function
that existed before.

Usage: find_orphan_functions.py GENERATED_DIR OUT_TOML [--program /xbox360/default.xex]
Needs the GhidraMCP server on 127.0.0.1:8089 (memory is read from Ghidra).
"""
import argparse
import glob
import importlib.util
import os
import re
import struct

BLR, BCTR = 0x4E800020, 0x4E800420


def load_reader():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "find_missing_entries.py")
    spec = importlib.util.spec_from_file_location("find_missing_entries", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def coverage(generated_dir):
    fn_re = re.compile(r"^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)")
    ins_re = re.compile(r"^\t// [a-z]")
    cover = {}
    for path in glob.glob(os.path.join(generated_dir, "*_recomp.*.cpp")):
        cur, n = None, 0
        for line in open(path, errors="replace"):
            m = fn_re.match(line)
            if m:
                if cur is not None:
                    cover[cur] = n
                cur, n = int(m.group(1), 16), 0
            elif cur is not None and ins_re.match(line):
                n += 1
        if cur is not None:
            cover[cur] = n
    return cover


def branch_target(word, addr):
    op = word >> 26
    if op == 18:
        off = word & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
    elif op == 16:
        off = word & 0xFFFC
        if off & 0x8000:
            off -= 0x10000
    else:
        return None
    return (off if word & 2 else addr + off) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("generated_dir")
    ap.add_argument("out_toml")
    ap.add_argument("--program", default="/xbox360/default.xex")
    a = ap.parse_args()

    reader = load_reader()
    text = next(s for s in reader.segments(a.program) if s["name"] == ".text")
    start, end = int(text["start"], 16), int(text["end"], 16) + 1
    data = reader.read(a.program, start, end - start)
    words = struct.unpack(f">{len(data) // 4}I", data[: len(data) // 4 * 4])

    def word(addr):
        return words[(addr - start) // 4]

    cover = coverage(a.generated_dir)
    known = sorted(f for f in cover if start <= f < end)
    known_set = set(known)
    targets = {t for i, w in enumerate(words) if (t := branch_target(w, start + 4 * i)) is not None}
    text_words = set(words)

    def is_term(w):
        return w in (BLR, BCTR) or ((w >> 26) == 18 and not w & 1)

    def ptr_like(w):
        return 0x82000000 <= w < 0x84000000

    found = []
    for i, f in enumerate(known):
        addr, limit = f + 4 * cover[f], known[i + 1] if i + 1 < len(known) else end
        while addr < limit:
            if word(addr) == 0:
                addr += 4
                continue
            block = addr
            while addr < limit and word(addr) and not is_term(word(addr)):
                addr += 4
            if addr >= limit or not word(addr):
                continue
            last, addr = word(addr), addr + 4
            if any(b in targets for b in range(block, addr, 4)) or block in text_words:
                continue
            if ptr_like(word(block)) or block in known_set:
                continue
            if last not in (BLR, BCTR) and branch_target(last, addr - 4) not in known_set:
                continue
            found.append(block)

    with open(a.out_toml, "w") as out:
        out.write("[functions]\n")
        for f in found:
            out.write(f'0x{f:08X} = {{ name = "sub_{f:08X}" }}\n')
    print(f"functions={len(known)} orphans={len(found)} -> {a.out_toml}")


if __name__ == "__main__":
    main()
