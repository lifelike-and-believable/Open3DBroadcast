# Open3DShared

The runtime module every other Open3DBroadcast module builds on: the transport interfaces and
registry, the send and receive building blocks transports share, the audio codec, the control
bus, credentials, metrics and option handling. It holds no transport and no editor code.

- **Dependencies:** public Core, CoreUObject, Engine, DeveloperSettings; private Projects,
  Sockets and Open3DStreamCore. The public headers do not include the core library. Opus is
  linked on Win64 (`O3D_WITH_OPUS`).
- **Targets:** Editor, Game and Client.
- **Related docs** (in the GitHub repository, not in the Fab package): the byte layouts in
  [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md); the runtime context and buses in
  [docs/dev/runtime-services.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/dev/runtime-services.md); the decisions in ADRs
  [0004](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0004-credentials-and-secret-transport-options.md) (credentials),
  [0007](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0007-transport-abstraction-and-registry.md) (transports),
  [0008](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0008-sender-pipeline-threading.md) (sender threading),
  [0009](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0009-protocol-versioning.md) (wire format),
  [0011](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0011-control-channel.md) (control) and
  [0012](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0012-runtime-services-and-global-state.md) (runtime services).

## What is where

Paths are relative to `Public/`.

| Area | Headers | Main types |
|---|---|---|
| Transport interface | `Transport/O3DSenderInterface.h`, `Transport/O3DReceiverInterface.h`, `Transport/O3DSerializedFrameConsumer.h`, `Transport/O3DTransportTypes.h`, `Transport/O3DConnectionState.h`, `Transport/O3DTransportApiVersion.h` | `IOpen3DSender`, `IO3DSenderAudioSink`, `IOpen3DReceiver`, `IO3DReceiverAudioSink`, `IO3DReceiverControlSink`, `ISerializedFrameConsumer`, `FO3DTransportConfig`, `FO3DTransportResult`, `FO3DTransportStats`, `FO3DConnectionStateTracker`, `O3D_TRANSPORT_API_VERSION` |
| Registry and options | `Transport/O3DTransportRegistry.h`, `O3DTransportOptionSchema.h`, `Transport/O3DTransportOptionsView.h`, `Transport/O3DTransportOptions.h`, `Transport/O3DTransportOptionSet.h`, `Open3DBroadcastSettings.h` | `FO3DTransportRegistry`, `FO3DTransportDescriptor`, `FO3DTransportOptionSchema`, `O3DTransportOptions::Get*` and `ParseHostPort`, `UOpen3DBroadcastSettings` (project-wide option defaults) |
| Send path | `Transport/O3DSendQueue.h`, `Transport/O3DTransportWorker.h`, `Transport/O3DSenderAudioSinkBase.h`, `O3DLifetimeGate.h`, `O3DEncodedPayloadQueue.h` | `FO3DSendQueue` (bounded, per-kind limits, never drops a full sync), `FO3DTransportWorker`, `FO3DReconnectPolicy`, `FO3DQueuedSenderAudioSink`, `FO3DLifetimeGate` |
| Receive path | `Transport/O3DUnifiedReceiveDemux.h` | `FO3DUnifiedReceiveDemux`: splits received messages into mocap, audio and control, and counts what it drops |
| Envelope | `O3DUnifiedMessage.h` | `FUnifiedHeader`, `ParseUnifiedMessage`, `CreateUnifiedMessage`, `WriteControlEnvelope`, `TryGetControlPayload` |
| Audio | `O3DAudioFrameCodec.h`, `O3DAudioSerialization.h`, `O3DAudioOpus.h`, `O3DAudioResampler.h`, `O3DSinkAudioEncoder.h`, `O3DAudioBus.h` | `FFrameEncoder`, `FFrameDecoder`, `FMultiStreamFrameDecoder`, the audio payload (de)serializers, the libOpus wrappers, `FO3DAudioResampler`, `FO3DSinkAudioEncoder`, `FO3DAudioBus` |
| Control | `O3DControlTypes.h`, `O3DControlBus.h`, `O3DControlConvert.h` | `FO3DControlValue`, `FO3DControlBus`, the engine-to-core conversions |
| Credentials | `O3DSecretStore.h`, `O3DCredentialLibrary.h`, `O3DRedact.h` | `FO3DSecretStore` (session, then environment, then user settings), `UO3DCredentialLibrary` (write-only Blueprint nodes), `O3DRedact` for logs |
| Runtime services and metrics | `O3DRuntimeContext.h`, `O3DRuntimeSubsystem.h`, `O3DPerformanceMetrics.h` | `FO3DRuntimeContext`, `UO3DRuntimeSubsystem`, the sender and receiver counters, `o3d.DumpMetrics` |
| FFI support | `O3DFfiLibrary.h`, `O3DFfiContextRegistry.h` | `FO3DFfiLibrary` (DLL load and unload), `TO3DFfiContextRegistry` (FFI token to a weak context) |
| Logging and helpers | `O3DSharedLogs.h`, `O3DLogThrottle.h`, `O3DHelpers.h` | `LogO3DShared`, `FO3DLogThrottle` (one line per interval per log site), subject-name and URL helpers, local name hashes |
| Blueprint | `O3DBlueprintTransportTypes.h` | `EO3DBroadcastConnectionState`, `FO3DBroadcastTransportStats`, `FO3DConnectionStateMailbox` |
| Tests | `Testing/O3DTransportLifetimeTestUtils.h` | Lifetime stress helpers, compiled only with `WITH_DEV_AUTOMATION_TESTS` |

