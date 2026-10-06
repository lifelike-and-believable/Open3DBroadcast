# Review: Open3DTransportWebRTC (LiveKit FFI) and Open3DTransportMoQ (moq-ffi)

1. Both modules wrap Rust C ABIs (`livekit_ffi.h`, `moq_ffi.h`) behind `IOpen3DSender` / `IOpen3DReceiver`. Win64 is the only platform with binaries; the DLLs are delay-loaded and also loaded explicitly with `GetDllHandle` in `StartupModule`.
2. WebRTC: `FO3DWebRTCSender` serializes and sends each subject on a labeled LiveKit data channel from the game thread, and publishes per-subject audio tracks from the audio thread. `FO3DWebRTCReceiver` queues FFI callbacks into per-subject maps that `Poll()` drains on the game thread. Tokens come from a manual JWT or from an HTTP token endpoint (`FO3DTokenManager`/`FO3DTokenFetcher`).
3. MoQ: `FMoQSessionWrapper` owns the `MoqClient`. It runs `moq_connect` on a task-graph thread and sends FFI callbacks through a custom dispatcher thread to the game thread. The sender has its own worker thread that publishes a byte-capped queue on one mocap track and one audio track. The receiver subscribes to both tracks and drains up to 16 payloads per `Poll()`.
4. Overall health: **fragile**. The happy path on Win64 works, but there are several lifetime and data races on the FFI and audio boundaries (TRF-1, 2, 9, 10, 12, 15). One functional path is fully broken: WebRTC token auto-fetch never connects (TRF-3).
5. Game-thread blocking is common: `lk_disconnect`, which the header documents as blocking, runs in `Stop()` and in the receiver's reconnect. MoQ `Disconnect()` waits on a mutex that the background `moq_connect` holds. MoQ announce, create-publisher and subscribe calls run synchronously on the game thread.
6. Reconnect behavior is inconsistent. The WebRTC receiver tears down the whole connection after 2 s without data, with no backoff. The WebRTC sender never reconnects. MoQ uses a sound 0.5–10 s backoff, but a flag can stick and stop it, and namespaces are not re-announced after a drop.
7. Security: LiveKit JWTs are stored in plain text in the component `UPROPERTY` / `GameUserSettings` and copied into `AdvancedParams` (`bPersistToken` is ignored). The token endpoint has no authentication, and the client asks for its own grants.
8. Portability and Fab: the WebRTC `Build.cs` throws on non-Win64. Neither module guards its sources on the enable flag, so turning a transport off (MoQ is forced off on Linux/Mac) leaves code that cannot compile.
9. Tests are mostly "does not crash while unconnected". Some are tautological, one MoQ test cannot pass, and one EngineFilter test reaches out to the public Cloudflare relay. There is no fake-FFI seam.
10. The docs are large but partly stale or inaccurate: they claim API-key auth, "thread safety validated", tunable backpressure CVars, and a missing implementation plan. Internal review and work-plan docs ship inside `Source/`.

Paths below are relative to `ProjectSandbox/Plugins/Open3DBroadcast/Source/` unless they start with `src/`.

---

### TRF-1: Audio sinks hold raw owner references and use FFI track and client handles without synchronization against Stop()
- Category: thread-safety
- Severity: critical
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:149-193, :216, :231-275, :527-572; Open3DTransportMoQ/Private/Sender/MoQSenderAudioSink.h:31, :60; Open3DTransportMoQ/Private/Sender/MoQSender.cpp:743
- Evidence: `FWebRTCSenderAudioSink` keeps `FO3DWebRTCSender& Owner` and is returned as a thread-safe `TSharedPtr` that can outlive the sender. `SubmitPcm` runs on the audio thread and reads `Owner.ClientHandle` / `bConnected` with no lock (l.161). `GetOrCreateAudioTrack` returns the raw `LkAudioTrackHandle*` after it releases `AudioTracksMutex` (l.233-274), and `lk_audio_track_publish_pcm_i16(Track, ...)` then runs unlocked (l.189). Meanwhile `Stop()` on the game thread calls `lk_audio_track_destroy` on every track and `lk_client_destroy(ClientHandle)` (l.543, :571). The destructor also calls `Stop()`. The result is use-after-free of the track, the client, or the whole sender. The MoQ sink has the same `FO3DMoQSender& Owner` pattern (`MakeShared<FO3DMoQSenderAudioSink>(*this, ...)`).
- Recommendation: Give the sink a `TWeakPtr` to shared "audio publish state" (or a ref-counted core object) instead of `Owner&`. Hold `AudioTracksMutex` (or a read lock) across publish, or remove tracks from the map under the lock and destroy them only after in-flight publishes finish (for example with an `FRWLock`, or an atomic "closing" flag plus a drain). `Stop()` should mark the sink invalid before it destroys any handle.
- Effort: M
- Owner: coding
- Status: closed in #269

