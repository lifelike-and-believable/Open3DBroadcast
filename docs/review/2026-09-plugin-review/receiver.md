# Open3DReceiver module review

Paths below are relative to `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DReceiver/` unless they start with `src/` or another module name.

**Summary (architecture and health)**
1. `FO3DReceiverSource` (Private/O3DReceiverSource.cpp, 1472 lines) is a LiveLink source that is also an `FTickableGameObject`. `Tick()` calls `ActiveReceiver->Poll()`, and every in-tree transport (Sockets TCP/UDP, NNG, MoQ, Loopback, WebRTC data) delivers mocap frames from inside `Poll()`, so mocap handling runs on the game thread.
2. Frame path: `FSerializedConsumer::SubmitFrame` → `HandleSerializedFrame`, which reads the header with `PeekMeta`. Frames with `tx_seq` go through `O3DS::ReorderGate` → `EmitGatedFrame` (clock-offset mapping, concealment). Frames without it go to `HandleLegacyFrame`, which drops by timestamp. Both paths parse into a single shared `O3DS::SubjectList SubjectScratch` and then call `ProcessParsedSubject` → `PushSubjectStaticData` / `PushSubjectFrameData`.
3. Audio path: transport → `FAudioSink::SubmitPcm16` (can run on any thread) → `AsyncTask(GameThread)` → global `FO3DAudioBus` delegate → `UO3DRemoteAudioComponent::OnAudioPcm16` → `USoundWaveProcedural::QueueAudio`.
4. Transport choice: two global registries. `O3DTransport::RegisterReceiver` holds the factories. `O3DReceiver::RegisterTransportCustomization` holds the config hooks and Slate panels. The factory panel (`SO3DReceiverSourceFactoryPanel`) edits a transient `UO3DReceiverSettingsObject` and serializes it into the LiveLink connection string.
5. Good: parsing is gated and verified, input is checked with `IsFinite`/NaN, static data is re-pushed on hierarchy or curve-set changes via a descriptor hash, and gate/concealment metrics are reported as deltas.
6. Biggest risks: (a) off-thread audio callbacks (WebRTC) reach into the source's `ActiveConfig` and can make an FFI thread the last owner of the source; (b) credentials (WebRTC token) are persisted to ini and connection strings; (c) the skeleton fast path caches bone names by parent topology only; (d) a stale test that fails against current code.
7. Hot path: several heap allocations and FName constructions per subject per frame, two `FString::Printf` metadata strings per pushed frame, and repeated copies of transform arrays. There is no timecode (`SceneTime` is always empty).
8. Audio component: no jitter buffer or queue bound; one `SoundWave` is shared by every matching stream and source; it can attach to itself when it is the root; the internal `UAudioComponent` is held by a raw, non-UPROPERTY pointer and is never cleaned up.
9. Architecture: the source class is a god object (transport lifecycle, parsing, reorder gate, clock mapping, concealment, LiveLink push, metrics, audio metadata), and its public header exposes third-party core headers. Build.cs is Win64-only, which blocks Fab multi-platform.
10. Tests: 3 automation tests cover only the audio filter, the SoundWave queue and FinalizeAudioMeta; the FinalizeAudioMeta test expects behavior the code no longer has. The LiveLink push path, hierarchy changes, gate/legacy ordering, concealment wiring and the factory have no tests.

Severity counts: critical 0, high 4, medium 20, low 10 (34 findings).

---

### RCV-1: Off-thread audio callback races with transport restart and can run the source destructor on an FFI thread
- Category: thread-safety
- Severity: high
- Location: Private/O3DReceiverSource.cpp:105-135, Private/O3DReceiverSource.cpp:1340-1386, Private/O3DReceiverSource.cpp:312-317, Private/O3DReceiverSource.cpp:236-239, Private/O3DReceiverSource.cpp:392-403; Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp:337,366,556-574
- Evidence: I re-checked this against the code. `RequestSourceShutdown` (line 261-266) and the destructor (line 236-239) both call `StopTransport()`, which calls `SetAudioSink(nullptr, …)`, `SetConsumer(nullptr)`, `Stop()` and `Reset()` in that order (lines 396-402). For mocap this is safe for the in-tree transports: they call `SubmitFrame` from `Poll()`, which runs on the game thread from `Tick()` (line 284). Stop/SetConsumer on the game thread is therefore serialized with delivery, and the "network thread vs SetConsumer(nullptr)" race does not happen for mocap today.

  The race that remains is on the audio path. WebRTC calls `OnAudioReceivedEx` on a LiveKit FFI thread. That function reads `Self->AudioSink` without `StateMutex` (`if (Self->AudioSink.IsValid())` … `Self->AudioSink->SubmitPcm16(...)`, WebRTCReceiver.cpp:337/366), while `Stop()` resets it under the lock (`AudioSink.Reset();` at :573). `Stop()` clears the callbacks and calls `lk_client_destroy`, but nothing in the repo shows that it waits for a callback already in flight.

  On that FFI thread, `FAudioSink::SubmitPcm16` does `Owner.Pin()` and calls `OwnerPinned->FinalizeAudioMeta(MetaCopy)` (line 112-115). `FinalizeAudioMeta` reads `ActiveConfig.Audio.bEnableAudio`, `ActiveConfig.StreamId` (FString), `ActiveConfig.Audio.SampleRate` and `SourceGuid` (lines 1342-1380). Meanwhile the game thread can be in `StartTransport()`, which does `ActiveConfig = BuildTransportConfig();` (line 317). That is an unsynchronized FString read/write.

  If LiveLink drops its last reference to the source while the FFI thread holds `OwnerPinned`, the FFI thread becomes the last owner. `~FO3DReceiverSource` → `StopTransport` → `FO3DWebRTCReceiver::Stop()` → `lk_client_destroy` then runs from inside a LiveKit callback, and the `FTickableGameObject` base is destroyed off the game thread.

  The same off-thread pattern exists in `HandleSerializedFrame` for any future transport that delivers mocap off-thread: `Client` is a plain pointer read at line 542 before the `IsInGameThread()` hop at line 551. Needs-UE-verification for whether destroying an `FTickableGameObject` off the game thread asserts. Needs FFI verification for whether `lk_client_set_audio_callback_ex(nullptr)` or `lk_client_destroy` quiesces callbacks already in flight.
