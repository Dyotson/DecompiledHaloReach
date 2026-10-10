#!/usr/bin/env python3
"""Reach Live matchmaking playlists: builds the title-storage files the game downloads before
it shows Matchmaking (docs/online_plan.md section 5.4).

    python3 server/reach_live_hoppers.py DATA_DIR --from-client PID   # once, see below
    python3 server/reach_live_hoppers.py DATA_DIR --maps GAME/maps     # writes DATA_DIR/storage/...

The files are BLF files (`_blf`, data chunks, `_eof` with no authentication). The game
downloads them from /storage/title/4d53085b/tracked/<build>/default_hoppers/ and checks each
against the hash listed for it in manifest_001.bin (`onfm`), and each game set and variant
against the hash its parent lists. The hash is SHA-1 over a 34-byte salt followed by the
file. Two inputs come from the operator's game and are not in this repository: the salt
(the start of the executable's resource "00") and the game's built-in network configuration,
which the server must serve back (the lobby needs the file). `--from-client PID` copies both
from a running client on this machine (Linux, /proc/PID/mem) into DATA_DIR/title_key.bin and
DATA_DIR/network_configuration.bin; `--maps` reads the map signatures from the operator's
.map files. docs/online_plan.md section 5.4 has the formats.
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
SALT_ADDRESS = 0x83AB0000       # resource "00" in the loaded executable
NETWORK_CONFIGURATION_ADDRESS = 0x82BD28A8
NETWORK_CONFIGURATION_SIZE = 0x2254
GUEST_BASE = 0x100000000         # where the client maps guest memory


def from_client(pid, data_dir):
    with open("/proc/%d/mem" % pid, "rb", buffering=0) as mem:
        for name, address, size in (("title_key.bin", SALT_ADDRESS, SALT_SIZE),
                                    ("network_configuration.bin", NETWORK_CONFIGURATION_ADDRESS,
                                     NETWORK_CONFIGURATION_SIZE)):
            mem.seek(GUEST_BASE + address)
            data = mem.read(size)
            os.makedirs(data_dir, exist_ok=True)
            with open(os.path.join(data_dir, name), "wb") as f:
                f.write(data)
            print(os.path.join(data_dir, name))


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
        # Player requirements (sub_82290EA8): none. Experience, games played and rank
        # ranges, access bit -1 = none, account type 2 = any.
        struct.pack_into(">iiiiii", t, o + 0x58, 0, 0, 0, 0, -128, 127)
        struct.pack_into(">ii", t, o + 0x70, h.get("min_party", 1), h.get("max_party", 16))
        struct.pack_into(">iiii", t, o + 0x78, -0x80000000, 0x7FFFFFFF, -1, 2)
        # Voting: options per vote (1-3), +0x9C (0-4), rounds (1-8), +0xA4 (0-8), +0xA8
        # (0-4), +0xAC (1-30), +0xB0 (3 bits). The host checks them (sub_82291260) and
        # sends them to the other players in the "matchmaking-hopper" session parameter,
        # whose decoder (sub_822D9508) drops the whole update for a value out of range.
        struct.pack_into(">iiiiii", t, o + 0x98, 1, 0, 1, 0, 0, 10)
        t[o + 0xB6] = 1
        # Four search stages of 0x94 bytes at +0x134 (sub_822D0760). The stage's ping
        # limit is base + step * increment (+0x28, +0x2C; sub_822D0D68), capped by the
        # network configuration (100 ms); 0 rejects every session found.
        for stage in range(4):
            struct.pack_into(">ii", t, o + 0x134 + stage * 0x94 + 0x28, h.get("max_ping", 200), 0)
        t[o + 0x384] = h["variant_source"]
        # No teams: a match needs +0x388 to +0x38C players (sub_82286418, sub_8227C040).
        # With a minimum of 1 a lone player's match starts at once.
        t[o + 0x385] = 1
        struct.pack_into(">ii", t, o + 0x388, h.get("min_players", 2), h.get("max_players", 8))
    return bytes(t)


# Hopper +0x384, where a game's variants come from: 0 a game and a map variant file,
# 1 or 2 a game variant file only, 3 none. A multiplayer game only starts with both
# (sub_82280140 needs the map variant parameter), and with none the first game of a
# session crashes reading a null game variant.
GAME_AND_MAP_VARIANT = 0

GAME_SET_SIZE = 0xD404
GAME_SET_ENTRY = 0xD4


def game_set(entries, variant_source):
    """The decoded `gset` structure: u32 count, then 0xD4-byte entries (sub_822916A0,
    sub_82291260): +0 i32 weight, +4 / +8 i32 min / max players, +0x14 i32 (at most 1),
    +0x18 i32 (at least 50), +0x44 i32 map id, +0x48 game variant {u8 used, +0x11 char
    name[32] (file `<hopper>/<name>_054.bin`), +0x31 hash}, +0x8D map variant (the same,
    `<hopper>/map_variants/<name>_031.bin`)."""
    t = bytearray(GAME_SET_SIZE)
    struct.pack_into(">I", t, 0, len(entries))
    for i, e in enumerate(entries):
        o = 4 + i * GAME_SET_ENTRY
        struct.pack_into(">iii", t, o, e.get("weight", 1), e.get("min_players", 1),
                         e.get("max_players", 16))
        struct.pack_into(">ii", t, o + 0x14, 0, 100)
        struct.pack_into(">i", t, o + 0x44, e["map"])
        for at, key, used in ((0x48, "game_variant", variant_source in (0, 1, 2)),
                              (0x8D, "map_variant", variant_source == 0)):
            t[o + at] = used
            if used:
                name, digest = e[key]
                t[o + at + 0x11:o + at + 0x11 + len(name)] = name.encode()[:31]
                t[o + at + 0x31:o + at + 0x45] = digest
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


MAPS = {"Sword Base": 1000, "Zealot": 1020, "Boardwalk": 1035, "Powerhouse": 1040,
        "Countdown": 1055, "Spire": 1080, "Reflection": 1150, "Boneyard": 1200,
        "Forge World": 3006}

# DATA_DIR/playlists.json replaces this. Game and map variants are named after files on the
# server's File Share (save a game type or map in game, then "Upload to File Share").
DEFAULT_PLAYLISTS = {
    "categories": [{"id": 1, "name": "Reach Live"}],
    "playlists": [
        {"id": 101, "category": 1, "name": "Free For All",
         "description": "Every Spartan for themselves.", "max_party": 8, "max_players": 8,
         "games": [{"game_variant": "Slayer", "map": "Sword Base"},
                   {"game_variant": "Slayer", "map": "Zealot"},
                   {"game_variant": "Slayer", "map": "Powerhouse"}]},
    ],
}
# A game's map variant defaults to the File Share file named after its map (a map saved
# unchanged from Forge). Games whose variants are not on the File Share are left out.


def shared_variants(data_dir):
    """{(file type, lowercase name): chunk payload} of the game variants (`mpvr`, type 6)
    and map variants (`mvar`, type 5) on the server's File Share."""
    import reach_live_files as share
    out = {}
    root = os.path.join(data_dir, "fileshare")
    for owner in sorted(os.listdir(root)) if os.path.isdir(root) else []:
        folder = os.path.join(root, owner)
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".bin"):
                continue
            with open(os.path.join(folder, name), "rb") as f:
                chunks = share.parse_chunks(f.read())
            header = next((p for c, _, _, p in chunks if c == b"chdr"), None)
            if header is None:
                continue
            title = header[4 + 0x80:4 + 0x180].decode("utf-16-be", "ignore").split("\0")[0]
            for fourcc, major, _, payload in chunks:
                if (fourcc, major) == (b"mpvr", 54):
                    out.setdefault((6, title.lower()), payload)
                elif (fourcc, major) == (b"mvar", 31):
                    out.setdefault((5, title.lower()), payload)
    return out