### TRF-2: Dangling UTF-8 pointer stored in LkAudioTrackConfig.track_name
- Category: bug
- Severity: high
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:244
- Evidence: `TrackConfig.track_name = TCHAR_TO_UTF8(*StreamLabel);`. `TCHAR_TO_UTF8` produces a temporary `FTCHARToUTF8` converter that is destroyed at the end of the statement, so `track_name` dangles when `lk_audio_track_create(...)` reads it on l.250. Track names (which the receiver uses as subject labels) can be garbage, and long names (heap-allocated) are a read-after-free. needs-UE-verification for the exact converter storage, although UE documents that the macro result must not be stored.
- Recommendation: `FTCHARToUTF8 TrackNameUtf8(*StreamLabel); TrackConfig.track_name = TrackNameUtf8.Get();` so the converter lives across the FFI call. Grep the codebase for other stored `TCHAR_TO_UTF8` results.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-3: WebRTC token auto-fetch mode never connects
- Category: bug
- Severity: high
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:492-497, :964-976, :1112-1125; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:514-519, :703-715, :864-877
- Evidence: `Start()` calls `EnsureTokenAvailable()`, which starts `RefreshTokenAsync` and returns `true` ("will connect when token available"). The completion lambda sets `Token = Result.Token`. `FO3DTokenManager::OnTokenFetchComplete` had already set `CurrentToken` to the same value (WebRTCTokenManager.cpp:319). `Tick()` / `Poll()` retry `Start()` only `if (Token.IsEmpty() || Token != CurrentToken)`, and that condition is now false. `Start()` is called only once, by `O3DSenderTransportController.cpp:35` / `O3DReceiverSource.cpp:357`. `lk_connect_with_role_async` is therefore never called in auto-fetch mode. No test covers this path.
- Recommendation: Track "connect pending" separately from the token value, for example `bConnectRequested && !bConnectInFlight && TokenAvailable`, and let `Tick`/`Poll` start the connect. Do not assign `Token` inside the HTTP callback; read it from `TokenManager` on the game thread. Add a test that uses a stub fetcher.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-4: Double free of borrowed transforms on the serialize-failure path in Send(SubjectList)
- Category: bug
- Severity: high
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:37-48, :689-705, :719; src/o3ds/model.h:129-133
- Evidence: Raw `Transform*` pointers owned by the caller's `List` are pushed into the pooled subject (`NewSubject->addTransform(Transform)`, l.693). They are detached with `NewSubject->mTransforms.mItems.clear()` only after a successful serialize (l.719). If `Serialize` returns `<= 0`, `SerializerPool->Release(PooledSubject)` runs first (l.704). `Reset()` then deletes the `Subject`, and `~TransformList` (`model.h:129-133`) deletes transforms that still belong to `List`, which leads to a double free and use-after-free in the caller.
- Recommendation: Detach the borrowed transforms right after serialization, before any early-out (or use a scope guard). Better: delete this path and have `Send()` delegate to `SendSerialized` using the shared serializer. Also see TRF-19.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-5: Heuristic "FFI backpressure" estimator drops frames on healthy links and spams warnings
- Category: bug
- Severity: high
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:279-329, :606-619, :797-806, :852-861, :948; Open3DTransportWebRTC/Private/Sender/WebRTCSender.h:78-80
- Evidence: Every successful send increments `EstimatedPendingFrames`. It is decremented by a fixed 10 at most once per >100 ms, and only from inside `UpdateFrameSendMetrics`. At a 60 Hz tick the decay runs about every 7th frame, which is roughly 86 frames/s of drain. With `SendSerialized` called per subject, two subjects at 60 Hz (120/s) or one subject at more than about 90 Hz pushes the estimate past 30. The sender then drops real frames even when the network is idle. Above 15 it logs a Warning on every send (l.801-806, :610). The FFI already exposes real counters (`lk_get_data_stats`, livekit_ffi.h:399), and nothing calls them. The header comment says "configurable via console variables", but the values are `static constexpr`.
- Recommendation: Remove the estimator, or base it on `lk_get_data_stats` deltas (dropped and sent bytes) or on a wall-clock drain rate. Make any thresholds CVars. Throttle the warnings.
- Effort: M
- Owner: design
- Status: closed in #271

### TRF-6: WebRTC receiver no-data watchdog forces full teardown and reconnect every 2 s, on the game thread, with no backoff
- Category: bug
- Severity: high
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:163-175, :730-736, :841-842, :1084-1150; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.h:101; Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h:224-227
- Evidence: `NoDataReconnectTimeoutSec` defaults to `2.0` in `ParseConfig` (l.841), although the header says `5.0`. If no data arrives for 2 s (an idle sender, a paused capture, or a room with no publisher yet), `Poll()` calls `RequestReconnect()`. `ProcessReconnectIfNeeded` then runs on the game thread: `lk_disconnect` (the header says it "Blocks until disconnect is complete"), `lk_client_destroy`, recreate, and `lk_connect_with_role_async`. This repeats every 2 s for as long as the room is idle. `LkConnFailed` and `LkConnDisconnected` also force an immediate reconnect. If `BeginConnect` fails, the code sets `bReconnectPending` again, so it retries on every tick with no backoff (l.1132-1136). This also competes with the SDK's own `LkConnReconnecting` logic, and `lk_set_reconnect_backoff` is never called.
- Recommendation: Default the watchdog to off, or to a large value with exponential backoff (reuse the MoQ `ComputeReconnectDelaySeconds` idea from a shared helper). Do not reconnect while the state is `Reconnecting`. Configure `lk_set_reconnect_backoff`. Move disconnect and destroy to a background task (see TRF-7).
- Effort: M
- Owner: design

### TRF-7: Blocking FFI calls on the game thread
- Category: performance
- Severity: high
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:557-571; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:567-569, :1119-1120; Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:118-126, :162-185, :221-228, :265-269, :305-310; Open3DTransportMoQ/Private/Sender/MoQSender.cpp:250-261, :738-741; Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:251-261, :487-494
- Evidence: (a) `lk_disconnect` is documented as blocking (livekit_ffi.h:224-227). It is called from `Stop()` and from the receiver's `Poll()` reconnect path, and `Poll` is called from `FO3DReceiverSource::Tick` (O3DReceiverSource.cpp:284). (b) The MoQ background task holds `SessionHandle` mutex for the whole `moq_connect` call ("may block on Tokio runtime", l.118). `Disconnect()` (l.172), `AnnounceNamespace`, `CreatePublisher` and `Subscribe` all take the same mutex on the game thread, so `Stop()`, PIE end, or a failed-connect retry can stall the game thread until the connect times out. (c) `HandleConnectionStateChanged(CONNECTED)` runs on the game thread through the dispatcher and synchronously calls `moq_announce_namespace`, `moq_create_publisher_ex` and `moq_subscribe`. Whether these wait for relay round-trips is needs-FFI-verification (the header does not say). The project rules forbid game-thread blocking.
- Recommendation: Do not hold the handle mutex across `moq_connect`. Guard only pointer lifetime (for example with a shared_ptr snapshot). Run disconnect, destroy, announce and subscribe on a dedicated transport worker and post results back. For LiveKit, call `lk_disconnect` / `lk_client_destroy` from a background task that owns the handle, and make `Stop()` non-blocking (hand off ownership).
- Effort: L
- Owner: design