- Recommendation: (1) Snapshot the immutable audio metadata the sink needs (SourceGuid, StreamId, default rate and channels) into the `FAudioSink` when it is constructed in `StartTransport`, and remove the back-reference to the owner. `FinalizeAudioMeta` then no longer touches source state. (2) If an owner reference is still required, never let a worker thread hold the last strong reference: hop to the game thread with a `TWeakPtr` before pinning. (3) In `FO3DWebRTCReceiver`, pin `AudioSink` under `StateMutex` (or hold a TWeakPtr and pin it) inside `OnAudioReceivedEx`, and make `Stop()` wait for in-flight callbacks. (4) Document on `IOpen3DReceiver` that after `Stop()` returns, no consumer or sink callback may be running or start.
- Effort: M
- Owner: coding
- Status: closed in #271

### RCV-2: FinalizeAudioMeta automation test asserts behavior the code no longer has
- Category: tests
- Severity: high
- Location: Private/Tests/O3DRemoteAudioComponentTests.cpp:133-173, Private/O3DReceiverSource.cpp:1340-1370, Private/O3DReceiverSource.cpp:1176
- Evidence: The test sets `LastObservedSubjectName = "Quinn"` and `Config.StreamId = "testanimchannel"` with `bEnableAudio = true`, then asserts `Meta.SubjectName == "Quinn"` ("Observed subject applied", line 151) and "Channel fallback replaced by subject" == "Quinn" (line 158). `FinalizeAudioMeta` never reads `LastObservedSubjectName`. With an empty `StreamLabel` it sets `StreamLabel = ActiveConfig.StreamId` (line 1348) and then `SubjectName = StreamLabel` (line 1355), which gives "testanimchannel". Both assertions fail against the current code. The only write to `LastObservedSubjectName` is line 1176, and nothing outside the test reads it.
- Recommendation: Decide the intended behavior, then either restore the "last observed subject" fallback or update the test to assert the StreamId/StreamLabel fallback. Remove `LastObservedSubjectName` if it stays unused. Run the test in CI so this kind of drift fails a build.
- Effort: S
- Owner: design
- Status: closed in #276

### RCV-3: Transport credentials are persisted in GameUserSettings.ini and in the LiveLink connection string
- Category: fab-readiness
- Severity: high
- Location: Private/O3DReceiverSourceFactory.cpp:189-199, Public/O3DReceiverSourceSettings.h:27-29,33-41; Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:509
- Evidence: The WebRTC receiver panel writes the access token into `Settings.TransportOptions` (`SetReceiverOption(SettingsObject, WebRTCConfig::TokenOptionKey, ...)`). `OnCreateClicked` then (a) `ExportText`s the whole `FO3DReceiverSourceConfig`, including `TransportOptions`, into `ConnectionString`, which LiveLink stores in presets and assets, and (b) does `MutableDefaults->Settings = Settings; MutableDefaults->SaveConfig();` into `Config = GameUserSettings` with `GlobalConfig`. Tokens end up in plain-text ini files and in assets that may be committed to source control. This violates the project rule "No hard-coded credentials … in source control" (copilot-instructions.md §2, §3 Prohibited).
- Recommendation: Mark secret keys as transient or secret in the customization contract (for example a `TSet<FString> SecretOptionKeys` on `FO3DReceiverTransportCustomization`). Strip those keys before `ExportText` and `SaveConfig`, and load them at runtime from an env var or a per-user credential store. At minimum, never persist `TransportOptions` values whose keys match token, secret or key.
- Effort: M
- Owner: design
- Status: closed in #275

### RCV-4: The skeleton fast path caches bone names by parent topology only, so renamed bones are never re-published
- Category: bug
- Severity: high
- Location: Private/O3DReceiverSource.cpp:1184-1203, Private/O3DReceiverSource.cpp:1257-1261, Private/O3DReceiverSource.cpp:1268-1273
- Evidence: `QuickSkeletonFingerprint = TransformCount; … = QuickSkeletonFingerprint * 31 + TransformPtr->mParentId;` covers only the count and parent ids. On a match, `BoneNames = ExistingCache->BoneNames;` reuses the cached names. `SkeletonHash` is then computed from these cached names (line 1268), so `bNeedStaticUpdate` stays false. If a sender republishes a subject with the same hierarchy but different bone names (a character or rig swap, a namespace change, or a reorder of siblings that keeps the parent layout), LiveLink keeps the old names and transforms are applied to the wrong bones. The fingerprint is also collision-prone (a linear multiply-add over ints).
- Recommendation: Key the cache on a descriptor-level signal instead of topology. One option is to invalidate when the parsed `SubjectList` contained a full `subjects` descriptor for this subject rather than only `updates`. Another is to hash `mName` together with `mParentId` (hash the std::string bytes directly, without constructing FNames). Rebuild names only when that hash changes.
- Effort: S
- Owner: coding
- Status: closed in #268

