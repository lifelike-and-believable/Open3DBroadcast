# Open3DShared module review

Paths below are relative to `ProjectSandbox/Plugins/Open3DBroadcast/` unless they start with `Source/`, which is also under that folder.

**Summary: architecture and health**
1. Open3DShared is a Runtime module with no UObjects and no startup logic. It holds the POD transport config and stats types (`O3DTransportTypes.h`), a 20-byte big-endian "unified" framing header (`O3DUnifiedMessage.h`), a little-endian audio metadata payload format (`O3DAudioSerialization`), an Opus wrapper plus a PCM16/Opus frame encoder and decoder (`O3DAudioOpus`, `O3DAudioFrameCodec`), a global PCM16 multicast audio bus, a global metrics singleton, string and hash helpers, 10 WebRTC console variables, and a single-slot factory registry for serialized-frame consumers.
2. The transport abstraction itself (`IOpen3DSender` / `IOpen3DReceiver` and the name-to-factory registries) is **not** in Shared. It lives in Open3DSender and Open3DReceiver as two copy-pasted registries. Shared only has an umbrella header, and that header's `__has_include` paths never resolve.
3. Transports register by `FName` in `StartupModule` and unregister in `ShutdownModule`. Nothing tracks the lifetime of live instances. There is no interface version, no capability query beyond `SupportsAudio()`, and errors are reported only as `bool` plus log lines.
4. Wire parsing (`ParseUnifiedMessage`, `Deserialize*AudioFrame`) is bounds-safe. It uses int64 length sums and checks the header size. It never validates the header version, the kind, or the semantic ranges of metadata (channels, sample rate) that come from untrusted peers.
5. The audio codec path has two real bugs:
   - Frames get labelled Opus while carrying PCM16 (on every non-Win64 build, and whenever Opus init fails).
   - The Opus encoder is fed arbitrary buffer sizes. Opus rejects those, so after the first frame the encoder permanently and silently falls back to PCM16.
6. `FO3DPerformanceMetrics` returns raw pointers into a `TArray` that can be reallocated by another thread (use-after-free risk). It also takes a global lock on every transport frame sent.
7. There is a lot of global state (audio bus, metrics, consumer registry, CVars, `O3DBuildFlags` cache), which the project rules discourage. About half of the public surface is dead: CVars, URL helpers, the consumer registry factory, CSV export, allocation tracking and several metric fields.
8. Build.cs hygiene:
   - Shared publicly depends on Engine and CoreUObject without using them.
   - It links open3dstream and flatbuffers publicly "for tests", but only on Win64, and skips them silently if they are missing.
   - It pulls Sender headers in via `PublicIncludePathModuleNames`, a layering inversion that breaks the build when `O3D_BUILD_SENDER=0`.
   - `ThirdParty/Include` is an orphan duplicate of the Opus headers at a different version.
9. Tests are thin. Four of the six "generic transport" tests are `TestTrue(true)` placeholders. The Opus round-trip test does not compensate for codec delay. There are no tests for unified framing, audio (de)serialization, malformed input, the helpers, metrics or the encoder fallback logic.
10. Overall health: **fair**. The code is small and readable and the parsers are bounds-checked. However, the audio codec layer is functionally broken for Opus, the global singletons have thread-safety holes, and the module is a grab-bag rather than the transport abstraction layer its README describes ("Shared utilities and base classes used by all modules").

**Counts:** critical 0, high 5, medium 19, low 14 (38 findings).

---

### SHR-1: Frames labelled Opus while carrying PCM16 when Opus is unavailable or fails to initialize
- Category: bug
- Severity: high
- Location: Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:151, :158, :65-66, :97-102; Source/Open3DShared/Open3DShared.Build.cs:41-53
- Evidence:
  - `BuildEncodedFrame` sets `Frame.Codec = ActiveCodec;` (line 151) before it tries Opus.
  - If `EnsureOpusEncoder` returns false, execution falls through to the PCM16 path (lines 179-197), but `Frame.Codec` is still `Opus`.
  - When `O3D_WITH_OPUS == 0`, `EnsureOpusEncoder` is just `return false;` (lines 65-66) and never resets `ActiveCodec`. This is the case on every non-Win64 platform (Build.cs:42) and on Win64 whenever `opus.lib` is missing. So every frame goes out as `Codec=Opus` with raw PCM16 bytes.
  - On an `OpusEncoder.Initialize` failure, `ActiveCodec` is reset (line 101), but the frame already built for this call still carries `Opus`.
  - The receiver then calls `FFrameDecoder::Decode(Opus, …)` on PCM bytes: it gets garbage, or a decode failure with a Warning per packet.
- Recommendation:
  - Set `Frame.Codec` only after encoding succeeds, e.g. `Frame.Codec = EUnifiedCodec::PCM16` at the start of the PCM path.
  - In `Initialize`, when `!O3D_WITH_OPUS`, force `ActiveCodec = PCM16` and log once at Warning.
  - Add a unit test for "Opus requested, Opus unavailable" that asserts `OutFrame.Codec == PCM16`.
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-2: Opus encoder is fed arbitrary buffer sizes, so Opus is effectively never used (silent permanent fallback)
- Category: bug
- Severity: high
- Location: Source/Open3DShared/Private/O3DAudioOpus.cpp:97-101; Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:163-175; ThirdParty/opus/include/opus.h:278-281; Source/Open3DSender/Private/O3DSenderAudioCaptureComponent.cpp:516
- Evidence:
  - `FO3DAudioOpusEncoder::Encode` passes the caller's `NumFrames` straight to `opus_encode_float`.
  - The vendored header says frame_size "must be an Opus frame size for the encoder's sampling rate" (2.5/5/10/20/40/60 ms). The Shared Opus test confirms this ("Opus encoder requires frames matching FrameSizeMs", O3DAudioOpusTests.cpp:42).
  - `FSettings::FrameSizeMs` (O3DAudioOpus.h:20) is only used to size the output buffer. Nothing re-chunks or accumulates input.
  - The capture component submits whatever buffer the submix or resampler produced (`WorkingBuffer`, `NumFrames = OutFrames`), with no 20 ms framing.
  - On the first encode error, `BuildEncodedFrame` sets `ActiveCodec = PCM16; bOpusReady = false;` (lines 172-173). That is permanent, even though the log says "for this frame", and it logs only at `Verbose`.
  - Result: users who select Opus get PCM16 with no visible warning. The exact submix buffer size depends on engine config (needs-UE-verification), but no code path guarantees a valid Opus frame size.
- Recommendation:
  - Add a per-stream accumulator in `FFrameEncoder` that buffers interleaved float input and emits exactly `SampleRate*FrameSizeMs/1000` frames per Opus packet. It should return 0..N packets per call, so change the API to output an array of `FEncodedFrame`.
  - Remove the permanent downgrade, or make it a counted, Warning-logged state with retry.
  - Add a test that feeds 512- and 1024-frame buffers.
- Effort: M
- Owner: coding
- Status: closed in #279

