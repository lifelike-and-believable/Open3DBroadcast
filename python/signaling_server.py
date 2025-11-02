# Simple peer-to-peer WebSocket signaling server for Open3DStream
# Broadcasts JSON messages tagged with an 'id' to all other peers sharing the same id.
# Requires: pip install websockets

import asyncio
import json
import websockets
from websockets.server import WebSocketServerProtocol
from collections import defaultdict

ROOMS: dict[str, set[WebSocketServerProtocol]] = defaultdict(set)

async def handler(ws: WebSocketServerProtocol):
    room = None
    try:
        async for msg in ws:
            try:
                obj = json.loads(msg)
            except Exception:
                continue
            # Expect at least { "id": "room1", "type": "offer|answer|candidate", ... }
            room = obj.get("id") or obj.get("room") or "default"
            peers = ROOMS[room]
            if ws not in peers:
                peers.add(ws)
                print(f"[join] {ws.remote_address} -> room={room} peers={len(peers)}")
            # Relay to all other peers in the same room
            dead = set()
            for peer in peers:
                if peer is ws:
                    continue
                try:
                    await peer.send(msg)
                except Exception:
                    dead.add(peer)
            # Cleanup dead sockets
            for d in dead:
                try:
                    peers.remove(d)
                except KeyError:
                    pass
    finally:
        # On disconnect, remove from room
        if room is not None:
            peers = ROOMS.get(room)
            if peers and ws in peers:
                peers.remove(ws)
                print(f"[leave] {ws.remote_address} <- room={room} peers={len(peers)}")

async def main(host: str = "127.0.0.1", port: int = 8080):
    print(f"Signaling server listening on ws://{host}:{port}")
    async with websockets.serve(handler, host, port, max_size=8*1024*1024):
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    args = ap.parse_args()
    asyncio.run(main(args.host, args.port))
