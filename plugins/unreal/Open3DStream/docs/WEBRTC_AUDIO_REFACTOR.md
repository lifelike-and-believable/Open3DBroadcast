# Open3DStream WebRTC Audio Path Refactor Plan

Owner: @atgoldberg  
Date: 2025-10-30  
Scope: Unreal Engine plugin (Open3DStream) – WebRTC send/receive audio over libdatachannel (Opus)  
Status: Draft – ready for conversion into planning issues

## 1) Executive Summary

We will simplify and stabilize the WebRTC audio path by:
- Centralizing all negotiation (PeerConnection, SDP, DataChannel, tracks) in the connector.
- Making the audio capture component a “dumb” PCM source, not a participant in connection setup.
- Enforcing a deterministic order of operations: configure audio → create PeerConnection → add audio track → create DataChannel → create offer.
- Removing hidden/masked failures so ordering mistakes fail loudly and early.

This aligns the plugin with the 300-line libdatachannel sample that works reliably.

Opus is present and not the blocker; the main obstacle is ordering, hidden errors, and cross‑component responsibilities.

---

## 2) Background and Problem Statement

Symptoms observed:
- DataChannel opens, but audio track does not open or is missing from SDP (no `m=audio`).
- PCM frames are pushed but never encoded/sent (pending buffer grows or stays near zero send rate).
- Debugging is hard due to responsibilities spread across multiple components and success masking.

Root causes:
- Audio enablement can be called after the PeerConnection is already created, so no audio track is added to the initial offer.
- The adapter returns success even when `EnableAudioSend` is rejected; the system believes audio is enabled but it isn’t.
- The audio capture component participates in negotiation/connector wiring, creating fragile ordering and coupling.

Why the small sample works:
- It always adds the audio track BEFORE creating the DataChannel and BEFORE creating the offer.
- It sends properly encoded frames once the track is open.

---

## 3) Goals and Non‑Goals

Goals:
- Deterministic and minimal setup: audio config before PC exists; track before DC; offer after both.
- Single owner of negotiation logic: `FWebRTCConnector`.
- Simple audio capture component: captures PCM and forwards to a sink; never negotiates or knows about WebRTC state.
- Strict, visible errors: configuration errors never silently succeed.
- Clear diagnostics: single status dump and a handful of must‑have logs.

Non‑Goals:
- Changing protocol formats or transport semantics beyond audio.
- Adding new features to signaling or remote audio routing.
- Replacing libdatachannel or Opus.

---

## 4) Target Architecture

Responsibilities:
- UO3DSBroadcastComponent
  - Computes StreamLabel (based on SubjectName and mix/mic mode).
  - Calls `connector->EnableAudioSend(config)` before any PeerConnection is created.
  - Starts/stops the transport.
  - Wires the audio capture component with a sink that calls `connector->PushPcm(...)`.

- FO3DSWebRtcTransport
  - Owns an `IWebRTCConnector` (backed by `FWebRTCConnector`).
  - Exposes: PrepareChannel (create connector only), Start (connect/start PC), Stop, Tick.

- FWebRTCConnector (libdatachannel)
  - Sole owner of `PeerConnection`, `DataChannel`, audio `Track`.
  - Adds audio track before creating DataChannel; then creates the SDP offer.
  - Encodes PCM via Opus and sends `sendFrame` on the audio track.
  - Decodes incoming audio and emits `OnRemoteAudio`.
  - Provides status and `LastError`.

- UO3DSBroadcastAudioCaptureComponent
  - Pure PCM source: taps mic/submix and forwards through a provided sink.
  - No connector fields, no negotiation logic.

---

## 5) Lifecycle and Order of Operations (contract)

1. Prepare (no PeerConnection yet)
   - Create internal transport and obtain connector (PrepareChannel).
   - Compute StreamLabel in BroadcastComponent.
   - Call `EnableAudioSend(config)` on connector with StreamLabel. If false, abort and surface error.

2. Wire capture source
   - Create/update the audio capture component.
   - Set its StreamLabel and an AudioSink that calls `connector->PushPcm(...)`.

