# 0007: Transport abstraction, registry and shared transport building blocks

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** every open question below was accepted with the recommended default given next to it. Needs-verification items stay open and are resolved in the implementing WPs; a result that invalidates a default is handled by a superseding ADR.
- **Date:** 2026-09-29
- **Plan decision:** D4 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0002](0002-webrtc-and-moq-in-first-fab-release.md) (WebRTC add-on, interface version), [ADR 0003](0003-core-library-delivery-to-plugin.md) (`Open3DStreamCore`), [ADR 0004](0004-credentials-and-secret-transport-options.md) (secret keys, `FO3DSecretStore`), [ADR 0005](0005-wire-resync-and-loss-contract.md) (delivery guarantee, new-peer signal), [ADR 0006](0006-test-module-layout-and-fakes.md) (conformance suite, fakes), ADR 0008 (D5), ADR 0009 (D8), ADR 0010 (D9); feeds WP-A1, WP-S5, WP-A5, WP-F3, WP-F11, WP-D3

**Recommendation in one line:** move `IOpen3DSender`, `IOpen3DReceiver` and **one** transport registry into `Open3DShared` (no new module). A transport registers one immutable descriptor (factories, capabilities, option schema, config functions) and gets a handle back. The registry tracks live instances and drains them before a module unloads. Every method has a written threading contract, errors are a result type, and an exported interface version lets the WebRTC add-on refuse a mismatched host. The queue, worker, receive demux, audio sink guard, host:port parsing, backoff and FFI loader are written once in Shared, and transports migrate one per PR behind forwarding shims kept for one release.

## Context

**Two registries, two customization maps, duplicated line for line (SHR-12, SND-23, RCV-28).**
- Factories: `O3DTransport::RegisterSender/CreateSender` (`Plugin/Source/Open3DSender/Public/O3DSenderRegistry.h:9-18`, a global `FCriticalSection` plus `TMap` at `Private/O3DSenderRegistry.cpp:10-11`) and the receiver copy (`Open3DReceiver/Private/O3DReceiverRegistry.cpp`).
- Customizations: `O3DSender::RegisterTransportCustomization` (`Open3DSender/Private/O3DSenderTransportCustomization.cpp:10-18`) and the receiver copy.
- Pickers list customization names (`O3DSenderComponent.cpp:723`, `O3DReceiverSource.cpp:424`), while instances come from the factory map (`O3DSenderTransportController.cpp:21`, `O3DReceiverSource.cpp:325`). The two can disagree.
- `FindTransportCustomization` returns a raw pointer into the map after the lock is released (`O3DSenderTransportCustomization.cpp:26-30`; receiver `:28-32`), and callers invoke its `TFunction`s (`O3DSenderComponent.cpp:341`, `O3DReceiverSource.cpp:480`) (RCV-27).
- `Open3DShared/Public/O3DTransportRegistry.h:5-11` `__has_include`s paths that never resolve, so it is empty.
- Every transport registers four times in `StartupModule` (for example `Open3DTransportSocketsModule.cpp:35-91`).

**No lifetime contract (SHR-13, TRF-14).** `ShutdownModule` only unregisters (`Open3DTransportSocketsModule.cpp:96-108`). Live instances whose code lives in the transport DLL stay referenced by components. WebRTC registers factories even when `livekit_ffi.dll` failed to load (`Open3DTransportWebRTCModule.cpp:617-623`) and calls `FreeDllHandle` with no instance check (`:777-781`). MoQ returns early on load failure (`Open3DTransportMoQModule.cpp:561-565`), which is the correct pattern.

**Interface shape (SHR-12, SHR-14).**
- `IOpen3DSender` (`Open3DSender/Public/O3DSenderInterface.h:27-90`) returns `bool` everywhere. `SendSerialized` has a default body that returns false (`:73-77`). `Send(const O3DS::SubjectList&)` (`:45`) is off the normal path (`O3DSenderComponent.cpp:363-385` only calls `SendSerialized`).
- `IOpen3DReceiver` (`Open3DReceiver/Public/O3DReceiverInterface.h:22-42`) has `Poll()` and `SetConsumer`; neither says which thread calls it.
- `FO3DTransportStats` (`Open3DShared/Public/O3DTransportTypes.h`, end of file) has no state, error or queue-depth field.
- Neither interface has a version or a capability query.