### SHR-3: `FO3DPerformanceMetrics` hands out raw pointers into a reallocatable `TArray` (use-after-free race)
- Category: thread-safety
- Severity: high
- Location: Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:86-104, :195-262; Source/Open3DShared/Public/O3DPerformanceMetrics.h:189-195, :319
- Evidence:
  - `GetOrCreateTransportMetrics` takes `MetricsMutex`, may call `TransportMetrics.SetNum(NewIndex + 1)`, and returns `&TransportMetrics[i]` after the lock is released.
  - `RecordTransportFrameSent` and the other transport functions then do `++TMetrics->FramesSent` outside the lock.
  - Suppose thread A (the WebRTC sender) holds a pointer while thread B (the MoQ or NNG sender) registers a new name for the first time. `SetNum` reallocates and memmoves the elements, and A writes into freed memory. Transports call these from their own worker threads (e.g. MoQSender.cpp:362, NngSender.cpp:337, WebRTCSender.cpp:793).
  - `GetAllTransportMetrics()` (h:195) returns `const TArray&` with no lock at all.
  - `FindTransportMetrics` is public and has the same problem.
- Recommendation:
  - Store entries as `TArray<TUniquePtr<FTransportMetrics>>` (stable addresses) or a fixed `TMap<FName, TSharedRef<...>>`.
  - Better: have each transport instance own a `TSharedRef<FTransportMetrics>` obtained once at `Initialize` and cache it. This also removes the per-frame lock (see SHR-17).
  - Replace `GetAllTransportMetrics()` with a function that returns a locked snapshot copy.
- Effort: M
- Owner: coding
- Status: closed in #279

### SHR-4: Shared's test file hard-includes a Sender header, which breaks the build when `O3D_BUILD_SENDER=0` and inverts module layering
- Category: bug
- Severity: high
- Location: Source/Open3DShared/Private/Tests/GenericTransportTests.cpp:5-6; Source/Open3DShared/Open3DShared.Build.cs:62-66
- Evidence:
  - `#include "O3DSenderInterface.h"` sits unconditionally inside `#if WITH_DEV_AUTOMATION_TESTS`.
  - Build.cs adds `PublicIncludePathModuleNames.Add("Open3DSender")` only `if (O3DBuildFlags.IsSenderEnabled(Target))`. So a receiver-only dev or editor build fails to compile Open3DShared.
  - Even when it compiles, the base module sees headers of a module that depends on it. Because the path is **Public**, every Shared dependent, including receiver-only transports, sees Sender headers too.
- Recommendation:
  - Move `GenericTransportTests.cpp` into Open3DSender, or better a dedicated `Open3DBroadcastTests` module with `Type: DeveloperTool` / `UncookedOnly`.
  - Remove the `PublicIncludePathModuleNames` line from Shared.
- Effort: S
- Owner: coding
- Status: closed in #283

### SHR-5: Four of the "generic transport" tests are placeholders that always pass
- Category: tests
- Severity: high
- Location: Source/Open3DShared/Private/Tests/GenericTransportTests.cpp:126-140, :191-213, :319-334, :81-110
- Evidence:
  - `FGenericTransportConcurrentSendTest`, `FGenericTransportBackpressureTest` and `FGenericTransportStatsConsistencyTest` contain only `TestTrue(TEXT("... structure defined"), true)`.
  - LargePayload, MultipleSubjects and EmptySubjectList only exercise `SubjectList::Serialize` and never touch a transport.
  - `FConcurrentSendWorker` is defined but never instantiated.
  - These show up as green "Open3DBroadcast.Generic.*" tests in CI while verifying nothing about transports. That gives false confidence, which matters for Fab QA.
- Recommendation:
  - Delete the placeholders, or turn them into a real parameterized conformance suite. It would iterate `O3DTransport::GetRegisteredSenders()` and `GetRegisteredReceivers()`, pair each with Loopback-style local config, and assert the Initialize→Start→Send→Stop idempotency, the backpressure `DroppedFrames` contract, concurrent `SendSerialized`, and `GetStats` monotonicity.
- Effort: L
- Owner: design
- Status: closed in #276

### SHR-6: No unit tests for wire parsing, audio (de)serialization, the encoder/decoder, helpers or metrics
- Category: tests
- Severity: medium
- Location: Source/Open3DShared/Public/O3DUnifiedMessage.h:85-156; Source/Open3DShared/Private/O3DAudioSerialization.cpp:87-382; Source/Open3DShared/Private/O3DAudioFrameCodec.cpp; Source/Open3DShared/Private/O3DHelpers.cpp; Source/Open3DShared/Private/Tests/
- Evidence:
  - The only Shared tests are the Opus round-trip and the placeholders.
  - A grep of all `*/Private/Tests/*.cpp` finds no test that calls `ParseUnifiedMessage`, `DeserializeEncodedAudioFrame`, `DeserializePcm16Frame`, `NormalizeTcpUrlHostPort`, `UrlSplitQuery` or `HashNames`.
  - These are the functions that parse untrusted network bytes. The bugs in SHR-1 and SHR-9 would have been caught by trivial tests.
- Recommendation: add `Open3DBroadcast.Shared.*` tests covering:
  - Round-trip for PCM16 and Opus payloads.
  - Truncation at every byte offset, oversized length fields, a wrong version byte, and a codec mismatch.
  - Random-bytes fuzzing for N iterations; it must never crash.
  - `CreateUnifiedMessage` edge cases (0, negative and >50 MB sizes, negative timestamp).
  - Helper tables.
  - The metrics `Reset` behaviour.
  - Consider a libFuzzer harness outside UE for the two parsers.
- Effort: M
- Owner: coding
- Status: closed in #276

### SHR-7: Wire format has no enforced versioning, mixed endianness, duplicated fields, and native-endian PCM samples
- Category: architecture
- Severity: medium
- Location: Source/Open3DShared/Public/O3DUnifiedMessage.h:24-51, :102-107, :141; Source/Open3DShared/Private/O3DAudioSerialization.cpp:7-9, :107, :244; Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:193-194, :256
- Evidence:
  - **Version and kind are never checked.** `ParseUnifiedMessage` reads `H.Version = Data[4]` but never checks it. A future v2 header is parsed as v1. Kind and Codec are also unchecked, so unknown kinds silently fall through in receivers (SocketsTcpReceiver.cpp:420-434).
  - **Version is written in two places.** `CreateUnifiedMessage` hard-codes `WritePtr[4] = 1`, separately from the `FUnifiedHeader::Version = 1` default.
  - **Endianness is mixed.** The unified header is big-endian, while the nested audio header (`SerializeEncodedAudioFrame`) is little-endian.
  - **Fields are duplicated.** The audio header repeats the codec (byte 2) and the timestamp (a double in seconds, versus uint64 µs in the outer header), and has its own version namespace (1 = PCM16, 2 = encoded).
  - **PCM16 samples are written native-endian.** `Frame.Encoded` is a `Memcpy` of `int16` (FrameCodec.cpp:194), and the receiver `Memcpy`s it back (line 256). The format is only correct between little-endian hosts. The header comment says "little-endian fields", but that does not cover the samples.
  - **No sequence number.** Receivers cannot detect loss or reordering for audio over UDP.
  - **The flags byte is unused.**