3. Start transport
   - StartInternalTransport → connector->Start creates the PeerConnection.
   - Connector’s `SetupPeerConnection`:
     - Add Opus audio track (from stored config).
     - Create DataChannel.
     - Create offer and set local/remote descriptions (via signaling).

4. Run
   - Once both descriptions are set and ICE connects, track opens.
   - Buffered PCM is encoded and sent; sends continue in real time.

5. Stop
   - Close DataChannel and audio track, then PeerConnection.
   - Clear pending buffers.

Invariant: “EnableAudioSend must be called before Start()” (hard requirement).

---

## 6) Detailed Changes by Component

### 6.1 FLibDataChannelAdapter (WebRTCConnectorFactory.cpp)
- Problem: masks failures; caches “enabled” even if inner rejected.
- Changes:
  - Return the real result of `Inner->EnableAudioSend(A)`.
  - Only add to `EnabledStreams` and emit announce when `bOk == true`.
  - Improve logs: print SUCCESS/FAILED with the stream label.

Acceptance:
- Calling `EnableAudioSend` after Start returns false and logs a precise reason.
- Planning agent should generate a task to implement and unit test this behavior.

### 6.2 UO3DSBroadcastAudioCaptureComponent
- Problem: owns connector reference and tries to negotiate (EnsureConnector/EnableAudioSend), creating timing hazards.
- Changes:
  - Remove connector reference and all negotiation/setup calls.
  - Add API:
    - `void SetStreamLabel(const FString& InLabel)`
    - `void SetAudioSink(TFunction<bool(const FString&, const float*, int32, int32, int32, double)> InSink)`
  - `PushFrames` calls the sink if present; otherwise no-op.
  - Keep mic/submix tap setup and teardown.

Acceptance:
- The component compiles and runs without IWebRTCConnector includes or calls.
- In logs, the component never prints negotiation/setup messages.

### 6.3 UO3DSBroadcastComponent
- Problem: splits responsibility with the audio component.
- Changes (in `StartCapture()`):
  1) Create internal transport (PrepareChannel). Retrieve the connector (no PC yet).
  2) Compute StreamLabel once:
     - Mix: `o3ds:mix` or `o3ds:subject/<Subject>`
     - Mic: `o3ds:mic` or `o3ds:mic/<DeviceName>`
  3) Call `connector->EnableAudioSend(cfg)` with label and properties (SR, channels, bitrate).
     - If returns false: abort and report `GetLastError()`.
  4) Configure/create `UO3DSBroadcastAudioCaptureComponent`:
     - Set capture parameters, `SetStreamLabel(StreamLabel)`, and `SetAudioSink([Conn] { return Conn->PushPcm(...); })`.
  5) StartInternalTransport (PeerConnection is created; connector adds track then DC then offer).
- Remove any injection of connector into the audio component.

Acceptance:
- With audio enabled, the local SDP contains `m=audio` and `a=rtpmap:111 opus`.
- `o3ds.WebRTC.Audio.Status` shows:
  - `AudioEnabled=1`, `TrackPresent=1`, `TrackOpen=1`.
- `SentPackets` increases when mic/submix produces audio.

### 6.4 FWebRTCConnector
- Keep and enforce ordering:
  - In `SetupPeerConnection()`: add audio track first; DataChannel second; then offer.
- Maintain `SetupAudioTrackAndHandlers(AudioRt.Config, PeerConnection)` and set `mediaHandler` chain (OpusRtpPacketizer, RTCP SR, NACK).
- `EnableAudioSend`:
  - Must be called before PC exists; otherwise log error and return false.
- `PushAudioPCM16`:
  - Buffer up to ~250ms pending; return true when buffering due to not-ready states.
  - Return false only on hard error (e.g., Opus encoder missing).
  - Increment RTP timestamp in 48kHz clock consistent with frame size.