### TRF-8: MoQ namespace is not re-announced after an unexpected disconnect and reconnect
- Category: bug
- Severity: high
- Location: Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:43, :162-197, :204-239 (cache check at :214), :406-445; Open3DTransportMoQ/Private/Sender/MoQSender.cpp:244-281, :366-382
- Evidence: `AnnounceNamespace` returns OK early if the namespace is already in `AnnouncedNamespaces`. The cache is cleared only in `Initialize()` and in an explicit `Disconnect()`. `HandleConnectionStateInternal` (FAILED or DISCONNECTED) does not clear it, and `Tick()` → `AttemptConnect()` → `Session->Connect()` reconnects without clearing it. After a network drop the new session never sends ANNOUNCE, and publishers are created on an un-announced namespace, so subscribers on the relay cannot find the track. Reusing the same `MoqClient` for a second `moq_connect` after a failure is also not documented as supported (needs-FFI-verification).
- Recommendation: Clear `AnnouncedNamespaces` (and bindings) on every transition to DISCONNECTED or FAILED. Preferably destroy and recreate the `MoqClient` on reconnect.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-9: Data race on the MoQ publisher TSharedPtr members between the worker and game threads
- Category: thread-safety
- Severity: high
- Location: Open3DTransportMoQ/Private/Sender/MoQSender.cpp:452, :488, :493-501, :558-569, :631, :396-420
- Evidence: `PublishPayload` / `IsPublisherReady` run on `MoQSenderWorker` and copy or read `MocapPublisherHandle` / `AudioPublisherHandle`. `EnsurePublisher`, `DestroyPublisher` (from `HandleConnectionStateChanged` on the game thread) and `CreateAudioSink` assign or `Reset()` the same members with no lock. Copying a `TSharedPtr` while another thread resets it is a data race (UB): torn reads, or reference-count corruption.
- Recommendation: Guard the handles with a mutex and snapshot them under the lock in the worker, or keep the publisher lifecycle on the worker thread (post connect-state changes to it).
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-10: MoQ audio encoder is shared across threads and streams without synchronization
- Category: thread-safety
- Severity: medium
- Location: Open3DTransportMoQ/Private/Sender/MoQSender.cpp:722-744, :746-767, :769-807; Open3DShared/Public/O3DAudioFrameCodec.h:32-59
- Evidence: One `O3DAudio::FFrameEncoder AudioEncoder` is used by `ProcessCapturedAudio` on every audio sink thread and for every `StreamLabel`. `CreateAudioSink` on the game thread calls `RefreshAudioEncoder()`, which re-runs `AudioEncoder.Initialize(...)` while capture may be encoding. `FFrameEncoder` has no internal locking. For Opus, interleaving several streams through one stateful encoder also corrupts each stream.
- Recommendation: Give each sink or stream label its own encoder, owned by the sink, or protect the encoder with a lock and reinitialize only when no sink is active.
- Effort: M
- Owner: coding
- Status: closed in #269

### TRF-11: MoQ bConnectInFlight can stay set, so reconnect never retries; there is no connect timeout
- Category: bug
- Severity: medium
- Location: Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:104-159; Open3DTransportMoQ/Private/Sender/MoQSender.cpp:217-242, :366-382; Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:218-243, :474-484
- Evidence: `Connect()` always returns `Ok("Connection initiated (async)")`. If the background `moq_connect` returns an error, it is only logged (l.142-151), and the FFI has no documented guarantee that it will then call `MOQ_STATE_FAILED`. `bConnectInFlight` stays `true`, and `Tick`/`Poll` only retry `if (... && !bConnectInFlight)`. The same happens if the `catch(...)` path runs. There is also no deadline for a connect that hangs.
- Recommendation: Have the background task publish a terminal result (state FAILED) through the dispatcher whenever `moq_connect` returns non-OK. Add a connect-timeout watchdog that clears `bConnectInFlight` and bumps `ConsecutiveFailures`.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-12: MoQ callback user_data lifetime relies on undocumented FFI quiescence
- Category: security
- Severity: medium
- Location: Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:122-127, :162-197, :300-341, :447-468; Open3DTransportMoQ/Private/Shared/MoQHandles.cpp:140-151; Open3DTransportMoQ/ThirdParty/moq-ffi/include/moq_ffi.h:160-164, :288-292
- Evidence: `moq_connect` receives the raw `FMoQSessionWrapper*` (`StrongThis.Get()`). `moq_subscribe` receives the raw `FSubscriberBinding*` owned by `SubscriberBindings`. `Disconnect()` frees every binding (`SubscriberBindings.Reset()`, l.188) even while `FMoQSubscriberHandle`s (and their `MoqSubscriber`s) are still alive. `RemoveSubscriberBinding` runs right after `moq_subscriber_destroy`. Unlike `livekit_ffi.h:408-409`, `moq_ffi.h` does not promise that no callbacks fire after `*_destroy` / `moq_disconnect` returns, or that in-flight callbacks have finished. A Tokio thread inside `HandleSubscriberDataThunk` can dereference a freed `Binding` (it copies `Binding->DataHandler`, l.467). needs-FFI-verification.
- Recommendation: Pass a heap-allocated, ref-counted context (for example a `TSharedPtr` raw pointer plus a registry lookup by id, or an "alive" generation counter) instead of raw object pointers. Never free a binding before its subscriber. Get the quiescence contract documented and tested in moq-ffi.
- Effort: M
- Owner: design
- Status: closed in #269

