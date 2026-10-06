# LiveKit FFI Enhancements for Game-Engine Streaming (O3DS Integration)

## Summary
- **Ask:** Expand the C ABI to better support real-time mocap + audio streaming from Unreal, with robust lifecycle, audio configuration, diagnostics, and data-channel control.
- **Why:** We are integrating LiveKit as an alternative backend alongside a P2P libdatachannel path. LiveKit should expose PCM audio at the edges, reliable/lossy data, strong reconnection semantics, and actionable metrics — all without blocking the game thread.
- **Scope:** C ABI additions with minimal surface changes, preserving current functions. Optional Unreal-side wrappers can come later.

## Current Wrapper (Baseline)
- **Audio:** Publish via `lk_publish_audio_pcm_i16()`. Receive via `lk_client_set_audio_callback()` with interleaved i16 frames + sr/ch/fpc. Good.
- **Data:** Send via `lk_send_data()` with `LkReliability { Reliable, Lossy }`. Receive via `lk_client_set_data_callback()`. Good.
- **Connect:** `lk_connect()` and `lk_connect_with_role(url, token, LkRole)`. `lk_client_is_ready()` available. Good.

## Feature Requests

### 1) Audio Publish Configuration
Goal: Control encode behavior to fit engine constraints and network budgets.

Proposed API:
```c
LkResult lk_set_audio_publish_options(
  LkClientHandle*,
  int32_t bitrate_bps,
  int32_t enable_dtx,
  int32_t stereo
);
```
Notes:
- Defaults: 48 kHz mono, DTX off, bitrate e.g. 24–48 kbps configurable.
- Stereo toggle optional (mono default). If `stereo=1`, clarify interleaving and downmix behavior.

### 2) Audio Subscribe Output Preferences
Goal: Prefer a stable output format or be notified of source format changes.

Option A — Fixed downstream format (resample/downmix inside FFI):
```c
LkResult lk_set_audio_output_format(LkClientHandle*, int32_t sample_rate, int32_t channels);
```
Option B — Notify on format changes (resample in engine):
```c
LkResult lk_set_audio_format_change_callback(
  LkClientHandle*,
  void(*cb)(void* user, int32_t sample_rate, int32_t channels),
  void* user
);
```
Notes:
- We’re fine with 48k/mono everywhere; Option A is simplest for engines.

### 3) Connection and Lifecycle Callbacks
Goal: Drive engine UI/state and reconnection logic without polling.

Proposed API:
```c
typedef enum {
  LkConnConnecting=0,
  LkConnConnected=1,
  LkConnReconnecting=2,
  LkConnDisconnected=3,
  LkConnFailed=4
} LkConnectionState;

LkResult lk_set_connection_callback(
  LkClientHandle*,
  void(*cb)(void* user, LkConnectionState state, int32_t reason_code, const char* message),
  void* user
);
```
Notes:
- Map Room events: ConnectionStateChanged, Disconnected(reason), TokenError, etc.
- Document threading: callbacks may occur on background threads; never block internally.

### 4) Data Channel Enhancements
Goal: Align with WebRTC capabilities; keep Reliable/Lossy defaults simple.

Proposed API:
```c
LkResult lk_send_data_ex(
  LkClientHandle*,
  const uint8_t* bytes,
  size_t len,
  LkReliability reliability,
  int32_t ordered,
  const char* label
);

LkResult lk_client_set_data_callback_ex(
  LkClientHandle*,
  void(*cb)(void* user, const char* label, LkReliability reliability, const uint8_t* bytes, size_t len),
  void* user
);

LkResult lk_set_default_data_labels(LkClientHandle*, const char* reliable_label, const char* lossy_label);
```
Notes:
- Size guidance: lossy ≤ ~1300 B, reliable ≤ ~15 KiB. We will enforce client-side; wrapper can return error if exceeded.

### 5) Diagnostics and Metrics
Goal: Observe health without blocking; useful for tuning/support.

Proposed API:
```c
typedef enum { LkLogError=0, LkLogWarn=1, LkLogInfo=2, LkLogDebug=3, LkLogTrace=4 } LkLogLevel;
LkResult lk_set_log_level(LkClientHandle*, LkLogLevel);

typedef struct {
  int32_t sample_rate;
  int32_t channels;
  int32_t ring_capacity_frames;
  int32_t ring_queued_frames;
  int32_t underruns;
  int32_t overruns;
} LkAudioStats;
LkResult lk_get_audio_stats(LkClientHandle*, LkAudioStats* out_stats);

typedef struct {
  int64_t reliable_sent_bytes;
  int64_t reliable_dropped;
  int64_t lossy_sent_bytes;
  int64_t lossy_dropped;
} LkDataStats;
LkResult lk_get_data_stats(LkClientHandle*, LkDataStats* out_stats);
```

