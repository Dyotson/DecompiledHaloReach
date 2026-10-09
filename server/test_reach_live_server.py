#!/usr/bin/env python3
"""Protocol test for reach_live_server.py: two fake clients register, broadcast and relay.

    python3 server/test_reach_live_server.py
"""

import os
import socket
import struct
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
MAGIC = b"RLV1"
HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD = 1, 2, 3, 4, 5, 6


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Client:
    def __init__(self, server, name, xuid, room=b""):
        self.server = server
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(2)
        self.name, self.xuid, self.room = name, xuid, room

    def send(self, kind, body):
        self.sock.sendto(MAGIC + bytes([kind]) + body, self.server)

    def recv(self):
        data, _ = self.sock.recvfrom(65536)
        assert data[:4] == MAGIC
        return data[4], data[5:]

    def hello(self, token=1):
        port = self.sock.getsockname()[1]
        body = struct.pack(">HQQIH", 1, token, self.xuid, 0x7F000001, port)
        body += bytes([len(self.room)]) + self.room + bytes([len(self.name)]) + self.name
        self.send(HELLO, body)
        kind, body = self.recv()
        assert kind == WELCOME, kind
        self.id, epoch, ip, port, motd_len = struct.unpack_from(">IHIHH", body)
        return self.id


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.port = free_port()
        cls.proc = subprocess.Popen([sys.executable, os.path.join(HERE, "reach_live_server.py"),
                                     "--host", "127.0.0.1", "--port", str(cls.port)],
                                    stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        cls.addr = ("127.0.0.1", cls.port)

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        cls.proc.wait()

    def test_broadcast_and_relay(self):
        a = Client(self.addr, b"Alpha", 0x0009000000000001)
        b = Client(self.addr, b"Bravo", 0x0009000000000002)
        c = Client(self.addr, b"Other", 0x0009000000000003, room=b"elsewhere")
        a_id, b_id, c_id = a.hello(), b.hello(), c.hello()
        self.assertEqual(len({a_id, b_id, c_id}), 3)

        a.send(BROADCAST, struct.pack(">HH", 1001, 1001) + b"search")
        kind, body = b.recv()
        self.assertEqual(kind, FORWARD)
        src, _, src_port, _, _, gsrc, gdst = struct.unpack_from(">IIHIHHH", body)
        self.assertEqual((src, gsrc, gdst, body[20:]), (a_id, 1001, 1001, b"search"))
        self.assertEqual(src_port, a.sock.getsockname()[1])
        with self.assertRaises(socket.timeout):
            c.sock.settimeout(0.3)
            c.recv()  # another room hears nothing

        b.send(RELAY, struct.pack(">IHH", a_id, 1000, 1000) + b"reply")
        kind, body = a.recv()
        self.assertEqual(kind, FORWARD)
        self.assertEqual(struct.unpack_from(">I", body)[0], b_id)
        self.assertEqual(body[20:], b"reply")

    def test_unregistered_sender_is_told(self):
        d = Client(self.addr, b"Delta", 0x0009000000000004)
        d.send(RELAY, struct.pack(">IHH", 1, 1000, 1000) + b"x")
        kind, body = d.recv()
        self.assertEqual((kind, body[0]), (ERROR, 1))


if __name__ == "__main__":
    unittest.main()
