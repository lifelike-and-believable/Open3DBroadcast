# Open3DBroadcast: handoff to the next session

Written 2026-09-30 by the cloud session that drove M1, M2 and the start of M3 of the plugin hardening roadmap. Updated 2026-10-01 with the control-channel work (ADR 0011, CTL-1..5) that landed afterwards; ADR 0011 accepted and WP-CTL added to the roadmap the same day. Updated 2026-10-03 when the cloud session finished WP-A1 (#305–#314) and WP-A2a to A2c (#316–#318) and handed the work over to Claude on desktop (§0); updated the same day when Claude on desktop finished WP-A2d (#319) and WP-A2e. Read this first, then the files it points to.

## 0. Handover to Claude on desktop (2026-10-03)

The cloud session stops after WP-A2c (#318). The next session runs on the maintainer's machine.

- **State of `develop`:** WP-A1 complete; WP-A2a (#316), A2b (#317) and A2c (#318) merged. WP-A2d (audio clock, cached device enumeration) merged as #319. WP-A2e (core CRC and builder reuse) done on desktop as #320, built and tested locally (UE 5.7 and the core with MSVC). `O3D_TRANSPORT_API_VERSION` is **5**, unreleased (last tag v0.9.6); everything since c98c92c is under Unreleased in the CHANGELOG. No open PRs from the cloud session.
- **Start here:** WP-A3 (god classes) is complete; its plan (eight steps, rules) is in the roadmap's WP-A3 section. Step 1 (`FO3DReceiverFrameDecoder`) is #325; step 2 (`FO3DLiveLinkPublisher`) is #326; step 3 (stream scheduler and concealment) is #327; step 4 (control router, header without `o3ds/` includes, `Open3DStreamCore` private) is #328; step 5 (RCV-13: doubles kept, `InvalidPosesDropped` metric) is #329; step 6 (sender pose sampler) is #330; step 7 (sender audio binding) is #331; step 8 (sender transport options and secrets, restart properties) is #332, the last of the plan. WP-A6 (global state, CVars) is complete (#333, #334). WP-A4 (protocol versioning, ADR 0009) is in progress: PR 1 (D8 core: frame word, identifier, protocol 2, core 1.1.0) is #335; PR 2 (UDP fragment header v2, datagram classification) is on branch `wp-a4-fragment-v2`; the audio envelope v2 is next. ADR 0005 step 1 (`ref_seq`, stream writer) waits for the maintainer. SHR-38: the maintainer leans to the staged hybrid (per-source metrics and keyed buses first, then a subsystem-hosted context passed through the transport config); an ADR is to be drafted. WP-A5 (reconnect; its acceptance needs live servers) waits until the maintainer is back at the desk. WP-A2 is complete except removing `o3d.Sender.AsyncPipeline` one release later (§2a item 6). SHR-38 (global singletons) needs a design decision before it is scheduled.
- **Local core build:** the core and its CTest suite build with MSVC like core-tests.yml's Windows job (vcvars64, Ninja, NNG/CRCpp/FlatBuffers installed to a prefix, `-DO3DS_BUILD_TESTS=ON`). `sync_o3ds_core.py` needs git to trust the submodule checkouts on this machine ("dubious ownership"); pass it for one process with `GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=*` rather than changing the global git config.
- **What a local UE 5.7 build can now do that the cloud session could not** (pitfall 25: the cloud session reached neither the UE source mirror nor the Epic docs, so every engine API added in WP-A2 was first checked by the CI compile):
  - Record the **Unreal Insights numbers** ADR 0008 Verification asks for (before/after with `o3d.Sender.AsyncPipeline` 0 and 1). The capture steps, timers (`O3D.Sender.Sample`, `O3D.Sender.Pipeline.Serialize`, `O3D.Sender.Pipeline.Send`), `o3d.Sender.DumpPipelineStats` and the budgets are in the ADR 0008 addendum "implementation notes (WP-A2c)". Record them on #318 or in a follow-up PR.
  - Check the unverified engine APIs listed in the ADR 0008 addenda (WP-A2b tick APIs; WP-A2c `UE::Tasks::Launch`, `ETaskPriority::BackgroundHigh`, `TRACE_CPUPROFILER_EVENT_SCOPE_STR`) and ADR 0008 open questions 1 and 2 against the engine source.
  - Add a small skeletal mesh test asset so the ADR 0008 root-bone pose-equality test can be written (§2a item 2).
- **Follow-ups found on the way (not started):** the WebRTC add-on's `SendSerialized` should take the lifetime gate `SendControl` uses (§2 leftovers); MoQ's own backoff, the UDP pause hook not waiting (pitfall 15) and the unused schema features (§2 leftovers).
- **How the cloud session worked each step:** branch from `develop`; local checks (§4); push; open the PR as a draft, then mark it ready, which starts the self-hosted UE job (build with `-FailOnWarnings`, automation tests with and without the WebRTC add-on, strict non-unity/no-PCH build, Fab zip) beside the core-tests workflow; squash-merge only when every check on the head commit is green. `Run-AutomationTests.ps1` reports only the first failed assertion per test (CI annotations show it); the full `Automation.log` is in the job artifacts. Runner-side failures seen so far (App Control 0x800711C7, the editor exiting at startup) cleared on one re-run.
- **Worktrees:** the cloud session's agent worktrees under `.claude/worktrees/` are local to its container and are not in the repository.

## 1. Where things stand

The plan is `docs/roadmap/plugin-hardening-and-fab-readiness.md`. Design decisions are in `docs/adr/0001`–`0011`, all **Accepted**; their open questions were accepted with the recommended defaults. Work is organised as work packages (WPs), one PR each (or a PR series for large ones), squash-merged into `develop`.

| Milestone | Status |
|---|---|
| M0 Decisions (ADRs 0001–0010) | Done (#261–#263) |
| M1 Safety and correctness: WP-S1..S11, WP-T1, WP-T2 | Done (#264–#279) |
| M2 Fab-buildable package: WP-F1..F4, F6..F9, F11 | Done (#274–#286). **F0 and F5 wait on the maintainer** (see §5) |
| M3 Architecture: WP-A1..A7 | **In progress: WP-A1 PR 1 (#289), PR 2 (lifetime), PR 3 (results, state, capabilities), PR 4a (building blocks + Loopback), PR 4b (TCP), PR 4c (UDP), PR 4d (NNG), PR 4e (MoQ), PR 4f (WebRTC add-on), PR 5a (typed config), PR 5b (consumer API), PR 5c (rest of item 8) and step 6 (shims removed) done: WP-A1 is complete. WP-A2 (async sender, ADR 0008) in progress: A2a (settings snapshot, serializer without component, frame pool, sampling clock), A2b (tick group and prerequisite) and A2c (the pose pipeline: serialization and sends on a worker) A2d (audio on the sender clock, cached device enumeration) and A2e (core CRC and builder reuse) done; WP-A2 is complete except removing `o3d.Sender.AsyncPipeline` one release later** (see §2 and §2a) |
| M4 Usability and docs: WP-U1..U6, WP-D1..D4, WP-Q1 | Not started |
| M5 Fab submission: WP-F10 | Not started; needs F0, F5 and the listing details in §5 |
| WP-CTL control channel (D11, ADR 0011) | CTL-1..7 done (#290–#295, CTL-6, CTL-7); live-server checks remain (see §2b) |

Recent merges on `develop`: WP-A1 PR 4a–4f (#305–#310), PR 5a–5c (#311–#313), step 6 (#314, 5ee5ac6), review-docs note (#315), WP-A2a (#316, fc147b7), WP-A2b (#317, aadc41f), WP-A2c (#318). Earlier: F7 editor split (#285), F11 WebRTC add-on (#286), WP-A1 PR 1 (#289), ADR 0011 (#290), CTL-1..5 (#291–#295).

## 2. The work in flight: WP-A1 (transport core consolidation)

Design: `docs/adr/0007-transport-abstraction-and-registry.md`, section "Implementation outline". WP-A1 is a series of PRs; each must keep every transport working and the conformance suite green.

1. **Interfaces and one registry** (SHR-12, SND-23, RCV-27, RCV-28, SHR-24). **Done: #289, merged as d365c34.**
   - `IOpen3DSender`, `IOpen3DReceiver`, their audio sinks, `ISerializedFrameConsumer` and `FO3DTransportConfig` now live in `Open3DShared/Public/Transport/`, exported by Open3DShared.
   - `FO3DTransportRegistry` (`Transport/O3DTransportRegistry.h`): each transport registers one immutable `FO3DTransportDescriptor` and holds a move-only `FO3DTransportRegistration`. `Find` returns `TSharedPtr<const FO3DTransportDescriptor>`; `GetNames(Role)` feeds the pickers from the same entries `CreateSender`/`CreateReceiver` use; `OnTransportsChanged` fires on every change. Duplicate names and API-version mismatches are refused.
   - Old Sender/Receiver headers are deprecated forwarding shims (comments only, no `UE_DEPRECATED`), removed next minor release with an API-version bump. Register/unregister were documented as game-thread only; step 2 added the `check`.
   - Tests: `Open3DBroadcast.Shared.TransportRegistry.*` (7 cases).
2. **Lifetime** (SHR-13, TRF-14). **Done (WP-A1 PR 2).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 2)".
   - The registry keeps a live list of the instances `CreateSender`/`CreateReceiver` hand out; `GetNumLiveInstances(Name)` counts them, leaks included.
   - Unregistering drains: the name goes, `OnTransportUnregistering(FName)` fires, the sender transport controller (through the component's `TeardownTransport`) and the receiver source stop and release their instance and its sinks, then the registry stops what is left and logs an Error naming the transport.
   - `FO3DFfiLibrary` asks the registry (`FO3DFfiLibraryDesc::TransportNames`) instead of tracking instances; `TrackInstance`/`StopLiveInstances` are gone. MoQ and the WebRTC add-on reset their registration, then unload.
   - Register/unregister `check(IsInGameThread())`. `O3D_TRANSPORT_API_VERSION` is now **3** (FFI library layout).
   - Tests: `Open3DBroadcast.Shared.TransportLifetime.*` (7 cases) and the reworked `Open3DBroadcast.Shared.FfiLibrary.*`.
3. **Results, state, capabilities** (SHR-14). **Done (WP-A1 PR 3).** Details and deviations from ADR 0007 item 3 in the ADR 0007 addendum "implementation notes (WP-A1 PR 3)".
   - `Initialize`/`Start` return `FO3DTransportResult` (code and message; explicit `operator bool`). `SendSerialized(FO3DSendPayload&&)` is pure virtual and returns `EO3DSendResult`, as does `SendControl`. Same failure, same code on every transport (`NotRunning`, `InvalidConfig`, `NoConsumer`, `AddressInUse`, `NotConnected`, ...).
   - `EO3DConnectionState` with `GetConnectionState()`/`SetStateChangedCallback()` on both interfaces, implemented by `FO3DConnectionStateTracker` (Open3DShared): lock-free reads, callback on the changing thread, nothing after `Stop`.
   - `FO3DTransportCapabilities` on both interfaces and on the descriptor (`FO3DTransportRegistry::GetCapabilities`); it holds ADR 0005's delivery guarantee and ADR 0011's control flag. `SupportsAudio()`/`SupportsControl()` became non-virtual forwarders (removed in step 6). `SetPeerJoinedCallback` is deferred to WP-A4b (`bPeerJoinSignal` false everywhere).
   - `O3D_TRANSPORT_API_VERSION` is now **4**. `FO3DTransportStats` gained `State`, `SendErrors`, `ReceiveErrors`, `PendingFrames`, `PendingBytes` (mostly zero until step 4's shared queue fills them).
   - Tests: `Open3DBroadcast.Shared.TransportResult.*`, `.ConnectionState.*`, `.TransportCapabilities.*`, `Open3DBroadcast.Transport.Results.*`, five new conformance cases, and the WebRTC add-on's `Results`/`State`/`Capabilities` tests.
4. **Shared building blocks, one transport per PR**, in order Loopback, TCP, UDP, NNG, MoQ, then WebRTC (in the add-on). Each PR deletes that transport's own queue, demux, sink and option-parsing copies and drops its Build.cs dependency on Open3DSender/Open3DReceiver. Control landed before this step, so each migration must also carry that transport's control path: the shared send queue has the "control" item type that is never dropped for mocap (ADR 0007 item 7), and the shared demux routes control through `TryGetControlPayload` (CTL-2/3).
   - **Building blocks + Loopback done (WP-A1 PR 4a).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4a)".
     - In `Open3DShared/Public/Transport/`: `FO3DSendQueue` (per-kind limits and atomic accounting; mocap `DropOldest` or `RefuseNewest`; audio and control never evicted; control cap 1,024; optional age limit), `FO3DTransportWorker` and `FO3DReconnectPolicy`, `FO3DUnifiedReceiveDemux`, `FO3DAudioPublishState` + `FO3DQueuedSenderAudioSink` (and the moved `FO3DSenderAudioSinkBase`/`FO3DGatedSenderAudioSink`; Open3DSender keeps a forwarding header), `O3DTransportOptions` (typed getters, `ParseHostPort`, `ResolveHostPort`). `O3DAudio::TryGetAudioPayloadCodec` added; `NormalizeTcpUrlHostPort` deleted (SHR-9).
     - Loopback: one shared queue per channel (`RefuseNewest`), demux in `Poll`, the shared sink; no Open3DSender/Open3DReceiver dependency. Net transport lines -311.
     - `O3D_TRANSPORT_API_VERSION` stays **4** (standalone types only).
     - Tests: `Open3DBroadcast.Shared.SendQueue.*`, `.TransportWorker.*`, `.ReconnectPolicy.*`, `.ReceiveDemux.*`, `.AudioSinkBase.*`, `.HostPort.*`, `.TransportOptions.TypedGetters`; `Open3DBroadcast.Transport.Loopback.Audio.IndependentOfFrameQueue`, `.Lifetime.StopWhileSending`.
   - **TCP done (WP-A1 PR 4b).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4b)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `tcp.maxqueue` per kind, `tcp.maxqueueage` for frames and audio); an `FO3DTransportWorker` writes each item as one TCP frame (header written on the worker, no copy); the sink is `FO3DQueuedSenderAudioSink`, refused without a client through the new `FO3DAudioPublishState::SetPeerReady`. The bind address must be an IP literal or a wildcard (no DNS on the game thread), as before.
     - Receiver: an `FO3DTransportWorker` resolves (`ResolveHostPort`), connects, reconnects with `FO3DReconnectPolicy` (jitter added), reads and frames; payloads go through a bounded hand-off queue (8 MiB, at least one `tcp.maxframe`) to `Poll`, which feeds `FO3DUnifiedReceiveDemux`. A full hand-off queue stops reading, so TCP flow control holds the sender back. The receiver holds the consumer strongly and releases it in `Stop` (TRF-38).
     - Config: `ParseTcpEndpoint` (strict, bracketed IPv6) and the configure functions read only the config. `SocketsTcpAudio.*` and `BuildTcpUri` are gone. The module still names Open3DSender/Open3DReceiver for UDP's configure functions.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.Sockets.Tcp.AudioIndependentOfFrameQueue`, `.ReceiverBacksOffWithoutSender`, `.StopWhileSending` (1,000 cycles, 50 with a client); `SocketsTesting.h` gained `TcpReceiverGetFailedConnectAttempts`.
   - **UDP done (WP-A1 PR 4c).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4c)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`DropOldest`, soft cap 4 frames / 16 MiB, refused at twice that; audio 1 MiB; control 1,024); an `FO3DTransportWorker` sends each item, fragmenting above `udp.maxdatagram` (wire format unchanged), so no caller calls `SendTo` (TRB-20). A host name resolves on the worker (`ResolveHostPort`, retried with `FO3DReconnectPolicy`; `Connecting` until then); an IP literal still resolves in `Initialize`. The sink is `FO3DQueuedSenderAudioSink`, refused while there is no socket.
     - Receiver: still `Poll`-driven (ADR 0007 leaves it open); complete messages go to `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`. Bind must be an IP literal (IPv6 now works) or a wildcard/`localhost` (`O3DTransportOptions::IsIpLiteral`, new).
     - Config: `O3DSockets::ParseEndpoint` (strict, shared with TCP) and the configure functions read only the config. **`Open3DTransportSockets` no longer depends on Open3DSender/Open3DReceiver.**
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.Sockets.Udp.QueueDropsOldestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles); `Open3DBroadcast.Shared.HostPort.IsIpLiteral`; `SocketsTesting.h` gained `UdpSenderSetWorkerPaused`.
   - **NNG done (WP-A1 PR 4d).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4d)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `nng.qmax` per kind; control 1,024) that an `FO3DTransportWorker` hands to `nng_send(NONBLOCK)`; `NNG_EAGAIN` still drops the oldest at the worker, so the policy is right for pub/sub and for pair/push. `FramesSent` is still counted after `nng_send` (#301), frames only. NNG keeps redialing dropped connections itself; `FO3DReconnectPolicy` only paces reopening a socket that failed or was closed. The sink is `FO3DQueuedSenderAudioSink`.
     - Receiver: still `Poll`-driven (NNG's threads do the I/O); messages go to `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`. Rejects count in `ReceiveErrors`.
     - Config: the NNG Uri shape is unchanged, but hosts, ports and `nng.qmax` are parsed strictly with `O3DTransportOptions`; the configure functions read only the config. **`Open3DTransportNNG` no longer depends on Open3DSender/Open3DReceiver.**
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.NNG.QueueRefusesNewestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles, every 50th connected); `NngTesting.h` gained `SenderSetWorkerPaused`.
   - **MoQ done (WP-A1 PR 4e).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4e)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `queue_bytes` for frames, audio 1 MiB, control 1,024) that an `FO3DTransportWorker` publishes per track; items whose publisher is not ready are dropped at the worker. The sink is `FO3DQueuedSenderAudioSink` (`AudioPayload`). Reconnect stays MoQ's own (game thread, jitter only downward, pinned by tests); `FO3DReconnectPolicy` is not used.
     - Receiver: data callbacks reach the game thread through the dispatcher, then an `FO3DSendQueue` hand-off that `Poll` drains into `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`.
     - Config: `O3DTransportOptions` for every key, strict numbers. **`Open3DTransportMoQ` no longer depends on Open3DReceiver**; it keeps Open3DSender only for `UO3DSenderComponent::SubjectName` (the default stream id) until step 5.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.MoQ.QueueRefusesNewestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles), all on the fake moq-ffi; `MoQTesting.h` gained `SenderSetWorkerPaused`.
   - **WebRTC add-on done (WP-A1 PR 4f); step 4 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4f)".
     - Receiver: frames and control go from LiveKit's data callback to `Poll` through two `FO3DSendQueue` hand-offs (frames `RefuseNewest` 16 MiB, newly bounded; control 1,024), and `Poll` delivers them through `FO3DUnifiedReceiveDemux`. Audio still goes straight from LiveKit's audio callback to the sink (LiveKit decodes Opus).
     - Sender: unchanged and synchronous. LiveKit's data channel buffers and refuses, its refusal is the backpressure, and the add-on's tests pin synchronous results. The gated PCM16 audio sink stays (LiveKit encodes Opus). LiveKit reconnects by itself; `FO3DReconnectPolicy` is not used.
     - Config: `O3DTransportOptions` (strict booleans and numbers); the configure functions read only the config, so no runtime file includes an Open3DSender or Open3DReceiver header.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverQueuePolicy`, `.ReceiverAudioIndependentOfFrameQueue`, `.SenderAudioIndependentOfDataChannel`, `.SenderStopWhileSending`, `.ReceiverStopWhileReceiving` (200 cycles each), on a fake LiveKit.
   - **Leftovers from step 4:**
     - MoQ keeps its own backoff (jitter only downward, pinned by its tests); TCP and UDP use `FO3DReconnectPolicy`.
     - The WebRTC sender has no send queue (see above); revisit if `lk_send_data_ex` turns out to block.
     - `UdpSenderSetWorkerPaused` does not wait for the worker to see the flag (pitfall 15).
     - (Closed by PR 5a: MoQ's Open3DSender dependency and the add-on's test-only Open3DSender/Open3DReceiver dependencies.)
   - **Step 5 is split:** 5a typed config (done), 5b consumer API (done; `SubmitFrame` forms, delete `Send(SubjectList)`), 5c (done) the rest of item 8 (TRB-27 `FName Transport`/`EO3DTransportRole Role`, Secret entries with their env var, schema `Float`/`bRestartOnChange`/`Validate`). Why: see the ADR 0007 addendum "implementation notes (WP-A1 PR 5a)".
   - **Typed config done (WP-A1 PR 5a).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5a)".
     - `FO3DTransportOptionsView` (non-owning; schema defaults, `VisibleWhen`); the `O3DTransportOptions` getters take it (a map converts).
     - The configure functions are `void(const FO3DTransportOptionsView&, FO3DTransportConfig&)` for both roles. The deprecated customizations adapt and still get the component or the settings.
     - `FO3DTransportConfig` lost `Token`, `bPersistToken`, `bUseAutoTokenFetch`, `TokenEndpointUrl`, `TokenRefreshLeadTimeSec`, `Backend`, and gained `SubjectName`, `OptionSchema` and `GetOptions()`. `AdvancedParams` kept its name.
     - WebRTC reads its token settings from the options and `Secrets` (`WebRTCUtils::ReadTokenSettings`).
     - MoQ takes `Config.SubjectName` and no longer depends on Open3DSender.
     - The add-on depends only on Open3DShared (the lifetime test helpers moved there as `Testing/O3DTransportLifetimeTestUtils.h`).
     - SND-35: switching transports keeps each transport's options in `InactiveTransportOptions` (sender component, receiver settings) instead of clearing. Keys are not renamed (TCP, UDP and NNG share `host`/`port`), secrets are never kept, and connection strings leave them out.
     - Saved data needed no migration: the config was never saved, and the `webrtc.*` keys already were the saved form.
     - `O3D_TRANSPORT_API_VERSION` is **5**.
     - Tests: `Open3DBroadcast.Shared.TransportOptionsView.*`, `.TransportOptions.SwitchKeepsOtherTransportsOptions`, `.TransportApiVersion.TypedConfigIsVersion5`, `Open3DBroadcast.Sender.TypedConfig.*`, `.Sender.TransportSwitch.*`, `Open3DBroadcast.Receiver.TypedConfig.*`, `.Receiver.TransportSwitch.*`, `Open3DBroadcast.Transport.WebRTC.TypedConfig.SavedOptionsReachTransport`.
   - **Consumer API done (WP-A1 PR 5b).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5b)".
     - `ISerializedFrameConsumer::SubmitFrame(const FString&, TConstArrayView<uint8>, double)` (view, valid for the call) and `SubmitFrameOwned(..., TArray<uint8>&&, ...)` (owned; defaults to the view form). The subject stays a case-sensitive `FString`.
     - The demux delivers mocap as a view (its scratch copy is gone); Loopback, MoQ and WebRTC hand over their queue items' buffers with `DeliverMocapOwned`. The LiveLink source implements both forms and moves an owned buffer across the game-thread hop.
     - `IOpen3DSender::Send(SubjectList)` is deleted, with every implementation and the sender component's dead `OnSubjectListReady` handler. Tests use `O3DTests::SendSubjectList` / `WebRTCSubjectListTest::SendSubjectList`.
     - `O3D_TRANSPORT_API_VERSION` stays **5**: 5a and 5b ship in the same release (no tag contains 5a).
     - Tests: `Open3DBroadcast.Shared.FrameConsumer.*`, `Open3DBroadcast.Transport.Loopback.FrameReachesConsumerWithoutCopy`.
   - **Rest of item 8 done (WP-A1 PR 5c); step 5 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5c)" and the ADR 0004 addendum.
     - TRB-27: `FO3DTransportConfig::Transport` is the registered `FName`, `Role` an `EO3DTransportRole`; constructor `FO3DTransportConfig(Name, Role)`. The hosts and every configure function set both; tests use "TCP"/"UDP"/"NNG"/"Loopback", not "sockets.tcp" or lower case. NNG's socket side stays in the `nng.role` option.
     - Secrets: a `Secret` schema entry declares its key and carries `SecretEnvVar` (the old lists were kept as deprecated inputs in 5c and removed in step 6). WebRTC declares its secrets in the schema only. ADR 0004 behaviour unchanged.
     - Schema: `Float` (appended), `Min`/`Max` now `double` (Int and Float; `GetDouble` clamps a Float), `bRestartOnChange` (panel calls `IO3DOptionTarget::RestartTransport`; the sender target restarts a capturing component in a game world), `Validate` (`TFunction<bool(const FString&, FText&)>`; `O3DTransportOptions::ValidateOptions` at the sender controller's and receiver source's start gives `InvalidConfig`; the panel shows the error and refuses the write). No built-in field uses the three yet.
     - `UO3DSenderComponent::GetLastTransportResult()` (C++); `FO3DSenderSerializer::OnSubjectListReady` deleted.
     - `O3D_TRANSPORT_API_VERSION` stays **5**: still no tag after v0.9.6, so 5a–5c are one release.
     - Tests: `Open3DBroadcast.Shared.TypedConfig.ConfigCarriesTransportAndRole`, `Open3DBroadcast.Shared.OptionSchema.*`, `Open3DBroadcast.Editor.OptionsPanel.RestartOnChangeRestartsTransport`, `.ValidateShowsErrorAndRefuses`, `.FloatIsClampedToRange`, `Open3DBroadcast.Transport.WebRTC.Secrets.SchemaEntriesCarryEnvVars`.
5. **Typed config and consumer API** (SHR-36, TRB-27, SHR-16, TRF-38): removes the LiveKit string fields from `FO3DTransportConfig`, deletes `Send(SubjectList)`. Split into 5a (typed config), 5b (consumer API) and 5c (rest of item 8). **Done**, interface version 5.
6. **Shims removed (WP-A1 step 6). Done; WP-A1 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 step 6)" and the CHANGELOG's "Removed" list.
   - Maintainer decision: removed now instead of one release later (ADR 0007 item 9 and open question 5), because no release carried the shims and no third-party add-on or project builds from this codebase.
   - `O3D_TRANSPORT_API_VERSION` stays **5**: still no tag after v0.9.6, and no tag contains 5a's merge (c98c92c), so 5a, 5b, 5c and step 6 are one version.
   - Gone: `O3DTransport::RegisterSender`/`RegisterReceiver` and the rest of the old registries; the sender and receiver transport customizations (`RegisterTransportCustomization`, `FindTransportCustomization`, the list, secret and schema forwarders); the `FScopedConfiguring*` scopes and customization caches; `FO3DTransportRegistry::EditLegacyDescriptor`; `FO3DTransportRoleOptions::SecretOptionKeys`/`SecretEnvVars` (a `Secret` schema entry is the only declaration); `SupportsAudio()`/`SupportsControl()` (use `GetCapabilities()`); and the forwarding headers (`O3DSenderInterface.h`, `O3DReceiverInterface.h`, `O3DSenderAudioSinkBase.h` and `Testing/O3DLifetimeTestUtils.h` in Open3DSender/Open3DReceiver; `O3DTransportTypes.h` and `SerializedFrameConsumerRegistry.h` at the Open3DShared root).
   - `O3DReceiverTransportCustomization.h` keeps its name for the receiver's secret and switching helpers.
   - Tests that tested only removed API were deleted (`TransportRegistry.DeprecatedFunctionsForward`, `Sender.TypedConfig.DeprecatedConfigureGetsComponent`, `Receiver.TypedConfig.DeprecatedConfigureGetsSettings`); tests that used the shims as setup now register a descriptor with a fake factory.
   - **Leftovers after WP-A1:**
     - MoQ keeps its own reconnect backoff (jitter only downward, pinned by its tests); TCP and UDP use `FO3DReconnectPolicy`.
     - The WebRTC sender has no send queue and stays synchronous; revisit if `lk_send_data_ex` turns out to block. Since WP-A2c it is called from the pose pipeline's worker. Its `SendSerialized` reads `ClientHandle` outside the lifetime gate `SendControl` uses, so it is not safe against a concurrent `Stop`; the sender controller detaches it from the pipeline (waiting for a send in flight) before `Stop`, so the pipeline cannot race, but the gate belongs in `SendSerialized` (add-on change).
     - `UdpSenderSetWorkerPaused` does not wait for the worker to see the flag (pitfall 15).
     - Schema features no built-in transport uses yet: `Validate` for host/port fields (`ParseHostPort`), `Float` for `webrtc.reconnect_timeout`, `bRestartOnChange` where a running transport ignores a change.
     - `docs/review/2026-09-plugin-review/*` still names the removed APIs; they are dated review findings with file:line evidence and are left as written.

**WP-A2 (async sender, ADR 0008) is in progress; see §2a.**

WP-A1 acceptance (roadmap): conformance suite green after each migration, net transport LOC goes down, no transport keeps its own queue/demux/sink. ADR 0007 "Verification / acceptance" lists the extra test cases.

After WP-A1, the M3 order in the roadmap is WP-A2 (async sender, ADR 0008) → WP-A3 (god classes); WP-A4 (protocol, ADR 0009), WP-A5, WP-A6, WP-A7 can go in parallel where files don't overlap.

## 2a. The work in flight: WP-A2 (asynchronous sender pipeline)

Design: `docs/adr/0008-sender-pipeline-threading.md`, "Implementation outline" items 2 to 7. One PR per sub-step (A2a to A2e); each must leave the WP-S3 sender tests passing unchanged.

1. **WP-A2a: settings snapshot, serializer without component, frame pool, sampling clock. Done (#316).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2a)".
   - `FO3DSenderEncodingSettings` holds the encoding and the curve filtering settings (pattern lists as shared immutable arrays); the component refreshes it per sampled frame (`UpdateEncodingSnapshot`) and copies a list only when it changed. Frames carry it by value (deviation 1: the WP-S3 tests use value semantics).
   - `FO3DSenderSerializer` has no `Attach`/`Detach` and no component pointer; the component calls `SerializePoseFrame` after filtering. `SetStatsLabel` names it for `o3ds.Sender.DumpStats`.
   - Curve capture (`FO3DSenderCurveProcessor`, raw values against a shared `FO3DSCurveList`) and filtering (`FO3DSenderCurveFilter`, on the frame) are split. Filtering still runs before `OnPoseFrameReady` (deviation 3).
   - `FO3DSPoseFramePool` (public header): bounded (default 4), reuses allocations, lock only around pointer moves.
   - The wire time is `FO3DSPoseFrame::CaptureTimeSec` (sampling time), not the serialization time.
   - Still synchronous on the game thread. `O3D_TRANSPORT_API_VERSION` stays **5** (no transport interface change).
   - Tests: `Open3DBroadcast.Sender.EncodingSnapshot.SerializerWorksWithoutComponent`, `.FramePool.ReusesFramesAndIsBounded`, `.CurveFilter.AfterSamplingMatchesBefore`, `.Wire.SerializedTimeIsSamplingTime`.
2. **WP-A2b: tick group and prerequisite. Done (#317).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2b)".
   - The sender ticks in `TG_PostUpdateWork` (was the default `TG_PrePhysics`). `BindToTarget`/`UnbindFromTarget` add and remove the target mesh's tick as a prerequisite through `SetTickPrerequisiteMesh` (one mesh at a time, no double add). `CanCaptureThisFrame` moves the prerequisite when `TargetMesh` is written during capture and removes it when the mesh is destroyed (deviation 1); an entry left by a mesh that was already collected is pruned (deviation 2).
   - The transport `Tick` and `TickControl` moved to `TG_PostUpdateWork` with the rest of the tick.
   - `RegisterOnBoneTransformsFinalizedDelegate` is not used; the old "removed in 5.4" comments are gone. Open question 2 is answered only from knowledge of the engine's tick design (addendum); the UE 5.7 source and the Epic API pages were unreachable (pitfall 25, now also the docs sites), so every tick API used is unverified and first compiled by CI.
   - **ADR 0008's root-bone acceptance test is not written:** it needs a skeletal mesh asset and the plugin has none (the CI host project holds only the packaged plugin). `SamplesAfterTargetMeshEachFrame` checks the ordering it rests on in a ticking game world instead (deviation 3). Adding a small skeletal mesh asset to a test content folder would allow the pose-equality test.
   - Still synchronous. `O3D_TRANSPORT_API_VERSION` stays **5**.
   - Tests: `Open3DBroadcast.Sender.TickOrder.TickGroupIsPostUpdateWork`, `.PrerequisiteFollowsTargetMesh`, `.SamplesAfterTargetMeshEachFrame` (the first test in the repo that creates and ticks a `UWorld`; its first CI runs failed until the test advanced `GFrameCounter` before each world tick, pitfall 28).
3. **WP-A2c: the pose pipeline. Done (#318).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2c)".
   - `FO3DSenderPipeline` (`Open3DSender/Private/O3DSenderPipeline.h/.cpp`), one per component, shared-owned so a task in flight keeps it alive: frame pool, curve filter, serializer, one FIFO of frames and control items (`Start`, `Stop`, `RemoveSubject`), and the transport sender. The game thread samples and hands the frame over; a `UE::Tasks::Launch` task (`BackgroundHigh`) filters, serializes and calls `SendSerialized` with the serializer's buffer moved in. The ADR's "drain task scheduled" flag allows one task per sender at a time; `UE::Tasks::FPipe` is not used (deviation 1: pipe lifetime when the task holds the last reference).
   - Depth `o3d.Sender.PipelineDepth` (default 2, 1 to 8), drop oldest before serialization; control items never dropped. `o3d.Sender.AsyncPipeline 0` (read at `StartCapture`) keeps the WP-A2b synchronous order, for one release.
   - `StopCapture` discards waiting frames and does not wait for the network; removing the `OnSerializedFrame` listener and detaching the sender (the transport controller's `Stop`, before `IOpen3DSender::Stop`) wait for the one item the worker is processing (deviation 3). The component destructor detaches both. `ShutdownModule` waits up to 1 s for drain tasks.
   - `OnSerializedFrame` fires on the worker; `OnPoseFrameReady` fires on the game thread with raw curves (filtering moved to the worker). A full sync refused with `DroppedBackpressure` is followed by a full sync (`FO3DSenderSerializer::RequestFullSync`); `bFullSync` is set on the payload.
   - `o3ds.Sender.DumpStats` reads every serializer under its new `StateLock` (A2a deviation 6 closed). New `UO3DSenderComponent::GetPipelineStats()`, `o3d.Sender.DumpPipelineStats` and trace scopes `O3D.Sender.Sample`, `O3D.Sender.Pipeline.Serialize`, `O3D.Sender.Pipeline.Send`.
   - **Not done here:** `tx_seq` on the wire (needs WP-A4a's `StreamWriter`; the pipeline counts `LastSendSequence` where it will be stamped), the new-peer flag (WP-A4b), and **the Unreal Insights numbers** ADR 0008 Verification asks for: no engine in the cloud session. The manual capture (what to record, which timers and stats, the budgets) is in the addendum; record the numbers in the A2c PR or a follow-up.
   - Every new engine API (`UE::Tasks::Launch`, `ETaskPriority::BackgroundHigh`, `TRACE_CPUPROFILER_EVENT_SCOPE_STR`, and in tests `CollectGarbage`, `IConsoleVariable::Set`) is unverified against 5.7 (pitfall 25) and first compiled by CI.
   - Still `O3D_TRANSPORT_API_VERSION` **5**: the interface did not change; the worker relies on the documented "any thread" contract of `SendSerialized`.
   - Tests: `Open3DBroadcast.Sender.Pipeline.SlowTransportDropsOldest`, `.RefusedFullSyncIsSentAgain`, `.StopDiscardsQueuedFrames`, `.StopStartAndRenameUnderLoad` and `.QuantizationChangeForcesOneFullSync` (both with the console variable at 0 and 1), `.OwnerReleasedWithTaskInFlight` (1,000 cycles), `.ComponentDestroyedWithTaskInFlight` (200 components, garbage-collected every 50). New hooks: `FO3DSenderPipelineProbe`, `FO3DSenderComponentTestAccess::SubmitSampledFrame`/`WaitForPipelineIdle`.
4. **WP-A2d: audio clock and cached device enumeration. Done (#319).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2d)".
   - `FO3DAudioClockMapper` (public, header-only): the submix tap maps `AudioClock` and the microphone `StreamTimeSec` onto `FPlatformTime::Seconds()`; offset from the first buffer, low-pass (5 s), reset on a jump over 100 ms (at once when the source goes backwards or ahead; after 0.5 s when it falls behind, so a hitch does not move the stamps). `PushFrames` is not mapped: its callers pass sender-clock times.
   - `FO3DAudioInputDevices` (public, exported): one cached device list, enumerated once per Input `StartCapture`, once in the editor after engine init, and on request (`UO3DSenderComponent::RefreshAudioInputDevices`, `o3d.Sender.Audio.RefreshDevices`). Pickers and name lookups read the cache.
   - The device is opened once per start: `InitializeTransport` no longer calls `UpdateAudioCaptureBinding` (it ran twice per start, reopening the microphone), and the capture component's `BeginPlay` opens the device only with a sink bound.
   - `O3D_TRANSPORT_API_VERSION` stays **5**. Every engine API used was checked against the local UE 5.7 headers (addendum).
   - Tests: `Open3DBroadcast.Sender.AudioClock.*` (4), `Open3DBroadcast.Sender.AudioDevices.LookupsReadTheCache`, `.StartEnumeratesAndOpensOnce`, `.SinkBindOpensAtMostOnce`; new hook `FO3DSenderAudioCaptureTestAccess`.
5. **WP-A2e: core CRC and builder reuse. Done (#320).** Details and numbers in the ADR 0008 addendum "implementation notes (WP-A2e)".
   - `O3DS::Crc32` (`src/o3ds/crc32.h/.cpp`): slicing-by-8 CRC-32 with CRCpp's `CRC_32()` parameters, so the value and the wire format are unchanged. `finalize()` and `Parse()` use it; the core no longer includes `CRC.h`, so the plugin mirror drops it and the CRC++ licence (`sync_o3ds_core.py` copies that licence only while a `crccpp/` file is mirrored), and `Open3DStreamCore.Build.cs` drops the include path.
   - The six whole-buffer `Serialize*` functions reuse a `thread_local` `FlatBufferBuilder`; `finalize()` writes in place.
   - MSVC Release, one 250-bone, 250-curve subject: Serialize 499 to 96 µs, Parse 406 to 162 µs, Serialize + Parse 3.5x (the acceptance asks for 3x).
   - Tests: `core.crc32_tests`; benchmark `o3ds_core_bench` (`core.bench.serialize`, label `bench`).
   - Follow-up done separately (#321): `Parse` reuses subjects, transforms and curve names on a full sync with exactly the result of a fresh parse (`core.parse_reuse_tests`); Parse 160 to about 85 µs. Still open: skipping the second Verifier pass after `PeekMeta`.
6. **Next: remove `o3d.Sender.AsyncPipeline` one release after WP-A2c ships** (deletes `FO3DSenderPipeline::FilterFrameInline` and the synchronous branch of `Push`).

## 2b. Control channel (WP-CTL, ADR 0011)

Done outside the cloud session, after the first version of this doc. Design: `docs/adr/0011-control-channel.md` ("Implementation outline" and "Verification / acceptance"). A one-way sender-to-receivers stream of **values** (keyed, last-writer-wins, periodic snapshots) and **events** (fire-once, de-duplicated, TTL, redundant copies), as envelope kind `Control = 2` with its own FlatBuffers root (`src/o3ds_control.fbs`, `"O3DC"`).

| Step | Status |
|---|---|
| CTL-1 core: schema, codec, publisher/receiver state machines, CTest + fuzz | Done (#291) |
| CTL-2 envelope kind, `SendControl`/`SetControlSink` on the interfaces, `FO3DControlBus`; API version 1 → 2 | Done (#292) |
| CTL-3 TCP, UDP, NNG, Loopback | Done (#293) |
| CTL-4 sender component API, receiver `FControlSink` with mocap alignment, `UO3DControlSettings`, `UO3DRemoteControlComponent` | Done (#294) |
| CTL-5 MoQ (`control/<session>` track) | Done (#295). The live-relay test case is not written (needs a relay) |
| CTL-6 WebRTC add-on: `__o3d.ctl` send path and receive classification, tests, manual test step | Done (CTL-6). Tests `Open3DBroadcast.Transport.WebRTC.Control.*` (fake FFI); manual case 12 in `docs/testing/webrtc-manual-test.md` not yet run against a live LiveKit server |
| CTL-7 docs: USER_GUIDE Control section, transport comparison row, CHANGELOG protocol entry | Done (CTL-7). The wire layout goes into `docs/wire-format.md` with WP-D3 |

Open items:
- ADR 0011 was accepted on 2026-10-01 (open questions with their defaults), and WP-CTL is in the roadmap as decision D11 with its own work package section.
- The WebRTC transport supports control since CTL-6. Its conformance profile is still deferred (WP-T2e: no add-on test module), so the control conformance cases are mirrored in the add-on's `Open3DBroadcast.Transport.WebRTC.Control.*` tests; when WP-T2e adds the profile, give it the control cases too. ADR 0011 open question 4 (is the reliable LiveKit data channel ordered; does the callback expose the participant) is still unverified; the manual case 12 checks ordering, and `source_id` in the payload covers sender identity.
- The packaged Shipping test ADR 0011 asks of CTL-4 (control enabled in a Shipping build through the project setting and the runtime call) is not in CI yet; the nightly Shipping build (`Build-ShippingGame.ps1`) is the natural place for it.
- New-peer snapshots wait for ADR 0005 (vi); until then recovery is bounded by the snapshot interval.
- CTL and WP-A1 overlap: see the notes on steps 3 and 4 in §2.

## 3. Repository map (what changed during M1/M2)

- Main plugin: `ProjectSandbox/Plugins/Open3DBroadcast`. Modules: `Open3DShared`, `Open3DSender`, `Open3DReceiver`, the transports (`Open3DTransportSockets`, NNG, MoQ, Loopback), `Open3DStreamCore`, `Open3DBroadcastEditor` (editor-only UI, ADR 0010), `Open3DBroadcastTests` (editor-only, ADR 0006).
- WebRTC add-on: `ProjectSandbox/Plugins/Open3DBroadcastWebRTC` (WP-F11, ADR 0002). Depends on Open3DBroadcast; checks the transport API version at startup. Publishing it waits on counsel question L1; the release workflow attaches it only when the repo variable `O3D_PUBLISH_WEBRTC_ADDON` is `true`.
- The o3ds core (`src/o3ds`) is compiled inside the plugin from a generated copy in `Source/ThirdParty/Open3DStreamCore` (ADR 0003). After touching `src/o3ds`, `src/o3ds_generated.h` or the flatbuffers/crccpp pins: run `python3 Build/Scripts/sync_o3ds_core.py` and commit the result. Never edit the copy by hand.
- Control channel: core in `src/o3ds/control.{h,cpp}` and `src/o3ds_control.fbs` (the second generated header is in the core manifest, so `sync_o3ds_core.py` mirrors it); UE types in `Open3DShared/Public/O3DControl*.h`; receiver-side `O3DControlSettings.h` and `O3DRemoteControlComponent.h` in `Open3DReceiver/Public`.
- Rider IDE settings are committed under `.idea/`.
- Rules for agents: `AGENTS.md` → `.github/copilot-instructions.md` (authoritative). `Build/README.md` documents every build script.

## 4. How to work in this repo

### Conventions
- Branch from `develop`; one WP (or one WP-A1 step) per PR; squash-merge. Before merging a later PR, merge `develop` into it and let CI re-run.
- Commit/PR titles start with the WP id and list finding ids, e.g. `WP-F7: editor module split (ADR 0010; FAB-7, SND-34)`.
- New source files start with `// Copyright Lifelike & Believable. All Rights Reserved.` and a blank line. Files that already carry `// Copyright (c) Open3DStream Contributors` keep it (maintainer decision).
- Every test name starts with `Open3DBroadcast.` so the CI filter finds it. Network tests register only with `O3DB_NETWORK_TESTS=1`.
- Update `CHANGELOG.md` and the relevant docs in the same PR.

### Local checks (run before every push)
```
python3 Build/Scripts/fab-package.py --out-dir <scratch>     # Fab tree/package checks
python3 Build/Scripts/check-copyright-headers.py
python3 Build/Scripts/check-runtime-editor-deps.py
bash    Build/Scripts/check-no-video-codecs.sh                # main plugin must pass; --addon fails by design
python3 Build/Scripts/sync_o3ds_core.py --check               # needs submodules initialised
```
With UE 5.7 installed locally you can also run the real build and tests: `Build/Scripts/Build-Plugin.ps1`, `Build-WebRTCAddOn.ps1`, `Run-AutomationTests.ps1 -TestFilter Open3DBroadcast` (usage in `Build/README.md`). The cloud session had no UE, so every C++ change was first compiled by CI; a local build before pushing will save cycles.

### CI
- `open3dbroadcast-plugin-ci.yml` (PRs): GitHub-hosted jobs (path filter, Fab source zip, copyright headers, runtime-editor deps) plus **one self-hosted Windows UE job**:
  1. BuildPlugin;
  2. automation tests with the main plugin only;
  3. build the WebRTC add-on against that package;
  4. automation tests with both plugins;
  5. a strict build (non-unity, no PCH, warnings as errors);
  6. BuildPlugin on the Fab zip.
- **Draft PRs skip the UE job**; mark the PR ready to get it.
- `core-tests.yml`: Linux ASan/UBSan, MSVC, libFuzzer, warning ratchet (baseline 8), core mirror sync check.
- Nightly (`open3dbroadcast-plugin-nightly.yml`): flag-combination builds, Linux exclusion check, Shipping game build. **These nightly additions (WP-F2, WP-F7, WP-F11) had not yet reported a run at handoff; check the first results.**
- When a test fails, `Run-AutomationTests.ps1` prints the last 150 lines of `Automation.log` into the job log.
- Known runner infrastructure failures (re-run once, only if the job died before any test ran): Windows Application Control blocking a UBT rules DLL (`0x800711C7`), and an occasional editor exit at startup with no report. A second failure is real.

### UE 5.7 compile pitfalls hit in this repo
1. Friend declarations must be unconditional (not inside `#if WITH_...`).
2. `*_API` classes with non-copyable members need `= delete` copy operations.
3. No `MakeShared` with a forward-declared type.
4. `NewObject<T>(GetTransientPackage())` needs `UObject/Package.h`.
5. Include `Widgets/Input/SComboBox.h`; never forward-declare `SComboBox<...>`.
6. Include what you use: the strict build has no PCH and no unity.
7. Anything tests or another module/plugin uses must be `*_API`-exported in a Public header.
8. `TStrongObjectPtr<T>` members need the complete type in the header.
9. `TSlateDelegates<int32>::FOnValueCommitted` does not exist in 5.7; use the SLATE_EVENT shorthand `.OnValueCommitted(this, &Method, Payload)`.
10. Windows PowerShell 5.1 ignores `-Include` together with `-LiteralPath`; filter with `Where-Object`.
11. Helpers in anonymous namespaces need names unique within their module: unity builds merge a module's .cpp files, and CI's strict build (non-unity) will not catch a collision. A local `ProjectSandboxEditor` build does (see #287, `GetPipeContextRegistry` in NNG).
12. Never name a free function, local or helper after a UE global namespace (`Audio`, `UE`, `Chaos`, ...). MSVC reports C2872 "ambiguous symbol" when that namespace is visible in the module; `O3DSendQueueTests.cpp` had helpers called `Audio()` (#305; renamed `MocapItem`/`AudioItem`/`ControlItem`).
13. The copyright line must be followed by a blank line; a bare `//` continuation line right after it fails `Build/Scripts/check-copyright-headers.py`. Run the script before pushing.
14. A test that needs a transport's queue to back up must not rely on the network being slow: on loopback a worker drains faster than a test can fill. Pause the worker with a test hook instead (`O3DSocketsTesting::UdpSenderSetWorkerPaused`, WP-A1 PR 4c), so the drop policy is deterministic.
15. A test pause hook must return only once the worker has seen the flag: an iteration that started before the flag was set can still dequeue the next item. `FO3DNngSender::SetWorkerPausedForTesting` waits for a paused iteration (WP-A1 PR 4d); `UdpSenderSetWorkerPaused` does not yet, so a UDP test should leave the queue empty for one worker wait before relying on it.
16. Never build a counter that reserves first and rolls back on overflow when another thread can read it: a reader sees the over-limit value for a moment. `FO3DSendQueue::Enqueue` did that and `Open3DBroadcast.Shared.SendQueue.ConcurrentAccounting` failed intermittently on #308; it now reserves with a compare-exchange that only succeeds when the result fits (af7cfcc). The same applies to any limit a test or `GetStats` observes.
17. A transport's existing tests may pin its own timing (MoQ's backoff jitter, connect timeout, subscribe retry on a manual clock). Before replacing such logic with a shared block, check that the shared block reproduces those exact values; if not, keep the transport's logic and say why, rather than editing the tests (WP-A1 PR 4e kept MoQ's backoff).
18. A fake FFI used for a Stop test must keep the real library's callback contract. LiveKit promises no callback after the call that clears it (or `lk_client_destroy`) returns, so the WebRTC fakes hold a lock around each callback and take it in the setters (`WebRTCSharedBlocksTests.cpp`, WP-A1 PR 4f). A fake that calls back without it reports races the real library cannot produce, and can hide the ones it can.
19. Before planning a saved-data migration, check what is actually serialized. `FO3DTransportConfig` is a plain struct built per start, so removing its LiveKit fields lost nothing; the saved form was the `webrtc.*` keys of a UPROPERTY option map (WP-A1 PR 5a). A new UPROPERTY loads with its default from older assets, ini files and `ImportText` strings, so adding one needs no migration either.
20. When an interface gains a second form of a virtual, give it its own name (`SubmitFrameOwned`, not a second `SubmitFrame` overload). A derived class that overrides one overload hides the others, so a call through the derived type silently picks the wrong one or fails to compile (WP-A1 PR 5b).
21. A local must not reuse the name of a parameter or of a local in an enclosing scope. MSVC reports C4457 ("declaration hides function parameter") or C4456, and CI builds with `-FailOnWarnings`, so it fails the build. The 5c options panel declared `double Value` inside a function whose parameter was `Value` (fixed in c5fb457). Check every new local against the enclosing function's parameters.
22. A sender-component test that expects its transport to start must set `bAutoCreateTransport = true`: it defaults to false, and `StartCapture` then never reaches the transport controller. It must also declare every warning the path logs with `AddExpectedError` (log warnings fail a test): a control-only `StartCapture` without a mesh logs "No TargetMesh set" once per call. `ValidateGivesInvalidConfig` needed both (fixed in 9bb6b3f); `O3DTransportLifetimeTests` shows the pattern.
23. Several Open3DSender sources (`O3DSenderComponent.h/.cpp`, `O3DSenderSerializer.h/.cpp`, `O3DSenderCurveProcessor.h/.cpp`) are CRLF while newer files are LF (`git ls-files --eol`). Keep each file's line endings when editing (convert to LF, edit, convert back), or the diff becomes a whole-file rewrite that hides the real change (WP-A2a).
24. `FO3DSenderCurveConfig` holds raw pointers to the include/exclude pattern arrays. Build it only from a frame's settings snapshot (`FO3DSenderEncodingSettings`, whose lists are shared and immutable), as `FO3DSenderCurveFilter::FilterFrame` does, never from the component's `IncludeCurvePatterns`/`ExcludeCurvePatterns`: once filtering runs on the WP-A2c worker those would be read while the game thread edits them (WP-A2a).
25. The GitHub connector in a cloud session may refuse the UE 5.7 source mirror (`lifelike-and-believable/UnrealEngine`). If it does, use only UE APIs this plugin already compiles in CI, in the same call form, and say so in the ADR addendum; a new engine API then needs a session that can reach the mirror (WP-A2a).
26. A test that needs real engine ticking creates its own world: `UWorld::CreateWorld(EWorldType::Game, false)` plus a world context, `InitializeActorsForPlay(FURL())`, then `World->Tick(LEVELTICK_All, Dt)`. Such a world has no game mode, so actors never begin play by themselves and component ticks are not registered: call `Actor->DispatchBeginPlay()` after registering the components. Destroy the world in a scope guard (`DestroyWorldContext`, `DestroyWorld(false)`) so a failed check does not leak it. `O3DSenderTickOrderTests.cpp` (WP-A2b) shows the pattern; it was written without a local UE build, so check its first CI run.
27. A sender test with a skeletal mesh component but no mesh asset samples frames without a skeleton descriptor, and the serializer drops them with a Warning ("no skeleton descriptor on the frame", rate-limited to one per 5 s). Declare it with `AddExpectedError(..., -1)`: `0` means "at least once" (`AutomationTest.h`), and since WP-A2c the drop happens on the pipeline's worker, so a short test may end before any frame was dropped; `SamplesAfterTargetMeshEachFrame` used `0` and failed intermittently until it was changed to `-1` (WP-A2b, fixed in the WP-A2 worker-cost follow-up). The cloud session could not reach `dev.epicgames.com` or `docs.unrealengine.com` either (network proxy), so API checks fell back to search snippets.
28. A test that ticks a world with `World->Tick` must advance `GFrameCounter` before each tick (`++GFrameCounter;`). The engine loop advances it once per frame, and a tick function that already ran in the current `GFrameCounter` frame is not queued again, so without it only the first `World->Tick` runs the component ticks (found in #317; `O3DSenderTickOrderTests.cpp` shows it).
29. Since WP-A2c the sender serializes and sends on a worker. A test that checks what a sender component sent must wait for its pipeline (`FO3DSenderComponentTestAccess::WaitForPipelineIdle`, an event with a timeout) before reading the transport or the serializer stats; a test that needs the queue to back up holds the worker inside `SendSerialized` with a scripted transport and waits for an "entered" event (pitfalls 14 and 15), as `O3DSenderPipelineTests.cpp` does. Warnings the worker logs (the serializer's drop warning) arrive on the worker thread; `StopCapture` waits for the item in flight, so stop the component before the test ends if a late warning could otherwise land in the next test.
30. Do not own a `UE::Tasks::FPipe` in an object whose last reference a piped task may drop: the pipe's destructor checks it has no work, and the pipe is touched after the task body and its captures are destroyed (as far as is known; unverified for 5.7). `FO3DSenderPipeline` uses plain tasks plus a "drain scheduled" flag for that reason (ADR 0008 addendum WP-A2c, deviation 1).
31. Adding a source file to a module reshuffles its unity blobs, so a latent name collision between two other files can surface in your PR (pitfall 11). WP-A2d's two new test files put `SocketsLifetimeTests.cpp` (an anonymous-namespace `FindFreeTcpPort`) in one blob with `SocketsTcpSharedBlocksTests.cpp` (its own `FindFreeTcpPort` in a named namespace it `using`s): C2668, ambiguous call. Renamed to `FindFreeLifetimeTcpPort`. Give file-local test helpers names unique within the test module.

## 5. Waiting on the maintainer

- **WP-F0:** check the live Fab technical requirements page against ADR 0001/0002 assumptions.
- **WP-F5:** counsel questions L1–L5 (ADR 0002). L1 gates publishing the WebRTC add-on before the codec-free `livekit_ffi` rebuild.
- **Listing details:** `CreatedByURL` (still `https://open3dstream.com/` in both `.uplugin` files); real `SupportURL` and `DocsURL` (stand-ins today); `MarketplaceURL` once the Fab listing exists; the WebRTC add-on download link (a marked placeholder in the user guides). Decided 2026-10-01: `CreatedBy` is "Lifelike & Believable and Open3DStream Contributors" in both descriptors.
- **Naming:** decided 2026-10-01: editor categories (`Category = "Open3DBroadcast|..."`, the Sender details customization) and `ClassGroup = (Open3DBroadcast)` use Open3DBroadcast. Still open: the LiveLink source name (factory display name "Open3DStream Receiver" and tooltip in `O3DReceiverSourceFactory.cpp`, source type "Open3D Stream" in `O3DReceiverSource.cpp`), and whether anything saved in assets, presets or config (property names, config section names, ini keys) should ever be renamed. None of those contain "Open3DStream" today; renaming them would need redirects.
- **ADR 0002 Q8(b):** confirm on a real Fab install that the installed plugin ships the import libraries (`UnrealEditor-Open3DShared.lib` etc.) a source build of the add-on needs.
- **Red-gate branches:** five throwaway branches proving each CI gate fails when it should (unused local with `-FailOnWarnings`, failing test, include only the PCH provided, dev notes in `Private/`, WebRTC in the Fab zip). They were never pushed. The patches are in `docs/roadmap/handoff/redgate-patches/`. To use one: branch from `develop`, `git am` the patch, open a **draft** PR, mark it ready, confirm the named check goes red, then close the PR and delete the branch. They predate F1/F7/F11, so a patch may need a small path fix.

## 6. Decisions already made (don't reopen)

- Rights holder: Lifelike & Believable. Open3DStream Contributors notices stay on files that had them.
- FriendlyName "Open3DBroadcast"; the add-on is "Open3DBroadcast WebRTC". Whole plugin is Beta for v1; MoQ ships as Experimental.
- WebRTC ships only as the separate free add-on, not in the Fab package.
- v1 platform scope and the rest: see ADRs 0001–0011.