### RCV-5: One SubjectList scratch shared by all packets: absent subjects are re-pushed, stale state survives restart, and multi-sender channels break
- Category: bug
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:131, Private/O3DReceiverSource.cpp:645-649, Private/O3DReceiverSource.cpp:740-744, Private/O3DReceiverSource.cpp:392-413, Private/O3DReceiverSource.cpp:506-528, Public/O3DReceiverSource.h:163-167; src/o3ds/model.cpp:1025-1031
- Evidence: `SubjectList::Parse(..., clearInactive=true)` deletes all subjects only when the packet carries full `subjects` descriptors. Update-only packets modify existing items in place (model.cpp:1025-1047). The receiver then loops over every subject in `SubjectScratch`, not only the subjects in this packet, so each subject missing from a packet is re-pushed with its last pose and a fresh `WorldTime`. That keeps "inactive" subjects alive and defeats `RemoveInactiveSubjects`.

  `StopTransport()` clears the hash maps but never resets `SubjectScratch` or `SubjectTransformCaches`, so after a restart the first update packet can re-publish subjects from the previous session. With several senders on one channel (the USER_GUIDE says "Multiple subjects can share the same transport channel"), each sender's descriptor packet deletes the other senders' subjects from the scratch. The single `ReorderGate` also mixes their `tx_seq` spaces; the header comment on lines 163-167 acknowledges this.
