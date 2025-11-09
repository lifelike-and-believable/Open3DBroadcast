# LiveKit vs. LibDataChannel (Unreal)

This short guide compares the two WebRTC backends available in the Unreal plugin and helps you choose the right one for your scenario.

## Summary

- LiveKit: SFU (server)-based, scalable, token-authenticated, easier NAT traversal. Best for internet streaming and 1→many.
- LibDataChannel: P2P, no server dependency beyond signaling, minimal overhead. Best for LAN, small peer counts, and lowest moving parts.

## Key Differences

- Architecture: 
  - LiveKit: SFU relays media. Sender publishes once; the server fans out to subscribers.
  - LibDataChannel: Pure P2P. Each additional peer increases sender bandwidth.

- Scalability:
  - LiveKit: 10+ participants practical; server scales and handles relay.
  - LibDataChannel: 2–4 peers typical before bandwidth and NAT become limiting.

- NAT Traversal:
  - LiveKit: Built-in TURN/STUN and server mediation simplify traversal.
  - LibDataChannel: Traversal depends on STUN/TURN availability and network.

- Authentication:
  - LiveKit: Requires a JWT Token (entered under WebRTC → Token). UI provides a backend-specific hint.
  - LibDataChannel: No token; simple signaling.

- URL & Roles:
  - Both: Do not append `role=` or backend hints to URLs anymore. Connectors assemble backend-specific signaling internally.
  - Roles are implied by context: Broadcaster acts as Publisher; Live Link Source acts as Subscriber.

- Data Path:
  - LiveKit: Topic-based data messaging under the hood; connector exposes the same Send/Receive API.
  - LibDataChannel: SCTP DataChannel directly.

- Audio:
  - Both: Opus compressed audio with RTP framing; optional per-subject/mix labels.

- Reliability:
  - Both: Reliable and lossy modes supported via connector internals (no URL flags needed for typical use).

## When To Choose Which

- Choose LiveKit if you need:
  - Internet delivery, 1→many or many→many, or cloud deployments
  - Simpler NAT traversal and room management
  - Token-based access control

- Choose LibDataChannel if you need:
  - Minimal dependencies (no SFU)
  - Local/LAN scenarios or controlled networks
  - Lowest moving parts and quick ad‑hoc testing

## Quick Setup Checklist (Unreal)

LiveKit (SFU)
- Transport: `WebRTC`, Backend: `LiveKit`
- URL: `wss://<your-livekit-host>`
- Room: `<room-name>`
- Token: paste JWT (UI shows hint)

LibDataChannel (P2P)
- Transport: `WebRTC`, Backend: `LibDataChannel`
- URL: `ws://<signaling-host>:<port>` (sample signaling)
- Room: `<room-name>`
- Token: leave empty

Notes
- Do not append `role=` or backend flags to URLs.
- Backend-specific URL semantics, roles, and reliability are handled by connectors.
- For audio playback on the receiver, add `O3DSRemoteAudioComponent`.

## Troubleshooting Pointers

- LiveKit: If connection fails, verify Token (JWT), Room, and URL; check LiveKit server logs.
- LibDataChannel: If peers don’t connect, verify signaling server is reachable and Room matches.
- For both: Enable verbose logging via CVars; see the testing guide for recommended settings.
