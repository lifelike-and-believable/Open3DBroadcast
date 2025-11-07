# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- New Unreal shared module: `Open3DShared` providing:
  - WebRTC abstractions and implementations (connector, data channel, signaling)
  - Shared loopback registry for serialized frames (decouples Broadcast from Stream)
  - Logging categories and console variables
  - Helper utilities (subject name sanitize, wildcard matching, URL parsing/normalization)
  - Consolidated third-party linking (libdatachannel, OpenSSL, Opus)
 - LiveKit SFU backend for WebRTC: operational for Broadcaster (Publisher) and Live Link Source (Subscriber)

### Changed
- `Open3DBroadcast` no longer depends on `Open3DStream`; audio capture component moved into Broadcast.
- `Open3DStream` registers a LiveLink-backed consumer via the shared loopback registry and now depends on `Open3DShared`.
- Built-in transports (e.g., NNG) use shared URL helpers; common tcp URL typos like `tcp://0.0.0.0.9000` are auto-normalized to `tcp://0.0.0.0:9000`.
 - Unified WebRTC configuration via `FO3DSWebRtcConfig`; backend-specific URL semantics are handled inside connectors. External URL `role=` and backend hints removed.
 - LiveKit connector: realtime strategy updated (single lossy unordered stream + forward-paced send scheduling). Removed size-based reliability switching and ordered lossy mode to eliminate head-of-line stalls causing freeze/speed-up cycles.
 - Token moved under the WebRTC section for both sender and receiver; backend-specific token hint surfaced in UI (e.g., LiveKit JWT guidance).
 - LiveLink receiver stability: marshal status/data to game thread and guard async dispatch with weak pointers; safe subject removal; remove remove-then-push during static refresh.
 - LiveKit teardown stability: unregister FFI callbacks before disconnect/destroy to avoid late callbacks into freed objects.
 - LibDataChannel backend encapsulation: backend-specific URL shaping and defaults moved into `LibDataChannelConnector`.
   - Server URL normalized to `ws://host/<room>`; `room=` and `role=` query stripped by connector.
   - Client signaling targets `id=<room>` (fallback `server`), server continues `id=client`.
   - Receiver no longer mutates LibDC URLs; passes through config to connector.

### Fixed
- Restored Broadcaster (Client) ↔ Live Link (Server) interop for LibDataChannel after URL handling refactor; removed duplicate/incorrect `room` handling and path mismatches.

### Compatibility
- No protocol/schema changes.
- Module boundaries changed in the Unreal plugin; consumer projects including headers from these modules should include from `Open3DShared` where applicable.

### Notes
- Consider future UI change to split Broadcast endpoint into separate Address/Port fields to avoid formatting mistakes; current URL field auto-corrects common tcp host/port typos.
 - WebRTC reliability modes are handled internally; typical users no longer need URL flags. Advanced reliability control is available via APIs where applicable.
