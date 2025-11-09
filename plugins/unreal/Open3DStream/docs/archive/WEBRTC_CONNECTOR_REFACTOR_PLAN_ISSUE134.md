# WebRTC Connector Refactor Plan (Issue #134)

This document records the implementation plan to refactor Open3DStream’s Unreal plugins to use a backend-agnostic WebRTC connector interface, with a reference libdatachannel implementation, and to integrate it into the broadcaster (sender) and receiver paths.

References:
- Issue: https://github.com/lifelike-and-believable/Open3DStream/issues/134
- Test pattern to follow:
  - `Open3DBroadcast/Private/O3DSTestLibDCClientComponent.cpp` (offerer with DC + audio send)
  - `Open3DStream/Private/O3DSTestLibDCServerComponent.cpp` (answerer with DC + audio recv)
- Build/link: WebRTC libraries are already linked via `Open3DShared.Build.cs`

### Note on build/test sandbox paths

The Editor builds and tests use `ProjectSandbox`, where `ProjectSandbox/Plugins/Open3DStream` is a symbolic link to this repository’s `plugins/unreal/Open3DStream` folder. As a result, compiler and linker diagnostics will commonly reference paths under `ProjectSandbox/Plugins/Open3DStream/...` rather than the repository root. When triaging errors, map those paths back to the corresponding files in `plugins/unreal/Open3DStream/...` within this repo.

---

## Goals and acceptance criteria

- Encapsulate all WebRTC signaling, SDP/candidate exchange, transport, and state in a Shared-layer interface `IWebRTCConnector`.
- Implement `LibDataChannelConnector` as the reference backend.
- Provide a `WebRTCConnectorFactory` for runtime backend selection (LibDataChannel now; LiveKit future-ready).
- Refactor the broadcaster (`UO3DSBroadcastComponent`) and receiver (`FOpen3DStreamSource` path) so they do not perform low-level WebRTC operations or include libdatachannel directly.
- Milestone 1: End-to-end connectivity established
  - DataChannel opens and can exchange a test message between broadcaster and receiver.
  - Audio track negotiates; RTP frames observed on receiver.
- Milestone 2: Wire actual data/audio
  - Broadcaster sends serialized O3DS frames via the connector’s data channel.
  - Audio send (optional) integrated; receiver observes audio RTP packets (decode can be follow-up).

Non-functional constraints:
- No blocking on the game thread. All network/negotiation is async with a Tick pump.
- Determinism first; log only state transitions and errors by default.
- No hard-coded credentials, ports, or absolute paths in source control.

---

## Target architecture

### Open3DShared (new interfaces and backend)

- Public `IWebRTCConnector.h`
  - Start(Config), Stop(), Tick(float), IsOpen()
  - Delegates: OnState(State, Reason), OnData(TArray<uint8>), OnRemoteAudioRtp(TArray<uint8>)
  - Methods for sending data (reliable/unordered default) and enabling audio send.
- Public `WebRTCConnectorFactory.h` (with `.cpp`)
  - `CreateWebRTCConnector(Backend, OptionalLiveKitCfg)` → `TSharedPtr<IWebRTCConnector>`
- Private `LibDataChannelConnector.{h,cpp}`
  - Implements `IWebRTCConnector` using libdatachannel and UE WebSockets for signaling.
  - Applies the working pattern seen in the test components: offerer pre-adds sendonly audio track, answerer pre-provisions recvonly audio, use onOpen/onMessage for DC, and onTrack/onMessage for audio RTP.

### Broadcaster (Open3DBroadcast)

- `UO3DSBroadcastComponent`
  - Properties to control WebRTC backend, signaling URL, token, room, audio enable + parameters (reuse existing where present).
  - When TransportFamily == WebRTC:
    - Create connector via `WebRTCConnectorFactory` during StartCapture (before starting the internal send queue drain).
    - Register connector callbacks (OnState logging; OnData optional for loopback/testing).
    - If audio is enabled, hook the existing audio capture component/submix tap to the connector.
    - In Tick, call `connector->Tick(DeltaTime)`; drain the serialized frame queue only when `connector->IsOpen()`.
    - StopCapture/EndPlay tears it down.

### Receiver (Open3DStream)

Two equivalent integration options are possible; we will start with Option A for minimal churn and document Option B for a cleaner long-term shape.

- Option A (initial scope): Extend `O3DSServer` with a WebRTC path
  - When protocol/family indicates WebRTC, construct a connector (Role=Server) via the factory.
  - Bind `connector->OnData` to `O3DSServer::inData` so `FOpen3DStreamSource` remains unchanged.
  - Bind `connector->OnState` to `O3DSServer::OnState` for status.
  - Drive `Start/Stop/Tick` of the connector in `O3DSServer`.

- Option B (alternative noted for future): Introduce `IO3DSReceiverTransport`
  - Define a transport abstraction for the receiver (TCP, UDP, NNG, WebRTC, etc.).
  - Implement a WebRTC transport backed by `IWebRTCConnector`.
  - Inject into `FOpen3DStreamSource` instead of using `O3DSServer` directly.

Rationale: Option A minimizes code movement and keeps the same `OnData` and `OnState` plumbing for LiveLink. Option B yields a more modular transport split and would be the recommended direction when extending this pattern to other protocols, but we will not refactor other protocols at this time.