### TRF-13: MoQ async dispatcher: redundant thread hop, event race, lazy restart after Shutdown, tasks outliving the module
- Category: thread-safety
- Severity: medium
- Location: Open3DTransportMoQ/Private/Shared/MoQAsyncDispatcher.cpp:18-40, :42-72, :74-87, :115-121; Open3DTransportMoQ/Private/Open3DTransportMoQModule.cpp:584-593
- Evidence: `EnqueueGameThreadTask` reads `TaskEvent` without `InitMutex` while `Shutdown()` may return it to the pool (l.78-86, :67-71). After `Shutdown()`, any late FFI callback lazily calls `Initialize()` again and restarts the thread during module teardown. The worker only forwards each task with `AsyncTask(ENamedThreads::GameThread, ...)` (l.120), which is already thread-safe from any thread, so the dedicated thread adds a hop, an allocation and latency. Queued game-thread tasks can run after `ShutdownModule` unloads the FFI library.
- Recommendation: Delete the dispatcher and call `AsyncTask(GameThread, ...)` (or a TS ticker queue drained in `Tick`/`Poll`) directly with weak-pointer guards. Make shutdown ordering explicit: stop instances, flush, unregister, then unload.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-14: FFI libraries are unloaded while instances may still be alive; WebRTC registers factories even if the DLL failed to load
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:614-623, :713-727, :732-784; Open3DTransportMoQ/Private/Shared/MoQFfiSupport.cpp:93-106; Open3DTransportMoQ/Private/Open3DTransportMoQModule.cpp:584-593
- Evidence: `ShutdownModule` calls `FreeDllHandle` on `livekit_ffi.dll` / `moq_ffi.dll` without checking for live senders or receivers (components or LiveLink sources can outlive module shutdown during editor exit or hot-reload). Delay-load thunks keep resolved addresses. In WebRTC, `LoadLiveKitFFI()` failure only logs, and the factories are still registered, so the first `lk_client_create` hits a delay-load failure (SEH `0xC06D007E`) instead of a clean error. MoQ returns early without registering, which is the correct behavior.
- Recommendation: Register factories only after a successful load. Keep the DLL loaded until process exit, or refuse to unload while instance count > 0.
- Effort: S
- Owner: coding
- Status: closed in #303

### TRF-15: WebRTC receiver shares state across the FFI, HTTP and game threads without synchronization
- Category: thread-safety
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:337-376, :390-414, :573-574, :641-680, :753-757, :869, :913; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.h:131; Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1117, :1161
- Evidence: FFI audio callbacks read and copy `Self->AudioSink` (TSharedPtr) while `SetAudioSink`/`Stop` write it under `StateMutex`, which the callbacks do not take. `LastAudioDropLogTime` is a plain double written from FFI threads. `Poll()` reads `Consumer` without `StateMutex` while `SetConsumer`/`Stop` write it. The token callbacks assign the `FString Token` from the HTTP completion context while the game thread reads it (the HTTP delegate thread is needs-UE-verification; the docs claim "HTTP module's thread").
- Recommendation: Snapshot the sink or consumer under a small lock (or with an atomic shared ptr), make the log timestamp atomic, and marshal token results to the game thread.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-16: Receiver registers both labeled and unlabeled data callbacks, which can double-enqueue frames
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:990-1022, :254-320
- Evidence: After `lk_client_set_data_callback_ex` succeeds, the code also registers `lk_client_set_data_callback(OnDataReceived)` "for diagnostics". `livekit_ffi.h` says the audio `_ex` overrides the plain callback (l.186), but says nothing like that for data. If the FFI calls both, each packet is queued twice, once under its label and once under `"default"`, and a phantom `default` subject is submitted to the consumer. needs-FFI-verification.
- Recommendation: Register only the `_ex` callback, and fall back to the plain one only when `_ex` fails.
- Effort: S
- Owner: coding

### TRF-17: WebRTC receiver queues are unbounded and allocation-heavy
- Category: performance
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.h:63-80; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:221-230, :293-302, :600-630, :656-664
- Evidence: Each message reserves 15 KB (`ReserveForTypicalFrame`) even for small payloads, and `PendingFramesBySubject` arrays grow without limit when `Poll` stalls (a hitch, a breakpoint, a paused world). `Poll` builds a new `TMap` every call and `MoveTemp`s every per-subject array, so the next enqueue reallocates. It also takes `StatsMutex` once per frame. The 8 MB per-message cap only limits one message, not the queue.
- Recommendation: Use `SetNumUninitialized(len)`. Cap each subject's queue (keep the latest N or a byte budget, and count drops). Swap buffers with pre-reserved arrays. Accumulate latency locally and publish it once per `Poll`.
- Effort: M
- Owner: coding

### TRF-18: MoQ receive path copies each payload about 4 times and makes 2 thread hops
- Category: performance
- Severity: medium
- Location: Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:460-474; Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:405-411, :437-443, :498-540, :559-563; Open3DTransportMoQ/Private/Receiver/MoQReceiver.h:128
- Evidence: The FFI buffer is copied to `TArray64`, the `TFunction` handler is copied into a `TUniqueFunction`, then it hops FFI thread → dispatcher thread → task graph → game thread. There it is copied into `FReceivedPayload::Data` (with `QueueMutex`, although everything is already on the game thread), and copied again into `FrameData` before `SubmitFrame`. The receiver drains at most 16 payloads per `Poll`, FIFO, for mocap and audio together, so at low frame rate or high send rate the latency grows until the 16 MB cap.
- Recommendation: Move the `TArray` from the FFI copy all the way to the consumer (one copy), queue directly into a lock-free SPSC queue drained in `Poll`, and drop stale mocap frames (keep the latest per subject) instead of a fixed 16-frame budget.
- Effort: M
- Owner: coding

