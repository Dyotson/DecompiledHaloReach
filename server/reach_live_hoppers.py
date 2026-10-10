#!/usr/bin/env python3
"""Reach Live matchmaking playlists: builds the title-storage files the game downloads before
it shows Matchmaking (docs/online_plan.md section 5.4).

    python3 server/reach_live_hoppers.py DATA_DIR        # writes DATA_DIR/storage/...

The files are BLF files (`_blf`, data chunks, `_eof` with no authentication). The game
downloads them from /storage/title/4d53085b/tracked/<build>/default_hoppers/ and checks each
against the hash listed for it in manifest_001.bin (`onfm`), and each game set and variant
against the hash its parent lists. The hash is SHA-1 over a 34-byte salt followed by the
file; the salt is the start of the game executable's resource "00" and is not in this
repository: the operator copies it from their game into DATA_DIR/title_key.bin (see
docs/online_plan.md section 5.4).
"""

import hashlib
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from reach_live_lsp import blf, chunk  # noqa: E402

TITLE_ID = 0x4D53085B
BUILD = 11860
HOPPERS_DIR = "storage/title/%08x/tracked/%d/default_hoppers" % (TITLE_ID, BUILD)


SALT_SIZE = 0x22


def load_salt(path):
    with open(path, "rb") as f:
        salt = f.read(SALT_SIZE)
    if len(salt) != SALT_SIZE:
        raise ValueError("%s: need the %d-byte salt" % (path, SALT_SIZE))
    return salt


def file_hash(salt, data):
    """The game's hash of a downloaded file (Crypto_Sha1Buffer with its salt flag set)."""
    return hashlib.sha1(salt + data).digest()


def manifest(salt, files):
    """`onfm` v1: u32 count, then {path[0x50] relative to default_hoppers with a leading
    slash, hash of the file} per file. A file the manifest does not list is accepted as it
    is."""
    body = struct.pack(">I", len(files))
    for path, data in sorted(files.items()):
        name = ("/" + path.lstrip("/")).lower().encode()[:0x4F]
        body += name.ljust(0x50, b"\0") + file_hash(salt, data)
    return blf(chunk(b"onfm", 1, 1, body))


def compressed_chunk(fourcc, major, minor, struct_bytes):
    """A chunk whose payload is a bitstream holding a 14-bit byte count and that many bytes:
    the uncompressed size (u32 big-endian) and a zlib stream of the struct."""
    packed = struct.pack(">I", len(struct_bytes)) + zlib.compress(struct_bytes, 9)
    if len(packed) >= 1 << 14:
        raise ValueError("%s too large" % fourcc)
    bits = BitWriter()
    bits.write(len(packed), 14)
    for b in packed:
        bits.write(b, 8)
    return chunk(fourcc, major, minor, bits.data())


