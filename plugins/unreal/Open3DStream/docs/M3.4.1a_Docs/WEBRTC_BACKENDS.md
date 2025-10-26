# WebRTC Backends

Open3DStream supports multiple WebRTC backends behind a unified interface and UI selection. This document explains how to choose, configure, and operate each backend.

## Backends

- Peer-to-Peer (libdatachannel)
  - URL: `webrtc://host:port/room`
  - Signaling: Simple WS signaling (examples/signaling-server.js) or your own
  - Data: SCTP DataChannel (ordered/unordered; reliable/unreliable)
  - Audio: Opus tracks (game mix + per-subject mic) when libdatachannel is built with media
  - Best for: direct, low-latency P2P, development, or small-scale rooms

- LiveKit SFU
  - Config: Server URL (wss), Room, Token (JWT)
  - Data: LiveKit data messages (kinds: reliable, lossy), topic-based
  - Audio: Opus tracks; label tracks by subject via Track.Name; works multiparty, cloud-native
  - Best for: scalable multi-party rooms, autoscaling, managed infra

Future candidates (not yet implemented here): mediasoup, Janus, Ion-SFU.

## Selecting a Backend

- Broadcaster (Open3DBroadcast):
  - In your Broadcast Transport settings:
    - Transport Family = WebRTC
    - Backend = “Peer-to-Peer (libdatachannel)” or “LiveKit SFU”
    - If LiveKit: fill ServerUrl, Room, Token

- Live Link Source (Open3DStream):
  - In the “Open3DStream Source” creation dialog:
    - Protocol = WebRTC Client/Server
    - Backend = libdatachannel or LiveKit
    - If LiveKit: fill ServerUrl, Room, Token

Note: URL is required for P2P mode. LiveKit typically uses its own fields (ServerUrl/Room/Token), independent of the URL box.

## Build-time flags and dependencies

- O3DS_ENABLE_WEBRTC: Turns WebRTC UI on in the plugin (default: ON)
- O3DS_ENABLE_LIVEKIT: Enables LiveKit connector build (default: 0)
  - Set to 1 and link LiveKit C++ SDK to enable the backend
- libdatachannel (P2P) for audio:
  - Must be built with:
    - USE_MEDIA=ON
    - USE_OPUS=ON
    - USE_MBEDTLS=ON
  - Static linking recommended for UE plugin distribution

## Recommended mappings

- Data
  - Animation: lossy (unordered, unreliable) or “lossy data” in LiveKit
  - Control/Announce: reliable (ordered) or “reliable data” in LiveKit
  - Topic names (LiveKit): `o3ds.anim`, `o3ds.ctrl`, `o3ds.audio.announce`

- Audio tracks
  - Game mix: `o3ds:mix`
  - Per-subject mic: `o3ds:subject/<SubjectName>`
  - Use these as MediaStream IDs (P2P) or Track.Name (LiveKit)

## Operational notes

- Message size: keep <= 15 KB wherever possible
- Backpressure:
  - Lossy path: tiny queue (e.g., 2 frames); drop oldest when full
  - Reliable path: low volume; small messages to avoid HOL blocking
- Subject routing:
  - P2P: Parse stream id, or use `o3ds.audio.announce`
  - LiveKit: Use Track.Name first; also send `o3ds.audio.announce` reliably

## Testing

- P2P: examples/signaling-server.js; verify SDP has `m=audio` with Opus; check DataChannel flow
- LiveKit: join a room with a browser client or CLI; verify topics, audio routing, and end-to-end latency
- Multi-actor: publish multiple per-subject mic tracks + game mix; verify receiver routing matches LiveLink subjects