### TRF-19: Send(SubjectList) path does deep copies and allocations despite "zero allocation" comments; the path is effectively dead
- Category: performance
- Severity: low
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:15-95, :459-461, :641-695; Open3DSender/Public/O3DSenderInterface.h (SendSerialized comment)
- Evidence: Each subject's `mJoints`, `mCurveNames` and `mCurveValues` vectors are copied, and `addSubject` does `new Subject` every time ("Reduces 3,000+ allocations/sec → 0" is false). The pool pre-allocates 10 items but only one is ever used at a time. The normal pipeline calls `SendSerialized`, so this complex path is mostly unused.
- Recommendation: Implement `Send()` as serialize-once plus `SendBytes`, or remove the pool and delete the misleading comments.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-20: MoQ receiver retries subscribe every Poll with no backoff
- Category: bug
- Severity: medium
- Location: Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:286-334, :486-494; Open3DTransportMoQ/Private/Receiver/MoQReceiver.h:113
- Evidence: While connected and not subscribed (for example, the publisher has not announced yet), `Poll()` calls `AttemptSubscribe()` and `AttemptAudioSubscribe()` every tick. Each makes a synchronous `moq_subscribe` under the session mutex on the game thread. `LastSubscribeAttemptTimeSeconds` is written (l.298) but never read.
- Recommendation: Gate subscribe retries with backoff, using `LastSubscribeAttemptTimeSeconds` and `ComputeReconnectDelaySeconds`.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-21: LiveKit JWTs are persisted in plain text and copied into AdvancedParams; bPersistToken is ignored
- Category: security
- Severity: high
- Location: Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:221-230, :507-510, :632-647, :672-688; Open3DSender/Public/O3DSenderComponent.h:158-161; Open3DReceiver/Public/O3DReceiverSourceSettings.h:27-33; Open3DShared/Public/O3DTransportTypes.h:70-74, :107-120
- Evidence: The settings panels write `webrtc.token` into `UO3DSenderComponent::TransportOptions` (a `UPROPERTY`, so it is saved into the level or blueprint asset) and into `UO3DReceiverSettingsObject` (`UCLASS(Config = GameUserSettings)`). `ConfigureTransport` copies the token into `Config.AdvancedParams["webrtc.token"]`. `FO3DTransportConfig::bPersistToken` ("Default false so callers explicitly opt-in to storing credentials") is never consulted. `ToDebugString()` says "secrets redacted" but prints every `AdvancedParams` key=value, token included, which is a latent log leak (no current callers). The `IsPassword(true)` text box hides the token only in the UI.
- Recommendation: Keep tokens out of serialized properties: mark them `Transient`, or store them in per-user config / an OS credential store keyed by id, and honor `bPersistToken`. Do not mirror the token into `AdvancedParams`. Redact keys matching `*token*|*secret*|*key*` in `ToDebugString`.
- Effort: M
- Owner: design
- Status: closed in #275

### TRF-22: Token endpoint is unauthenticated, the client requests its own grants, and no auth or TLS policy is enforced
- Category: security
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Shared/WebRTCTokenFetcher.cpp:46-55, :96, :205-278, :344; Open3DTransportWebRTC/Tests/mock-token-server.py:45-53, :83-85; Open3DTransportWebRTC/TOKEN_AUTO_FETCH_IMPLEMENTATION.md:30, :45, :396, :525; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:1059-1061
- Evidence: The request has only `Content-Type`, with no `Authorization` header or API key, even though the docs describe "Optional API key authentication" / `TokenApiKey` and the mock server supports `Bearer`. The body includes client-chosen `grants` (`roomCreate:true`, `canPublish`), and the mock server honors them. Anyone who can reach the endpoint can mint publisher tokens. `http://` endpoints are accepted silently. The error path logs the full response body at Warning (l.96), and parse failures log it at Verbose (l.344). `BeginConnect` logs the first 20 characters of the JWT at Warning. The plugin itself contains no hard-coded secrets (checked).
- Recommendation: Add an optional auth header sourced from env or per-user config. Do not send grants; the server decides them from an authenticated identity. Warn or refuse on non-HTTPS unless the host is localhost. Stop logging token fragments and response bodies.
- Effort: M
- Owner: design
- Status: closed in #275

### TRF-23: Token refresh is fetched but never applied; an expired manual token spams warnings
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1141-1174; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:893-926; Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h:365-369; Open3DTransportWebRTC/Private/Shared/WebRTCTokenManager.cpp:84-91
- Evidence: `CheckTokenRefresh` fetches a new token and then `// TODO: Reconnect with new token`. `lk_refresh_token()` exists in the FFI and is never called, so any SDK-internal reconnect after expiry uses the stale JWT. `GetCurrentToken` logs `Warning "Current token is expired"` on every call, and `Tick`/`Poll` call it every frame while disconnected.
- Recommendation: Call `lk_refresh_token(handle, newToken)` when refresh succeeds, and fall back to a controlled reconnect. Log the expiry once, per state transition.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-24: Token fetcher lifetime and retry rely on raw `this` and GWorld's timer manager
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Shared/WebRTCTokenFetcher.cpp:69-72, :115-143, :159-178, :180-203; Open3DTransportWebRTC/Private/Shared/WebRTCTokenManager.cpp:196-199, :217-232; Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1034, :1128-1135
- Evidence: The HTTP completion lambda and the retry timer capture raw `this` (fetcher and manager). `TokenManager->Reset()` destroys the fetcher, and `ParseConfig` replaces the whole manager, so if `CancelRequest()` still fires the completion delegate, the callback dereferences freed objects (needs-UE-verification for cancel-delegate semantics). Retries use `GWorld`'s timer manager, which can be null, an editor world, or change at PIE start or stop, so a timer can be lost (leaving `bRefreshInProgress`/`bWaitingForToken` stuck) or fire into a dead object. With no world, it "retries immediately" in a tight loop of up to 5 requests. If `ProcessRequest()` fails, `OnComplete` may run twice (the delegate may also fire; needs-UE-verification). `ActiveRequests` is modified from callbacks without a lock. The 30 s `TokenFetchTimeoutSec` check runs only inside `Start()`, which is never re-entered while waiting, so it cannot trigger.
- Recommendation: Unbind the delegates before cancelling. Capture `TWeakPtr`/shared state. Use `FTSTicker` (not world timers) for backoff. Enforce the fetch timeout in `Tick`/`Poll`.
- Effort: M
- Owner: coding
- Status: closed in #271