### 6) Reconnection and Token Refresh Hooks
Goal: Client-first reconnection with optional token refresh.

Proposed API:
```c
LkResult lk_set_reconnect_backoff(LkClientHandle*, int32_t initial_ms, int32_t max_ms, float multiplier);
LkResult lk_refresh_token(LkClientHandle*, const char* token); // if SDK permits
```
Notes:
- If token refresh is unsupported, document best practice (disconnect + reconnect).
- Connection callback should signal transitions (Reconnecting, Disconnected, Connected).

### 7) Role Management (Optional)
Goal: Allow role changes without full reconnect (if SDK supports).

Proposed API:
```c
LkResult lk_set_role(LkClientHandle*, LkRole role, int32_t auto_subscribe);
```
Notes:
- If dynamic role switching is not viable, a documented “not supported” is fine.

### 8) Error Taxonomy and Messages
Goal: Actionable errors with stable codes; free strings via `lk_free_str`.

Proposed:
- Keep `LkResult { code, message }`, add published error code ranges:
  - 1xx: Connect/Token; 2xx: Data send; 3xx: Audio publish; 4xx: Lifecycle; 5xx: Internal.
- Ensure all error messages are allocated consistently and freed by caller.

### 9) Threading and Callback Guarantees
Goal: Deterministic behavior in engines.

Requests:
- Document that callbacks may occur on background threads and won’t block internally.
- Reentrancy: API calls are safe from non-callback threads while callbacks may be in-flight (or document locking requirements).
- Shutdown guarantees: After `lk_disconnect()`, no callbacks will fire once it returns (or provide a quiesce call).

### 10) Backward Compatibility
- Keep existing functions/behavior intact.
- All new APIs are optional; defaults leave current behavior unchanged.

### 11) Participant Events (requested 2026-10)
Goal: let a sender react when a receiver joins the room. Open3DBroadcast sends a full pose descriptor ("full sync") immediately when a new peer appears, so a late joiner does not wait up to a second for the periodic one (ADR 0005, item (vi)). The TCP and NNG transports already do this; WebRTC cannot, because `livekit_ffi.h` exposes connection-state, data and audio callbacks but no participant events.

Checked against `livekit_ffi` `main` (50e6663, 0.3.0) and the pinned SDK `livekit` 0.7.24:
- The SDK has the events: `RoomEvent::ParticipantConnected(RemoteParticipant)` and `RoomEvent::ParticipantDisconnected(RemoteParticipant)` (`livekit/src/room/mod.rs:89-90`, dispatched at `:1009` and `:1715`).
- Both event loops in `backend_livekit.rs` (sync connect `:632`, async connect `:816`) drop them in their catch-all arm (`other =>`, `:738`, `:885`).
- Participants already in the room at connect are **not** announced: `Room::connect` creates them from `join_response.other_participants` without dispatching an event (`livekit/src/room/mod.rs:637-649`). They are available from `room.remote_participants()` (`:732`).

Proposed API:
```c
typedef enum { LkParticipantJoined = 0, LkParticipantLeft = 1 } LkParticipantEvent;

typedef void (*LkParticipantCallback)(void* user, LkParticipantEvent event,
                                      const char* identity, const char* name);

LkResult lk_set_participant_callback(LkClientHandle*, LkParticipantCallback cb, void* user);
```
Implementation sketch: in both event loops, map `ParticipantConnected`/`ParticipantDisconnected` to the callback (identity from `participant.identity()`, name from `participant.name()`). Right after `Room::connect` succeeds, before the loop starts, report every entry of `room.remote_participants()` as `LkParticipantJoined`, so a sender that connects after its receivers behaves the same as one that connects first.

Notes:
- Remote participants only; the local participant is not reported.
- `identity` and `name` are valid only during the callback (the caller copies them); `name` may be empty.
- Same threading rules as the other callbacks (section 9): may fire on a background thread, never blocks, and does not fire after `lk_disconnect()` returns.

### 12) Data Delivery Contract (documentation, and two fixes)
Goal: confirm the delivery contract our residual (delta) coding relies on. We only send frames that depend on earlier frames over a channel that is reliable *and* ordered.

