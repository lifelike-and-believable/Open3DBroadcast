# LiveKit Data Messaging vs DataChannel

This document captures the considerations when mapping Open3DStream’s data path from WebRTC DataChannels to LiveKit’s data messages.

## Model differences

- DataChannel (P2P):
  - Arbitrary channels
  - Per-channel ordered/unordered, reliable/unreliable
  - Exposes `bufferedAmount` for backpressure

- LiveKit data:
  - Two message kinds: reliable (ordered) and lossy (unordered, may drop)
  - Topic-based multiplexing (string topic)
  - No per-channel bufferedAmount API

## Recommended mapping

- Topics:
  - `o3ds.anim` → lossy
  - `o3ds.ctrl` → reliable
  - `o3ds.audio.announce` → reliable
- Message header: include `{topic, v, seq, ts, subject?, stream?}`
- Size target: <= 15 KB
- Cadence: 30–60 Hz for anim (decimate/coalesce if faster)
- Backpressure:
  - Lossy queue <= 2 frames; drop oldest on overflow
  - Reliable queue strict budget; coalesce non-critical messages

## Targeting

- By default broadcast in-room
- If you need peer-specific messages later, include destination SIDs

## Testing

- Simulate loss (1–10%) and verify:
  - anim remains smooth via drop policy
  - control traffic is not starved
- Instrument queue depth and E2E latency
- Multi-participant: verify topic routing and subscription behavior