### TRF-25: LiveKit identity collisions and room mismatch between sender and receiver in auto-fetch mode
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1043-1044; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:796-797; Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:679; Open3DSender/Private/O3DSenderComponent.cpp:328
- Evidence: Identities are `sender-<PID>` / `receiver-<PID>`, so two senders (or two receivers) in one editor process join with the same identity, and LiveKit disconnects the earlier participant (DUPLICATE_IDENTITY). The room is `Config.StreamId`. The sender resets `StreamId` to empty (so the room is empty and the token manager only warns), while the receiver hard-codes `StreamId = "WebRTCStream"`, so the two sides request tokens for different rooms.
- Recommendation: Add a per-instance GUID suffix to the identity. Expose an explicit `webrtc.room` option used by both sides, and fail fast when it is empty in auto-fetch mode.
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-26: WebRTC sender never reconnects; SDK reconnect settings are unused
- Category: bug
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:354-371, :959-984; Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h:355-363
- Evidence: On `LkConnFailed` / `LkConnDisconnected` the sender only clears `bConnected`. `Tick()` retries `Start()` only when the token string changes (see TRF-3), so in manual-token mode a failed connection stays dead until the user restarts. The receiver has its own (over-aggressive) reconnect (TRF-6), so the two sides behave differently. `lk_set_reconnect_backoff` is never called on either side.
- Recommendation: Use one shared reconnect policy (see TRF-32) for both sides: backoff, jitter, max attempts, and fresh token acquisition.
- Effort: M
- Owner: design

### TRF-27: Non-Win64 builds break: WebRTC throws in Build.cs, and a disabled MoQ or WebRTC module still compiles FFI-dependent sources
- Category: fab-readiness
- Severity: high
- Location: Open3DTransportWebRTC/Open3DTransportWebRTC.Build.cs:14-27; Open3DTransportMoQ/Open3DTransportMoQ.Build.cs:14-17, :20-36, :46-57, :99-114; Open3DShared/Open3DShared.Build.cs:107-111; Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:750-754; Open3DTransportMoQ/Private/Shared/MoQFfiSupport.cpp:169-177; Open3DBroadcast.uplugin:52-61
- Evidence: `O3D_WITH_TRANSPORT_WEBRTC` defaults to true with no platform auto-disable, and the WebRTC `Build.cs` throws `BuildException` for any non-Win64 target, which breaks Linux, Mac and Linux-server builds by default. MoQ is force-disabled off Win64, but its `Build.cs` then `return`s before adding include paths and dependencies (`Core`, `Open3DShared`, `Open3DSender`...). Only `Open3DTransportMoQModule.cpp` and the tests are wrapped in `#if O3D_WITH_TRANSPORT_MOQ`. `MoQSender.cpp`, `MoQReceiver.cpp` and `Shared/*.cpp` still `#include "moq_ffi.h"`, so they fail to compile. WebRTC sources have no guard at all, and the module cpp has `#error "Unsupported platform"`. The Linux/Mac branches in the MoQ `Build.cs` and `MoQFfiSupport` are unreachable. The `.uplugin` has no `PlatformAllowList` for either module. Whether UBT implicitly supplies Core is needs-UE-verification; the missing `moq_ffi.h` include path is certain.
- Recommendation: Add `PlatformAllowList: ["Win64"]` to both modules in `.uplugin` (the simplest fix for Fab). Alternatively, always add dependencies and include paths, and wrap every TU in `#if O3D_WITH_TRANSPORT_*`, with stub modules as MoQ already has. Make the WebRTC flag auto-disable on non-Win64 like MoQ, and remove the dead platform branches.
- Effort: S
- Owner: coding
- Status: closed in #283

### TRF-28: Duplicated and inconsistent DLL loading paths
- Category: fab-readiness
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:29-85, :463, :968; Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:732-774; Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:374-377; Open3DTransportMoQ/Private/Shared/MoQFfiSupport.cpp:149-180
- Evidence: The module loads `Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.dll`. The receiver's own `EnsureLiveKitFfiLoaded` looks in `Binaries/Win64/` and `ThirdParty/livekit_ffi/bin/Win64/Release/`, neither of which exists, so it only logs "relying on system loader" (dead code), and it keeps a global handle it never frees. The sender comment says the DLL "is loaded automatically via .lib linkage". Both modules load binaries from the plugin's `Source/` tree at runtime. Packaging relies on `RuntimeDependencies` preserving that path (needs-UE-verification for installed or Fab plugin layouts, where Source may be stripped). The loaders are near-duplicates across modules.
- Recommendation: Stage the DLLs to `$(PluginDir)/Binaries/ThirdParty/<Lib>/Win64/` via `RuntimeDependencies.Add(target, source)`. Load them through one shared `FO3DFfiLibraryLoader` in Open3DShared, and delete the receiver loader.
- Effort: S
- Owner: coding
- Status: closed in #286

### TRF-29: MoQ library validation contradicts itself and misses symbols that are actually used
- Category: bug
- Severity: low
- Location: Open3DTransportMoQ/Private/Shared/MoQFfiSupport.cpp:60-74, :113-137, :182-233
- Evidence: `moq_version` is marked optional (l.207), but when it is missing `GetVersion()` returns `"unknown"`, which fails the `Contains("Draft 07")` check, and the library is unloaded. The required list omits functions the module calls: `moq_init`, `moq_create_publisher_ex`, `moq_announce_namespace`, `moq_publish_data`, `moq_is_connected`, `moq_last_error`. A stale DLL therefore passes validation and later crashes on delay-load. Gating on a human-readable version substring is brittle.
- Recommendation: Validate the full set of symbols used, and compare against a structured ABI version (for example an exported `moq_abi_version()`).
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-30: Diagnostic logging noise at Warning level
- Category: code-quality
- Severity: low
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:141-171, :456, :467-474, :988-1036, :1059-1061, :1079, :691-694; Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:324-326, :610-612, :801-806
- Evidence: Normal lifecycle events are logged as `[DIAG] ... Warning`, and several are duplicated with a Log-level twin. When there is no consumer, `Poll()` logs a Warning every tick ("%d frames dropped" with `FramesProcessed`, which is always 0). `%hs` is used with a possibly null `message` (l.164, :171). This violates the rule "Quiet hot paths; only state transitions and errors".
- Recommendation: Downgrade to Verbose, rate-limit, and remove the `[DIAG]`/`[ARCH]` scaffolding.
- Effort: S
- Owner: coding

