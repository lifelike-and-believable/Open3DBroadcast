# 0012: Runtime services and global state

- **Status:** Accepted (maintainer sign-off 2026-10-03)
- **Accepted with defaults:** the open questions below are accepted with the default given next to each. The needs-verification item stays open and is resolved in the implementing PR; a result that invalidates the decision is handled by a superseding ADR.
- **Revised before acceptance (2026-10-03):** the first draft deferred the runtime context (stage 2) until a multi-world trigger. The maintainer asked what argued against doing it now. Deferring it would turn a free transport API change into a breaking one after the first release, rewrite the stage 1 call sites, and ship two overlapping ways to scope audio, so both stages are done in one series.
- **Date:** 2026-10-03
- **Plan decision:** SHR-38 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) (WP-A3 left it for a design decision)
- **Related:** [ADR 0007](0007-transport-abstraction-and-registry.md) (registry, `FO3DTransportConfig`, transport metrics handles), [ADR 0011](0011-control-channel.md) (control bus), [ADR 0006](0006-test-module-layout-and-fakes.md) (test isolation)

**Decision in one line:** keep the process-wide services that are process-wide by nature, and move the data-path services (metrics, audio bus, control bus) into an `FO3DRuntimeContext` now, while the transport API it touches is still unreleased. A process default context keeps every static accessor working; transports get their context through `FO3DTransportConfig`; receivers and senders hold per-instance metrics handles from their context; and an engine subsystem hosts named contexts that receiver sources, sender components and the remote audio and control components select by name.

## Context

**The finding.** SHR-38 (`docs/review/2026-09-plugin-review/shared.md`) lists five process-wide mutable globals in Shared and recommends an injectable `FO3DRuntimeContext`. Effort L, owner design. One of the five, the serialized-frame consumer registry, has since been removed (`Plugin/Source/Open3DShared/Public/Transport/O3DSerializedFrameConsumer.h:31`).

**What is global today.**

| Service | Where | Kind |
|---|---|---|
| Audio bus | one static multicast delegate, `Plugin/Source/Open3DShared/Private/O3DAudioBus.cpp:7`; `FO3DAudioBus::OnPcm16()` and `PublishPcm16()`, `Plugin/Source/Open3DShared/Public/O3DAudioBus.h:24,30` | data path |
| Control bus | static state keyed by source id, `Plugin/Source/Open3DShared/Private/O3DControlBus.cpp` (`FSourceState`, `:41`); `OnChange`, `Publish`, `ForgetSource`, `ResetForTesting`, `Plugin/Source/Open3DShared/Public/O3DControlBus.h:52-82` | data path, already per source |
| Performance metrics | Meyers static, `Plugin/Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:15-17`; sender and receiver counters live in the singleton (`Plugin/Source/Open3DShared/Public/O3DPerformanceMetrics.h:191`) | data path |
| Transport metrics | per-transport handles from `FO3DTransportMetricsRegistry` (`O3DPerformanceMetrics.h:155`), acquired with `AcquireTransportMetrics` (`:319`) | already per transport name |
| Transport registry | `FO3DTransportRegistry::Get()`, `Plugin/Source/Open3DShared/Public/Transport/O3DTransportRegistry.h:195` | process-wide by nature |
| Secret store | `FO3DSecretStore::Get()`, `Plugin/Source/Open3DShared/Public/O3DSecretStore.h:104` | process-wide by nature (one user session) |
| Audio input devices | `FO3DAudioInputDevices::Get()`, `Plugin/Source/Open3DSender/Public/O3DAudioInputDevices.h:25` | process-wide by nature (hardware) |
| MoQ async dispatcher | `Plugin/Source/Open3DTransportMoQ/Private/Shared/MoQAsyncDispatcher.cpp:11` | process-wide by nature (one FFI runtime) |
| Console variables | 16 `TAutoConsoleVariable`s in 7 files (Receiver, Sender, Loopback) | process-wide by nature |

**Who uses the data-path globals.**
- The receiver source publishes PCM to the one audio delegate (`Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:226`), and every `UO3DRemoteAudioComponent` binds to it (`Plugin/Source/Open3DReceiver/Private/O3DRemoteAudioComponent.cpp:76`) and filters by subject and stream label only (`:103`, `:235-237`). The frame metadata already carries the receiver source's GUID (`O3DS::FAudioFrameMeta::SourceGuid`, `Plugin/Source/Open3DShared/Public/O3DUnifiedMessage.h:70-72`; set in `FinalizeAudioMeta`, `O3DReceiverSource.cpp:970`), but no component filters on it, so two receivers that carry the same labels cannot be told apart.
- The control router publishes with its source id (`Plugin/Source/Open3DReceiver/Private/O3DReceiverControlRouter.cpp:67`) and forgets it on teardown (`:77`). The control bus is therefore already keyed; only its delegate and settings override are shared.
- `FO3DPerformanceMetrics::Get()` is called from the receiver source (16 sites), stream scheduler (2), frame decoder (1), concealment (1), and the NNG and MoQ senders (6 each). Every receiver and sender in the process adds into the same counters.