Acceptance:
- If `EnableAudioSend` is called too late, it returns false and logs “[CONNECTOR] EnableAudioSend must be called before Start()”.
- When called in time, and Start is invoked, the local SDP always has `m=audio`.
- Track `onOpen` fires on connect, and `SentPackets` increments.

### 6.5 FO3DSWebRtcTransport
- Ensure `PrepareChannel()` does NOT create/start the PeerConnection.
- `Start(url, proto, key)` calls `connector->Start()` which creates and negotiates PC.

Acceptance:
- Before Start, `FWebRTCConnector::GetAudioSendStatus()` reports `bLocalDesc=false`, `bRemoteDesc=false`, `bAudioTrackPresent` depends only on stored config (track not yet created).
- After Start, local SDP includes `m=audio` when audio was enabled prior to Start.

---

## 7) Public API Contracts

IWebRTCConnector (subset used here):
- `bool EnableAudioSend(const FAudioSendConfig& cfg)`  
  Must be called before `Start`. Returns false otherwise; never masked.

- `bool Start(const FString& url, bool bIsServer)`  
  Creates PeerConnection, triggers offer/answer flow.

- `bool PushPcm(const FString& StreamLabel, const float* interleaved, int32 numFrames, int32 numChannels, int32 sampleRate, double tsSec)`  
  Converts to int16 and forwards to `PushAudioPCM16`; returns true on accept/buffer/send; false only on hard failure.

- `FString GetLastError() const`  
  Clear on success transitions (e.g., when PC reaches Connected).

Audio capture component new API:
- `void SetStreamLabel(const FString&)`
- `void SetAudioSink(TFunction<bool(const FString&, const float*, int32, int32, int32, double)>)`

---

## 8) Diagnostics and Logging

Must‑have logs:
- On adapter EnableAudioSend: “Inner->EnableAudioSend(<label>) -> SUCCESS/FAILED”.
- On connector:
  - “Adding audio track BEFORE datachannel (StreamLabel='<label>')”
  - Local SDP checks:
    - “Local SDP has no m=audio” (warning)
    - “Local SDP includes Opus PT=111” (verbose)
  - Peer state transitions (Connected/Disconnected/Failed)
  - Track opened/closed with MID
  - `sendFrame` exceptions (warning)

Console/status:
- `o3ds.WebRTC.Audio.Status` prints:
  - Signaling/peer states
  - `LocalSDP.m=audio`, `RemoteSDP.m=audio`
  - Local/remote audio directions and Opus PT presence
  - `AudioEnabled`, `TrackPresent`, `TrackOpen`, `OpusReady`
  - `StreamLabel`, `PendingSamples`, `SentPackets`, `SentBytes`
  - `LastError` if non-empty

CVars for bring‑up:
- `o3ds.WebRTC.Audio.Debug`, `o3ds.WebRTC.Verbose`, `o3ds.WebRTC.DebugRx`
- Optional: temporarily disable auto reconnect/reoffer during stabilization.

---

## 9) Implementation Plan (Tasks for the planning agent)

Phase A – Unmask and contain
1. Adapter: propagate real EnableAudioSend result
   - Modify `FLibDataChannelAdapter::EnableAudioSend` to return inner result and only cache/announce on success.
   - Unit test: inner rejects when PC exists → adapter returns false.
   - Acceptance: status shows `AudioEnabled=0` when rejected; logs show FAILED.

2. Stop negotiation from audio capture component
   - Remove connector fields, `EnsureConnector`, `SetConnector`, and all negotiation calls from `UO3DSBroadcastAudioCaptureComponent`.
   - Add `SetStreamLabel` and `SetAudioSink` APIs; `PushFrames` forwards to sink.
   - Acceptance: component compiles without IWebRTCConnector; no negotiation logs from this class.

Phase B – Centralize and order
3. BroadcastComponent: wire audio before start
   - In `StartCapture()`:
     - Create internal transport (PrepareChannel), get connector (no PC yet).
     - Compute StreamLabel and call `EnableAudioSend(cfg)` (assert/handle false).
     - Create/update AudioCapture component; set label and sink to call `connector->PushPcm`.
     - Start internal transport (PeerConnection created now).
   - Acceptance: local SDP has `m=audio` and Opus 111 when audio enabled.

