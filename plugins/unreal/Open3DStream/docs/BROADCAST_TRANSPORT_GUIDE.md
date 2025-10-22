# Broadcast Transport Guide (M3 series)

Purpose
- Outline sender roles and receiver pairings for transports available in the Unreal Open3DStream broadcast plugin

Transports (status)
- TCP client: Implemented (`FO3DSTcpTransport`)
- UDP sender (with light fragmentation): Implemented (`FO3DSUdpTransport`) [M3.2]
- NNG: Planned
- WebRTC DataChannel: Optional/beta (planned)

Unreal sender roles
- `UO3DSBroadcastTransportAdapter`
  - Binds to `UO3DSBroadcastComponent::OnSerializedFrame`
  - Queues serialized frames with backpressure and drains on tick
  - Instantiates a concrete transport per `Transport` selection (TCP/UDP)

CVars
- `o3ds.Broadcast.Enable` (0/1)
- `o3ds.Broadcast.Url` (e.g., `tcp://127.0.0.1:9000` or `udp://127.0.0.1:9001`)
- `o3ds.Broadcast.Key`
- `o3ds.Broadcast.MaxQueuedBytes` (bytes)
- Stats: `o3ds.Broadcast.Transport.DumpStats`

Usage examples
- TCP: set `Transport=TCP`, `Url=tcp://127.0.0.1:9000`
- UDP: set `Transport=UDP`, `Url=udp://127.0.0.1:9001`

Manual TCP test (local loop)
- Start a TCP listener (e.g., `nc -l 9000` on macOS/Linux, or PowerShell: `ncat -l 9000` if available)
- In Unreal:
  - Place an actor with `UO3DSBroadcastComponent` and `UO3DSBroadcastTransportAdapter`.
  - Set `Transport=TCP`, `Url=tcp://127.0.0.1:9000`.
  - Optionally override with CVars at runtime: `o3ds.Broadcast.Enable 1`, `o3ds.Broadcast.Url tcp://127.0.0.1:9000`.
- Play-in-editor and observe logs:
  - `TCP: Connected to 127.0.0.1:9000`
  - `O3DS Broadcast: Started capture ...`
- On the listener side you should see incoming bytes.
- Stop PIE: adapter unbinds and transport closes.

Notes
- UDP fragmentation: frames larger than MTU are split into multiple datagrams with a tiny header.
  The current receiver stack must handle reassembly (planned in M3 receivers). Use TCP when unsure.
- Both transports count FramesSent/BytesSent; drops are measured at the adapter level when the queue is full.