**What it costs today.**
- **Tests depend on run order.** Tests read metric deltas instead of values (for example `Plugin/Source/Open3DBroadcastTests/Private/Receiver/O3DReceiverFrameDecoderTests.cpp:195`, and `UpdatesAwaitingFullSync` in `O3DReceiverResyncTests.cpp`), and control tests call `ResetForTesting`. Automation tests cannot run in parallel.
- **No per-instance numbers.** With two receivers, the HUD and `o3d.DumpMetrics` show their sum.
- **No isolation.** Audio and control from one receiver reach every listening component in the process (multi-client PIE, editor plus PIE, a monitor receiver next to a live one).

**Maintainer input (2026-10-03).** Multi-world is "rare but not something I'd rule out".

**Not in scope of any option below.** LiveLink subjects are owned by the LiveLink client, which this plugin does not create; whether the client is shared across PIE worlds is **needs-verification** (Q4). A context can isolate audio, control and metrics, not LiveLink subject names, which stay distinct per receiver as today.

## Decision drivers

1. Do not break users who bind `FO3DAudioBus::OnPcm16()`, `FO3DControlBus::OnChange()` or read `FO3DPerformanceMetrics::Get()` directly.
2. Make tests independent of run order without workarounds.
3. Per-instance metrics, so two receivers can be told apart.
4. Isolation for multi-world and multi-receiver setups, proportionate to how rare they are.
5. Change the transport API at most once, and while the current version is unreleased if it must change at all.

## Options considered

### Option 1: keep the globals, add test isolation only
`ResetForTesting` on every global, tests reset in setup.
- Pros: smallest change.
- Cons: no per-instance metrics, no isolation; tests still cannot run in parallel.
- Cost: S. Risk: low.

### Option 2: per-instance handles and keyed buses
Metrics as handles per receiver source and sender component, aggregated by the global view (the transport metrics registry already works this way). Audio frames carry the receiver source id; components may filter on it. The control bus is already keyed.
- Pros: additive (no API removed, no transport API change); per-instance metrics; tests read their own handle; a component can listen to one receiver.
- Cons: buses stay process-wide, so isolation is opt-in filtering, not separation.
- Cost: M. Risk: low.

### Option 3: host the services in a `UEngineSubsystem`
- Pros: defined lifetime instead of function statics.
- Cons: alone, it changes ownership but not reach; every caller still finds one instance.
- Cost: M. Risk: low.

### Option 4: inject an `FO3DRuntimeContext` through `FO3DTransportConfig`
SHR-38's recommendation.
- Pros: explicit dependencies; real isolation per context; tests build their own context; a stable seam for third-party transports.
- Cons: touches every transport including the WebRTC add-on, changes the transport API, and in practice needs option 3 as the owner. Its benefit over option 2 is isolation, which today is a rare case.
- Cost: L. Risk: medium (plumbing creep).

## Decision

**Options 2, 3 and 4 together, in one PR series now.** The transport API version this changes (`O3D_TRANSPORT_API_VERSION` 5) has not shipped in a release (the last tag is v0.9.6), so the transport seam is free now and would be a breaking change after the first Fab release. Doing per-instance handles first and the context later would rewrite the same call sites twice and ship a stop-gap audio filter that the context then duplicates.