KIND = {"game_variant": 6, "map_variant": 5}  # File Share file types


def game_variant_file(mpvr):
    """`gvar` v54 holds the bitstream that a saved game variant's `mpvr` v54 chunk holds
    after its hash (+0), padding and u32 size (+0x18)."""
    (size,) = struct.unpack_from(">I", mpvr, 0x18)
    return blf(chunk(b"gvar", 54, 1, mpvr[0x1C:0x1C + size]))


def file_name(title):
    return "".join(c if c.isalnum() else "_" for c in title.lower())[:24]


def build(salt, signatures, network_configuration, playlists, variants):
    """{path relative to default_hoppers: file bytes}"""
    files = {}
    # `netc` v241, raw. The lobby reports the server unavailable while it is missing.
    files["network_configuration_241.bin"] = blf(chunk(b"netc", 241, 1, network_configuration))
    hoppers = [dict(h) for h in playlists["playlists"]]
    for h in hoppers:
        h["variant_source"] = GAME_AND_MAP_VARIANT
        entries = []
        for g in h["games"]:
            names = {"game_variant": g["game_variant"], "map_variant": g.get("map_variant", g["map"])}
            missing = [(key, name) for key, name in names.items()
                       if (KIND[key], name.lower()) not in variants]
            if missing:
                print("playlist %r: left out %s on %s, no %s on the File Share" % (
                    h["name"], g["game_variant"], g["map"],
                    " or ".join("%s named %r" % (k.replace("_", " "), n) for k, n in missing)),
                    file=sys.stderr)
                continue
            entry = {"map": MAPS.get(g["map"], g["map"]), "weight": g.get("weight", 1)}
            for key, folder, version in (("game_variant", "", 54), ("map_variant", "map_variants/", 31)):
                payload = variants[(KIND[key], names[key].lower())]
                data = (game_variant_file(payload) if key == "game_variant"
                        else blf(chunk(b"mvar", 31, 1, payload)))
                name = file_name(names[key])
                files["%05u/%s%s_%03u.bin" % (h["id"], folder, name, version)] = data
                entry[key] = (name, file_hash(salt, data))
            entries.append(entry)
        if not entries:
            raise SystemExit("playlist %r: none of its games has its variants on the File Share"
                             % h["name"])
        data = blf(compressed_chunk(b"gset", 15, 1, game_set(entries, h["variant_source"])))
        files["%05u/game_set_015.bin" % h["id"]] = data
        h["game_set_hash"] = file_hash(salt, data)
    files["matchmaking_hopper_027.bin"] = blf(
        compressed_chunk(b"mhcf", 27, 1, hopper_table(playlists["categories"], hoppers)))
    files["en/matchmaking_hopper_descriptions_003.bin"] = hopper_descriptions(hoppers)
    files["en/rsa_manifest.bin"] = map_manifest(signatures)
    files["dlc_map_manifest.bin"] = blf(chunk(b"dlcd", 1, 1, bytes(DLC_MANIFEST_SIZE)))
    return files


def write(data_dir, salt, signatures, network_configuration):
    playlists = DEFAULT_PLAYLISTS
    config = os.path.join(data_dir, "playlists.json")
    if os.path.isfile(config):
        import json
        with open(config) as f:
            playlists = json.load(f)
    files = build(salt, signatures, network_configuration, playlists, shared_variants(data_dir))
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
    parser.add_argument("--maps", help="the game's maps directory (for the map signatures)")
    parser.add_argument("--from-client", type=int, metavar="PID",
                        help="copy the salt and network configuration from a running client")
    args = parser.parse_args()
    if args.from_client:
        from_client(args.from_client, args.data_dir)
        sys.exit(0)
    inputs = [os.path.join(args.data_dir, n) for n in ("title_key.bin", "network_configuration.bin")]
    missing = [p for p in inputs if not os.path.isfile(p)]
    if missing or not args.maps:
        sys.exit("need %s and --maps: run once with --from-client PID (a running client)"
                 % " and ".join(missing or inputs))
    salt = load_salt(inputs[0])
    with open(inputs[1], "rb") as f:
        network_configuration = f.read()
    for name in write(args.data_dir, salt, map_signatures(args.maps), network_configuration):
        print(name)
