# Broadcast Transport Guide (M3 series)

Purpose
- Outline sender roles and receiver pairings for transports available in the Unreal Open3DStream broadcast plugin

Transports (status)
- TCP client: Implemented (`FO3DSTcpTransport`)
- TCP server (LiveLink-compatible framing): Implemented (`FO3DSTcpServerTransport`) [M3.2]
- UDP sender (with light fragmentation): Implemented (`FO3DSUdpTransport`)
- NNG: Implemented (`FO3DSNngTransport`) [M3.3]
- WebRTC DataChannel: Optional/beta (planned) [M3.4]

Unreal sender roles
- `UO3DSBroadcastTransportAdapter`
  - Binds to `UO3DSBroadcastComponent::OnSerializedFrame`
  - Queues serialized frames with backpressure and drains on tick
  - Instantiates a concrete transport per `Transport` selection (TCP/TCPServer/UDP/NNG/WebRTC)

CVars and commands
- `o3ds.Broadcast.Enable` (0/1)
- `o3ds.Broadcast.Url` (e.g., `tcp://127.0.0.1:9000`, `udp://127.0.0.1:9001`, `tcp://127.0.0.1:9002?mode=pub`, `webrtc://localhost:8080/room1?role=server`)
- `o3ds.Broadcast.Key`
- `o3ds.Broadcast.MaxQueuedBytes` (bytes; adapter-level queue cap)
- Stats:
  - `o3ds.Broadcast.Transport.DumpStats` (adapter queue/counters overview)
  - `o3ds.Broadcast.Transport.DumpTransportStats` (per-transport: Connected, FramesSent, BytesSent, Dropped, Reconnects)

Usage examples
- TCP client -> custom TCP server: `Transport=TCP`, `Url=tcp://127.0.0.1:9000`
- TCP server -> LiveLink TCP client: `Transport=TCPServer`, `Url=tcp://0.0.0.0:9000`
- UDP -> LiveLink UDP server: `Transport=UDP`, `Url=udp://127.0.0.1:9001`
- NNG publisher -> NNG subscriber receiver: `Transport=NNG`, `Url=tcp://0.0.0.0:9002` (defaults to pub)
- NNG pair (client): `Transport=NNG`, `Url=tcp://127.0.0.1:9003?mode=pair&role=client`
- NNG pair (server): `Transport=NNG`, `Url=tcp://0.0.0.0:9003?mode=pair&role=server`
- WebRTC DataChannel (optional/beta):
  - Broadcast (server) -> LiveLink (client): `Transport=WebRTC`, `Url=webrtc://signaling-host:8080/room1?role=server`
  - Broadcast (client) -> LiveLink (server): `Transport=WebRTC`, `Url=webrtc://signaling-host:8080/room1?role=client`

NNG specifics
- URL query options:
  - `mode=pub` (default), or `mode=pair&role=client|server`
  - `qmax=<bytes>`: transport-local worker queue cap (default 4 MB). Frames beyond this cap are dropped and counted. Example: `tcp://0.0.0.0:9002?mode=pub&qmax=8388608` (8 MB).
- Reconnection (pair client): automatic exponential backoff re-dial triggered on send errors. Connection changes logged via "NNG: Pipe added/removed".
- Quick test (subscriber): `nngcat --sub --dial tcp://127.0.0.1:9002`

WebRTC specifics (optional/beta)
- Purpose: NAT-friendly, encrypted transport using libdatachannel DataChannel API.
- URL format: `webrtc://<signaling-host>:<port>/<room>?role=client|server&stun=<stun-url>&turn=<turn-url>`
  - Typical: `webrtc://localhost:8080/o3ds?role=server&stun=stun:stun.l.google.com:19302`
- Roles: choose complementary roles on sender/receiver; both use the same signaling room.
- Data: serialized O3DS frames sent per DataChannel message (message boundary preserved).
- Dependencies: requires signaling server (see repo WebRTC docs) and STUN/TURN for NAT as needed.
- Status: optional/beta for M3.4. Receiver hookup tracked in LiveLink source tasks.
- Audio (follow-on): see ISSUE_M3_4_1_WEBRTC_AUDIO.md for optional audio multiplexing over WebRTC.

Notes
- UDP fragmentation: frames larger than MTU are split into multiple datagrams with a small header and require reassembly on the receiver side. The Open3DStream LiveLink source includes a basic reassembler (`mUdpMapper`).
- NNG publisher: listens and pushes frames to any connected subscribers. Pair modes support 1:1 communication; the pair client will attempt automatic reconnection with exponential backoff.
- WebRTC: relies on signaling; ensure firewall allows signaling port. For internet scenarios, configure STUN/TURN.
- Backpressure:
  - Adapter level: `o3ds.Broadcast.MaxQueuedBytes` (and component/adapter settings) drop frames before reaching transports.
  - Transport level (NNG): additional per-transport cap via `qmax` as described above.
- All transports count FramesSent/BytesSent; drops are measured at the adapter level when the queue is full, and at the transport level if a send fails.
