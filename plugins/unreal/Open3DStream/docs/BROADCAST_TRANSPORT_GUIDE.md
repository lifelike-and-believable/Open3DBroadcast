# Broadcast Transport Guide (M0.4, prep for M3)

Purpose
- Outline sender roles and receiver pairings for transports available in the core O3DS library

Transports (core library references)
- TCP/UDP: `src/o3ds/tcp.cpp`, `src/o3ds/udp.cpp`, `src/o3ds/udp_fragment.cpp`
- NNG: `src/o3ds/nng_connector.h`, `src/o3ds/publisher.cpp`, `src/o3ds/subscriber.cpp`, `src/o3ds/pair.cpp`
- Optional/Beta: WebRTC DataChannel (see repo docs below)

WebRTC references (optional/beta)
- `WEBRTC_IMPLEMENTATION_SUMMARY.md`
- `WEBRTC_IMPLEMENTATION_COMPLETE.md`
- `WEBRTC_SUPPORT.md`
- `LIBDATACHANNEL_INTEGRATION.md`

Unreal sender roles (planned for M3)
- TCP client (connect to receiver server)
- UDP sender (supports fragmentation for large payloads)
- NNG publisher / pair (configurable endpoint)
- WebRTC DataChannel (peer connection using libdatachannel; optional)

Receiver pairing
- Unreal Open3DStreamSource (LiveLink)
- Maya/Mobu receivers (repo plugins)

Configuration (planned)
- CVars (runtime override):
  - `o3ds.Broadcast.Transport` = `TCP` | `UDP` | `NNG` | `WebRTC`
  - `o3ds.Broadcast.Url` = endpoint (e.g. `tcp://127.0.0.1:9000`, `udp://127.0.0.1:9001`, `nng://ipc:///tmp/o3ds`)
  - `o3ds.Broadcast.Key` = optional authentication / session key
- Component Settings (editor): mirroring the above via `UO3DSBroadcastTransportAdapter`

Operational notes
- M2 uses in-memory loopback only; network transports are out-of-scope there
- M3 will implement transport adapters and reconnection strategy
- WebRTC requires signaling and SDP exchange; use repo-provided helper or minimal signaling in editor to establish a DataChannel

Troubleshooting
- No receiver: ensure endpoint is reachable and receiver protocol matches
- UDP packet loss: prefer TCP/NNG for reliability, or enable fragmentation helpers
- WebRTC: verify successful ICE gathering and DataChannel open state before sending