- Recommendation:
  - Write a single versioned spec (e.g. `Docs/WireFormat.md`). Reject `Version != kCurrent` (or `> kMaxSupported`), and reject unknown Kind and Codec values.
  - Add a sequence number and a stream id to the unified header in a v2.
  - Pick one endianness and byte-swap PCM samples explicitly, or declare them LE and convert on BE hosts.
  - Drop the duplicated codec and timestamp from the inner header in v2.
- Effort: M
- Owner: design

### SHR-8: Untrusted audio metadata is not range-validated at the parse boundary
- Category: security
- Severity: medium
- Location: Source/Open3DShared/Private/O3DAudioSerialization.cpp:163-165, :212-213, :328-331, :377-378; Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:200-233
- Evidence:
  - `NumChannels` (uint16, 0..65535) and `SampleRate` (uint32, then `static_cast<int32>`, which can go negative) are copied straight into `FAudioFrameMeta`.
  - `TimestampSec` can be NaN or Inf.
  - Label and subject strings up to 65535 bytes are accepted unsanitised and later become LiveLink or audio stream identifiers.
  - `FFrameDecoder::EnsureOpusDecoder` recreates the Opus decoder whenever the peer changes `SampleRate` or `NumChannels`. A peer that alternates these values every packet forces an allocate/destroy per packet and resets decoder state.
  - Downstream code clamps in some places (O3DRemoteAudioComponent.cpp:253-254) but not everywhere.
- Recommendation:
  - Validate inside `Deserialize*`: channels 1..8, sample rate in {8000, 12000, 16000, 24000, 32000, 44100, 48000, 96000}, `FMath::IsFinite(TimestampSec)`, and label and subject lengths ≤ 256.
  - Run subject names through `O3DHelpers::SanitizeSubjectName`.
  - Return a typed error enum so callers can count errors instead of logging a Warning per packet (as SocketsTcpReceiver.cpp:467 does today, which floods the log from untrusted peers).
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-9: `NormalizeTcpUrlHostPort` corrupts IPv4 URLs that have no port
- Category: bug
- Severity: medium
- Location: Source/Open3DShared/Private/O3DHelpers.cpp:101-150
- Evidence:
  - For `tcp://192.168.1.10`, `HostPort` has no `:`, so the function finds the last `.` and sees that `"10"` is all digits. It rewrites the URL to `tcp://192.168.1:10`.
  - Any dotted host whose last label is numeric gets mangled in the same way.
  - The function is exported public API. It currently has no callers outside Shared (grep), so the bug is latent.
- Recommendation:
  - Delete the function (it is dead code).
  - If it must be kept, only rewrite when the host part before the last dot is a valid hostname or IPv4 address with exactly 4 dotted parts plus a port, and add a test table.
- Effort: S
- Owner: coding
- Status: closed in #305

### SHR-10: `FO3DAudioBus` is a global non-thread-safe multicast delegate that documents itself as thread-safe
- Category: thread-safety
- Severity: medium
- Location: Source/Open3DShared/Public/O3DAudioBus.h:8-21; Source/Open3DShared/Private/O3DAudioBus.cpp:5-24; Source/Open3DReceiver/Private/O3DReceiverSource.cpp:121-123; Source/Open3DReceiver/Private/O3DRemoteAudioComponent.cpp:76, :99
- Evidence:
  - `GO3DAudioBusDelegate` is a namespace-scope `DECLARE_MULTICAST_DELEGATE` instance.
  - The header comment says "Data is copied to ensure thread safety across publisher threads". Copying the payload does not make `Broadcast` safe against a concurrent `AddUObject` or `Remove`. UE's default multicast delegate is not thread-safe; the thread-safe variant uses `FDefaultTSDelegateUserPolicy` (needs-UE-verification for the exact 5.7 spelling).
  - The only current publisher marshals to the game thread through `AsyncTask(ENamedThreads::GameThread, …)`. Nothing enforces that, and the API invites transports to call it from worker threads.
  - It is also global state that cannot be scoped per world or PIE instance: all PIE clients hear all streams.
  - `PublishPcm16` allocates and copies even when nothing is bound.
- Recommendation:
  - Add `check(IsInGameThread())` in `PublishPcm16` and document "game thread only", or switch to a TS multicast delegate.
  - Early-out when `!IsBound()`.
  - Take `TConstArrayView<uint8>` in the delegate signature to avoid the copy.
  - Longer term, replace the singleton with a per-receiver-source delegate or a `UWorldSubsystem` so PIE instances are isolated.
- Effort: S
- Owner: coding
- Status: closed in #269

### SHR-11: `FO3DTransportConfig::ToDebugString` claims "secrets redacted" but prints AdvancedParams, which contain the WebRTC token
- Category: security
- Severity: medium
- Location: Source/Open3DShared/Public/O3DTransportTypes.h:107-151 (esp. :111-120, :142-150); Source/Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:647, :688
- Evidence:
  - `ToDebugString` joins every `AdvancedParams` pair as `%s=%s` and prints `Uri` verbatim.
  - The WebRTC customization does `Config.AdvancedParams.Add(WebRTCConfig::TokenOptionKey /*"webrtc.token"*/, TokenValue)`, so the JWT would be logged in clear.
  - There are no callers today (grep), so this is latent, but the doc comment ("secrets redacted") invites use.
  - URIs may also carry `?token=` query strings.
- Recommendation:
  - Redact any key matching `token|secret|key|password|auth` (case-insensitive), and strip query values from `Uri`.
  - Better: keep secrets out of `AdvancedParams` entirely and pass them only through the dedicated `Token` field.
  - Add a test that asserts the token substring never appears in the output.
- Effort: S
- Owner: coding
- Status: closed in #275

### SHR-12: The transport abstraction is split across modules, duplicated, unversioned and inconsistent between sender and receiver
- Category: architecture
- Severity: medium
- Location: Source/Open3DSender/Public/O3DSenderInterface.h:27-92; Source/Open3DReceiver/Public/O3DReceiverInterface.h:22-44; Source/Open3DSender/Private/O3DSenderRegistry.cpp:1-101; Source/Open3DReceiver/Private/O3DReceiverRegistry.cpp:1-101; Source/Open3DShared/Public/O3DTransportRegistry.h:1-15
- Evidence:
  - **Duplicated registries.** The two registries are line-for-line copies (`IsFactoryValid`, `RemoveFactoryInternal`, `SnapshotKeys`), each with its own global `FCriticalSection` and `TMap`.
  - **Asymmetric interfaces.** The sender has `Tick(float)`, the receiver has `Poll()`. The receiver has `SetConsumer`, but the sender has no equivalent sink. `SendSerialized` has a default body that returns false, which silently drops every frame for a transport that has not overridden it. The comment in O3DSenderInterface.h:65-72 admits this.
  - **Inconsistent shared-pointer modes.** Audio sinks use `ESPMode::ThreadSafe`, while the sender, receiver and consumer use the default mode (needs-UE-verification whether the default is ThreadSafe in 5.7).
  - **No versioning or capabilities.** There is no interface version or capability bitmask (audio, SendSerialized, bidirectional), so the fitness of a transport is only discovered at runtime by a `false` return.
  - **Broken umbrella header.** Shared's `O3DTransportRegistry.h` tries to include `"Open3DSender/O3DSenderRegistry.h"`. That path never exists on the include path, because Public dirs are added directly. `__has_include` is therefore false and the header is effectively empty. Its only includer (Open3DTransportNNGModule.cpp:4) also includes the real headers directly.