### TRF-31: Inconsistent UTF-8 handling for subject and track labels; label cache keyed only by CRC32
- Category: bug
- Severity: low
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:653, :735, :750; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:928-964; Open3DTransportMoQ/Private/Sender/MoQSender.cpp:298
- Evidence: The WebRTC code builds `FString(Subject->mName.c_str())` and `FString NewLabel(RawLabel)` from UTF-8 bytes through the ANSI constructor (needs-UE-verification: this widens bytes rather than decoding UTF-8), then re-encodes with `TCHAR_TO_UTF8`. Non-ASCII subject names get double-encoded, so the label no longer matches the name inside the payload. MoQ correctly uses `UTF8_TO_TCHAR`. `SubjectLabelCache` is a `TMap<uint32 CRC, FString>`, so a CRC collision routes to the wrong subject. The cache never shrinks, and it still computes strlen and CRC and returns a copy, so there is little gain.
- Recommendation: Use `UTF8_TO_TCHAR` / `FUTF8ToTCHAR` everywhere. Key the cache by the string itself (or drop the cache).
- Effort: S
- Owner: coding
- Status: closed in #271

### TRF-32: Transport plumbing is duplicated instead of shared through Open3DShared
- Category: architecture
- Severity: medium
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1019-1174 vs Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:771-926; WebRTCSender.cpp:396-412 vs WebRTCReceiver.cpp:436-452; Open3DTransportWebRTCModule.cpp:58-74 vs Open3DTransportMoQModule.cpp:61-77; MoQSender.cpp:217-281 vs MoQReceiver.cpp:218-284; Open3DTransportMoQ/Private/Receiver/MoQReceiver.h:125-127 vs Open3DTransportMoQ/Private/Shared/MoQHelpers.h:61-67
- Evidence: `ParseConfig` token setup, `EnsureTokenAvailable` and `CheckTokenRefresh` are copy-pasted between the WebRTC sender and receiver, along with the platform-validation block. `SetReceiverOption` is identical in both modules. The MoQ connect, state and backoff code is duplicated between its sender and receiver, and `MoQReceiver.h` re-declares the MoQHelpers constants. Each module has its own FFI loader, `FLatencyStats`, and a "free FFI message string" pattern (`LogIfFailed` vs `FMoQResult::FromResult`). The WebRTC sender audio sink does not derive from `FO3DSenderAudioSinkBase`, which every other transport uses.
- Recommendation: Move into Open3DShared: `FO3DReconnectPolicy` (backoff with jitter), `FO3DFfiLibraryLoader`, `FO3DTransportOptionStore` helpers, `FO3DLatencyAccumulator`, and a bounded per-subject receive queue. Move the token lifecycle into a single `FO3DWebRTCConnection` used by both WebRTC roles. Make the WebRTC sink derive from `FO3DSenderAudioSinkBase`.
- Effort: L
- Owner: design
- Status: closed in #310

### TRF-33: Dead or unused code and unused FFI capabilities
- Category: code-quality
- Severity: low
- Location: Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:380-415 (OnAudioReceived is never registered); Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.h:46 (SubjectName is never read), :122 and Open3DTransportWebRTC/Private/Sender/WebRTCSender.h:98 (`struct FCallbacks` is never defined); Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:347-386 (SubscribeAsync is unused); Open3DTransportMoQ/Private/Receiver/MoQReceiver.h:113, :125-127; Open3DShared/Open3DShared.Build.cs:82-83, :112-113 (O3D_WEBRTC_BACKEND_* is defined but never referenced in C++)
- Evidence: The symbols above have no callers or readers. `lk_get_data_stats`, `lk_get_audio_stats`, `lk_client_is_ready`, `lk_set_reconnect_backoff`, `lk_refresh_token` and `lk_set_role` are never used, even though several would fix TRF-5, 6, 23 and 26. The LibDC backend flag has no implementation in this tree.
- Recommendation: Delete the dead code. Either wire in the useful FFI APIs or document why they are unused. Drop the LibDC flag or implement the backend.
- Effort: S
- Owner: coding

### TRF-34: Tests rarely exercise real behavior; one MoQ test cannot pass; one EngineFilter test needs the internet
- Category: tests
- Severity: medium
- Location: Open3DTransportMoQ/Private/Tests/MoQTrackNamespaceTests.cpp:262-300 (assert at :291); Open3DTransportMoQ/Private/Sender/MoQSender.cpp:283-289; Open3DTransportMoQ/Private/Tests/MoQCloudflareRelayTests.cpp:30, :671-708; Open3DTransportWebRTC/Private/Tests/WebRTCPerSubjectTests.cpp:153, :258, :332, :395, :502; Open3DTransportWebRTC/Private/Tests/WebRTCTransportTests.cpp:499-543
- Evidence: `BackpressureByteLimit` calls `Send()` on a sender that was never started, and it asserts `Stats.DroppedFrames > 0`. That path only calls `FO3DPerformanceMetrics::RecordFrameDropped()` and never touches `Stats`, so the test always fails, and it never exercises the byte cap anyway. `Cloudflare.Basic` is in EngineFilter (default runs): it calls `Connect()` against `relay.cloudflare.mediaoverquic.com`, and its `Disconnect()` can block the test thread on the connect mutex (TRF-7), so it is flaky offline or in CI. The other Cloudflare tests need the public relay (ProductFilter). The WebRTC tests all use fake URLs and tokens, so they only cover the "not connected" branches. Several asserts are tautologies (`FramesSent >= 0`, `TestTrue(..., true)`). `Audio.Clipping` returns before any conversion code runs. There is no fake-FFI seam, so the reconnect, token, per-subject routing and audio paths are untested.
- Recommendation: Put an injectable FFI facade (function table) in front of `lk_*` / `moq_*` so tests can script callbacks and states deterministically. Fix or delete the backpressure test. Move network tests to a separately tagged, opt-in filter controlled by an env var.
- Effort: L
- Owner: design
- Status: closed in #276

