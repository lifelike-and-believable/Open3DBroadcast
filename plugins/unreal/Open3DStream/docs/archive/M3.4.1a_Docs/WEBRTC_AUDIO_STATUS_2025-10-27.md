# WebRTC Audio (Opus) Integration Status — M3.4.1a

Date:2025-10-27
Scope: Issue #94 — Refactor WebRTC transport: backend-agnostic interface, LiveKit SFU support, subject-aware audio, and unified data messaging

This document summarizes the implementation status in the repository as of the date above, with emphasis on Opus encoding/decoding over libdatachannel for WebRTC audio.

## Summary
- Backend-agnostic interface: Present and partially wired
- LibDataChannel Opus audio: Implemented (send/receive) behind compile flags
- LiveKit: Deferred (not implemented yet)
- Subject-aware audio: Partial (announce + manual routing hook), missing auto-bind and multi-track
- Unified data messaging: Pending (no topic/seq/timestamp header yet)
- Resiliency/UX: Good progress (auto-reconnect, negotiated channel CVar)
- Docs: Added and aligned with current scope

## Implemented Details

### Backend-Agnostic Connector Interface
- `IWebRTCConnector` (Public) with lifecycle, data, and audio APIs plus `OnRemoteAudio` delegate.
- `CreateWebRTCConnector(...)` factory returns a libdatachannel-backed adapter today:
 - `FLibDataChannelAdapter` wraps `FWebRTCConnector` and adapts types (PCM16<->float), emits `o3ds.audio.announce` JSON once per stream.
- LiveKit connector not implemented; factory returns null for non-LibDataChannel backends.

Files:
- `Plugins/Open3DStream/Source/Open3DStream/Public/IWebRTCConnector.h`
- `Plugins/Open3DStream/Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`

### LibDataChannel + Opus (Send/Receive)
- Send path:
 - `FWebRTCConnector::EnableAudioSend` configures Opus encoder (48 kHz,1–2 ch), sets frame size (default20 ms), adds an Opus audio `rtc::Track`, renegotiates on client.
 - `PushAudioPCM16` chunks interleaved PCM16 into frame-sized buffers, encodes with `opus_encode`, builds `rtc::binary`, and sends frames with RTP `FrameInfo` timestamp (48 kHz clock, increment by frame size).
- Receive path:
 - `PeerConnection->onTrack` filters audio, `onFrame` decodes with `opus_decode` into PCM16 (worst-case buffer sized for120 ms), queues to game thread.
 - `Tick()` dispatches decoded PCM via `AudioRxCallback` and broadcasts a subject-aware event using stored labels.
- Guarded with `O3DS_WITH_OPUS` and `!O3DS_OPUS_NO_HEADER`; includes `<opus.h>`.

Files:
- `Plugins/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.h/.cpp`

### Data Channel
- Unified send for now (`SendDataReliable/Lossy` both call `FWebRTCConnector::Send`).
- Negotiated channel support via CVars:
 - `o3ds.Broadcast.WebRTC.NegotiatedChannel` (0/1)
 - `o3ds.Broadcast.WebRTC.ChannelId`

### Resiliency and Timers
- Auto re-offer/reconnect backoff CVars and timers:
 - `o3ds.Broadcast.WebRTC.AutoReconnect`
 - `o3ds.Broadcast.WebRTC.BackoffInitialMs`
 - `o3ds.Broadcast.WebRTC.BackoffMaxMs`
- Collision/re-offer handling on the client.

## Subject-Aware Audio Status
- Announce: Adapter sends minimal `o3ds.audio.announce` JSON on first enable of a unique `StreamLabel`.
- Routing: `FWebRTCConnector` exposes `SetRxAudioRouting(StreamLabel, SubjectName)`; broadcast uses these labels on RX.
- Pending:
 - Label the outbound audio track using `StreamLabel` (currently the SDP m-line is named `"audio"`).
 - Parse `o3ds.audio.announce` on RX to auto-bind subject routing (currently requires higher-layer call).
 - Multiple audio tracks (mix + per-actor), separate encoders and track instances.

## Unified Data Messaging Status
- Pending application-level header (topic/seq/timestamp, optional subject/stream) for both reliable and lossy paths.
- Lossy drop policy (queue max depth/time) not implemented.
- Reliable vs lossy DataChannel behavior (ordered/unordered/partial reliability) not configured yet.

## LiveKit Backend Status
- Not implemented; `CreateWebRTCConnector` returns null for LiveKit backend.
- Build flag `O3DS_ENABLE_LIVEKIT` not yet wired.
- Future work to map: reliable/lossy data topics, publish/subscribe audio with track labels, and identical application headers.

## Build and Flags
- Ensure libdatachannel is built with media + Opus support; see `BUILD_LIBDATACHANNEL_MEDIA.md`.
- UE build guards: `O3DS_WITH_OPUS`, `O3DS_OPUS_NO_HEADER` determine availability of encoder/decoder in code.

## Gaps and Next Steps
1) LiveKit connector stub behind `O3DS_ENABLE_LIVEKIT` and config plumbed from UI.
2) Add application header (topic/seq/ts) and lossy drop policy; size limit guidance (<15 KB).
3) Label audio track with `StreamLabel` and parse/auto-bind RX based on `o3ds.audio.announce`.
4) Multi-track audio support (mix + per-actor), per-track Opus encoders, and per-source gain.
5) Wire DTX (OPUS_SET_DTX) and expose codec params; confirm FEC/complexity tuning.
6) UI: backend selector, LiveKit config, subject-aware audio options.
7) Tests: smoke-webrtc P2P audio, announce routing, data message headers; later smoke-livekit.

## References
- Planning docs under `plugins/unreal/Open3DStream/docs/M3.4.1a_Docs/`:
 - WEBRTC_BACKENDS.md
 - WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md
 - WEBRTC_CONNECTOR_INTERFACE.md
 - LIVEKIT_DATA_MESSAGING.md
 - UE_AUDIO_COMPONENTS.md
 - BUILD_LIBDATACHANNEL_MEDIA.md