- Recommendation:
  - Move a templated `TO3DFactoryRegistry<TInterface>` into Shared, instantiate it once for senders and once for receivers, and delete the duplicates.
  - Add `virtual uint32 GetInterfaceVersion()` and `GetCapabilities()` (`EO3DTransportCaps`).
  - Make `SendSerialized` pure virtual, since all transports should implement it.
  - Specify the `ESPMode` explicitly everywhere.
  - Delete `O3DTransportRegistry.h`.
- Effort: M
- Owner: design
- Status: closed in #289

### SHR-13: No lifetime contract between registered factories and module unload
- Category: architecture
- Severity: medium
- Location: Source/Open3DTransportSockets/Private/Open3DTransportSocketsModule.cpp:96-101; Source/Open3DTransportLoopback/Private/Open3DTransportLoopbackModule.cpp:349-354; Source/Open3DSender/Private/O3DSenderRegistry.cpp:72-90
- Evidence:
  - Factories return `TSharedPtr<IOpen3DSender>` / `TSharedPtr<IOpen3DReceiver>` to components.
  - `ShutdownModule` only unregisters the factory. Already-created instances, whose vtables and code live in the transport DLL, can outlive the module: the components still hold them.
  - Nothing counts live instances, and no "module going away" callback asks owners to `Stop()` and release them.
  - Whether this crashes depends on module unload ordering at editor shutdown or during Live Coding (needs-UE-verification).
- Recommendation:
  - Have the registry hand out instances that carry a module reference, or keep a weak list of instances per transport name.
  - On `UnregisterSender` / `UnregisterReceiver`, call `Stop()` on the live instances and broadcast an `OnTransportUnregistered(FName)` delegate, so components can drop them before the DLL unloads.
- Effort: M
- Owner: design
- Status: closed in #303

### SHR-14: Error reporting is `bool` plus log only, and stats are not specified for concurrency
- Category: usability
- Severity: medium
- Location: Source/Open3DSender/Public/O3DSenderInterface.h:33-45, :83; Source/Open3DReceiver/Public/O3DReceiverInterface.h:27-35; Source/Open3DShared/Public/O3DTransportTypes.h:155-175
- Evidence:
  - `Initialize`, `Start` and `Send` return `bool`, and the reason exists only in logs. UI and Blueprint callers cannot surface "token expired" versus "port in use" versus "backpressure".
  - There is no connection-state query or event on the interface. Connection state lives only in the global metrics singleton (`SetTransportConnected`).
  - `FO3DTransportStats` has no field for connection state, errors or queue depth, and it never says whether `GetStats()` may be called from any thread.
  - The Opus wrapper uses `FString& OutError`; the codec helpers use `bool`. The error model is inconsistent.
- Recommendation:
  - Introduce `enum class EO3DTransportError` with a `TValueOrError<void, FO3DTransportError>` (or `FO3DTransportResult`) return type.
  - Add `GetConnectionState()` and an `OnStateChanged` delegate.
  - Extend `FO3DTransportStats` with `SendErrors`, `ReceiveErrors`, `PendingFrames` and `State`.
  - Document that `GetStats()` is any-thread and lock-free.
- Effort: M
- Owner: design
- Status: closed in #304

### SHR-15: One `FFrameDecoder` per receiver is shared across all incoming audio streams
- Category: bug
- Severity: medium
- Location: Source/Open3DShared/Public/O3DAudioFrameCodec.h:69-85; Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.h:75-76; Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:479
- Evidence:
  - `FFrameDecoder` holds a single stateful `OpusDecoder` keyed only by `(SampleRate, NumChannels)`.
  - Receivers hold one `AudioDecoder` and feed it every Opus packet regardless of `Meta.SourceGuid` or `StreamLabel`.
  - Opus decoding is stateful. When two publishers (NNG pull, MoQ, or several stream labels) interleave packets through one decoder, both outputs are corrupted.
- Recommendation:
  - Key decoders by `(SourceGuid, StreamLabel)`, e.g. an `FMultiStreamFrameDecoder` in Shared holding `TMap<FStreamKey, FFrameDecoder>` with LRU eviction.
  - Document that `FFrameDecoder` is single-stream.
- Effort: S
- Owner: coding
- Status: closed in #269

### SHR-16: The `ISerializedFrameConsumer` API forces a payload copy, and its threading and ownership are unspecified
- Category: performance
- Severity: medium
- Location: Source/Open3DShared/Public/SerializedFrameConsumerRegistry.h:8-21; Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:428-431, :440-443; Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.h:73; Source/Open3DTransportLoopback/Private/Receiver/LoopbackReceiver.h:25
- Evidence:
  - `SubmitFrame(const FString&, const TArray<uint8>& Buffer, double)` takes a `const TArray&`. Receivers that hold a pointer and size into a larger receive buffer must allocate and memcpy a new `TArray` per frame (the `PayloadCopy` pattern), and the consumer then typically copies again.
  - The interface does not say which thread `SubmitFrame` runs on, or whether the receiver owns the consumer. TCP stores `TWeakPtr`, while Loopback and WebRTC store `TSharedPtr`.
- Recommendation:
  - Change the signature to `SubmitFrame(FName Subject, TConstArrayView<uint8> Buffer, double Ts)`, plus an overload taking `TArray<uint8>&&` for zero-copy hand-off.
  - Document "called on the transport worker thread; must be thread-safe; the receiver holds a weak reference".
- Effort: M
- Owner: design
- Status: closed in #312

### SHR-17: A global lock and a linear string search run on every transport frame sent
- Category: performance
- Severity: medium
- Location: Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:86-104, :195-202; Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:793, :946; Source/Open3DTransportNNG/Private/Sender/NngSender.cpp:337; Source/Open3DTransportMoQ/Private/Sender/MoQSender.cpp:362
- Evidence:
  - `RecordTransportFrameSent(TEXT("MoQ"), Len)` builds an `FString` from a literal (an allocation), takes `MetricsMutex`, and does a linear `FString ==` scan.
  - This happens per frame, per transport, and contends with `DumpMetrics`, which holds the same lock while it emits about 60 log lines.
  - This contradicts the header's "minimal overhead (<1% CPU impact)" claim (h:13), which nothing measures.