---

## Milestones and tasks

### Milestone 1 — Connectivity

- Add `IWebRTCConnector` interface (Shared/Public).
- Add `WebRTCConnectorFactory` (Public header; Private impl).
- Add `LibDataChannelConnector` (Shared/Private) implementing the interface with:
  - UE WebSockets signaling (URL + token).
  - Offer/answer, ICE candidate handling entirely inside the connector.
  - DataChannel setup/callbacks.
  - Audio track sendonly/recvonly negotiation with RTP observation callback.
- Integrate with `UO3DSBroadcastComponent` (sender role) and `O3DSServer` (receiver role), ensuring no libdatachannel includes outside Shared.

Verification
- Editor automation test spawns a server (receiver) and a client (broadcaster) in one Editor session:
  - Assert: DataChannel open within timeout; test text message round-trip.
  - Assert: ≥1 audio RTP packet observed on receiver callback (log).
- Manual smoke: preserve the existing libdatachannel test components to compare behavior/logs.

### Milestone 2 — Data/audio wiring

- Bridge broadcaster’s internal send queue to `connector->Send(Data)`.
- On the receiver, `OnData` continues to feed `FOpen3DStreamSource::OnPackage` (parses O3DS payloads → LiveLink frames).
- Audio send: use existing audio capture/submix tap to send RTP (decode/playback can be a follow-up PR).

---

## Configuration surface (properties)

- Broadcast (`UO3DSBroadcastComponent`)
  - TransportFamily = WebRTC
  - WebRTC settings: Backend (LibDataChannel; future LiveKit), SignalingUrl, Token, Room
  - Audio: enable flag + rate/channels/bitrate + device/submix (existing fields reused)

- Receiver (`UOpen3DStreamSourceSettings` / `FOpen3DStreamSettings`)
  - Add: WebRtcBackend, SignalingUrl, Token (Room already exists)

Helper behavior:
- Ensure URL has a `role` parameter (client/server) and carries `room` when provided (keep parity with existing URL helpers).

---

## UE 5.6 API verification checklist

Verify signatures and usage in the 5.6 docs before coding:
- WebSockets
  - `FWebSocketsModule::CreateWebSocket(const FString&, const TArray<FString>& /*optional headers*/)`
  - Delegates: `OnConnected()`, `OnClosed()`, `OnConnectionError(const FString&)`, `OnMessage(const FString&)`
- Audio capture/mixer (if wiring PCM now): `Audio::FAudioCapture`, submix taps in `AudioMixer`
- Component lifecycle: `BeginPlay`, `EndPlay`, `PostEditChangeProperty`
- Skinned mesh post-eval hook: `USkinnedMeshComponent::RegisterOnBoneTransformsFinalizedDelegate` / `Unregister…`
- LiveLink: `CreateSubject`, `PushSubjectStaticData_AnyThread`, `PushSubjectFrameData_AnyThread`

---

## Tests and validation

- Automation test (Editor-only)
  - Name: `webrtc_smoke_e2e`
  - Spawns receiver/server and broadcaster/client; checks DC open, message delivery, and audio RTP observation.
- Unit-level
  - Factory returns LibDataChannelConnector by default; invalid config handled with actionable errors.
  - URL role/room injection verified.
- Build
  - No new external dependencies outside `Open3DShared` (which already links WebRTC libs); CI uses the existing Editor build task.

---

## Incremental PRs

- PR1 (Milestone 1)
  - Add Shared interface, factory, and LibDataChannelConnector.
  - Integrate in broadcaster and minimal receiver path (Option A) to establish DC + audio negotiation.
- PR2 (Milestone 2)
  - Bridge serialized data send and confirm LiveLink path works over WebRTC.
  - Add automation test.
- PR3 (optional polish)
  - Property UI tidy-up, doc updates, and LiveKit backend switch stub.

---

## Alternative: IO3DSReceiverTransport abstraction (note)

As an alternative to extending `O3DSServer`, we can introduce an `IO3DSReceiverTransport` abstraction that the receiver (`FOpen3DStreamSource`) consumes. A WebRTC transport would then be implemented on top of `IWebRTCConnector`.

- Benefits:
  - Cleaner separation of receiver transports (TCP, UDP, NNG, WebRTC, future options) with consistent `OnData`/`OnState` surface.
  - Easier to extend/replace transports independently.
- Current scope:
  - Adopt this pattern only for WebRTC in a future step to avoid broad refactoring now.
  - Do not change TCP/UDP/NNG implementations at this time; they continue to work via existing `O3DSServer`.

This note is to guide future consolidation work; the initial PRs will implement Option A for minimal disruption.

---

## Docs & changelog updates

- Update plugin README and in-repo docs with:
  - New WebRTC properties and how to use them.
  - Troubleshooting (signaling URL, token, room, and state logs).
- CHANGELOG.md
  - Add an “Unreal/WebRTC” section describing the refactor, connectors, and runtime backend selection.

---

## Definition of Done (per repo rules)

- Passing Editor build on Win64 (CI task provided).
- Automation test demonstrates DC open + text round-trip + audio RTP observation.
- No libdatachannel includes in broadcaster/receiver code; only `IWebRTCConnector` interface usage.
- All signaling and state transitions handled in Shared.
- Docs updated and linked to Issue #134.