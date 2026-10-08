#!/usr/bin/env python3
"""List texture/vertex fetch slots used by shaders in a Xenia/ReXGlue .xsh store.

Usage: xsh_fetches.py STORE.xsh [HASH ...]

Parses Xenos microcode control flow (exec blocks with their fetch/ALU
serialization bits) and reports, per shader, the texture fetch constant
indices (tfetch const_index) and vertex fetch constants it uses. With HASH
arguments (0x-prefixed), only those shaders are listed.
"""
import struct
import sys

EXEC_OPCODES = {1, 2, 3, 4, 5, 6, 13, 14}  # exec / exec_end / cond_exec* variants
TFETCH_NAMES = {1: "tfetch", 16: "getBCF", 17: "getCompLOD", 18: "getGradients",
                19: "getWeights", 24: "setLOD", 25: "setGradH", 26: "setGradV"}


def parse_store(path):
    data = open(path, "rb").read()
    if data[:4] != b"XESH":
        raise SystemExit(f"{path}: not an XESH shader store")
    pos, shaders = 8, {}
    while pos + 12 <= len(data):
        ucode_hash, count = struct.unpack_from("<QI", data, pos)
        n = count & 0x7FFFFFFF
        if pos + 12 + n * 4 > len(data):
            break  # truncated trailing entry (store written by a killed process)
        words = struct.unpack_from(f">{n}I", data, pos + 12)
        shaders[ucode_hash] = ("ps" if count >> 31 else "vs", words)
        pos += 12 + n * 4
    return shaders


def control_flow(words):
    """Yield (opcode, address, count, serialize) for CF instructions."""
    first_exec = None
    i = 0
    while i + 2 < len(words):
        d0, d1, d2 = words[i], words[i + 1], words[i + 2]
        for w0, w1 in ((d0, d1 & 0xFFFF), ((d1 >> 16) | ((d2 & 0xFFFF) << 16), d2 >> 16)):
            opcode = (w1 >> 12) & 0xF
            if opcode in EXEC_OPCODES:
                address, count, serialize = w0 & 0xFFF, (w0 >> 12) & 7, (w0 >> 16) & 0xFFF
                if first_exec is None or address < first_exec:
                    first_exec = address
                yield opcode, address, count, serialize
        i += 3
        if first_exec is not None and i // 3 >= first_exec:
            break


def fetches(words):
    tex, vtx = set(), set()
    for _, address, count, serialize in control_flow(words):
        for k in range(count):
            if not (serialize >> (2 * k)) & 1:
                continue  # ALU instruction
            base = (address + k) * 3
            if base >= len(words):
                continue
            w0 = words[base]
            op = w0 & 0x1F
            if op == 0:
                vtx.add((w0 >> 20) & 0x1F)
            elif op in TFETCH_NAMES:
                tex.add((TFETCH_NAMES[op], (w0 >> 20) & 0x1F))
    return tex, vtx


def main():
    shaders = parse_store(sys.argv[1])
    wanted = {int(h, 16) for h in sys.argv[2:]}
    for ucode_hash, (kind, words) in shaders.items():
        if wanted and ucode_hash not in wanted:
            continue
        tex, vtx = fetches(words)
        tex_list = ", ".join(f"{n}[{c}]" for n, c in sorted(tex, key=lambda t: t[1]))
        print(f"0x{ucode_hash:016x} {kind} {len(words):4d} dw  textures: {tex_list or '-'}"
              f"  vfetch: {sorted(vtx) or '-'}")


if __name__ == "__main__":
    main()