- Recommendation:
  - Resolve a stable `FTransportMetrics&` once per transport instance (see SHR-3) and bump its atomics directly.
  - Use `FName` keys.
  - In `DumpMetrics`, copy a snapshot under the lock and log outside it.
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-18: The audio encode/send path allocates and copies several times per frame
- Category: performance
- Severity: medium
- Location: Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:150-196, :294-303; Source/Open3DShared/Private/O3DAudioOpus.cpp:95, :110, :183, :199; Source/Open3DShared/Private/O3DAudioSerialization.cpp:94-95, :231-232; Source/Open3DShared/Private/O3DAudioBus.cpp:17-21
- Evidence:
  - **PCM16 encode copies twice.** It converts into `PCM16Scratch`, then allocates `Frame.Encoded` and memcpys again (lines 187-194). `PCM16ScratchCapacity` duplicates `TArray::Max()`.
  - **Opus encode reallocates twice per frame.** It allocates a local `TArray<uint8> Encoded` per frame (line 161), and `Encode` does `SetNumUninitialized(MaxPacketBytes)` then `SetNum(EncodedBytes, EAllowShrinking::Yes)`, which forces a realloc.
  - **Opus decode causes grow/shrink churn.** `Decode` does `SetNumUninitialized(FrameCapacity*Channels)` then `SetNum(..., EAllowShrinking::Yes)` on the receiver's reused `DecodedPcmScratch`, so the scratch array reallocates every packet.
  - **Framing copies the payload twice.** `CreateUnifiedAudioMessage` serializes into a temporary `Payload` array, then `CreateUnifiedMessage` copies it again into `OutMessage`.
  - **String work per frame.** Each frame performs two `FTCHARToUTF8` conversions and two `FString` copies for Meta.
  - **Receive side copies PCM twice more.** The receiver does `Payload.Append` (O3DReceiverSource.cpp:118-119), then `PublishPcm16` copies again.
- Recommendation:
  - Write PCM16 directly into `OutFrame.Encoded`.
  - Use `EAllowShrinking::No` for scratch buffers.
  - Serialize the audio header directly after a reserved 20-byte unified header in a single buffer, and patch the length at the end.
  - Cache the UTF-8 label and subject in the encoder when they have not changed.
  - Pass `TConstArrayView` through the bus.
- Effort: M
- Owner: coding
- Status: closed in #279

### SHR-19: Ten WebRTC CVars in Shared: dead, not exported, duplicated header, and one verbose logger on by default
- Category: code-quality
- Severity: medium
- Location: Source/Open3DShared/Public/O3DConsoleVars.h:1-29; Source/Open3DShared/Private/O3DConsoleVars.h:1-30; Source/Open3DShared/Private/O3DConsoleVars.cpp:6-65
- Evidence:
  - A grep over `Source/` finds no reference to any `CVarO3DWebRTC*` or `CVarO3DBroadcastWebRTC*` symbol, or to the `CVarO3DS*` aliases, outside Shared. The WebRTC module does not include `O3DConsoleVars.h`.
  - The public header declares `extern TAutoConsoleVariable<int32> ...` without `OPEN3DSHARED_API`. On a modular Windows build, any other module that uses them would fail to link.
  - The public header also lacks `#include "HAL/IConsoleManager.h"`, which the private copy has (IWYU).
  - Public and Private carry two nearly identical copies of the same header name, and which one a translation unit picks depends on include-path order.
  - The `#define CVarO3DS... CVarO3D...` aliases are global macros.
  - `o3ds.WebRTC.DebugRx` defaults to 1 ("Logs first packet and occasional stats"), which contradicts the "quiet hot paths" rule.
  - Transport-specific tuning sits in the shared module.
- Recommendation:
  - Delete both headers and the .cpp from Shared. If any of these knobs are still wanted, re-home them as `static` CVars in Open3DTransportWebRTC.
  - Use a single `o3d.` prefix (metrics use `o3d.`, CVars use `o3ds.`).
  - Default debug logging to 0.
- Effort: S
- Owner: coding
- Status: closed in #286

### SHR-20: Build.cs exposes unnecessary public dependencies and links third-party libs "for tests"
- Category: fab-readiness
- Severity: medium
- Location: Source/Open3DShared/Open3DShared.Build.cs:16-38, :55-60, :68
- Evidence:
  - `PublicDependencyModuleNames` has `Core, CoreUObject, Engine`, but no file in Shared uses UObject or Engine. A grep for `UObject|UCLASS|USTRUCT|GEngine|UWorld` finds nothing. This forces Engine onto every dependent and prevents use from programs or light modules.
  - open3dstream include and lib and flatbuffers are added as **Public** only because tests need them (comments at lines 16, 20, 24). Every Shared dependent inherits them.
  - Libraries are added only if `File.Exists`, so a missing lib fails silently at link time with unresolved symbols. Sender and Receiver throw a clear `BuildException` in the same situation (Open3DSender.Build.cs:38-45).
  - On non-Win64 platforms the includes are added but no library is linked, so `GenericTransportTests` (`SubjectList::Serialize`) fails to link in Linux or Mac dev-editor builds.
  - The empty `PrivateDependencyModuleNames.AddRange(new string[] {})` is noise.
- Recommendation:
  - Make the public dependency Core only.
  - Move the open3dstream and flatbuffers dependency into the test module (see SHR-4), or make it Private with a `PrivateIncludePaths` entry.
  - Throw a `BuildException` on a missing lib.
  - Gate the tests with `#if WITH_O3DS_CORE`, or restrict the plugin's platforms (see SHR-21).
- Effort: S
- Owner: coding
- Status: closed in #283

### SHR-21: Platform support is undeclared. Opus is Win64-only, Sender throws on other platforms, and the uplugin has no allow-list
- Category: fab-readiness
- Severity: medium
- Location: Source/Open3DShared/Open3DShared.Build.cs:41-53, :5; Open3DBroadcast.uplugin:21-61; Source/Open3DSender/Open3DSender.Build.cs:25-32
- Evidence:
  - Opus is linked only on Win64, and `O3D_WITH_OPUS=0` elsewhere, which triggers the mislabelling in SHR-1.
  - Sender and Receiver throw a `BuildException` for non-Win64 platforms.
  - The `.uplugin` modules declare no `PlatformAllowList`, so Fab or UBT will try to build them on Mac and Linux and fail.
  - `[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]` excludes Server and Client targets without documenting why.
- Recommendation:
  - Add `"PlatformAllowList": ["Win64"]` to each module in the uplugin until other platforms ship binaries.
  - Document the Server-target exclusion, or allow Server for receiver-only builds.
  - Emit a build-time Warning when Opus is unavailable.
- Effort: S
- Owner: design
- Status: closed in #283

### SHR-22: `O3DBuildFlags` caches platform-dependent flags process-wide and lives inside Shared's Build.cs
- Category: code-quality
- Severity: medium
- Location: Source/Open3DShared/Open3DShared.Build.cs:72-129 (esp. :87-96, :107-111)
- Evidence:
  - `private static Settings Cached` is computed on the first `Get(Target)` call and returned for all later targets.
  - The MoQ decision depends on `Target.Platform` (line 107). If one UBT process builds several targets or platforms (for example BuildCookRun, or Editor Win64 plus a Linux game), the second target inherits the first target's MoQ setting.
  - The helper class is defined in `Open3DShared.Build.cs` but consumed by every other module's rules. This is hidden coupling; Open3DSender.Build.cs:2 even has a commented `//using O3DBroadcastBuild;`.
  - It also sets `bEnableExceptions = true` for every module, Shared included, even though Shared has no try/catch.