class BitWriter:
    """Most significant bit first, as the game's bitstream reads."""

    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def write(self, value, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                self.acc, self.n = 0, 0

    def data(self):
        if self.n:
            return bytes(self.out) + bytes([self.acc << (8 - self.n)])
        return bytes(self.out)


TABLE_SIZE = 0x8F48     # decoded hopper configuration table (s_hopper_configuration_table)
CATEGORY_SIZE = 0x44
HOPPER_SIZE = 0x458
CATEGORIES_AT = 0x08
HOPPERS_AT = 0x448


def hopper_table(categories, hoppers):
    """The decoded `mhcf` table: u32 hopper count, u32 category count, 16 categories
    {u16 id, char name[32], u16 image, ...} and 32 hoppers {char name[32], game set hash
    at +0x20, u16 id at +0x34, u16 category at +0x36, i32 min/max party at +0x70, ...}."""
    t = bytearray(TABLE_SIZE)
    struct.pack_into(">II", t, 0, len(hoppers), len(categories))
    for i, c in enumerate(categories):
        o = CATEGORIES_AT + i * CATEGORY_SIZE
        name = c["name"].encode()[:31]
        struct.pack_into(">H", t, o, c["id"])
        t[o + 2:o + 2 + len(name)] = name
        struct.pack_into(">H", t, o + 0x22, c.get("image", 0))
    for i, h in enumerate(hoppers):
        o = HOPPERS_AT + i * HOPPER_SIZE
        name = h["name"].encode()[:31]
        t[o:o + len(name)] = name
        t[o + 0x20:o + 0x34] = h.get("game_set_hash", bytes(20))
        struct.pack_into(">HH", t, o + 0x34, h["id"], h["category"])
        struct.pack_into(">ii", t, o + 0x70, h.get("min_party", 1), h.get("max_party", 16))
    return bytes(t)


def hopper_descriptions(hoppers):
    """`mhdf` v3, a bitstream: hopper count - 1 (6 bits), then per hopper its ID (16 bits),
    a flag (1 bit) and a NUL-terminated description (8 bits a character, 256 at most)."""
    bits = BitWriter()
    bits.write(len(hoppers) - 1, 6)
    for h in hoppers:
        bits.write(h["id"], 16)
        bits.write(0, 1)
        for b in h.get("description", "").encode()[:0xFF] + b"\0":
            bits.write(b, 8)
    return blf(chunk(b"mhdf", 3, 1, bits.data()))


MAP_SIGNATURE_AT = 0x36C    # RSA signature in a .map file's header
MAP_SIGNATURE_SIZE = 0x100
MAP_MANIFEST_SIZE = 0x8004  # `mapm`: u32 count, 128 signatures
DLC_MANIFEST_SIZE = 0xF504  # `dlcd`


def map_signatures(maps_dir):
    """The header signatures of the operator's .map files: a map whose signature the RSA
    manifest does not list marks the console as running modified content."""
    signatures = []
    for name in sorted(os.listdir(maps_dir)):
        if name.endswith(".map"):
            with open(os.path.join(maps_dir, name), "rb") as f:
                f.seek(MAP_SIGNATURE_AT)
                signatures.append(f.read(MAP_SIGNATURE_SIZE))
    return signatures[:(MAP_MANIFEST_SIZE - 4) // MAP_SIGNATURE_SIZE]


def map_manifest(signatures):
    body = struct.pack(">I", len(signatures)) + b"".join(signatures)
    return blf(chunk(b"mapm", 1, 1, body.ljust(MAP_MANIFEST_SIZE, b"\0")))


def build(salt, signatures):
    """{path relative to default_hoppers: file bytes}"""
    files = {}
    categories = [{"id": 1, "name": "Reach Live"}]
    hoppers = [
        {"id": 101, "category": 1, "name": "Team Slayer", "description": "Two teams, kills win."},
        {"id": 102, "category": 1, "name": "Free For All", "max_party": 1,
         "description": "Every Spartan for themselves."},
    ]
    files["matchmaking_hopper_027.bin"] = blf(
        compressed_chunk(b"mhcf", 27, 1, hopper_table(categories, hoppers)))
    files["en/matchmaking_hopper_descriptions_003.bin"] = hopper_descriptions(hoppers)
    files["en/rsa_manifest.bin"] = map_manifest(signatures)
    files["dlc_map_manifest.bin"] = blf(chunk(b"dlcd", 1, 1, bytes(DLC_MANIFEST_SIZE)))
    return files


def write(data_dir, salt, signatures):
    files = build(salt, signatures)
    root = os.path.join(data_dir, "storage", HOPPERS_DIR)
    files["manifest_001.bin"] = manifest(salt, {k: v for k, v in files.items()})
    for path, data in files.items():
        full = os.path.join(root, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
    return sorted(files)


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Write the Reach Live matchmaking files.")
    parser.add_argument("data_dir", nargs="?", default="reach_live_data")
    parser.add_argument("--maps", required=True,
                        help="the game's maps directory (for the map signatures)")
    args = parser.parse_args()
    key = os.path.join(args.data_dir, "title_key.bin")
    if not os.path.isfile(key):
        sys.exit("%s is missing: copy the 34-byte salt from a running client, e.g.\n"
                 "  python3 tools/guestmem.py dump PID 0x83AB0000 34 %s" % (key, key))
    salt = load_salt(key)
    for name in write(args.data_dir, salt, map_signatures(args.maps)):
        print(name)
