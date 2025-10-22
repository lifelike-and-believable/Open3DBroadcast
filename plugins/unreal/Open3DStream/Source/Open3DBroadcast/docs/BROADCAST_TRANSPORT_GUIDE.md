# Broadcast Transport Guide (M3 series)

Purpose
- Outline sender roles and receiver pairings for transports available in the Unreal Open3DStream broadcast plugin

Transports (status)
- TCP client: Implemented (`FO3DSTcpTransport`)
- TCP server (LiveLink-compatible framing): Implemented (`FO3DSTcpServerTransport`) [M3.2]
- UDP sender (with light fragmentation): Implemented (`FO3DSUdpTransport`)
- NNG: Implemented (`FO3DSNngTransport`) [M3.3]
- WebRTC DataChannel: Optional/beta (planned)

Unreal sender roles
- `UO3DSBroadcastTransportAdapter`
  - Binds to `UO3DSBroadcastComponent::OnSerializedFrame`
  - Queues serialized frames with backpressure and drains on tick
  - Instantiates a concrete transport per `Transport` selection (TCP/TCPServer/UDP/NNG)

CVars
- `o3ds.Broadcast.Enable` (0/1)
- `o3ds.Broadcast.Url` (e.g., `tcp://127.0.0.1:9000`, `udp://127.0.0.1:9001`, `tcp://127.0.0.1:9002?mode=pub`)
- `o3ds.Broadcast.Key`
- `o3ds.Broadcast.MaxQueuedBytes` (bytes)
- Stats: `o3ds.Broadcast.Transport.DumpStats`

Usage examples
- TCP client -> custom TCP server: `Transport=TCP`, `Url=tcp://127.0.0.1:9000`
- TCP server -> LiveLink TCP client: `Transport=TCPServer`, `Url=tcp://0.0.0.0:9000`
- UDP -> LiveLink UDP server: `Transport=UDP`, `Url=udp://127.0.0.1:9001`
- NNG publisher -> NNG subscriber receiver: `Transport=NNG`, `Url=tcp://0.0.0.0:9002` (defaults to pub)
- NNG pair (client): `Transport=NNG`, `Url=tcp://127.0.0.1:9003?mode=pair&role=client`
- NNG pair (server): `Transport=NNG`, `Url=tcp://0.0.0.0:9003?mode=pair&role=server`

Notes
- UDP fragmentation: frames larger than MTU are split into multiple datagrams with a small header and require reassembly on the receiver side. The Open3DStream LiveLink source includes a basic reassembler (`mUdpMapper`).
- NNG publisher: listens and pushes frames to any connected subscribers. Pair modes support 1:1 communication; the pair client will attempt automatic reconnection with exponential backoff.
- All transports count FramesSent/BytesSent; drops are measured at the adapter level when the queue is full, and at the transport level if a send fails.
