# Runtime services: what is per context and what is process-wide

The decision is [ADR 0012](../adr/0012-runtime-services-and-global-state.md) (SHR-38). It was implemented in #357 to #361, #364 and this page.

## Per context: `FO3DRuntimeContext`

A runtime context (`Open3DShared/Public/O3DRuntimeContext.h`) owns the services that carry stream data:

| Service | Type in the context | Static accessor (default context) |
|---|---|---|
| Performance metrics, including the transport metrics registry | `FO3DPerformanceMetrics` (`GetMetrics()`) | `FO3DPerformanceMetrics::Get()` |
| Audio bus | `FO3DAudioBus::FInstance` (`GetAudioBus()`) | `FO3DAudioBus::OnPcm16()`, `PublishPcm16()` |
| Control bus | `FO3DControlBus::FInstance` (`GetControlBus()`) | the `FO3DControlBus` statics |

- **The default context.** `FO3DRuntimeContext::Default()` is the process default. Every static accessor uses it, so code written against the statics behaves as it did before ADR 0012.
- **Named contexts.** `UO3DRuntimeSubsystem` (an engine subsystem) creates a named context the first time a name is used. The empty name (`NAME_None`) is the default context.
- **Who selects a context.** These classes select one with a `ContextName`:
  - the receiver source config (`FO3DReceiverSourceConfig`);
  - `UO3DSenderComponent`;
  - `UO3DRemoteAudioComponent`;
  - `UO3DRemoteControlComponent`.
- **Name rules.** Names are FNames, so they compare case-insensitively.
- **Isolation.** Two contexts never see each other's audio, control or metrics.

**Who gets what.** Item 6 of the ADR fixes this:
- Transports, receiver sources, sender components and the gameplay components hold a context.
- The classes they own receive only the handle or bus they need, never the context. Examples:
  - the receiver's decoder, scheduler, concealment and control router;
  - the receiver's audio sink;
  - the sender's pipeline and serializer.
- A transport gets its context in `FO3DTransportConfig::Context`. It records into that context, never into `FO3DPerformanceMetrics::Get()`. CI checks this with `Build/Scripts/check-transport-metrics.py`.
- A sender transport records sender metrics through `FO3DTransportConfig::SenderMetrics`, the sender component's handle. A handle from another context is `InvalidConfig`.

**Metrics handles.** Each receiver source and each sender owns a handle: `FO3DReceiverMetricsHandle` or `FO3DSenderMetricsHandle`.
- A handle adds to its own counters and to its context's aggregate. The aggregate therefore always equals the sum of every handle ever acquired, released ones included.
- Rolling averages and peaks are kept in the aggregate only.
- `o3d.DumpMetrics` prints the default context, then each named context, with one line per live handle.
- `o3d.ResetMetrics` resets every context.
- The HUD and the CSV export show the default context only.

**Tests.** Build an `FO3DRuntimeContext` of your own and read its numbers directly. That needs no deltas and no `ResetForTesting`. Named contexts created through the subsystem last for the whole process, so give test names the prefix `O3DTest.` and forget any control sources you publish.

## Process-wide by nature

These services are deliberately not part of a context (ADR 0012 item 1). In each case, the thing the service represents exists once per process.

| Service | Where | Why it is process-wide |
|---|---|---|
| Transport registry | `FO3DTransportRegistry::Get()`, `Open3DShared/Public/Transport/O3DTransportRegistry.h` | It lists the transports that the loaded modules registered. Modules load once per process and every context uses the same transports. Tests may still build a registry of their own. |
| Secret store | `FO3DSecretStore::Get()`, `Open3DShared/Public/O3DSecretStore.h` | It holds the credentials of the one user session the process runs for: session values, environment variables, then per-user settings. |
| Audio input devices | `FO3DAudioInputDevices::Get()`, `Open3DSender/Public/O3DAudioInputDevices.h` | It caches the machine's capture devices. Those are hardware, the same for every context. |
| MoQ async dispatcher | `Open3DTransportMoQ/Private/Shared/MoQAsyncDispatcher.h` | It moves work from moq-ffi threads onto the game thread. The process loads one moq-ffi runtime. |
| Console variables | 16 `TAutoConsoleVariable`s (`o3d.Sender.*`, `o3ds.Receiver.*`, `o3ds.Sender.*`, `o3ds.RemoteAudio.Debug`, `o3ds.Loopback.Audio.Debug`) | The console is one per process. These are debugging and tuning switches, not per-stream settings. Per-stream settings live on the components and sources. |
| Control receive override | `FO3DControlBus::SetReceiveOverride`, `UO3DControlLibrary` | It overrides a project setting (`UO3DControlSettings::bAcceptControl`) for the whole process, so it applies to receivers in every context. A per-source setting other than Project Default still wins over it. |

## Not covered by contexts

LiveLink subject names belong to the LiveLink client, and Unreal Engine 5.7 has one client per process. `FLiveLinkModule` holds a single `FLiveLinkClient` (`LiveLinkModule.h:60`) and registers it as a modular feature in `StartupModule` (`LiveLinkModule.cpp:57-61`). PIE clients therefore share it. Receivers in different contexts must still use distinct subject names.