- Recommendation:
  - Cache the environment-derived values only, and compute platform-dependent overrides per call.
  - Move the class into a clearly named `Open3DBroadcastBuildFlags` file. UBT compiles every `.cs` in the plugin's rules assembly (needs-UE-verification for 5.7).
  - Enable exceptions only where third-party code needs them.
- Effort: S
- Owner: coding
- Status: closed in #283

### SHR-23: `ThirdParty/Include` is an orphan Opus header set at a different API version, and the docs disagree about the Opus version
- Category: fab-readiness
- Severity: medium
- Location: ThirdParty/Include/opus*.h; ThirdParty/opus/include/opus*.h; ThirdParty/README.md:24-28; THIRD_PARTY_LICENSES.md:97-109; Source/Open3DTransportWebRTC/Open3DTransportWebRTC.Build.cs:62
- Evidence:
  - No Build.cs references `ThirdParty/Include`; a grep of every `*.Build.cs` confirms it.
  - `diff -r` shows the two header sets differ: `ThirdParty/Include` declares `opus_encode24`, `opus_decode24`, `OPUS_SET_QEXT` and OSCE_BWE, which are newer than the linked lib's headers.
  - `ThirdParty/README.md` says "Version: 1.5.2 … Used by: Open3DShared, Open3DTransportWebRTC".
  - `THIRD_PARTY_LICENSES.md` says the shipped `opus.lib` reports "libopus unknown", ≥1.2.
  - WebRTC's Build.cs says "Opus library NOT needed".
  - `flatbuffers/include/flatbuffers/` also ships compiler-only headers (`bfbs_generator.h`, `code_generators.h`) that a runtime plugin does not need.
  - The `open3dstream` include and lib folders are gitignored (ProjectSandbox/.gitignore:31-36), so a fresh clone cannot build Shared tests without an undocumented build step.
- Recommendation:
  - Delete `ThirdParty/Include`.
  - Rebuild `opus.lib` from a pinned upstream tag and record the tag and hash in the README.
  - Fix the "Used by" lists.
  - Prune the flatbuffers compiler headers.
  - Document or script how `open3dstream/include` and `lib` are produced (a link to the build script).
- Effort: M
- Owner: review

### SHR-24: `FSerializedFrameConsumerRegistry` is never populated and calls user code under a global lock
- Category: architecture
- Severity: medium
- Location: Source/Open3DShared/Public/SerializedFrameConsumerRegistry.h:25-39; Source/Open3DShared/Private/SerializedFrameConsumerRegistry.cpp:7-29; Source/Open3DTransportLoopback/Private/Receiver/LoopbackReceiver.cpp:33-41; Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:527-534
- Evidence:
  - A grep finds no call to `RegisterFactory` or `ClearFactory` anywhere, so `Create()` always returns nullptr. The Loopback and WebRTC "fallback consumer" paths are dead, and they log "frames will be dropped".
  - Only two of the five receivers consult the registry, which is inconsistent.
  - `Create()` invokes the factory while holding `GFactoryLock`, so a factory that touches the registry deadlocks. A `TFunction` is also copied into a `TUniqueFunction`.
  - It is a single global slot. The last registrant wins, silently.
- Recommendation:
  - Delete the registry and require `SetConsumer` before `Start()`, returning an error if none is set.
  - If a fallback is needed, copy the factory under the lock and invoke it outside, as the Sender registry already does (O3DSenderRegistry.cpp:74-89).
- Effort: S
- Owner: design
- Status: closed in #289

### SHR-25: Metrics `Reset()` misses fields, many fields are dead, and the dump verbosity is wrong
- Category: code-quality
- Severity: low
- Location: Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:22-84, :302-472, :474-512; Source/Open3DShared/Public/O3DPerformanceMetrics.h:26-48, :130-158
- Evidence:
  - **Reset is incomplete.** `Reset()` does not clear `AvgParseTimeMs`, `AvgPoseExtractionTimeMs`, `AvgLiveLinkPushTimeMs`, `AvgTotalProcessingTimeMs`, either `ActiveSubjectCount`, `bConnected` or `PipeCount`.
  - **Many fields are never written.** A grep of callers shows these are never recorded:
    - Sender: `FramesQueued`, `AllocationCount`/`AllocationBytes` (`RecordAllocation` is unused), `AvgSerializationTimeMs`, `FrameIntervalMs`, `LastCaptureTime`.
    - Transport: `AvgPacketLossPercent`, `AvgLatencyMs`, `AvgBandwidthMbps`, `ReconnectCount`, `ReceiveErrors`, `PendingFrames`.
    - Receiver: `LastApplyTime`.
    - Unused functions: `RecordAllocationsForContext`, `UpdateTransportPendingFrames`, `GetMetricsAsCSV`.
    - `DumpMetrics` still prints these as zeros ("Allocations: 0").
  - **Output problems.** The dump logs roughly 60 lines at `Warning`, when this is informational output. The CSV omits transport metrics.
  - **Clock choice.** Uptime uses `FDateTime::Now()` (local wall clock) rather than a monotonic clock.
- Recommendation:
  - Remove the dead fields, or wire them up.
  - Reset every field. Generating the list with an X-macro would keep Reset, Dump and CSV in sync.
  - Log at `Display`.
  - Use `FPlatformTime::Seconds()` for uptime.
- Effort: S
- Owner: coding
- Status: closed in #334

### SHR-26: Metric EMA and max updates are non-atomic read-modify-write races
- Category: thread-safety
- Severity: low
- Location: Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:125-189, :229-238
- Evidence:
  - `RecordFrameLatency`, `Record*TimeMs` and `RecordClockOffsetSampleMs` each do `Load` → compute → `Store` without CAS, so concurrent receivers lose updates.
  - `MaxLatencyMs` and `MaxPendingFrames` use check-then-store and can go backwards.
  - `SenderMetrics.FrameIntervalMs` (plain double) and the `FDateTime` fields are written without synchronization (`UpdateFrameInterval`, h:220).
- Recommendation:
  - Use a CAS loop (`CompareExchange`) for the max values, and accept approximate EMAs with a comment saying so.
  - Make `FrameIntervalMs` atomic, or remove it.
  - Consider `std::atomic`, since UE is moving away from `TAtomic` (needs-UE-verification for 5.7 deprecation status).
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-27: Development-diary console command shipped in a runtime module
- Category: fab-readiness
- Severity: low
- Location: Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:541-588, :518-539
- Evidence:
  - The `o3d.ProfileGuide` command prints "PHASE 13: MAIN THREAD PROFILING GUIDE", "To diagnose the 2000+ ms latency spike root cause", and "Receiver processes in 0.219-0.256 ms (EXCELLENT)". These are internal investigation notes, logged at Warning.
  - The free functions `DumpO3DMetrics`, `ResetO3DMetrics` and `PrintProfileGuide` have external linkage (not `static` and not in an anonymous namespace).