1. **Keep as process-wide, and document it** in their headers and the developer docs: the transport registry, the secret store, the audio input devices, the MoQ dispatcher and the console variables. Each is process-wide because what it represents is (registered modules, one user session, the machine's hardware, one FFI runtime, the console).

2. **`FO3DRuntimeContext`** (Open3DShared, `TSharedRef<..., ESPMode::ThreadSafe>`) owns one instance of each data-path service: performance metrics (including its transport metrics registry), an audio bus and a control bus. `FO3DRuntimeContext::Default()` is the process default, created when Open3DShared starts.
   - The static accessors keep their signatures and forward to the default context: `FO3DAudioBus::OnPcm16()` and `PublishPcm16()`, every `FO3DControlBus` static, and `FO3DPerformanceMetrics::Get()`. Code that uses them today, including third-party code, behaves as before.
   - `FO3DPerformanceMetrics`, the audio bus and the control bus become instantiable classes (the metrics singleton's private destructor goes).
   - A test builds its own context and reads its own numbers, with no deltas and no `ResetForTesting`.

3. **Transport seam.** `FO3DTransportConfig` gains `Context` (a shared pointer; empty means the default context). Transports take their metrics handles from the config's context instead of `FO3DPerformanceMetrics::Get()`. Today that is three senders: NNG, MoQ and the WebRTC add-on. This joins transport API version 5, recorded in its history comment like the other unreleased changes.

4. **Per-instance metrics handles.** A receiver source and a sender component acquire a receiver or sender metrics handle from their context, the same pattern as `FO3DTransportMetricsRegistry` (`O3DPerformanceMetrics.h:155`), and release it when destroyed; the registry keeps the totals of released handles.
   - A handle's `Record*` adds to its own counters and to the context's aggregate, so `GetReceiverMetrics()` and `GetSenderMetrics()` keep returning the same structs by reference (`O3DPerformanceMetrics.h:309,312`), and the HUD, `o3d.DumpMetrics` and the CSV show what they show today. Rolling averages and peaks stay aggregate only, since they cannot be summed.
   - The receiver source's private classes (scheduler, decoder, concealment) get the handle from the source, never the context.
   - `o3d.DumpMetrics` also lists each live handle with its owner's name.

5. **Hosting and selection.** `UO3DRuntimeSubsystem` (`UEngineSubsystem`) owns named contexts, created on first use; the empty name is `FO3DRuntimeContext::Default()`.
   - New `ContextName` properties (default empty) on the receiver source settings, `UO3DSenderComponent`, `UO3DRemoteAudioComponent` and `UO3DRemoteControlComponent`. A receiver source publishes audio and control to its context's buses; a component listens to its context's buses; a sender component passes its context to its transport.
   - With every name left empty, nothing changes. Two receivers given different names are separated: their audio, control and metrics never meet.
   - Selection is by name, not by world, so it works whether or not the LiveLink client is shared across worlds (Q4).
   - The GUID filter on the audio component that the first draft proposed is not added; contexts replace it.

6. **Boundary.** The context stops at transports, receiver sources, sender components and the gameplay components. Decoder, publisher, scheduler, pipeline and serializer classes receive the specific handle or bus they need from their owner.

## Consequences

- **Easier:** per-receiver and per-sender numbers; tests that build their own context instead of computing deltas or resetting globals, and could later run in parallel; real separation of two receivers, or of PIE clients, by giving them context names; one seam for third-party transports, versioned with the transport API.
- **Harder:** a larger series than handles alone (three transports and four user-facing classes); each counter is incremented twice (handle and aggregate); `ContextName` is one more property to explain in the docs.
- **Unchanged:** the wire; code that uses the static accessors; every setup that leaves the context names empty.
- **Not covered:** LiveLink subject names are owned by the LiveLink client and stay distinct per receiver as today.

## Implementation outline

1. **PR 1, the context** (items 2, 6): `FO3DRuntimeContext`, the default context, instantiable metrics and buses, statics forwarding. No behaviour change. Test: two contexts do not see each other's audio, control or metrics; the statics reach the default.
2. **PR 2, the transport seam** (item 3): `FO3DTransportConfig::Context`, the NNG, MoQ and WebRTC senders, the transport API history note. Test: a transport given a context records into it; a CI check that no transport calls `FO3DPerformanceMetrics::Get()`.
3. **PR 3, metrics handles** (item 4): handles, the receiver source and sender component on them, `DumpMetrics` per handle. Test: two receiver sources record into separate handles, and the aggregate equals the sum.
4. **PR 4, hosting and selection** (item 5): the subsystem, the `ContextName` properties, docs. Test: two receiver sources with different names deliver audio and control only to components with the same name.
5. **PR 5, documentation** (item 1): the process-wide services and why.

## Verification / acceptance

- With all context names empty, the HUD, `o3d.DumpMetrics` and CSV output, and every existing test, are unchanged.
- New tests read per-instance handles and per-context buses without deltas or resets.
- No transport calls `FO3DPerformanceMetrics::Get()` (checked in CI).

## Open questions for the maintainer

1. **Direct bus users.** Does any project outside this repository bind `FO3DAudioBus::OnPcm16()` or `FO3DControlBus::OnChange()`? **Accepted default:** the forwarding statics stay for at least two releases after this series, then are reviewed.
2. **Per-receiver metrics in the UI.** **Accepted default:** `o3d.DumpMetrics` lists them; the HUD shows the aggregate as today.
3. **Context names in the editor.** **Accepted default:** free-text names; the LiveLink source panel and the component details show the names in use.
4. **needs-verification:** whether the LiveLink client is shared across PIE worlds in UE 5.7. Selection by name does not depend on it; the answer goes into the user docs for multi-client PIE.

## References

- Findings: SHR-38; related SHR-25 (incomplete metrics `Reset()`), SHR-10 and SHR-18 (audio bus threading and zero-copy).
- Files: `Plugin/Source/Open3DShared/Public/O3DAudioBus.h`, `O3DControlBus.h`, `O3DPerformanceMetrics.h`, `Transport/O3DTransportRegistry.h`, `O3DSecretStore.h`; `Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp`, `O3DRemoteAudioComponent.cpp`, `O3DReceiverControlRouter.cpp`.