## Threading

"Game thread" means the thread that owns the instance (the game thread today). Nothing here may
block it. The header named in each row states the contract; the code checks the game-thread
ones with `check(IsInGameThread())` where the row says so.

| Type | Contract |
|---|---|
| `IOpen3DSender` | `Initialize`, `Start`, `Stop`, `Tick`, `CreateAudioSink`, `SetStateChangedCallback` (before `Start`): game thread, non-blocking. `Stop` is safe while a send or audio call is in flight, and no state callback runs after it returns. `SendSerialized`: any thread, thread-safe, never blocks. `SendControl`: game thread, copies the bytes. `GetStats`, `GetCapabilities`, `GetConnectionState`: any thread. |
| `IO3DSenderAudioSink` | `SubmitPcm`: any thread, possibly several at once, must not block. `OnCaptureStopped`: game thread; a `SubmitPcm` already running may still return after it. |
| `IOpen3DReceiver` | `Initialize`, `Start`, `Stop`, `SetConsumer`, `SetAudioSink`, `SetControlSink`, `SetStateChangedCallback`: game thread, the setters before `Start`. `Poll`: game thread, and the only place the frame consumer is called. `GetStats`, `GetCapabilities`, `GetConnectionState`: any thread. |
| `IO3DReceiverAudioSink`, `IO3DReceiverControlSink` | Called on any thread; implementations are thread-safe and must not call back into the transport. |
| State, peer-joined and frames-dropped callbacks | Run on any thread and must not call back into the transport. State callbacks are serialized per instance; peer-joined and frames-dropped callbacks only record the fact. |
| `FO3DTransportRegistry` | `Find`, `CreateSender`, `CreateReceiver` and the other lookups: any thread (read lock). `Register` and unregistering: game thread (checked). Factories and configure functions run on the game thread, outside the lock. |
| `FO3DSendQueue` | `Enqueue`, `SetLimits`, `GetStats`, `Wake`: any thread, lock-free accounting. `Dequeue`, `WaitForWork`, `Empty`: the one consumer thread. |
| `FO3DTransportWorker` | `Start`, `Stop` (joins the thread; never from the body): game thread. `Wake`, `IsRunning`, `IsStopRequested`: any thread. |
| `FO3DLifetimeGate` | `Open`, `Close`: game thread. `FReadScope`: any thread; not nested, not held across a blocking socket call. |
| `FO3DQueuedSenderAudioSink` | `SubmitPcm`: any thread; no lock is held across a socket or FFI call. |
| `FO3DUnifiedReceiveDemux` | Not thread-safe: used only on the thread that calls the receiver's `Poll`, including its setters. |
| `FFrameEncoder`, `FFrameDecoder`, `FMultiStreamFrameDecoder`, `FO3DAudioOpusEncoder`, `FO3DAudioOpusDecoder`, `FO3DAudioResampler`, `FO3DReconnectPolicy` | Not thread-safe; stateful. One instance per stream (or producer thread), used by one thread. |
| `FO3DSinkAudioEncoder`, `FO3DAudioSubjectSlot` | Thread-safe; each takes only its own lock. |
| `FO3DAudioBus`, `FO3DControlBus` | Game thread only (checked). |
| `FO3DRuntimeContext` | Any thread; its metrics are thread-safe, its buses game thread only. `UO3DRuntimeSubsystem`: game thread (checked). |
| Metrics (`FO3DPerformanceMetrics`, the counter handles, the metrics registry) | Thread-safe; counter updates take no lock. |
| `FO3DSecretStore` | Thread-safe. The user-settings store is consulted only on the game thread. |
| `FO3DFfiLibrary` | `Load`, `Unload`: game thread. `GetNumLiveInstances`: any thread. |
| `FO3DConnectionStateTracker` | `Get`: any thread, lock-free. `Begin`, `SetCallback`: game thread. `Set`, `End`: any thread; the callback runs on the changing thread, never overlapping. |
| `FO3DConnectionStateMailbox`, `FO3DLogThrottle`, `TO3DFfiContextRegistry` | Thread-safe. |
| `O3DTransportOptions` | Pure functions, any thread, except `ResolveHostPort` (may block on DNS; never on the game thread) and `ValidateOptions` (game thread). |

## Wire format

This module writes and reads the **unified envelope** (`O3DUnifiedMessage.h`; magic `O3DU`, 24
bytes, little-endian) through the core library's codec, and owns the **audio payload**
(`O3DAudioSerialization.h`; versions 1 and 2). Both layouts, byte by byte, and the rules for
changing them are in [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md) sections 2 and 8.
`EO3DAudioWireFormat` (`Transport/O3DSenderAudioSinkBase.h`) picks between an audio payload
inside an envelope (Loopback, TCP, UDP, NNG) and a bare one (MoQ's audio track).
