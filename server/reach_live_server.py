#!/usr/bin/env python3
"""Reach Live: a self-hostable rendezvous and relay server for the Halo: Reach PC port.

Players point the game at a server with REACH_SERVER=host[:port]. Everyone connected
to the same server (and room) sees everyone else's System Link games, and game traffic
flows between them: directly when UDP hole punching works, through this server when it
doesn't. Nothing here talks to Microsoft or Bungie services.

    python3 server/reach_live_server.py [--port 21100] [--http-port 21101]

The wire format is in docs/online_plan.md ("Reach Live protocol"). Standard library only.
"""

import argparse
import asyncio
import json
import logging
import os
import random
import struct
import time

import reach_live_lsp as lsp

MAGIC = b"RLV1"
VERSION = 1

HELLO, WELCOME, ERROR, BROADCAST, RELAY, FORWARD, PUNCH, PUNCH_ACK, DATA, BYE, LIST, PEERS = range(1, 13)

PEER_TIMEOUT = 30.0  # seconds without a HELLO before a peer is dropped
MAX_PAYLOAD = 1500
MAX_NAME = 15  # XUSER_NAME_SIZE minus the terminator

log = logging.getLogger("reach-live")


class Peer:
    def __init__(self, peer_id, addr, token, xuid, name, room, local):
        self.id = peer_id
        self.addr = addr  # public (ip, port) as this server sees it
        self.token = token
        self.xuid = xuid
        self.name = name
        self.room = room
        self.local = local  # (ip, port) the client reports for its LAN side
        self.seen = time.monotonic()
        self.since = time.time()
        self.relayed_bytes = 0


def ip_to_u32(ip):
    a, b, c, d = (int(x) for x in ip.split("."))
    return a << 24 | b << 16 | c << 8 | d


def u32_to_ip(v):
    return f"{v >> 24}.{v >> 16 & 255}.{v >> 8 & 255}.{v & 255}"


class ReachLive(asyncio.DatagramProtocol):
    def __init__(self, motd, max_peers):
        self.motd = motd.encode()[:512]
        self.max_peers = max_peers
        self.epoch = random.randrange(1, 0x10000)  # changes when the server restarts
        self.peers = {}  # id -> Peer
        self.by_addr = {}  # (ip, port) -> Peer
        self.next_id = random.randrange(1, 0x1000)
        self.transport = None
        self.started = time.time()
        self.relayed_packets = 0
        self.http_port = 0  # TCP port of the title servers, told to clients; 0 = none

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, kind, body=b""):
        self.transport.sendto(MAGIC + bytes([kind]) + body, addr)

    def error(self, addr, code, message):
        text = message.encode()
        self.send(addr, ERROR, struct.pack(">BH", code, len(text)) + text)

    def datagram_received(self, data, addr):
        if len(data) < 5 or data[:4] != MAGIC:
            return
        kind, body = data[4], data[5:]
        try:
            if kind == HELLO:
                self.on_hello(body, addr)
                return
            peer = self.by_addr.get(addr)
            if peer is None:
                # Unknown sender (e.g. the server restarted): ask it to register again.
                self.error(addr, 1, "not registered")
                return
            peer.seen = time.monotonic()
            if kind == BROADCAST:
                self.on_broadcast(peer, body)
            elif kind == RELAY:
                self.on_relay(peer, body)
            elif kind == LIST:
                self.on_list(peer, addr)
            elif kind == BYE:
                self.drop(peer, "left")
        except (struct.error, IndexError, UnicodeDecodeError) as e:
            log.debug("bad packet from %s: %s", addr, e)

    def on_hello(self, body, addr):
        version, token, xuid, local_ip, local_port = struct.unpack_from(">HQQIH", body, 0)
        pos = 24
        room_len = body[pos]
        room = body[pos + 1:pos + 1 + room_len].decode()
        pos += 1 + room_len
        name_len = body[pos]
        name = body[pos + 1:pos + 1 + name_len].decode()[:MAX_NAME]
        if version != VERSION:
            self.error(addr, 2, f"server speaks protocol {VERSION}, client {version}")
            return
        peer = self.by_addr.get(addr)
        if peer is not None and peer.token != token:
            self.drop(peer, "restarted")  # same address, new game process
            peer = None
        if peer is None:
            if len(self.peers) >= self.max_peers:
                self.error(addr, 3, "server full")
                return
            while self.next_id in self.peers or self.next_id == 0:
                self.next_id = (self.next_id + 1) & 0xFFFFFF
            peer = Peer(self.next_id, addr, token, xuid, name, room, (local_ip, local_port))
            self.next_id = (self.next_id + 1) & 0xFFFFFF
            self.peers[peer.id] = peer
            self.by_addr[addr] = peer
            log.info("join  #%d %r xuid %016X room %r from %s:%d (lan %s:%d)", peer.id, name, xuid,
                     room, addr[0], addr[1], u32_to_ip(local_ip), local_port)
        peer.seen = time.monotonic()
        peer.name, peer.xuid, peer.room, peer.local = name, xuid, room, (local_ip, local_port)
        body = struct.pack(">IHIHH", peer.id, self.epoch, ip_to_u32(addr[0]), addr[1], len(self.motd))
        self.send(addr, WELCOME, body + self.motd + struct.pack(">H", self.http_port))

    def forward_header(self, src):
        return struct.pack(">IIHIH", src.id, ip_to_u32(src.addr[0]), src.addr[1], src.local[0],
                           src.local[1])

    def room_peers(self, src):
        return [p for p in self.peers.values() if p.room == src.room and p is not src]

    def on_broadcast(self, src, body):
        if len(body) < 4 or len(body) - 4 > MAX_PAYLOAD:
            return
        packet = self.forward_header(src) + body
        for peer in self.room_peers(src):
            self.send(peer.addr, FORWARD, packet)

    def on_relay(self, src, body):
        if len(body) < 8 or len(body) - 8 > MAX_PAYLOAD:
            return
        (dst_id,) = struct.unpack_from(">I", body, 0)
        dst = self.peers.get(dst_id)
        if dst is None or dst.room != src.room:
            return
        src.relayed_bytes += len(body) - 8
        self.relayed_packets += 1
        self.send(dst.addr, FORWARD, self.forward_header(src) + body[4:])

    def on_list(self, src, addr):
        peers = [src] + self.room_peers(src)
        body = struct.pack(">H", len(peers))
        for p in peers[:200]:
            name = p.name.encode()
            body += struct.pack(">IQB", p.id, p.xuid, len(name)) + name
        self.send(addr, PEERS, body)

    def drop(self, peer, why):
        self.peers.pop(peer.id, None)
        if self.by_addr.get(peer.addr) is peer:
            del self.by_addr[peer.addr]
        log.info("leave #%d %r (%s)", peer.id, peer.name, why)

    def expire(self):
        now = time.monotonic()
        for peer in [p for p in self.peers.values() if now - p.seen > PEER_TIMEOUT]:
            self.drop(peer, "timed out")

    def status(self):
        rooms = {}
        for p in self.peers.values():
            rooms.setdefault(p.room, []).append(
                {"id": p.id, "gamertag": p.name, "xuid": f"{p.xuid:016X}",
                 "online_for": int(time.time() - p.since), "relayed_bytes": p.relayed_bytes})
        return {"server": "reach-live", "protocol": VERSION, "uptime": int(time.time() - self.started),
                "players": len(self.peers), "relayed_packets": self.relayed_packets, "rooms": rooms}