- Recommendation: Track which subjects a packet actually touched (for example, have `Parse` return the set of touched names, or compare against the flatbuffer's `subjects()`/`updates()` names) and push only those. Reset `SubjectScratch = O3DS::SubjectList()` and `SubjectTransformCaches.Empty()` in `StopTransport`. For multi-sender support, keep one `SubjectList` and one `ReorderGate` per sender or stream id.
- Effort: M
- Owner: design
- Status: closed in #268

### RCV-6: LiveLink subjects are removed after a hard-coded 5 s of silence, which discards user subject settings
- Category: usability
- Severity: medium
- Location: Public/O3DReceiverSource.h:137-139, Private/O3DReceiverSource.cpp:506-528
- Evidence: `static constexpr double InactivityThresholdSeconds = 5.0;`. `RemoveInactiveSubjects()` calls `Client->RemoveSubject_AnyThread(SubjectKey)`. Any pause over 5 s (an actor stepping out, a sender restart, a network blip) deletes the subject. Any translators, preprocessors or interpolation the user configured on it in the LiveLink panel are then lost, and Blueprints and anim graphs bound to the subject briefly see it disappear. The value is not configurable.
- Recommendation: Add `InactiveSubjectTimeoutSeconds` to `UO3DReceiverSourceSettings` with 0 meaning never remove. Prefer marking the subject stale via source status over removing it. Keep `ULiveLinkSubjectSettings` when a subject is re-created.
- Effort: S
- Owner: design
- Status: closed in #395

### RCV-7: CreateSubject is called with a fresh ULiveLinkSubjectSettings on every static-data change
- Category: bug
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:1396-1409, Private/O3DReceiverSource.cpp:1279-1282
- Evidence: Each time `bNeedStaticUpdate` is set (first frame, a hierarchy change, or any curve-set change), `PushSubjectStaticData` runs `NewObject<ULiveLinkSubjectSettings>()` with no outer and then `Client->CreateSubject(Preset)`, even when the subject already exists, before `PushSubjectStaticData_AnyThread`. Needs-UE-verification: in UE 5.7 `FLiveLinkClient::CreateSubject` may reject an existing key (with a log warning, leaving the new settings object as garbage) or may replace the subject and reset the user's per-subject settings. Either way, a sender that changes curves often generates repeated warnings or loses settings.
- Recommendation: Call `CreateSubject` only when the subject is not yet in `InitializedSubjects`, and only for the first push. For hierarchy or curve changes, re-push static data alone via `PushSubjectStaticData_AnyThread`. Verify the 5.7 `CreateSubject` semantics against the UE source before changing this.
- Effort: S
- Owner: coding
- Status: closed in #268

### RCV-8: No timecode is ever set, and the sender time is carried as a per-frame string
- Category: bug
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:1440-1442
- Evidence: `BaseFrameData.MetaData.SceneTime = FQualifiedFrameTime();` is always empty, while `SubjectListTime` is formatted into `StringMetaData` with `FString::Printf(TEXT("%.6f"), ...)` on every frame. Subjects using LiveLink's Timecode evaluation mode (and Take Recorder or genlock workflows) get no usable timing. The FName/FString map insertions also cost allocations per frame (see RCV-11).
- Recommendation: Map `SubjectList.time` (or a timecode field in the schema, if one is added) to `FQualifiedFrameTime` using a configured frame rate on the source settings. Drop the per-frame string metadata, or only emit it when a debug cvar is on.
- Effort: M
- Owner: design

### RCV-9: How the mapped presentation time and synthetic frames interact with LiveLink's WorldTime is unverified
- Category: bug
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:1439, Private/O3DReceiverSource.cpp:710-714, Private/O3DReceiverSource.cpp:903
- Evidence: `BaseFrameData.WorldTime = (WorldTimeSecondsOverride >= 0.0) ? WorldTimeSecondsOverride : FPlatformTime::Seconds();` builds `FLiveLinkWorldTime` from a double. Needs-UE-verification: if the UE 5.7 `FLiveLinkWorldTime(double)` constructor computes `Offset = FPlatformTime::Seconds() - InTime`, then `GetOffsettedTime()` equals the arrival instant, and the whole A2.c mapping has no effect on evaluation. Concealment frames are pushed with `WorldTime = Predicted.t` (line 903) and are interleaved with real frames. If LiveLink's buffer assumes monotonically increasing times, the order of predicted and real frames matters.
- Recommendation: Check the UE 5.7 `FLiveLinkWorldTime` and `FLiveLinkSubject` frame-insertion semantics, and write down the intended clock contract in the header. If the offset has to be controlled, use the two-argument constructor `FLiveLinkWorldTime(Time, Offset)`. Add an automation test that pushes frames with out-of-order WorldTime and checks the evaluated result.
- Effort: M
- Owner: review

### RCV-10: Render-ahead pushes an identical future frame on every tick
- Category: bug
- Severity: low
- Location: Private/O3DReceiverSource.cpp:876-903
- Evidence: When `TryConceal` returns false, `TryRenderAhead(Predicted)` is called on every tick. It predicts `lastReal + renderAhead`, which is the same target until the next real frame arrives, and each result is pushed with a new `FrameId` and the same future `WorldTime`. The next real frame then carries an earlier WorldTime than frames already in the buffer. The feature is opt-in (`RenderAheadMs = 0` by default).
- Recommendation: Push a render-ahead frame only once per new real frame (track the last `Predicted.t` pushed per subject), and confirm LiveLink's buffer ordering (see RCV-9).
- Effort: S
- Owner: coding
- Status: closed in #268

### RCV-11: Several heap allocations per subject per frame on the decode and push path
- Category: performance
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:593-594, 635-639, 732-736, 1202-1203, 1435, 1441-1442, 156-181, 890-891, 903, 1336
- Evidence: Per frame: `Frame.bytes.assign(...)` copies the whole payload into a std::vector (a second copy after the transport's own `PayloadCopy`). The locals `TArray<FName> BoneNames … TArray<float> CurveValues` are declared inside `HandleLegacyFrame` and `EmitGatedFrame`, so capacity is not reused across frames. `BoneNames = ExistingCache->BoneNames; BoneParents = ExistingCache->BoneParents;` copies both arrays on the "fast" path. `FrameData.Transforms = BoneTransforms;` copies instead of moving. Two `FString::Printf` results are added to a `TMap<FName,FString>`. `BuildPoseSampleFromLiveLink` makes 4 std::vector allocations. `TickConcealment` allocates new arrays and `TArray<FName>()` per subject per tick. `SubjectLastUpdateTime.Add` does a map write. At 120 Hz × N subjects this becomes significant GC-free heap churn on the game thread.
- Recommendation: Make the scratch arrays members and `Reset()` them. Pass cached names by const reference instead of copying. `MoveTemp` into `FrameData.Transforms`. Drop the string metadata (RCV-8). Reuse a `PoseSample` scratch per subject. Store the payload as `TArray<uint8>` moved into the gate, or let `Frame` own the transport buffer.
- Effort: M
- Owner: coding
- Status: closed in #368

### RCV-12: FName construction and name hashing run every frame for the subject and all curves
- Category: performance
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:1153, 1173-1174, 1268-1269
- Evidence: `BuildSubjectCurves` runs `FName(UTF8_TO_TCHAR(CurveNameUtf8.c_str()))` for every curve on every frame, including the fast path (line 1245). `ProcessParsedSubject` builds an `FString` and an `FName` for the subject name every frame. `HashArray(BoneNames, &BoneParents)` and `HashCurveNames(CurveNames)` rehash every name every frame. FName construction goes through the global name table (hashing plus locking), and a face rig can have 50-250 curves.
- Recommendation: Cache curve FNames per subject, keyed on a cheap hash of the raw `mCurveNames` strings, and rebuild only when that hash changes. Cache the subject FName keyed on the `Subject*` or `mName`. Compute the skeleton and curve hashes only when the cache is rebuilt.
- Effort: S
- Owner: coding
- Status: closed in #325

### RCV-13: Transform extraction code is duplicated and truncates doubles to float
- Category: code-quality
- Severity: low
- Location: Private/O3DReceiverSource.cpp:1076-1117, Private/O3DReceiverSource.cpp:1208-1240
- Evidence: The finite, NaN and zero-quaternion validation plus `FQuat`/`FVector` construction is copied between `BuildSubjectPose` and the fast path. Both do `static_cast<float>(Translation.v[0])` even though UE5 `FVector`/`FQuat` are double (LWC), so precision is lost for no reason. The fast path returns silently on the first invalid transform, which drops the frame with no metric.
- Recommendation: Extract a single `bool TryConvertTransform(const O3DS::Transform&, FTransform&)`, keep double precision, and call `RecordDeserializationError` (or a dedicated "invalid pose" counter) when a frame is dropped.
- Effort: S
- Owner: coding
- Status: closed in #329

### RCV-14: Skipping null transforms misaligns LiveLink parent indices
- Category: bug
- Severity: low
- Location: Private/O3DReceiverSource.cpp:1076-1081, 1127-1129, 1208-1211
- Evidence: `if (!TransformPtr) { continue; }` drops the bone, but the `mParentId` values of later bones are still indices into the original `mTransforms` list, so after a skip, parents point at the wrong LiveLink bone. The fingerprint (lines 1187-1195) still counts the null entry in `TransformCount`.
- Recommendation: Treat a null transform as a malformed frame and reject it, or insert an identity placeholder so indices stay aligned.
- Effort: S
- Owner: coding
- Status: closed in #268

### RCV-15: Concealment settings are captured once per subject, so edits in the LiveLink Settings panel do nothing
- Category: usability
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:799-817, Public/O3DReceiverSourceSettings.h:47-76
- Evidence: `GetOrCreateSubjectConcealment` copies `StarvationThresholdMs`, `MaxHorizonMs`, `CorrectionWindowMs` and `RenderAheadMs` into `O3DS::ConcealmentConfig` when the engine is created. Existing engines never see later edits; only a subject timeout, transport restart or reset re-creates them. The UPROPERTY comments present these as live tuning knobs. `bEnableConcealment` is re-read every tick, but turning it off leaves the engines and their state allocated.
- Recommendation: Track a settings revision (compare against cached values on each Tick, or override `PostEditChangeProperty` on `UO3DReceiverSourceSettings` to bump a counter) and rebuild or reconfigure engines when it changes. Empty `SubjectConcealment` when concealment is disabled.
- Effort: S
- Owner: coding
- Status: closed in #390

### RCV-16: Source status does not reflect data flow or error causes
- Category: usability
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:250-258, 386, 565-569, 1013, Public/O3DReceiverSource.h:39
- Evidence: A failed start sets `SourceStatus = LOCTEXT("StatusError", "Inactive")`, which gives no reason; the real reason goes only to the log (lines 320, 328, 334, 359). Once started, the status says "Receiving via X" whether or not any data arrives, and it never goes back to "No data" or "Waiting". `ConnectionLastActive` is updated only on the legacy path (line 1013), never on the gated path. `IsSourceStillValid()` returns true even when the transport failed to start, so LiveLink shows a healthy (yellow or green) source.
- Recommendation: Keep an enum state (Starting, Waiting for data, Receiving, Stalled, Error: <reason>). Update the last-activity time on both paths and derive a "Stalled" state in Tick. Put the failure reason in `SourceStatus`. Consider returning false from `IsSourceStillValid` after an unrecoverable start failure.
- Effort: S
- Owner: coding
- Status: closed in #390

### RCV-17: The factory panel and CreateSource do no validation and fall back silently
- Category: usability
- Severity: medium
- Location: Private/O3DReceiverSourceFactory.cpp:182-202, 548-559; Private/O3DReceiverSource.cpp:219, 416-433; Public/O3DReceiverSourceSettings.h:17
- Evidence: `OnCreateClicked` creates the source regardless of whether a transport is selected or registered, or whether required options such as a URL are filled in; no inline error is shown. `CreateSource` ignores `ImportText`'s return value, so a corrupt or old connection string silently falls back to defaults. `EnsureValidTransportName` falls back to the hard-coded `"loopback"` when nothing is registered, which also matches the struct default `TransportName = TEXT("loopback")`. The user then gets a source that can never receive anything, with no explanation beyond the log.
- Recommendation: Disable the Create button (`IsEnabled`) until the selected transport is registered in both registries, and let customizations expose `bool Validate(const FO3DReceiverSourceConfig&, FText& OutError)`. Log a warning and set an error status when `ImportText` fails. Don't default to a transport that is not registered.
- Effort: M
- Owner: design
- Status: closed in #390

### RCV-18: Creating a source silently overwrites the global defaults, and those settings are not actually in Project Settings
- Category: usability
- Severity: low
- Location: Private/O3DReceiverSourceFactory.cpp:194-196, Public/O3DReceiverSourceSettings.h:32-41
- Evidence: Each Create click does `MutableDefaults->Settings = Settings; MutableDefaults->SaveConfig();`, which changes defaults for every future source. The header says the class "exposes default receiver settings via the Project Settings UI", but it is `Config = GameUserSettings`, and no `ISettingsModule::RegisterSettings` call exists in the module (Open3DReceiverModule.cpp:12-20).
- Recommendation: Either register it as a real Developer Settings entry (`UDeveloperSettings`, `Config=EditorPerProjectUserSettings`) or remember the last-used values explicitly (a "Remember as default" checkbox). Fix the comment.
- Effort: S
- Owner: design
- Status: closed in #385

### RCV-19: Audio frames make two copies and a game-thread hop per packet, so playback depends on game frame rate
- Category: performance
- Severity: medium
- Location: Private/O3DReceiverSource.cpp:118-133; Open3DShared/Private/O3DAudioBus.cpp (PublishPcm16)
- Evidence: `FAudioSink::SubmitPcm16` copies the payload (`Payload.Append(Data, NumBytes)`), then `AsyncTask(ENamedThreads::GameThread, ...)`. `PublishPcm16` copies again (`Copy.Append`) before `Broadcast`. Every 10-20 ms audio packet becomes a game-thread task. When the game thread hitches, or the editor is throttled in the background, audio arrives in bursts, and bursts feed the underrun/overrun problems in RCV-20.
- Recommendation: Publish directly into a thread-safe per-stream ring buffer (or call `USoundWaveProcedural::QueueAudio` from the producer thread; needs-UE-verification that it is thread-safe in 5.7) and keep the game thread only for the routing and subscription changes. Pass a single shared buffer (`TSharedRef<TArray<uint8>>`) instead of copying.
- Effort: M
- Owner: design

### RCV-20: The remote audio queue has no jitter buffer, no bound and no drift handling
- Category: bug
- Severity: medium
- Location: Private/O3DRemoteAudioComponent.cpp:238-278
- Evidence: `SoundWave->QueueAudio(...)` is called for every packet with no pre-roll, no maximum queue depth and no sample-rate drift correction. If the sender clock runs slightly fast, or after a hitch burst (RCV-19), queued audio and therefore latency grow without bound. If it runs slow, the wave starves and you hear dropouts. `GetAvailableAudioByteCount()` is never checked.
- Recommendation: Add a target latency (for example 60 ms) with pre-roll before `Play()`. Trim when queued bytes exceed the target plus a margin, and optionally insert or drop samples for drift. Expose the target latency as a UPROPERTY.
- Effort: M
- Owner: coding
- Status: closed in #396

### RCV-21: Audio routing mixes streams and sources into one SoundWave and relies on magic labels
- Category: bug
- Severity: medium
- Location: Private/O3DRemoteAudioComponent.cpp:106-140, 142-188; Private/O3DReceiverSource.cpp:1346-1356
- Evidence: Mix mode accepts any label that `StartsWith(TEXT("o3ds:mix"))`, and Subject mode matches only on the name. `Meta.SourceGuid` is set (line 1342) but never checked. Two sources, or two mix streams, are interleaved into the same `USoundWaveProcedural`, which garbles the audio. If they differ in sample rate or channel count, `EnsureSoundWave` creates a new `SoundWave` on every alternating packet (line 144-147). `FinalizeAudioMeta` sets `StreamLabel` to `ActiveConfig.StreamId` (for example a URI) when the transport leaves it empty, so Mix mode, the default, never matches for those transports. WebRTC sets `StreamLabel` to the track or subject name (WebRTCReceiver.cpp:361), so a default-configured component plays nothing for WebRTC subject audio.
- Recommendation: Add an optional source or stream filter (StreamLabel or SourceGuid) to the component. Lock the component to the first matching stream until it goes idle. Replace the `"o3ds:mix"` magic string with a shared constant in Open3DShared, and document the labelling contract (RCV-32).
- Effort: M
- Owner: design
- Status: closed in #398

### RCV-22: The remote audio component tries to attach to itself when it is the root component
- Category: bug
- Severity: medium
- Location: Private/O3DRemoteAudioComponent.cpp:35-54
- Evidence: When `AC_AttachParent` is unset, `ParentToAttach = Owner->GetRootComponent()`. If the component is the root (for example it was added as the first component of an empty actor), `ParentToAttach == this` while `GetAttachParent() == nullptr`, so `AttachToComponent(this, ...)` is called. It also runs in `OnRegister`, including in the editor, where it re-parents a component the user placed elsewhere in the hierarchy.
- Recommendation: Guard with `ParentToAttach != this` and skip auto-attach when the component already has an attach parent that the user chose. Only apply `AC_AttachParent` when it is explicitly set.
- Effort: S
- Owner: coding
- Status: closed in #393

### RCV-23: The internal UAudioComponent is held by a raw pointer, never cleaned up, and ignores AutoActivate
- Category: code-quality
- Severity: medium
- Location: Public/O3DRemoteAudioComponent.h:124,131; Private/O3DRemoteAudioComponent.cpp:60-74, 95-104, 170-176, 268-275
- Evidence: `UAudioComponent* AudioComp = nullptr;` has no UPROPERTY or TObjectPtr, which goes against UE GC conventions (it happens to be kept alive by the owner actor). `bOwnsAudioComponent` is set but never read. `EndPlay` removes the bus delegate but does not `Stop()` or `DestroyComponent()` the created component, and does not clear `SoundWave`. `AudioComp->bAutoActivate = bAC_AutoActivate;` is set after `RegisterComponent()`, and `OnAudioPcm16` calls `Play()` unconditionally (line 270-273), so `bAC_AutoActivate = false` has no effect.
- Recommendation: Use `UPROPERTY(Transient) TObjectPtr<UAudioComponent> AudioComp`. Set properties before `RegisterComponent`. Only auto-play when `bAC_AutoActivate` is true, and expose Play/Stop as BlueprintCallable functions. In `EndPlay`, stop and destroy the owned component and reset `SoundWave`.
- Effort: S
- Owner: coding
- Status: closed in #393

### RCV-24: OnAudioFrame is dead code with a double-gain bug
- Category: code-quality
- Severity: low
- Location: Private/O3DRemoteAudioComponent.cpp:190-236, Public/O3DRemoteAudioComponent.h:116
- Evidence: `OnAudioFrame` is never bound; only `OnAudioPcm16` is subscribed (line 76). It multiplies samples by `Gain` (line 216), and `EnsureSoundWave` also applies `Gain` to the volume multiplier (line 177), so gain would be applied twice.
- Recommendation: Delete `OnAudioFrame`, or fix the gain handling and add a test.
- Effort: S
- Owner: coding
- Status: closed in #393

### RCV-25: Function-static log-once flags are shared across all component instances
- Category: code-quality
- Severity: low
- Location: Private/O3DRemoteAudioComponent.cpp:78-87, 196-197, 225-226, 261-266
- Evidence: `static bool bOnce`, `static bool bFirstFrame`, `static int32 DropEvery`, `static int32 LogEvery`. Only the first component in the process ever logs subscription or first-frame events, which misleads multi-actor and PIE debugging. These statics are also written from the game thread only by convention.
- Recommendation: Make them member variables, or use `UE_LOG` with the `LogO3DReceiverAudio` verbosity instead of manual throttling.
- Effort: S
- Owner: coding
- Status: closed in #333

### RCV-26: Hot-path logging and a debug cvar that is on by default
- Category: code-quality
- Severity: low
- Location: Private/O3DReceiverSource.cpp:34-38, 1286-1291, 1326-1331
- Evidence: `o3ds.Receiver.DebugParse` defaults to `1`. The per-frame `Warning` logs "PushSubjectFrameData took %.2f ms (... PRIMARY SUSPECT for latency)" fire on every slow push with no throttle, so a sustained stall floods the log (and each log line makes the stall worse). This goes against copilot-instructions §2 "Quiet hot paths".
- Recommendation: Default DebugParse to 0. Throttle the slow-push warnings (once per N seconds per subject) or move them to Verbose and record them as metrics instead. Remove the diagnostic "PRIMARY SUSPECT" wording.
- Effort: S
- Owner: coding
- Status: closed in #333

### RCV-27: FindTransportCustomization returns a pointer into a TMap after releasing its lock
- Category: thread-safety
- Severity: medium
- Location: Private/O3DReceiverTransportCustomization.cpp:28-32, Private/O3DReceiverSource.cpp:480-485, Private/O3DReceiverSourceFactory.cpp:263-273
- Evidence: `FScopeLock Lock(&GReceiverCustomizationMutex); return GReceiverCustomizations.Find(TransportName);` returns a pointer that goes stale as soon as another `Register` or `Unregister` rehashes or removes the entry, for example on module load or unload, hot reload, or a plugin being enabled while the panel is open. The caller then invokes `TFunction`s through it. The lambdas live in transport modules, so calling them after that module has unloaded would run unloaded code. The mutex implies these functions may be called from multiple threads, which the API does not honor.
- Recommendation: Return the customization by value (`TOptional<FO3DReceiverTransportCustomization>`) or as a `TSharedPtr<const ...>` held in the map, so callers keep it alive. Document that registration happens in StartupModule/ShutdownModule on the game thread only.
- Effort: S
- Owner: coding
- Status: closed in #289

### RCV-28: Two parallel registries decide what can be selected versus what can run
- Category: architecture
- Severity: medium
- Location: Private/O3DReceiverRegistry.cpp:34-101, Private/O3DReceiverTransportCustomization.cpp:16-44, Private/O3DReceiverSource.cpp:423-424, 324-330, Private/O3DReceiverSourceFactory.cpp:391-392
- Evidence: The combo box and `EnsureValidTransportName` list names from `O3DReceiver::GetRegisteredTransportNames` (the customizations). `StartTransport` instantiates from `O3DTransport::CreateReceiver` (the factories). A transport with a factory but no customization cannot be selected. A transport with a customization but no factory can be selected, and it fails at runtime with only a log warning. `O3DTransport::GetRegisteredReceivers()` is never called. Neither registry notifies about changes, so a transport that unloads while sources are live leaves `ActiveReceiver` pointing into an unloaded module; this ordering is up to each transport's ShutdownModule.
- Recommendation: Merge them into one `FO3DReceiverTransportDescriptor { Factory; ConfigureTransport; BuildWidget; DisplayName; }` registry, list only entries that have a factory, and broadcast an `OnTransportsChanged` event so the panel can refresh and live sources can stop before a module unloads.
- Effort: M
- Owner: design
- Status: closed in #289

### RCV-29: FO3DReceiverSource is a god object, and its public header exposes core third-party headers
- Category: architecture
- Severity: medium
- Location: Public/O3DReceiverSource.h:14-19, 26, 55-186; Private/O3DReceiverSource.cpp (whole file)
- Evidence: One class holds transport lifecycle, the legacy ordering heuristics, `ReorderGate` and `ClockOffsetEstimator` wiring, concealment engines, FlatBuffer parsing, pose and curve conversion, LiveLink static and frame push, subject GC, metrics delta reporting and audio metadata. The public (`OPEN3DRECEIVER_API`) header includes `o3ds/model.h`, `reorder_gate.h`, `clock_offset.h` and `predict/concealment.h`, which pushes the core include paths (`PublicIncludePaths`, Build.cs:17-22) onto every dependent module. Very little of this can be tested in isolation; see RCV-31.
- Recommendation: Split it into (a) `FO3DFrameDecoder` (parse, then build pose and curves, with caches), (b) `FO3DFrameScheduler` (gate, clock mapping, concealment), (c) `FO3DLiveLinkPublisher` (static and frame push, subject lifetime), and (d) a thin `FO3DReceiverSource` that composes them. Move the core includes to Private using a pimpl, and make the Build.cs include paths private.
- Effort: L
- Owner: design
- Status: closed in #328

### RCV-30: Build.cs rejects every platform except Win64
- Category: fab-readiness
- Severity: medium
- Location: Open3DReceiver.Build.cs:24-32, 78-89
- Evidence: `else { throw new BuildException($"Open3DReceiver does not define third-party binaries for platform {Target.Platform} yet."); }`. A Mac or Linux editor build of any project with the plugin enabled fails outright instead of the module being excluded. Fab listings are expected to declare supported platforms and build cleanly on them. `"AudioMixer"` is listed as a dependency but no AudioMixer API is used (only `USoundWaveProcedural` from Engine).
- Recommendation: Add Mac and Linux binaries, or restrict the module via `PlatformAllowList` in the .uplugin so unsupported platforms skip it instead of throwing. Remove unused dependencies.
- Effort: M
- Owner: design
- Status: closed in #283

### RCV-31: The core receiver path has no test coverage
- Category: tests
- Severity: medium
- Location: Private/Tests/O3DRemoteAudioComponentTests.cpp:64-173
- Evidence: The only tests are the audio filter, the SoundWave queue and FinalizeAudioMeta (which is broken, see RCV-2). Nothing tests: `ProcessParsedSubject` (fast path vs rebuild, hierarchy change → static re-push, bone rename, RCV-4), curve-set changes, gated vs legacy dispatch, `ShouldProcessFrame` duplicate and out-of-order handling, `RemoveInactiveSubjects`, concealment wiring (`TickConcealment`), the `BuildTransportConfig` customization hook, or the `CreateSource` connection-string round trip. Test paths are inconsistent: `Open3DBroadcast.O3DReceiver.RemoteAudioComponent.AudioQueue` (line 85) vs `Open3DBroadcast.Open3DReceiver...`.
- Recommendation: Add a mock `ILiveLinkClient` (or use the real LiveLink client in an editor test) and feed serialized `SubjectList` buffers built with `O3DS::SubjectList::Serialize`/`SerializeUpdate` through `FSerializedConsumer`. Assert the pushed static and frame data. Standardize the test path prefix.
- Effort: M
- Owner: coding

### RCV-32: Docs are missing or inconsistent for receiver cvars, UI names and audio labelling
- Category: docs
- Severity: low
- Location: Private/O3DReceiverSource.cpp:34-62, Private/O3DReceiverSourceFactory.cpp:528-531; USER_GUIDE.md:83,282; README.md:102,144
- Evidence: `o3ds.Receiver.DebugParse`, `DropOutOfOrder`, `SilenceResetSeconds`, `TimestampJumpResetSeconds`, `Audio.Debug` and `o3ds.RemoteAudio.Debug` are not documented in any .md file. USER_GUIDE says to select "Open3D Receiver Source", but the factory's display name is "Open3DStream Receiver". README calls the component `O3DSRemoteAudioComponent`, but the class is `UO3DRemoteAudioComponent`. The Mix vs Subject label convention (`o3ds:mix` prefix, StreamId fallback) is not documented anywhere.
- Recommendation: Add a "Receiver reference" section covering cvars, source settings (concealment, inactivity), status meanings and the audio routing rules, and align the names with the code.
- Effort: S
- Owner: coding
- Status: closed in #424

### RCV-33: The "round-trip latency" metric measures only local queueing
- Category: code-quality
- Severity: low
- Location: Private/O3DReceiverSource.cpp:628-633
- Evidence: `LatencyMs = (FPlatformTime::Seconds() - TimestampSeconds) * 1000.0` is labelled "round-trip latency". Every in-tree transport passes `FPlatformTime::Seconds()` at receive time as `TimestampSeconds` (for example SocketsUdpReceiver.cpp:416), so this measures only the time between receive and apply. It is recorded only on the legacy path; the gated path has the real `excess_delay_us`.
- Recommendation: Rename the metric to "receive-to-apply latency", record it on both paths, and document the metric semantics in O3DPerformanceMetrics.
- Effort: S
- Owner: coding
- Status: closed in #333

### RCV-34: A legacy-path silence reset wipes the gate, clock estimator and concealment state
- Category: bug
- Severity: low
- Location: Private/O3DReceiverSource.cpp:983-990, 1449-1469
- Evidence: `ShouldProcessFrame` → `ResetOrderingState()` resets `ReceiverGate`, `ClockEstimator`, `SubjectConcealment` and `bHasClockOffsetEstimate`. The code comment (lines 1452-1458) acknowledges that a stream mixing legacy and `tx_seq` frames would reset gated state on a legacy timestamp jump. Separately, `StopTransport` does not clear `SubjectTransformCaches` (lines 406-409), unlike the other per-subject maps.
- Recommendation: Split this into `ResetLegacyOrdering()` and `ResetGatedSession()`, calling only the relevant one from each path, and clear `SubjectTransformCaches` in `StopTransport`.
- Effort: S
- Owner: coding
- Status: closed in #268