What the source shows (`livekit_ffi` 50e6663, `livekit` 0.7.24):
- Every `lk_send_data`/`lk_send_data_ex` call, `LkReliable` or `LkLossy`, is sent as a byte stream (`stream_bytes`, `backend_livekit.rs:1335-1342`), and byte streams always use `Reliable` data packets (`livekit/src/room/data_stream/outgoing.rs:199-230`) on the publisher's reliable data channel, which is created `ordered: true` (`livekit/src/rtc_engine/rtc_session.rs:455-457`). So **`LkLossy` is in fact reliable**, and the `ordered` parameter of `lk_send_data_ex` is ignored (`_ordered`, `:1271`).
- A send runs synchronously (`block_on`) while holding the client lock, and a failed attempt is retried once after 100 ms (`:1328-1357`). While the room is reconnecting, the SDK's `publish_data` waits for the reconnection (`livekit/src/rtc_engine/mod.rs:249-259`), so the caller's thread blocks for the whole reconnect.
- The receiving loop reads each stream to the end before handling the next event (`read_all().await` inside the loop, `:634-658`, `:818-826`), so callbacks arrive in the order the streams were opened.

Requests:
1. **Document** that reliable data from one sender reaches each receiver in send order without loss while the session stays up, and what happens to data in flight when the SDK replaces the session during a full reconnect (delivered, lost, or reported).
2. **Honor `LkLossy`** (send unreliable data packets with `publish_data` instead of a byte stream) or document that it is reliable. Today a "lossy" sender pays reliable-channel latency under loss.
3. **Don't block the caller during reconnects:** return an error (for example a new 2xx code, "reconnecting") instead of waiting inside `lk_send_data_ex` while holding the client lock. A game-thread caller freezes for the length of the reconnect, and every other FFI call on that client waits for the lock.

### 13) Bug: the async connect path never calls the extended data callback
`lk_connect_async`/`lk_connect_with_role_async` start an event loop that delivers data only to the plain `data_cb` (`backend_livekit.rs:818-826`); the sync loop prefers `data_cb_ex` and falls back to `data_cb` (`:642-655`). The async loop has done this since it was added (3a0a610, 2025-11-05); `data_cb_ex` was added to the sync loop only (da50d08, 2025-11-19). A client that connects asynchronously and registers only `lk_client_set_data_callback_ex` receives no data at all. Open3DBroadcast's receiver does exactly that (it connects with `lk_connect_with_role_async`).

Request: make the async loop identical to the sync one (extended callback with the topic as the label, falling back to the plain callback), ideally by sharing one event-loop function between both connect paths so they cannot drift again; the async loop also lacks the sync loop's topic label and logging.

## Acceptance Criteria
We can:
- Configure audio publish to meet bandwidth/quality targets (bitrate/DTX/stereo).
- Receive PCM at a predictable format or be notified on format changes.
- React to connection state transitions for reconnection UX.
- Tag data sends with label/ordering and observe payload limits via errors.
- Query lightweight metrics for audio/data to aid tuning.
- Optionally refresh token or set backoff, per SDK feasibility.
- Be told when a remote participant joins or leaves the room, including those present at connect (section 11); rely on a documented order and loss contract for reliable data, send lossy data unreliably, and not block during reconnects (section 12); receive labeled data on the async connect path (section 13).

No regressions: current publisher/subscriber flows remain functional.

## Non-Goals
- Exposing raw Opus/RTP payloads — unnecessary; LiveKit should abstract that.
- Complex fragmentation/FEC in the wrapper — we’ll handle fragmentation client-side when needed.

## Compatibility & Deployment
- **Platform:** Windows x64 first (UE 5.6). Linux/macOS later.
- **CRT:** Keep dynamic (/MD) by default to match UE runtime.
- **Artifacts:** Continue shipping headers, staticlib, and `livekit_ffi.dll` (if present) in predictable locations.

## Open Questions
- Token refresh: Does the SDK allow updating JWT at runtime without reconnect?
- Role switching: Can we change role and `auto_subscribe` dynamically?
- Audio resample/downmix: Prefer doing it inside FFI for consistency; happy to do it engine-side if you’d rather keep FFI minimal.

## Thank You
These additions would let us offer LiveKit as a first-class, scalable backend in O3DS with strong UX, while preserving the wrapper’s minimal API surface and backward compatibility. We’re happy to adapt naming/signatures to fit your conventions.