### TRF-35: Mock token server bugs make local testing misleading
- Category: tests
- Severity: low
- Location: Open3DTransportWebRTC/Tests/mock-token-server.py:46-53, :70-79, :91
- Evidence: `datetime.datetime.utcnow()` followed by `.timestamp()` treats a naive UTC time as local time, so `exp`/`nbf` are off by the host's UTC offset. East of UTC, tokens with the default 1 h TTL arrive already expired. The Bearer `API_KEY` mode cannot be used with the plugin, because the C++ fetcher never sends `Authorization` (TRF-22). The JWT `iss` is `mock-token-server` and the secret defaults to `test-secret`, so a real LiveKit server rejects these tokens. The docs do not say this.
- Recommendation: Use `datetime.now(timezone.utc)`. Document that the mock server does not produce LiveKit-valid tokens, or accept `LIVEKIT_API_KEY`/`SECRET` via env and set `iss` accordingly.
- Effort: S
- Owner: coding
- Status: closed in #275

### TRF-36: Module docs are inaccurate or stale in several places
- Category: docs
- Severity: medium
- Location: Open3DTransportWebRTC/TOKEN_AUTO_FETCH_IMPLEMENTATION.md:30, :45, :366, :396, :525; Open3DTransportWebRTC/IMPLEMENTATION_SUMMARY.md:199; Open3DTransportWebRTC/Private/Sender/WebRTCSender.h:78; Open3DTransportMoQ/README.md (Threading Model; See Also link); Open3DTransportMoQ/MOQ_FFI_REVIEW_AND_WORK_PLAN.md:43; Open3DTransportMoQ/OPEN3DTRANSPORTMOQ_REVIEW.md:14; Open3DTransportWebRTC/OPEN3DTRANSPORTWEBRTC_ANALYSIS.md:5
- Evidence: The docs claim "Optional API key authentication" / `TokenApiKey`, which does not exist in code. They say "Callbacks execute on HTTP module's thread" (unverified). They say "Thread safety validated (no race conditions found)", which is contradicted by TRF-1, 9, 10 and 15. The backpressure thresholds are described as "configurable via console variables" but are constexpr. The MoQ README links to a missing `MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md` and says callbacks are "routed to queues" (they go through a dispatcher thread to the game thread). The work-plan doc still lists "Implement FO3DMoQReceiver" as outstanding. Self-graded review docs ("EXCELLENT (9/10)", "GOOD") ship inside `Source/`. The WebRTC auto-fetch flow in USER_GUIDE is documented as working, but it does not connect (TRF-3).
- Recommendation: Move historical review and plan docs out of the plugin (to `/docs/archive`). Per project rule §5, fix the code to match the intended auto-fetch and API-key design, or correct the docs with justification. Fix the broken link.
- Effort: M
- Owner: review

### TRF-37: MoQ receiver picks the audio codec from local config instead of the frame
- Category: bug
- Severity: low
- Location: Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:582-591
- Evidence: `O3DAudio::SelectCodec(ActiveAudioConfig)` on the receiver decides how to deserialize and decode. If the sender's codec (from its own config or defaults) differs, every audio frame fails with "failed to deserialize". The receiver `ConfigureTransport` does not set a codec (Open3DTransportMoQModule.cpp:673-686).
- Recommendation: Carry the codec in the serialized frame header (or in the track name) and decode according to it.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-38: WebRTC and MoQ receivers own their consumer differently
- Category: usability
- Severity: low
- Location: Open3DTransportMoQ/Private/Receiver/MoQReceiver.h:92-93; Open3DTransportMoQ/Private/Receiver/MoQReceiver.cpp:130-133, :552-556; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:527-535
- Evidence: MoQ keeps only a `TWeakPtr` to the consumer and silently counts frames as dropped if the caller does not also hold it. WebRTC holds a strong reference and falls back to `FSerializedFrameConsumerRegistry::Create()`. The two transports behave differently behind the same interface contract.
- Recommendation: Define the ownership in `IOpen3DReceiver::SetConsumer` docs and apply it in both transports, preferably with a shared base.
- Effort: S
- Owner: design
- Status: closed in #312

### TRF-39: Ineffective catch(...) around Rust FFI and misuse of thread-local moq_last_error
- Category: code-quality
- Severity: low
- Location: Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:116-151, :313-318, :419-427
- Evidence: A Rust panic that unwinds across `extern "C"` is UB or an abort, and it is not a C++ exception, so `catch (...)` gives false assurance. `moq_last_error()` is documented as per-thread (moq_ffi.h:311-314), but the connection-state callback thread reads it (l.419) for an error that another thread set.
- Recommendation: Remove the try/catch and rely on moq-ffi's `catch_unwind`. Put error details in `MoqResult.message` or in the callback payload.
- Effort: S
- Owner: coding
- Status: closed in #273

### TRF-40: WebRTC audio sink scratch buffer is not thread-safe; PCM conversion truncates
- Category: performance
- Severity: low
- Location: Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:174-186, :220
- Evidence: One `PcmConversionBuffer` per sink is reused without a lock. `IO3DSenderAudioSink::SubmitPcm` may be called from any thread, and one sink serves several `StreamLabel`s, so concurrent calls corrupt audio. `static_cast<int16>(Sample * 32767.0f)` truncates toward zero, which adds small bias and distortion. Other transports use the shared encoder path.
- Recommendation: Use a thread-local or per-label scratch buffer and round-to-nearest (for example via the shared O3DAudio PCM16 helpers), or lock around conversion and publish.
- Effort: S
- Owner: coding
- Status: closed in #269

---

## Incomplete or stubbed functionality
- WebRTC token refresh is fetched but never applied (`// TODO: Reconnect with new token`, WebRTCSender.cpp:1163, WebRTCReceiver.cpp:915). `lk_refresh_token` is unused.
- The WebRTC auto-fetch connect path does not work end to end (TRF-3).
- API-key authentication for the token endpoint is documented but not implemented (TRF-22, TRF-36).
- MoQ Linux/Mac: `Build.cs` and loader branches exist, but no binaries ship and the flag is force-disabled (TRF-27).
- The `O3D_WEBRTC_BACKEND_LIBDC` backend flag has no implementation in this module (TRF-33).
- `FMoQSessionWrapper::SubscribeAsync` is implemented but unused. MoQ `moq_connect` failures are not propagated as states (TRF-11).