4. Connector ordering and strict contract
   - Enforce `EnableAudioSend` must be before Start; return false otherwise.
   - In `SetupPeerConnection()`: add audio track first; create DataChannel; then create offer.
   - Keep bounded buffer and timestamp progression in `PushAudioPCM16`.
   - Acceptance: Track opens and `SentPackets` > 0 in normal flow; rejected when called late.

Phase C – Diagnostics and tests
5. Logging polish + status
   - Ensure “Local SDP has no m=audio” warns once; Opus PT=111 log.
   - Confirm `o3ds.WebRTC.Audio.Status` prints all fields correctly.

6. Test matrix
   - Localhost, non‑negotiated DataChannel (default), no STUN.
   - Both roles (client/server).
   - Mix vs Mic capture mode.
   - Optional: with STUN (Google) as follow‑up.
   - Acceptance: in each case, `m=audio=1`, `TrackOpen=1`, `SentPackets` increasing.

Phase D – Cleanup
7. Remove dead code and comments related to the old flow.
8. Document the new contract: “EnableAudioSend BEFORE Start.”

---

## 10) Acceptance Criteria (end‑to‑end)

- When audio is enabled:
  - Local and remote SDPs both contain `m=audio` and Opus PT=111.
  - The audio track opens on both ends.
  - The connector status reports: `AudioEnabled=1`, `TrackPresent=1`, `TrackOpen=1`.
  - `SentPackets` and `SentBytes` increase over time while audio is active.
- When `EnableAudioSend` is intentionally called after `Start`:
  - It returns false with log: “EnableAudioSend must be called before Start()”.
  - No audio track is added; status shows `AudioEnabled=0`.
- The audio capture component does not mention negotiation in logs and only reports capture/forwarding.

---

## 11) Risks and Mitigations

- Risk: Hidden dependencies rely on audio component calling EnsureConnector.
  - Mitigation: Search and remove usages; add compile‑time breaks if methods are missing.
- Risk: Behavior changes in negotiated channel mode.
  - Mitigation: Keep default non‑negotiated for bring‑up; add explicit test for negotiated mode later.
- Risk: Reoffer/reconnect backoff interferes during stabilization.
  - Mitigation: Gate via CVars; default off during initial testing.

---

## 12) Rollout Plan

- PR 1: Adapter fix + unit test.
- PR 2: Audio capture component API (pure source) + BroadcastComponent wiring changes.
- PR 3: Connector contract enforcement and ordering hardening + logging/status polish.
- PR 4: Test harness updates, docs, and cleanup.

Each PR should compile and run; enable audio locally to verify SDP and track open.

---

## 13) Estimates

- PR 1: 0.5 day
- PR 2: 1 day
- PR 3: 1 day
- PR 4: 0.5 day
Total: ~3 days (buffered), with improvements visible after PR 1 and PR 2.

---

## 14) Open Questions

- Should we default negotiated channels to off in production for parity with the sample? (Recommendation: yes)
- Any existing consumers relying on the audio component owning connector references? (If yes, provide a compatibility shim, but prefer removal.)

---

## 15) References

- Working minimal sample (libdatachannel) showing correct order (add track → DC → offer):  
  [examples/audio-comm-test/client.cpp](https://github.com/lifelike-and-believable/libdatachannel/blob/b00f8ede37a65905eb78ebb90dfd21832885f980/examples/audio-comm-test/client.cpp)

- Current plugin code (to be refactored):
  - Broadcast audio capture component (will become a pure PCM source):  
    [O3DSBroadcastAudioCaptureComponent.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp)
  - WebRTC connector (negotiation owner):  
    [WebRTCConnector.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.cpp)
  - Broadcast component (will own StreamLabel, call EnableAudioSend before Start):  
    [O3DSBroadcastComponent.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp)
  - Connector factory/adapter (stop masking failures):  
    [WebRTCConnectorFactory.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnectorFactory.cpp)

---
