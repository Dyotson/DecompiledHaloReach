#!/usr/bin/env python3
"""Tests for reach_live_lsp.py with synthetic uploads (no game data).

    python3 server/test_reach_live_lsp.py
"""

import os
import struct
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reach_live_lsp as lsp  # noqa: E402

BOUNDARY = b"--BUNGIEr0x0rz"


def multipart(data, kind="reward-sync"):
    return (BOUNDARY + b'\r\nContent-Disposition: form-data; name="upload"; filename="blob"\r\n'
            b"Content-Type: application/x-reach-" + kind.encode() + b"\r\n\r\n" + data +
            b"\r\n" + BOUNDARY + b"--\r\n")


def rpul(cookies, owned=(), gamertag=b"Noble Six"):
    payload = bytearray(lsp.RPUL_SIZE)
    struct.pack_into(">ii", payload, 0, cookies, 1)
    for item in owned:
        payload[0x108 + item] = 1
    payload[0x735:0x735 + len(gamertag)] = gamertag
    return lsp.blf(lsp.chunk(b"rpul", 3, 1, bytes(payload)),
                   lsp.chunk(b"chpr", 2, 1, bytes(88)))


class LspTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        lsp.configure(types.SimpleNamespace(data_dir=self.dir.name, dump_requests=False))

    def tearDown(self):
        self.dir.cleanup()

    def post(self, path, data, kind="reward-sync"):
        return lsp.handle(None, "POST", path, {}, multipart(data, kind), ("127.0.0.1", 1))

    def rpdl(self, reply):
        status, _, body = reply
        self.assertEqual(status, 200)
        found = lsp.chunks(body)
        self.assertIn(b"_eof", found)
        major, _, payload = found[b"rpdl"]
        self.assertEqual((major, len(payload) + 12), (2, 0x227))  # the game insists on 0x227
        return payload

    def test_blf_layout(self):
        body = lsp.blf(lsp.chunk(b"test", 1, 1, b"abcd"))
        self.assertEqual(body[:4], b"_blf")
        self.assertEqual(struct.unpack_from(">IHHH", body, 4), (0x30, 1, 2, 0xFFFE))
        eof = body.index(b"_eof")
        self.assertEqual(struct.unpack_from(">IHHIB", body, eof + 4), (0x11, 1, 1, eof, 0))

    def test_rewards_never_go_down(self):
        url = "/gameapi_omaha/UserUpdateRewards.ashx?&getDailyChallenges=1&userId=0009000000000042"
        first = self.rpdl(self.post(url, rpul(12000, owned=(3, 7))))
        self.assertEqual(struct.unpack_from(">i", first, 0)[0], 12000)
        # A reinstalled game uploads less: the server answers with what it knows.
        second = self.rpdl(self.post(url, rpul(5000, owned=(9,))))
        self.assertEqual(struct.unpack_from(">i", second, 0)[0], 12000)
        self.assertEqual([i for i in range(256) if second[0x108 + i] & 1], [3, 7, 9])
        record = lsp.load_player(0x0009000000000042)
        self.assertEqual(record["gamertag"], "Noble Six")
        self.assertIn("challenge_progress", record)

    def test_presence(self):
        payload = bytearray(0x1BB)
        payload[1] = 1
        struct.pack_into(">Q", payload, 2, 0x0009000000000077)
        status, _, body = self.post("/ReachPresenceApi/heartbeat.ashx",
                                    lsp.chunk(b"phbt", 5, 1, bytes(payload)), "presence")
        self.assertEqual(status, 200)
        major, _, reply = lsp.chunks(body)[b"phbr"]
        self.assertEqual((major, len(reply) + 12), (2, 0x9F))
        self.assertIn(0x0009000000000077, lsp.presence)
        query = struct.pack(">II", 0, 1) + struct.pack(">Q", 0x0009000000000077) + bytes(0x78)
        status, _, body = self.post("/ReachPresenceApi/query.ashx",
                                    lsp.chunk(b"preq", 3, 1, query), "presence")
        major, _, reply = lsp.chunks(body)[b"pplr"]
        self.assertEqual((major, len(reply)), (5, 0x1094))

    def test_storage(self):
        path = "/storage/title/4d53085b/tracked/11860/default_hoppers/manifest_001.bin"
        self.assertEqual(lsp.handle(None, "GET", path, {}, b"", None)[0], 404)
        full = os.path.join(self.dir.name, "storage", path.lstrip("/"))
        os.makedirs(os.path.dirname(full))
        with open(full, "wb") as f:
            f.write(b"operator file")
        self.assertEqual(lsp.handle(None, "GET", path, {}, b"", None)[2], b"operator file")
        self.assertEqual(lsp.handle(None, "GET", "/storage/../../etc/passwd", {}, b"", None)[0], 404)


if __name__ == "__main__":
    unittest.main()