- Recommendation:
  - Remove `o3d.ProfileGuide`, or move the text to docs.
  - Make the command handlers `static` or lambdas.
  - Wrap the commands in `#if !UE_BUILD_SHIPPING` if they are dev-only.
- Effort: S
- Owner: coding
- Status: closed in #282

### SHR-28: Public log categories are declared without the export macro
- Category: code-quality
- Severity: low
- Location: Source/Open3DShared/Public/O3DAudioFrameCodec.h:10; Source/Open3DShared/Public/O3DPerformanceMetrics.h:8; Source/Open3DShared/Public/O3DSharedLogs.h:7
- Evidence:
  - `DECLARE_LOG_CATEGORY_EXTERN(LogO3DAudioCodec, …)` and `(LogO3DPerformanceMetrics, …)` sit in Public headers without `OPEN3DSHARED_API`. Any other module that logs to them fails to link in modular builds.
  - `LogO3DShared` is exported correctly but is only used for two startup and shutdown Display lines (Open3DSharedModule.cpp:15, :19).
  - The module has three categories for about 1.5k lines of code.
- Recommendation:
  - Add `OPEN3DSHARED_API` to the public declarations, or move them to Private.
  - Consolidate on `LogO3DShared` (plus `LogO3DAudioCodec` if needed).
  - Drop the Display-level startup and shutdown logs.
- Effort: S
- Owner: coding
- Status: closed in #441

### SHR-29: Header-level `static` functions and IWYU issues in `O3DUnifiedMessage.h` and `O3DAudioOpus.h`
- Category: code-quality
- Severity: low
- Location: Source/Open3DShared/Public/O3DUnifiedMessage.h:64-82, :90, :123, :124, :141; Source/Open3DShared/Public/O3DAudioOpus.h:4; Source/Open3DShared/Private/O3DAudioOpus.cpp:3
- Evidence:
  - `static void WriteBE32` and `WriteBE64` are non-inline `static` functions in a namespace in a public header. Every translation unit gets a copy, and those that do not use them get `-Wunused-function` on clang.
  - `WireHeaderSize = 20` is duplicated as a local `constexpr` in two functions.
  - The 50 MB limit is a magic number.
  - The version byte is the literal `1`.
  - `O3DAudioOpus.h` includes `O3DUnifiedMessage.h` without using it.
  - `O3DAudioOpus.cpp` includes `LogMacros.h` but never logs.
  - `FUnifiedHeader` field names ("MagicBE", "…Host") are misleading, since all fields are stored host-order after parsing.
- Recommendation:
  - Make the helpers `inline` or move them to a .cpp.
  - Add `static constexpr int32 WireSize = 20; static constexpr uint8 CurrentVersion = 1; static constexpr int32 MaxPayloadBytes`.
  - Remove the unused includes.
  - Rename the fields.
- Effort: S
- Owner: coding
- Status: partly fixed in #441 (WriteBE64 removed, WriteBE32 inline, unused includes); open: renaming the exported FUnifiedHeader fields (MagicBE, ...Host) and its Version = 1 default (maintainer decision)

### SHR-30: `CreateUnifiedMessage` truncates the timestamp with undefined behaviour for negative or NaN input, and the clock domain is undocumented
- Category: bug
- Severity: low
- Location: Source/Open3DShared/Public/O3DUnifiedMessage.h:121-133; Source/Open3DShared/Public/O3DUnifiedMessage.h:124
- Evidence:
  - `static_cast<uint64>(TimestampSec * 1000000.0)` is undefined behaviour for negative, NaN or out-of-range doubles.
  - Callers pass `FPlatformTime::Seconds()`-derived values, which are process-relative, not wall-clock. Nothing tells a receiver on another machine how to interpret them.
  - A `PayloadSize <= 0` payload is rejected, so zero-length keepalive or control messages are impossible.
- Recommendation:
  - Clamp and validate with `FMath::IsFinite`, and reject negative values.
  - Document the timestamp as "sender-local monotonic µs; not comparable across hosts without clock-offset estimation".
  - Allow empty payloads for a future control kind.
- Effort: S
- Owner: coding
- Status: closed in #339

### SHR-31: The Opus decoder buffer is capped at 60 ms, the encoder buffer is sized ad hoc, and ctl return values are ignored
- Category: bug
- Severity: low
- Location: Source/Open3DShared/Public/O3DAudioOpus.h:58; Source/Open3DShared/Private/O3DAudioOpus.cpp:12-25, :59-62
- Evidence:
  - The decoder's `FrameSizeMs = 60` default gives a capacity of 60 ms. Opus packets can carry up to 120 ms, and `opus_decode` returns `OPUS_BUFFER_TOO_SMALL` for those.
  - `CalculateMaxPacketBytes` = `max(1283, samples*ch/4)`. Opus recommends 4000 bytes for a single packet.
  - The `opus_encoder_ctl` return codes are discarded, so a rejected bitrate goes unnoticed.
  - `FrameSizeMs` in the encoder settings is not validated against legal Opus durations.
  - There is no packet-loss concealment API (a `Decode(nullptr)` / FEC path) exposed for jitter handling.
- Recommendation:
  - Size decoder capacity to 120 ms at the configured rate.
  - Use 4000 bytes as the encoder's maximum packet size.
  - Check the ctl results and put them in `OutError`.
  - Validate `FrameSizeMs ∈ {2.5, 5, 10, 20, 40, 60}`.
  - Expose `DecodeLost(int32 Frames)`.
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-32: The Opus round-trip test ignores codec delay and uses a very loose tolerance
- Category: tests
- Severity: low
- Location: Source/Open3DShared/Private/Tests/O3DAudioOpusTests.cpp:175-219, :90-99, :164-167
- Evidence:
  - The test compares `Input[i]` against `Decoded[i]` directly. Opus has algorithmic delay (lookahead), so the comparison is phase-shifted.
  - To pass anyway, the threshold is set to an average absolute error of 0.15 (15% of full scale). That is loose enough to hide real regressions.
  - A frame-count mismatch produces only `AddWarning`.
  - The test contains extensive debug logging (the "near sample 2880" dumps).
  - It compiles only when `O3D_WITH_OPUS`, so on non-Win64 platforms no audio codec test runs.
- Recommendation:
  - Query `OPUS_GET_LOOKAHEAD`, offset the comparison by it, and tighten to SNR > 20 dB or an average error < 0.02.
  - Turn the frame-count mismatch into an error.
  - Remove the debug dumps.
  - Add PCM16 codec-path tests that run on every platform.
- Effort: S
- Owner: coding
- Status: closed in #279

