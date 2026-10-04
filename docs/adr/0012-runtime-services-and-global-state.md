# 0012: Runtime services and global state

- **Status:** Proposed (pending maintainer sign-off)
- **Date:** 2026-10-03
- **Plan decision:** SHR-38 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) (WP-A3 left it for a design decision)
- **Related:** [ADR 0007](0007-transport-abstraction-and-registry.md) (registry, `FO3DTransportConfig`, transport metrics handles), [ADR 0011](0011-control-channel.md) (control bus), [ADR 0006](0006-test-module-layout-and-fakes.md) (test isolation)

**Recommendation in one line:** keep the process-wide services that are process-wide by nature, and fix the data-path globals in two stages. **Stage 1, now:** metrics become per-instance handles that also feed the existing global counters, and the remote audio component can filter on the receiver source that produced a frame (frames already carry its GUID). Both changes are additive; every static accessor keeps working. **Stage 2, only when a real multi-world or multi-receiver isolation case appears:** an `FO3DRuntimeContext` (metrics, audio bus, control bus), hosted by an engine subsystem and passed to transports through `FO3DTransportConfig`, with the statics forwarding to a default context.

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
5. Keep the transport API stable unless isolation actually requires a change.

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

**A staged hybrid: option 2 now, options 3 and 4 together later, only on a trigger.**

1. **Keep as process-wide, and document it** in `Plugin/Source/Open3DShared/Public` headers and the developer docs: the transport registry, the secret store, the audio input devices, the MoQ dispatcher and the console variables. Each is process-wide because what it represents is (registered modules, one user session, the machine's hardware, one FFI runtime, the console).

2. **Stage 1, metrics handles.**
   - New `FO3DReceiverMetrics` and `FO3DSenderMetrics` hold the counters now in `FO3DPerformanceMetrics::FReceiverMetrics` and `FSenderMetrics`. A receiver source and a sender component each acquire one handle from a registry in `FO3DPerformanceMetrics`, the same pattern as `FO3DTransportMetricsRegistry` (`O3DPerformanceMetrics.h:155`), and release it when destroyed; the registry keeps the totals of released handles.
   - A handle's `Record*` adds to its own counters and to the singleton's, so `FO3DPerformanceMetrics::Get().GetReceiverMetrics()` and `GetSenderMetrics()` keep returning the same structs by reference (`O3DPerformanceMetrics.h:309,312`), and the HUD, `o3d.DumpMetrics`, the CSV and every existing caller see what they see today. The `Record*` methods on the singleton stay, for callers that have no handle. Rolling averages and peaks stay global only, since they cannot be summed.
   - The receiver source and its private classes (scheduler, decoder, concealment) record into the source's handle; tests read that handle directly.
   - `o3d.DumpMetrics` also lists each live handle with its owner's name.

3. **Stage 1, audio source filter.**
   - `UO3DRemoteAudioComponent` gains an optional source filter matched against `FAudioFrameMeta::SourceGuid`, which the receiver source already sets (empty: every source, as today). A Blueprint-callable setter takes the GUID of a receiver source; how a user finds that GUID in the editor (for example from the LiveLink source list) is settled in the PR.
   - `FO3DAudioBus::OnPcm16()` and `PublishPcm16()` keep their signatures. `FO3DAudioBus::ResetForTesting()` is added, like the control bus's.

4. **Stage 2, a runtime context, on a trigger.** Built when a real case needs separation rather than filtering: multi-client PIE with distinct audio or control per client, nDisplay, or running automation tests in parallel. Its shape, decided now so stage 1 does not block it:
   - `FO3DRuntimeContext` owns a metrics registry, an audio bus and a control bus. A `UO3DRuntimeSubsystem` (`UEngineSubsystem`) owns a default context and named contexts.
   - `FO3DTransportConfig` gains a context pointer; empty means the default. Transports take metrics handles from it instead of `FO3DPerformanceMetrics::Get()`.
   - Receiver source settings and the remote audio and control components select a context by name (empty: default).
   - The static accessors forward to the default context, so stage 1 code and third-party callers keep working.
   - The transport API version changes (`O3D_TRANSPORT_API_VERSION`), unless no release has carried the current number, as before.

## Consequences

- **Easier:** per-receiver and per-sender numbers; tests that read their own handles; a component that listens to one receiver; stage 2 becomes a change of owner, not a rewrite, because the handle and source-id seams exist.
- **Harder:** each counter is incremented twice (handle and global), a small cost on atomics that are already per frame; the global must stay equal to the old numbers, which a test checks.
- **Unchanged:** the transport API, third-party code that binds the buses or reads the global metrics, the wire.
- **Not solved by stage 1:** two receivers in one process still share the audio and control delegates; isolation is by filter. Stage 2 is the fix if that is ever not enough.

## Implementation outline

1. **PR 1, metrics handles** (stage 1 item 2): registry, handles, receiver source and its classes on the handle, `DumpMetrics` per handle. Test: two receiver sources record into separate handles, and the global counters equal the sum.
2. **PR 2, audio source filter** (item 3): component filter, `ResetForTesting`. Test: two sources, one component filtering on one of them.
3. **PR 3, documentation** (item 1): the process-wide services and why, in headers and the developer guide.
4. **Stage 2:** its own work package, when triggered, with an implementing PR series and a conformance check that every transport takes its metrics from the config's context.

## Verification / acceptance

- Existing metrics tests and the HUD, `o3d.DumpMetrics` and CSV output are unchanged with one receiver and one sender.
- New tests read per-instance handles without computing deltas.
- No transport source file changes in stage 1.

## Open questions for the maintainer

1. **Stage 2 trigger.** Is "a concrete multi-world or parallel-test need" the right trigger, or should stage 2 be scheduled outright? Default: the trigger.
2. **Direct bus users.** Does any project outside this repository bind `FO3DAudioBus::OnPcm16()` or `FO3DControlBus::OnChange()`? Stage 1 keeps them working either way; the answer only matters for how long the stage 2 forwarding statics must stay. Default: keep them for at least two releases after stage 2.
3. **Per-receiver metrics in the UI.** Should the HUD show per-source rows, or is `o3d.DumpMetrics` enough? Default: `DumpMetrics` only.
4. **needs-verification:** whether the LiveLink client is shared across PIE worlds in UE 5.7, which bounds what any context can isolate for mocap.

## References

- Findings: SHR-38; related SHR-25 (incomplete metrics `Reset()`), SHR-10 and SHR-18 (audio bus threading and zero-copy).
- Files: `Plugin/Source/Open3DShared/Public/O3DAudioBus.h`, `O3DControlBus.h`, `O3DPerformanceMetrics.h`, `Transport/O3DTransportRegistry.h`, `O3DSecretStore.h`; `Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp`, `O3DRemoteAudioComponent.cpp`, `O3DReceiverControlRouter.cpp`.