async def read_request(reader):
    """One HTTP/1.x request: (method, path, headers, body)."""
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 15)
    lines = head.decode("latin-1").split("\r\n")
    method, path = lines[0].split(" ")[:2]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", 0) or 0)
    body = await asyncio.wait_for(reader.readexactly(length), 30) if length else b""
    return method, path, headers, body


def response(status, body, content_type="application/octet-stream", extra=()):
    reason = {200: "OK", 400: "Bad Request", 404: "Not Found", 500: "Internal Server Error"}
    head = [f"HTTP/1.0 {status} {reason.get(status, 'OK')}", f"Content-Type: {content_type}",
            f"Content-Length: {len(body)}", "Access-Control-Allow-Origin: *", *extra]
    return ("\r\n".join(head) + "\r\n\r\n").encode() + body


async def http_handler(server, reader, writer):
    """The HTTP side of the server, on --http-port. The game reaches it as its title
    servers ("LSP": title/user storage, rewards; reach_live_lsp.py). GET / or /status
    returns a JSON status page for people and web front ends."""
    try:
        method, path, headers, body = await read_request(reader)
        peer = writer.get_extra_info("peername")
        if path in ("/", "/status"):
            reply = response(200, json.dumps(server.status(), indent=2).encode(), "application/json")
        else:
            try:
                status, content_type, data = lsp.handle(server, method, path, headers, body, peer)
            except Exception:  # a bad request must not take the server down
                log.exception("LSP %s %s failed", method, path)
                status, content_type, data = 500, "text/plain", b"error"
            log.info("http  %s %s %s -> %d (%d bytes)", peer[0], method, path, status, len(data))
            reply = response(status, data, content_type)
        writer.write(reply)
        await writer.drain()
    except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError, ValueError):
        pass
    finally:
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on")
    parser.add_argument("--port", type=int, default=int(os.environ.get("REACH_LIVE_PORT", 21100)),
                        help="UDP port for the game (default 21100)")
    parser.add_argument("--http-port", type=int, default=0,
                        help="TCP port for the game's title servers (LSP) and a JSON status "
                             "page (default: off)")
    parser.add_argument("--motd", default="Welcome to Reach Live", help="message sent to clients")
    parser.add_argument("--max-peers", type=int, default=1024)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(message)s")

    loop = asyncio.get_running_loop()
    transport, server = await loop.create_datagram_endpoint(
        lambda: ReachLive(args.motd, args.max_peers), local_addr=(args.host, args.port))
    log.info("Reach Live listening on UDP %s:%d (epoch %04X)", args.host, args.port, server.epoch)
    if args.http_port:
        server.http_port = args.http_port
        await asyncio.start_server(lambda r, w: http_handler(server, r, w), args.host, args.http_port)
        log.info("title servers and status page on http://%s:%d/", args.host, args.http_port)
    try:
        while True:
            await asyncio.sleep(5)
            server.expire()
    finally:
        transport.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