### SHR-33: `HashNames` concatenates names without separators or length, so collisions go undetected
- Category: bug
- Severity: low
- Location: Source/Open3DShared/Private/O3DHelpers.cpp:164-183; Source/Open3DSender/Private/O3DSenderComponent.cpp:820; Source/Open3DReceiver/Private/O3DReceiverSource.cpp:143-151
- Evidence:
  - `HashNames` feeds each name's TCHAR bytes into FNV-1a with no delimiter and no count. `["ab","c"]` and `["a","bc"]` hash identically, and so do an empty list and a list of empty names.
  - The sender and receiver use this hash to detect skeleton or curve topology changes. A collision would leave stale LiveLink static data.
  - FNV-1a 64 is also a weak hash for change detection.
- Recommendation:
  - Hash the element count and each name's length (or a 0 separator).
  - Hash names in a canonical form (lowercase UTF-8, since FName comparison is case-insensitive).
  - Consider `CityHash64` or `FXxHash64` from Core (needs-UE-verification for availability in 5.7).
- Effort: S
- Owner: coding
- Status: closed in #338

### SHR-34: Dead and misleading helper APIs remain exported
- Category: code-quality
- Severity: low
- Location: Source/Open3DShared/Public/O3DHelpers.h:9-10, :15-23, :31-35; Source/Open3DShared/Private/O3DHelpers.cpp:10, :71-99
- Evidence:
  - `UrlSplitQuery`, `StripQuery`, `NormalizeTcpUrlHostPort` and the `O3DSHelpers` compatibility namespace have no callers outside Shared (grep).
  - `UrlSplitQuery` lowercases **values** (line 86), which would corrupt case-sensitive tokens, room names and stream ids. It also does no percent-decoding.
  - `SanitizeSubjectName` is documented as "Replace whitespace with '_'" but replaces only `' '`. Tabs and newlines are dropped, not replaced.
- Recommendation:
  - Delete the unused helpers and the alias namespace.
  - Fix the `SanitizeSubjectName` doc or behaviour (use `FChar::IsWhitespace`).
  - If URL parsing is needed later, use UE's `FGenericPlatformHttp::UrlDecode` together with `FURL` or `FParse` (needs-UE-verification).
- Effort: S
- Owner: coding
- Status: closed in #334

### SHR-35: Audio serializers and deserializers are about 90% copy-paste
- Category: code-quality
- Severity: low
- Location: Source/Open3DShared/Private/O3DAudioSerialization.cpp:87-139 vs :219-278; :141-217 vs :280-382
- Evidence:
  - `SerializePcm16Frame` and `SerializeEncodedAudioFrame` differ only in two extra header bytes.
  - The two deserializers duplicate about 60 lines of field reads, bounds checks and UTF-8 conversions.
  - The header-size constants are recomputed as literal sums in four places (lines 107, 148, 244, 301).
  - `FPcm16Frame` / `FEncodedAudioFrame` in O3DAudioSerialization.h overlap with `FEncodedFrame` in O3DAudioFrameCodec.h: three near-identical structs.
- Recommendation:
  - Factor out `WriteCommonAudioHeader` and `ReadCommonAudioHeader` with an `FByteWriter` / `FByteReader` cursor that does its own bounds checks.
  - Keep a single `FEncodedAudioFrame` type.
  - Define the header sizes once as `constexpr`.
- Effort: S
- Owner: coding

### SHR-36: `FO3DTransportConfig` mixes LiveKit-specific fields into the canonical config, and the documented identifiers do not match the registry
- Category: architecture
- Severity: low
- Location: Source/Open3DShared/Public/O3DTransportTypes.h:52-53, :67-99, :101-102
- Evidence:
  - `bUseAutoTokenFetch`, `TokenEndpointUrl` and `TokenRefreshLeadTimeSec` carry LiveKit-specific doc ("SECURITY NOTE … LiveKit API credentials") in the transport-neutral struct, even though `AdvancedParams` exists for exactly that.
  - `bPersistToken` is a persistence concern living in a runtime DTO.
  - The doc says the canonical ids are `"sockets", "webrtc", "loopback"`, but transports register `"TCP"`, `"UDP"`, `"NNG"`, `"WebRTC"`, `"MoQ"`, `"Loopback"` (Open3DTransportSocketsModule.cpp:26-27 and the other modules). There is no `"sockets"`.
  - `Transport` and `Role` are free-form `FString`s instead of `FName` or an enum.
  - The struct has no version field for forward compatibility.
- Recommendation:
  - Move the token-fetch settings into a WebRTC-owned struct that is passed through AdvancedParams or a typed extension (`TMap<FName, TSharedPtr<FO3DTransportExtension>>`).
  - Fix the doc to list the actual registered names, and use `FName Transport`.
  - Define `Role` as an enum.
- Effort: M
- Owner: design
- Status: closed in #311

### SHR-37: Module docs do not describe the Shared APIs, the threading contracts or the wire format
- Category: docs
- Severity: low
- Location: README.md:16; Source/Open3DShared/Public/O3DUnifiedMessage.h:24; Source/Open3DShared/Public/SerializedFrameConsumerRegistry.h:7-21; Source/Open3DShared/Public/O3DAudioFrameCodec.h:28-85
- Evidence:
  - The README describes Shared as "Shared utilities and base classes used by all modules". There are no base classes, and it has no API list.
  - Nowhere does the code or docs define the 20-byte header layout or the magic `0x4F334441` ("O3DA").
  - The audio payload v1/v2 layouts, which thread may call `SubmitFrame`, `Decode` or `BuildEncodedFrame`, and whether `FFrameEncoder` or `FFrameDecoder` are thread-safe are also undocumented. They are not: both have mutable scratch state and no locks.
  - `Transport_Module_Comparison.md` only says "Unified message format".
- Recommendation:
  - Add `Source/Open3DShared/README.md` with a byte-level wire spec (a table per header and version), threading rules per class, and the codec fallback behaviour.
  - Reference it from the plugin README and from CHANGELOG "Schema/Protocol" entries, as the project rules require.
- Effort: S
- Owner: design

### SHR-38: Global singletons are used throughout instead of injectable services
- Category: architecture
- Severity: low
- Location: Source/Open3DShared/Private/O3DAudioBus.cpp:7; Source/Open3DShared/Private/O3DPerformanceMetrics.cpp:12-16; Source/Open3DShared/Private/SerializedFrameConsumerRegistry.cpp:7-8; Source/Open3DShared/Private/O3DConsoleVars.cpp:6-65; Source/Open3DShared/Open3DShared.Build.cs:87
- Evidence:
  - Shared exposes five process-wide mutable globals: the audio bus delegate, the metrics singleton (Meyers static with a private destructor), the consumer factory with its lock, 10 CVars, and the Build.cs cache.
  - Project rules (§3 "Avoid Global State", "Dependency Injection") discourage this.
  - It makes tests order-dependent. For example, metrics accumulate across automation tests, and `Reset()` is incomplete (SHR-25).
  - It also prevents per-world or per-PIE isolation.
- Recommendation:
  - Introduce an `FO3DRuntimeContext` (metrics sink, audio bus, logger) passed into the transport `Initialize` via `FO3DTransportConfig`, or hosted by a `UEngineSubsystem`.
  - Keep the static accessors only as thin defaults for backward compatibility.
- Effort: L
- Owner: design
