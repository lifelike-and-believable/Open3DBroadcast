# WebRTC Receiver Integration Plan (UE 5.6)

This document defines the implementation plan to ingest mocap (animation + curves) and audio on the receiver side using WebRTC in the Open3DStream Unreal plugin. It complements the existing broadcast/sender work and aims for a deterministic, non‑blocking receiver path compatible with UE 5.6.

## Scope and goals

- Add a WebRTC “receiver transport” for the Open3DStream Live Link source.
- Receive mocap on the WebRTC DataChannel and deliver it to the existing Open3DStream Live Link pipeline (subjects, poses, curves).
- Receive Opus audio over RTP, decode to PCM16, and route to UE audio (Audio Bus/component).
- Preserve determinism and avoid blocking the game thread. All I/O and decoding should be done off the game thread.

Out of scope (follow‑ups): jitter buffer for out‑of‑order RTP, resampling for non‑48k audio, advanced reconnection strategies.

## Architecture overview

- Shared connector interface: `IWebRTCConnector` (already present) handles signaling, DataChannel, and remote audio RTP notifications.
- New receiver adapter: a light class that owns a connector instance in server role, binds to DataChannel and RTP callbacks, and forwards:
  - DataChannel bytes → Open3DStream Live Link source’s existing parsing path (FlatBuffers).
  - Remote audio RTP packets → new Opus decoder.
- New Opus decoder: minimal pipeline that parses RTP header (12 bytes), extracts PT=111 Opus payload, decodes to PCM16 @ 48k, and publishes into UE audio (Audio Bus/component).

## Key integration points

- Live Link source (Open3DStream module):
  - Start/Stop/Teardown hooks to create and manage the WebRTC receiver adapter when protocol/mode indicates WebRTC.
  - Tick hook to pump the adapter (if needed) without blocking the game thread.
  - Existing frame parsing path should remain unchanged; receiver adapter simply feeds raw O3DS buffers.
- Audio routing:
  - The decoder publishes PCM16 audio frames to the existing Audio Bus or a dedicated remote‑audio component for in‑world playback.

## Components to implement

1) Receiver adapter (WebRTC)
- Purpose: encapsulate `IWebRTCConnector` in server role and provide a clean sink interface to the Live Link source.
- Responsibilities:
  - Configure and start the connector (Signaling URL, room, role=Server).
  - Bind callbacks:
    - OnState → propagate status (for logging/UX).
    - OnData → forward raw bytes to the source’s package parsing method.
    - OnRemoteAudioRtp → push RTP payloads to the Opus decoder.
  - Non‑blocking lifecycle: Start(), Stop(), Tick().

2) Opus decoder (RTP → PCM16)
- Purpose: decode remote audio payloads for runtime playback.
- Responsibilities:
  - Parse RTP header (12 bytes). Expect PT=111 (Opus).
  - Maintain an `OpusDecoder*` configured for 48 kHz, 1–2 channels (match sender).
  - Decode per packet (20 ms frames). For MVP, no jitter buffering.
  - Publish PCM16 frames to UE (Audio Bus/component). Use RTP timestamp/48000.0 to approximate timing if needed; real‑time playback is acceptable for MVP.
  - Errors are throttled and logged behind a CVar.

## Settings & UX

- Source settings expose WebRTC as a protocol option: Signaling URL and Room (for compatibility with sample signaling).
- URL normalization: ensure a room/local id is present where the signaling expects it (receiver is typically the server role).
- CVars:
  - `o3ds.Receiver.WebRTC.Log` (0/1) – receiver adapter logging.
  - `o3ds.Receiver.Audio.Log` (0/1) – Opus decode/RTP logging (throttled).

## Data flow

- DataChannel (mocap):
  - Sender publishes O3DS FlatBuffers over the DataChannel.
  - Receiver adapter delivers bytes to the existing Open3DStream parsing path → Live Link subjects/poses/curves update as today.
  - Optional future: if a unified header is later adopted for mocap, detect it and branch; otherwise treat payloads as legacy O3DS buffers.

- Audio (RTP/Opus):
  - Sender encodes PCM at 48 kHz into Opus frames (~20 ms), packs into RTP with PT=111.
  - Receiver gets RTP bytes via `IWebRTCConnector::OnRemoteAudioRtp` → Opus decoder → PCM16 → Audio Bus/component.

## Edge cases & robustness

- Connection lifecycle:
  - Start/Stop idempotent. If signaling disconnects, propagate status and allow operator restart (reconnect with backoff can be a follow‑up).
- Audio‑only / Data‑only:
  - If no audio m‑line: RTP callback never fires; mocap still flows.
  - If no DataChannel updates: Live Link subjects remain until existing inactivity timeout elapses.
- Packet loss:
  - Rely on `opus_decode` PLC for simple concealment; no jitter buffer in MVP.
- Non‑48 kHz audio:
  - For MVP, drop and log at Verbose (resampler can be a follow‑up).

## Milestones

A) DataChannel → Live Link
- Implement receiver adapter and wire it into the Live Link source when protocol is WebRTC.
- Live Link subjects update from WebRTC DataChannel.
- Build PASS; subject appears and animates in editor.

B) Audio RTP → Opus → UE playback
- Implement Opus decoder and connect RTP callback.
- Audio plays in‑world via Audio Bus/component; packet sizes vary with content; silence is minimal.
- Build PASS; audible smoke test in editor.

C) Polish & guards
- CVars, throttled logs, bounded queues where applicable.
- Clean shutdown on PIE stop; no crashes or thread leaks.
- README/CHANGELOG updates.

## Definition of Done (receiver side)

- WebRTC receiver provides end‑to‑end ingestion for mocap and audio in UE 5.6.
- No blocking on the game thread; I/O/decoding off the game thread.
- Deterministic behavior and quiet defaults; verbose diagnostics behind CVars.
- Compatible with existing broadcaster and sample signaling.

## Acceptance tests

- Editor smoke:
  - Start sender (client role) and receiver source (server role) pointing to the same signaling server/room.
  - Verify Live Link subject(s) appear and animate.
  - Verify audio plays; packet sizes vary with content; ~15 byte packets during silence (Opus DTX + RTP header).
  - Stop PIE cleanly; no errors/crashes in logs.

## Files & structure (proposed)

- New (Open3DStream module):
  - `Private/WebRTC/Open3DSWebRtcReceiver.h/.cpp` – receiver adapter
  - `Private/WebRTC/O3DSOpusDecoder.h/.cpp` – RTP/Opus → PCM16 decode
- Modified (Open3DStream module):
  - Live Link source to branch into WebRTC adapter (Start/Stop/Tick) based on protocol setting
  - Module build file may require ensuring Opus include/lib if not already available
- Docs:
  - README: add a “WebRTC Receiver” quickstart and limitations
  - CHANGELOG: add receiver entry with behavior notes

## Risks & follow‑ups

- No jitter buffer may cause rare artifacts on lossy links; acceptable for MVP – track as follow‑up.
- Non‑48 kHz input is dropped (log Verbose). Add resampling later if needed.
- Reconnect logic can be added with exponential backoff if required by production.

---

Authoritative constraints:
- UE 5.6 API signatures must be verified against official docs/source.
- Never block the game thread; use async work for networking/decoding.
- Maintain deterministic behavior and minimal default logging, per repo standards.
- WebRTCConnectorComponent is the ground truth for connector behavior; changes to LibDataChannelConnector must align with it and must not introduce external dependencies from streaming or broadcast modules.