**What each `SendSerialized` does today** (the D5 worker will call it):

| Transport | Behaviour | Evidence |
|---|---|---|
| TCP | copies into a framed buffer and enqueues; rejects the newest when full | `SocketsTcpSender.cpp:205-213`, `:553-588` |
| UDP | calls `SendTo` synchronously under `OwnerGuard->Lock` (TRB-20) | `SocketsUdpSender.cpp:211-240` |
| NNG | enqueues; the worker requeues at the tail on `EAGAIN` and sleeps (TRB-34) | `NngSender.cpp:294-315`, `:533-545` |
| MoQ | enqueues into its own MPSC queue | `MoQSender.cpp:315-331`, `MoQSender.h:121` |
| Loopback | enqueues into the in-process channel | `LoopbackSender.cpp:175-189` |
| WebRTC | calls `lk_send_data_ex` inline on the caller's thread, behind a heuristic backpressure estimate (TRF-5) | `WebRTCSender.cpp:842-935` |

Four transports carry their own `TQueue<..., EQueueMode::Mpsc>` (TCP `SocketsTcpSender.h:104`, NNG `NngSender.h:83`, MoQ `MoQSender.h:121`, Loopback `LoopbackChannel.h:51`). TCP and NNG account bytes with a non-atomic load-then-store (`SocketsTcpSender.cpp:517-518`, `:564-580`; TRB-3).

**Audio sinks (T2).** Sockets hands the sink a `TSharedPtr` to an `OwnerGuard { FCriticalSection Lock; Owner* }` (`SocketsTcpSender.h:28-32`, `SocketsTcpSender.cpp:48-76`). That fixes the use-after-free that Loopback, NNG, WebRTC and MoQ still have (TRB-30, TRB-35, TRF-1). But the same lock is held across the whole encode on the audio thread and across `Socket->Send` on the worker (`SocketsTcpSender.cpp:523-533`; TRB-10), so copying the guard as-is would copy the contention. The sink base `FO3DSenderAudioSinkBase` lives in Open3DSender (`Open3DSender/Public/O3DSenderAudioSinkBase.h:11`).

**Receive side.** TCP, UDP and NNG each carry a copy of the demux (`SocketsTcpReceiver.cpp:405-492`, `SocketsUdpReceiver.cpp:389-476`, `NngReceiver.cpp:487-582`; TRB-38). Each copies the payload into a new `TArray` only because `ISerializedFrameConsumer::SubmitFrame` takes `const TArray<uint8>&` (`Open3DShared/Public/SerializedFrameConsumerRegistry.h:20`; SHR-16). Every receiver calls `SubmitFrame` from inside `Poll()`, and `Poll()` runs on the game thread from `FO3DReceiverSource::Tick` (`O3DReceiverSource.cpp:278-285`). `FSerializedFrameConsumerRegistry` is never populated (SHR-24). MoQ holds the consumer weakly, WebRTC strongly (TRF-38).

**Config.** `FO3DTransportConfig` carries LiveKit-only fields (`O3DTransportTypes.h:66-99`, SHR-36) and free-form `FString Transport/Role`. `ConfigureTransport` takes `const UO3DSenderComponent*` (`O3DSenderTransportCustomization.h:15`), so every transport module depends on Open3DSender and Open3DReceiver (`Open3DTransportLoopback.Build.cs:28-29`, `Open3DTransportNNG.Build.cs:59-60`, `Open3DTransportWebRTC.Build.cs:86-87`, MoQ `:136-137`, Sockets `:35-40`).

**Constraints from accepted ADRs.**
- 0002: the add-on uses only exported API and needs an interface version checked at `StartupModule`.
- 0003: the core is the `Open3DStreamCore` module.
- 0004: `SecretOptionKeys`/`SecretEnvVars` on the customizations, `FO3DSecretStore` and `O3DRedact` in Shared; a typed config must keep "declared secret, resolved from the store, never serialized".
- 0005: `EO3DDeliveryGuarantee` in Shared, `GetDeliveryGuarantee` on the customization "moves to the capability query with the same values", and `IOpen3DSender::SetPeerJoinedCallback`.
- 0006: fakes are registered through the registry under unique names; the conformance suite enumerates registered transports and needs concurrent `SendSerialized` to be safe.

