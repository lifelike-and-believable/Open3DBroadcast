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

## Acceptance Criteria
We can:
- Configure audio publish to meet bandwidth/quality targets (bitrate/DTX/stereo).
- Receive PCM at a predictable format or be notified on format changes.
- React to connection state transitions for reconnection UX.
- Tag data sends with label/ordering and observe payload limits via errors.
- Query lightweight metrics for audio/data to aid tuning.
- Optionally refresh token or set backoff, per SDK feasibility.

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