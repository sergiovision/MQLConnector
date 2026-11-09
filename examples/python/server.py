#!/usr/bin/env python3
"""A reference server for MQLConnector (standard library only, Python 3.9+).

Every frame is one UTF-8 JSON object. The server answers each request on the same connection
with a reply that carries the request's route member (ChartId) and correlation member (ReqId),
and can push broadcasts to every route it has seen, which MQL programs read with
DoListenMessage. See docs/PROTOCOL.md.

    python server.py                         # 127.0.0.1:2023, 0x1A-delimited frames
    python server.py --framing length        # u32 little-endian length prefix (FBE layout)
    python server.py --delimiter 10          # newline-delimited JSON
    python server.py --broadcast 5           # push a frame to every route every 5 s

Match the connector's InitLib keys: Framing/Delimiter as here, Profile "generic", and
CorrelationKey "ReqId" if you want correlated replies.
"""

import argparse
import asyncio
import json
import logging
import struct
import time

MAX_FRAME_BYTES = 16 * 1024 * 1024  # the connector's cap; larger frames are refused
SIGNAL_GETMAINLOGPATH = 1026  # sent by the connector's default "fincore" profile only

log = logging.getLogger("mqlc-server")


class Framing:
    """Reads and writes frames in one of the connector's two framings."""

    def __init__(self, kind: str, delimiter: int):
        self.kind = kind
        self.delimiter = bytes([delimiter])

    async def read(self, reader: asyncio.StreamReader):
        """The next frame's payload, or None at end of stream."""
        try:
            if self.kind == "length":
                total = struct.unpack("<I", await reader.readexactly(4))[0]  # includes these 4 bytes
                if total < 4 or total - 4 > MAX_FRAME_BYTES:
                    raise ValueError(f"invalid length prefix {total}")
                return await reader.readexactly(total - 4)
            return (await reader.readuntil(self.delimiter))[:-1]
        except (asyncio.IncompleteReadError, ConnectionError):
            return None

    def encode(self, payload: bytes) -> bytes:
        if self.kind == "length":
            return struct.pack("<I", len(payload) + 4) + payload
        if self.delimiter in payload:
            raise ValueError("payload contains the delimiter")
        return payload + self.delimiter


class Server:
    def __init__(self, args):
        self.args = args
        self.framing = Framing(args.framing, args.delimiter)
        self.clients = {}  # writer -> set of routes seen on that connection

    def frame(self, message: dict) -> bytes:
        # json.dumps escapes every control character, so the payload can never contain the
        # delimiter (1..31); compact separators add no whitespace.
        text = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        return self.framing.encode(text.encode("utf-8"))

    def reply_to(self, request: dict) -> dict:
        route_key, corr_key = self.args.route_key, self.args.correlation_key
        reply = {route_key: request.get(route_key, 0)}
        if corr_key in request:
            reply[corr_key] = request[corr_key]  # echo it: the connector matches on it
        if request.get("Id") == SIGNAL_GETMAINLOGPATH:
            reply.update(Id=SIGNAL_GETMAINLOGPATH, Data="")  # "" = keep the connector's own log
            return reply
        reply.update(Type="reply", ServerTime=time.time(), Echo=request)
        return reply

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        log.info("client %s connected", peer)
        routes = self.clients.setdefault(writer, set())
        try:
            while True:
                payload = await self.framing.read(reader)
                if payload is None:
                    break
                try:
                    # strict=False: raw control characters inside strings are accepted, as the
                    # connector accepts them.
                    request = json.loads(payload.decode("utf-8"), strict=False)
                except (UnicodeDecodeError, json.JSONDecodeError) as error:
                    log.warning("bad frame from %s: %s", peer, error)
                    continue
                if not isinstance(request, dict):
                    continue
                routes.add(request.get(self.args.route_key, 0))
                log.info("<- %s", payload.decode("utf-8", "replace")[:300])
                writer.write(self.frame(self.reply_to(request)))
                await writer.drain()
        except ValueError as error:  # a corrupt length prefix: the stream cannot be resynchronised
            log.warning("closing %s: %s", peer, error)
        finally:
            self.clients.pop(writer, None)
            writer.close()
            log.info("client %s disconnected", peer)

    async def broadcast_forever(self, every: float):
        sequence = 0
        while True:
            await asyncio.sleep(every)
            sequence += 1
            for writer, routes in list(self.clients.items()):
                for route in routes:
                    writer.write(self.frame({self.args.route_key: route, "Type": "broadcast",
                                             "Seq": sequence, "ServerTime": time.time()}))

    async def run(self):
        server = await asyncio.start_server(self.handle, self.args.host, self.args.port,
                                            limit=MAX_FRAME_BYTES + 1)
        names = ", ".join(str(s.getsockname()) for s in server.sockets)
        framing = "length prefix" if self.args.framing == "length" else f"delimiter {self.args.delimiter}"
        log.info("listening on %s, %s framing", names, framing)
        if self.args.broadcast > 0:
            asyncio.ensure_future(self.broadcast_forever(self.args.broadcast))
        async with server:
            await server.serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2023)
    parser.add_argument("--framing", choices=["delimiter", "length"], default="delimiter")
    parser.add_argument("--delimiter", type=int, default=26, help="delimiter byte 1..31 (default 26 = 0x1A)")
    parser.add_argument("--route-key", default="ChartId", help="the connector's RouteKey")
    parser.add_argument("--correlation-key", default="ReqId", help="the connector's CorrelationKey")
    parser.add_argument("--broadcast", type=float, default=0, help="push a frame to every route every N seconds")
    args = parser.parse_args()
    if not 1 <= args.delimiter <= 31:
        parser.error("--delimiter must be 1..31")
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    try:
        asyncio.run(Server(args).run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
