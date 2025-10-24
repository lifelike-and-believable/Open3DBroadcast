# Open3DStream Broadcast: WebRTC User Guide

This guide describes running the sample signaling server and how Open3DStream uses WebRTC for streaming, including late-join resilience and glare (simultaneous offer) handling.

## Signaling server

A minimal signaling server is provided for local testing:

- Path: `Plugins/Open3DStream/examples/signaling-server.js`
- Requires Node.js >= 16

Install and run:

```
npm install
node signaling-server.js --port 8080
```

The server listens on `http://localhost:8080/ws` and relays:
- Room join/leave notifications
- SDP `offer`/`answer`
- `ice` candidates

### Late-join offer replay

To handle a client joining after an offer was sent, the server caches the last `offer` per room for 5 seconds. When a peer joins, if a fresh offer exists, the server replays it to the new peer so they can respond with an `answer` without requiring the other peer to re-send.

### Glare handling (simultaneous offers)

When both peers try to send an `offer` at the same time (glare), the server serializes the negotiation per room using a short lock. Only one `offer` is accepted within a small window (3s by default). Any concurrent `offer` results in a response:

```
{ "type": "collision", "action": "wait-retry", "retryAfterMs": <number> }
```

Clients should wait `retryAfterMs` and try creating a new offer. Open3DStream’s client automatically schedules a re-offer when it receives a `collision` message.

Notes:
- The lock is released on `answer` or timeout.
- If the active offerer disconnects, the lock is cleared immediately.

## Unreal integration

Open3DStream uses libdatachannel for WebRTC and a WebSocket signaling client:

- Connector: `Plugins/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.*`
- Signaling: `Plugins/Open3DStream/Source/Open3DStream/Private/WebRTCSignalingClient.*`

Key behaviors:
- Client role creates the data channel and initiates offers.
- Server role waits for an offer and replies with an answer.
- If `collision` is received, the client waits then re-offers.
- Auto re-offer and reconnect backoff can be tuned via CVars:
  - `o3ds.Broadcast.WebRTC.AutoReconnect` (default 1)
  - `o3ds.Broadcast.WebRTC.BackoffInitialMs` (default 500)
  - `o3ds.Broadcast.WebRTC.BackoffMaxMs` (default 10000)
- Optional negotiated data channel to reduce glare risk further:
  - `o3ds.Broadcast.WebRTC.NegotiatedChannel` (0/1)
  - `o3ds.Broadcast.WebRTC.ChannelId` (default 42)

## Troubleshooting

- Ensure the signaling server is reachable at the configured host/port.
- For local testing, use `webrtc://localhost:8080/<room>` URLs.
- Use `o3ds.WebRTC.Verbose=1` and `o3ds.WebRTC.DebugRx=1` for detailed logs.
- If you see repeated `collision` messages, enable negotiated data channels or increase the negotiation window on the server.