## Decision drivers

1. Memory safety across module unload and across audio, FFI and worker threads (T2).
2. The add-on plugin compiles against a small, exported, versioned API and fails cleanly on a mismatch.
3. One place to fix queue, demux and sink bugs (T5); net transport code goes down.
4. No per-frame allocation or lock contention added on the send and receive paths.
5. Incremental: each PR leaves every transport working and the conformance suite green.

## Options considered

### Where the abstraction lives
- **A. `Open3DShared` (chosen).** Shared already holds `FO3DTransportConfig`, the unified envelope, the audio codec and the consumer interface, and 0004 and 0005 already put `FO3DSecretStore`, `O3DRedact` and `EO3DDeliveryGuarantee` there. Transports then depend on Shared (and `Open3DStreamCore` where they use core code) and no longer on Sender or Receiver. No new module. Con: Shared grows by about 1,500 lines.
- **B. New `Open3DTransportCore` module.** Cleaner name, but Shared would then hold only audio and metrics helpers, both of which the transport core needs, so the split adds a module (twelve with 0003, 0006 and D9) without removing a dependency edge. Rejected.
- **C. Keep the interfaces in Sender and Receiver, merge only the registries.** Keeps the Sender/Receiver edge from every transport and from the add-on. Rejected.

### Registry shape
- **R1. One registry of immutable descriptors, one per transport name, returned as `TSharedPtr<const>` (chosen).** Fixes RCV-27 and RCV-28 by construction.
- **R2. A templated `TO3DFactoryRegistry<T>` per role (SHR-12's suggestion).** Removes the copy-paste but keeps factories and customizations apart.

### Error model
- **E1. `FO3DTransportResult { EO3DTransportError Code; FString Message; }` for lifecycle calls, a plain enum for per-frame calls (chosen).** No `FString` allocation per frame.
- **E2. `TValueOrError`.** Equivalent, but its UE 5.7 header and API are **needs-verification** (Q1), and it gives no advantage for `void` results.

### Config
- **K1. Typed per-transport `UStruct`s (`FInstancedStruct`).** Blueprint-friendly, but an asset saved with a struct from the add-on fails to load its settings when the add-on is disabled, and existing `TransportOptions` maps need migration.
- **K2. Keep the persisted `TMap<FString,FString>`, add a declared option schema with typed, validated accessors (chosen).** Assets stay loadable without the transport module. The schema is also the data the D9 editor module builds widgets from, and its `Secret` type is the typed form of 0004's `SecretOptionKeys`.

## Decision

**1. Home and module graph.** `IOpen3DSender`, `IOpen3DReceiver`, `ISerializedFrameConsumer`, the registry and the building blocks live in `Open3DShared/Public/Transport/`, exported with `OPEN3DSHARED_API`. Transport modules depend on `Open3DShared` (and `Open3DStreamCore` if they include core headers); they drop Open3DSender and Open3DReceiver. Sender and Receiver keep depending on Shared.

**2. Interface version.** `Open3DShared/Public/Transport/O3DTransportApiVersion.h` defines `O3D_TRANSPORT_API_VERSION` (an integer). `O3DTransport::GetHostApiVersion()` is an exported function whose name and signature never change. Add-ons compare the host value with their compile-time value in `StartupModule` and do not register on a mismatch (ADR 0002). `FO3DTransportDescriptor::ApiVersion` carries the compile-time value and the registry rejects mismatches as a second check. **Bump rule:** any change to a public header under `Transport/` that alters a class layout, a vtable, a descriptor field or a documented threading rule. WP-F11 introduces version 1 before this ADR's code lands; WP-A1 PR 1 moves to 2.

**3. The interfaces.** Threading rules are part of the contract and are written in the header next to each method.

| `IOpen3DSender` | Called from | Contract |
|---|---|---|
| `Initialize(const FO3DTransportConfig&) -> FO3DTransportResult` | game thread | non-blocking; validates config |
| `Start() -> FO3DTransportResult` | game thread | non-blocking; connects on the transport worker (WP-A5) |
| `Stop()` | game thread | idempotent; non-blocking; safe while `SendSerialized` or an audio sink call is in flight; later sends return `NotRunning` |
| `SendSerialized(FO3DSendPayload&&) -> EO3DSendResult` | any thread (the D5 pipeline worker in practice) | pure virtual; thread-safe; never blocks; takes ownership of the bytes |
| `Tick(float)` | game thread | upkeep only; never blocks |
| `GetStats()`, `GetConnectionState()` | any thread | lock-free snapshot |
| `CreateAudioSink(...)` | game thread | returns a sink that obeys item 7 |
| `SetPeerJoinedCallback(TFunction<void()>)` | game thread, before `Start` | as ADR 0005 (vi): callback may fire on any thread |
| `SetStateChangedCallback(TFunction<void(EO3DConnectionState, const FO3DTransportResult&)>)` | game thread, before `Start` | callback may fire on any thread; must not call back into the transport |

`FO3DSendPayload { TArray<uint8> Bytes; FName Subject; double CaptureTimeSec; bool bFullSync; }`. `EO3DSendResult { Queued, DroppedBackpressure, NotRunning, NotConnected, Invalid }`. The D5 worker uses `bFullSync` plus a `DroppedBackpressure` result to request the next full sync early. `Send(const O3DS::SubjectList&)` is deprecated in PR 1 and deleted in PR 5 (also removes the core dependency ADR 0002 blocker 2 mentions).

| `IOpen3DReceiver` | Called from | Contract |
|---|---|---|
| `Initialize`, `Start`, `Stop` | game thread | as the sender; `Start` without a consumer returns `NoConsumer` |
| `SetConsumer(TSharedPtr<ISerializedFrameConsumer>)` | game thread, before `Start` | the receiver holds a **strong** reference and releases it in `Stop` (TRF-38) |
| `Poll() -> int32` | game thread | bounded work (TRB-18); the only place the consumer is called |
| `SetAudioSink(...)` | game thread | the sink may be called on any thread |
| `GetStats()`, `GetConnectionState()` | any thread | lock-free snapshot |

`ISerializedFrameConsumer` becomes `SubmitFrame(FName Subject, TConstArrayView<uint8> Bytes, double ReceiveTimeSec)` (view valid for the call only) plus `SubmitFrameOwned(FName, TArray<uint8>&&, double)` whose default forwards to the view form. Consumers are called only from `Poll()`, on the thread that calls `Poll()`. SHR-16 suggested "the transport worker thread"; this ADR keeps the Poll thread because every receiver already delivers there and the receiver source's gate is single-threaded (`O3DReceiverSource.cpp:286-289`). `FSerializedFrameConsumerRegistry` is deleted (SHR-24).

`FO3DTransportResult { EO3DTransportError Code; FString Message; bool IsOk() const; }` with codes `None, InvalidConfig, NoConsumer, NotRunning, ResourceUnavailable, AddressInUse, ConnectFailed, AuthFailed, Timeout, Unsupported, Internal`. `EO3DConnectionState { Idle, Connecting, Connected, Reconnecting, Failed }`. `FO3DTransportStats` gains `State`, `SendErrors`, `ReceiveErrors`, `PendingFrames`, `PendingBytes`.

**4. The descriptor and registry.**
```cpp
struct FO3DTransportDescriptor {                // immutable after Register
  FName Name; FText DisplayName; uint32 ApiVersion; FName OwningModule;
  TFunction<TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>()>   CreateSender;    // optional
  TFunction<TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>()> CreateReceiver;  // optional
  TFunction<FO3DTransportCapabilities(const FO3DTransportConfig&)> GetCapabilities;
  FO3DTransportOptionSchema SenderOptions, ReceiverOptions;     // item 8; drives D9 widgets
  TFunction<void(const FO3DTransportOptionsView&, FO3DTransportConfig&)> ConfigureSender, ConfigureReceiver;
};
```
- `FO3DTransportCapabilities { bool bSend, bReceive, bAudioSend, bAudioReceive, bBidirectional, bPeerJoinSignal; EO3DDeliveryGuarantee Delivery; int32 MaxPayloadBytes; }`. `Delivery` takes over ADR 0005's `GetDeliveryGuarantee` with the same per-transport values.
- `ESPMode::ThreadSafe` is written explicitly everywhere (SHR-12).
- `FO3DTransportRegistry::Get()` is owned by the Open3DShared module object. `Register(TSharedRef<const FO3DTransportDescriptor>) -> FO3DTransportRegistration` (a move-only handle whose destructor unregisters). `Find(FName) -> TSharedPtr<const FO3DTransportDescriptor>`. `GetNames(EO3DTransportRole)` lists only entries that have a factory for that role (RCV-28).
- Mutation and `Create*` are game-thread-only (`check(IsInGameThread())`). `Find` and `GetNames` are any-thread under an `FRWLock` read lock. Factories and config functions are always invoked outside the lock, as `O3DSenderRegistry.cpp:74-89` already does.
- `OnTransportsChanged` fires after each register and unregister so pickers refresh.
- A transport registers only after its dependencies (FFI DLL, `moq_init`) succeeded (TRF-14).

**5. Lifetime and unload (SHR-13, TRF-14).**
- `Create*` wraps each instance's weak reference in a per-name live list.
- Unregistering broadcasts `OnTransportUnregistering(FName)`. The sender transport controller and the receiver source subscribe, call `Stop()` and drop their references. The registry then calls `Stop()` on anything still alive and logs an Error with the remaining count, naming the owner if known.
- `FO3DFfiLibrary` (item 7) refuses `FreeDllHandle` while its transport's live count is non-zero and leaves the DLL loaded until process exit instead.
- Whether UE shuts transport modules down before or after world teardown at editor exit and during Live Coding is **needs-verification** (Q2); the drain makes either order safe for our own references.

**6. Global state (SHR-38).** No injected runtime context in v1: nothing needs per-PIE isolation, and SHR-38 rates it L. What remains global, and why:
- the transport registry and `FO3DSecretStore` (0004): process-wide by nature; module-owned, created in `StartupModule`, destroyed in `ShutdownModule`;
- `FO3DPerformanceMetrics`: an aggregate diagnostic; transports own their own counters (WP-S10) and publish into it;
- `FO3DAudioBus`: game-thread-only, enforced with `check` (SHR-10);
- console variables and commands: module-scoped (WP-A6).
- Deleted: `FSerializedFrameConsumerRegistry`, the `O3DTransportRegistry.h` umbrella, the `O3DBuildFlags` cache (WP-F2).
- Each keeps a `ResetForTesting()` for the 0006 suite.

**7. Shared building blocks (`Open3DShared/Public/Transport/`).**
- **`FO3DSendQueue` plus `FO3DTransportWorker`.** Bounded MPSC queue of typed items (mocap bytes, encoded audio, control), with `fetch_add`/`fetch_sub` byte and count accounting (TRB-3). Soft cap: the worker drops the oldest mocap items while over the cap. Hard cap (twice the soft cap): producers get `DroppedBackpressure`. Audio and control items are never dropped for mocap. Optional age limit (TRB-14). The worker is an `FRunnable` with a wake event that also runs connect, disconnect, reconnect and FFI lifecycle calls (TRF-7, WP-A5) and a `FO3DReconnectPolicy` backoff (exponential, jitter, reset on success; TRB-4, TRF-6, TRF-20). UDP and WebRTC move their sends onto it (TRB-20).
- **`FO3DUnifiedReceiveDemux`.** Classifies bytes by the D8 header rules (ADR 0009), routes mocap to the consumer without copying and audio to a per-stream decoder map keyed by `(SourceGuid, StreamLabel)` with an LRU cap (SHR-15). Replaces the three copies and closes TRB-37.
- **`FO3DSenderAudioSinkBase` (moves from Open3DSender) with `FO3DAudioPublishState`.** The sink holds a `TSharedPtr<FO3DAudioPublishState, ESPMode::ThreadSafe>`, never the sender. The state holds an `FRWLock`, a `bOpen` flag, the per-stream encoders and a handle to the transport's `FO3DSendQueue`. `SubmitPcm` takes the read lock, returns if closed, encodes into sink-local scratch, and enqueues. `Stop()` takes the write lock, clears `bOpen`, and only then releases handles. No lock is ever held across a socket or FFI call, and FFI audio publish runs on the transport worker (TRF-1, TRB-10, TRB-30, TRB-35). The receiver side gets the mirror `FO3DReceiverAudioSinkBase` holding immutable metadata only (RCV-1).
- **`O3DTransportOptions`**: strict `ParseHostPort` (port 1 to 65535, bracketed IPv6) and typed getters; hostname resolution happens on the worker (`GetAddressInfoAsync`, **needs-verification**, Q3) (TRB-26, SHR-9: delete `NormalizeTcpUrlHostPort`).
- **`FO3DFfiLibrary`**: finds the DLL relative to the *owning* plugin (fixes ADR 0002 blocker 1 and TRF-28), reports load status, and implements the unload rule in item 5.
- Shared audio (de)serializers with one header reader and writer (SHR-35).

**8. Typed config (K2).** `FO3DTransportConfig` keeps `FName Transport`, `EO3DTransportRole Role`, `Uri`, `StreamId`, `Audio`, `Secrets` (0004) and gains `FO3DTransportOptionsView Options`. The LiveKit fields and `bPersistToken` are removed (SHR-36); WebRTC declares them in its schema. `FO3DTransportOptionSchema` is an array of `{Key, DisplayName, Tooltip, Type (String, Int, Float, Bool, Enum, Url, Secret), Default, Min, Max, EnumValues, VisibleWhen, bRestartOnChange}`. Keys stay namespaced (`webrtc.url`), and switching transports no longer clears other transports' keys (SND-35). A `Secret` entry is the typed form of 0004's `SecretOptionKeys`; its env var name moves into the entry. Persistence, the store and redaction behave exactly as 0004 says. Role names and transport names use the registered names everywhere (TRB-27).

**9. Migration.** The old headers (`O3DSenderInterface.h`, `O3DSenderRegistry.h`, `O3DSenderTransportCustomization.h` and the receiver equivalents) stay for one release as forwarding shims marked `UE_DEPRECATED` (**needs-verification** of the 5.7 macro, Q1). The old `RegisterSender` plus `RegisterTransportCustomization` pair builds a descriptor internally, so the add-on built against the previous release keeps working. Shims are deleted in the release after.

## Consequences

- **Easier:** one fix for each queue, demux and sink bug; transports and the add-on depend on Shared alone; the conformance suite (0006) and the D9 editor module both enumerate one registry; the editor gets a data schema instead of per-transport Slate code.
- **Harder:** a large, staged migration (WP-A1, size L); the interface version must be bumped with discipline; the add-on must be rebuilt per release (already true under 0002).
- **Constrains:** D5 calls `SendSerialized` from its worker and relies on "non-blocking, thread-safe". D9 builds panels from `FO3DTransportOptionSchema`. ADR 0004's per-customization fields become schema entries with unchanged semantics. ADR 0006's fake transports register descriptors. FFI transports keep the per-instance function tables (0006 F2) behind the worker.
- **Not decided here:** receive-side threading for UDP beyond bounded `Poll` (TRB-18 stays in WP-S2).

## Implementation outline

WP-A1 PR sequence; each PR keeps all transports working and the conformance suite green.
1. **Interfaces and registry (SHR-12, SND-23, RCV-27, RCV-28, SHR-24).** New `Open3DShared/Public/Transport/{O3DTransportApiVersion.h, O3DSenderInterface.h, O3DReceiverInterface.h, O3DTransportRegistry.h, O3DTransportTypes.h}` and `Private/Transport/O3DTransportRegistry.cpp`. Old Sender/Receiver headers become shims; delete `Open3DShared/Public/O3DTransportRegistry.h` and `SerializedFrameConsumerRegistry.*`. Sender and Receiver call the new registry (`O3DSenderComponent.cpp:341`, `:723`, `O3DSenderTransportController.cpp:21`, `O3DReceiverSource.cpp:325`, `:424`, `:480`, `O3DReceiverSourceFactory.cpp:263`, `:392`, `O3DSenderComponentCustomization.cpp:389`, `:475`).
2. **Lifetime (SHR-13, TRF-14).** Live lists, `OnTransportUnregistering`, controller and source subscriptions, `FO3DFfiLibrary`; WebRTC and MoQ modules adopt it (`Open3DTransportWebRTCModule.cpp:614-785`, `Open3DTransportMoQModule.cpp:558-597`). Lands with WP-F3.
3. **Results, state, capabilities (SHR-14).** `FO3DTransportResult`, `EO3DSendResult`, `FO3DSendPayload`, connection state and callbacks, `FO3DTransportCapabilities` (absorbs 0005's `GetDeliveryGuarantee`), `SendSerialized` pure virtual.
4. **Building blocks, one transport per PR.** Order: Loopback (smallest), TCP, UDP, NNG, MoQ, then WebRTC in the add-on. Each PR deletes that transport's queue, demux, sink and option-parsing copies (TRB-3, TRB-38, TRF-32, SHR-35, TRB-26, SHR-9) and removes its Sender/Receiver Build.cs dependency.
5. **Typed config and consumer API (SHR-36, TRB-27, SHR-16, TRF-38).** Option schemas per transport, `FO3DTransportOptionsView`, removal of the LiveKit fields, `SubmitFrame` view and owned forms, deletion of `Send(SubjectList)`.
6. **Following release:** delete the shims and bump `O3D_TRANSPORT_API_VERSION`.

Docs (WP-D3): the threading tables above go into `docs/transports.md`.

## Verification / acceptance

- The 0006 conformance suite passes for every transport after each PR, plus new cases: `Start` without consumer fails with `NoConsumer`; `Stop` while four threads call `SendSerialized` and a fake audio thread submits PCM is clean under ASan (WP-S5 stress, 1,000 cycles); an unregister during a live session stops and releases the instance and logs no leak.
- An add-on built with a different `O3D_TRANSPORT_API_VERSION` logs the mismatch and registers nothing (WP-F11 acceptance).
- `grep -rn "TQueue<.*Mpsc>" Plugin/Source/Open3DTransport*` finds nothing outside tests; no transport Build.cs names Open3DSender or Open3DReceiver.
- No raw pointer or reference to a sender, receiver or UObject is reachable from a sink or worker (review checklist §7 item 2).
- Net lines under `Plugin/Source/Open3DTransport*` go down (WP-A1 acceptance).

## Open questions for the maintainer

1. **needs-verification:** UE 5.7 spellings of `TValueOrError`, `UE_DEPRECATED` and `FRWLock` read/write scope helpers. Default: plain result struct (E1), which needs none of them.
2. **needs-verification:** at editor exit, hot reload and Live Coding, are plugin modules shut down before or after worlds and components are destroyed? Default: drain in `ShutdownModule` regardless, and never free an FFI DLL with live instances.
3. **needs-verification:** does `ISocketSubsystem::GetAddressInfoAsync` exist in 5.7 with a callback usable from a worker thread? Default: call the synchronous `GetAddressInfo` on the transport worker, which is off the game thread either way.
4. Keep global singletons (module-owned) instead of an injected `FO3DRuntimeContext` for v1? **Default: yes**, revisit only if per-PIE isolation is requested.
5. Forwarding shims for exactly one release? **Default: yes**, removed in the next minor release with an API version bump.
6. Consumers called on the Poll thread (game thread today) rather than on a transport worker? **Default: Poll thread**, as above.

## References

- Findings: SHR-12, SHR-13, SHR-14, SHR-16, SHR-24, SHR-36, SHR-38, SND-23, RCV-27, RCV-28, TRB-38, TRF-32, TRF-38, TRF-14; building blocks TRB-3, TRB-10, TRB-30, TRB-35, TRF-1, TRF-5, TRF-6, TRF-7, TRF-17, TRF-18; also TRB-26, TRB-27, SHR-9, SHR-35, SHR-15, RCV-1, TRB-20, TRB-34, TRF-28.
- Files: `Plugin/Source/Open3DSender/Public/{O3DSenderInterface.h,O3DSenderRegistry.h,O3DSenderTransportCustomization.h,O3DSenderAudioSinkBase.h}` and `Private/` counterparts; `Plugin/Source/Open3DReceiver/Public/{O3DReceiverInterface.h,O3DReceiverTransportCustomization.h}`; `Plugin/Source/Open3DShared/Public/{O3DTransportRegistry.h,O3DTransportTypes.h,SerializedFrameConsumerRegistry.h,O3DPerformanceMetrics.h,O3DAudioBus.h}`; every `Open3DTransport*/Private/*Module.cpp`, `*/Private/Sender/*Sender.{h,cpp}`, `*/Private/Receiver/*Receiver.cpp`; the transport `*.Build.cs` files.
- External: none fetched specifically for this ADR.
