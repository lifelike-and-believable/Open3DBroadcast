# Open3DSender module review

Paths below are relative to `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DSender/` unless they start with `src/` (core library) or `Open3DShared/`/`Open3DTransport*` (sibling modules).

**Architecture and health summary**
1. `UO3DSenderComponent` (1235 lines of .cpp) is the hub. From `TickComponent`, it captures the pose (component-space to local bone transforms, curves through `FO3DSenderCurveProcessor`), then broadcasts `OnDescriptorReady` / `OnPoseFrameReady` to its own `FO3DSenderSerializer`.
2. `FO3DSenderSerializer` (public, exported) turns frames into O3DS FlatBuffers. It uses one of three paths: legacy full snapshot, C2 residual, or D1 quantized. It then fires `OnSerializedFrame`, which the component forwards to `IOpen3DSender::SendSerialized` through the private `FO3DSenderTransportController`.
3. Transports come from two parallel global registries: `O3DTransport::RegisterSender` (factories) and `O3DSender::RegisterTransportCustomization` (config and editor widgets). An editor-only `IDetailCustomization` in the runtime module builds the details panel.
4. Audio: the component creates or finds a `UO3DSenderAudioCaptureComponent`. That component taps a submix (audio render thread) or opens a microphone (capture thread), then resamples or mixes and pushes PCM into a transport-provided `IO3DSenderAudioSink`.
5. Everything from sampling to encoding to the transport call runs synchronously on the game thread every tick. Nothing is offloaded to a worker, which goes against the project rule that encoding runs async.
6. Correctness is the weakest area. After any Stop/StartCapture cycle (including every edit made in the details panel during PIE) or a subject rename, the skeleton descriptor is never re-sent. The stream then carries unnamed bones that all have parent 0.
7. The D1 quantization and curve filtering paths interact badly with full resyncs (anchor reset, index misalignment). The epsilon filter never sends a curve's return to zero.
8. The audio path shares UObject state (`Config`, `CaptureMode`, `WorkingBuffer`) between the game thread and audio threads without synchronization. The submix listener can be unregistered from the wrong submix.
9. The Blueprint and editor experience is only partly finished. Residual and quantization settings sit outside the custom layout. Transport options (including the WebRTC access token) are saved to disk. Delegates cannot be used from Blueprint, and the default setup sends nowhere without any warning.
10. Tests cover only two pure helper functions. Nothing tests the serializer, the curve processor, the audio path, the registries or the capture lifecycle. The build is hard-wired to Win64 only.

---

### SND-1: Skeleton descriptor is never re-sent after StopCapture/StartCapture or subject rename, so bones go out with empty names and parent 0
- Category: bug
- Severity: critical
- Location: Private/O3DSenderComponent.cpp:237-248, :823-842, :859-893, :788-791, :1224-1230; Private/O3DSenderSerializer.cpp:74-84, :123-134, :218-227, :237-246
- Evidence:
  - `StopCapture()` calls `Serializer->Detach(this)` and `ClearAllCaches()`, which empty `SubjectState`. It does not reset `CachedSkeletalMesh`, `CachedSkeleton` or `DescriptorCache.Hash`.
  - On the next `StartCapture()`, `BindToTarget()` then `EnsureSkeletonCache()` sees the same mesh and skips `RefreshSkeletonCache()`. Even if it were called, `bDescriptorDirty` is only set when the hash changes (:877), so `OnDescriptorReady` is not broadcast.
  - `OnPoseFrameReady` then builds `DescriptorSnapshot` from an empty cache. `SetNum(RequiredCount)` (:224-225) fills `ParentIndices` with 0 and `BoneNames` with `NAME_None`. `BuildSubjectFromDescriptor` emits every bone named "" with parent 0, including bone 0, which ends up as its own parent.
  - The same thing happens when `SubjectName` changes at runtime (it is BlueprintReadWrite). `EnsureSubjectNameCached` purges the old subject's cache (:790), but no descriptor is ever produced under the new name.
  - `PostEditChangeProperty` goes through Stop and Start for 9 properties (:1224-1230), so any details edit during PIE breaks the stream.
- Recommendation: Make descriptor delivery pull-based rather than edge-triggered:
  - When the serializer has no cache (`BoneNames.Num()==0`), or the hash or subject is unknown, it should ask the component for `DescriptorCache`. Alternatively, have `StartCapture()` and `EnsureSubjectNameCached()` always re-broadcast `OnDescriptorReady(CachedSubjectName, DescriptorCache)`.
  - Also reset `CachedSkeletalMesh` in `StopCapture()`.
  - In `OnPoseFrameReady`, drop frames (with a rate-limited warning) when the cached descriptor count does not match the bone count, instead of padding with 0 or None.
  - Add an automation test that runs Stop and Start and asserts that the bone names and parents in the serialized output are correct.
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-2: A D1 quantized full resync re-anchors translations on the sender while the receiver keeps the old anchors, corrupting every later quantized delta
- Category: bug
- Severity: high
- Location: Private/O3DSenderSerializer.cpp:508-551 (esp. :522 and the comment at :540-549), :231-235; src/o3ds/model.cpp:356-360, :1108-1195
- Evidence:
  - Every quantized full sync calls `BuildSubjectFromDescriptor`, which runs `OutSubject.clear()` (:235). That deletes every `O3DS::Transform` and creates new ones with `mQuantAnchorSet=false`. `Subject::Serialize()` then anchors to the current (animated) pose (model.cpp:356-360).
  - The receiver's `ParseSubject` deliberately keeps the old anchors by name across a resync (model.cpp:1125-1130, :1185-1190). Sender and receiver anchors therefore disagree after the first resync, and every Byte or Half tier translation delta decodes to the wrong position.
  - Resyncs happen with unchanged topology whenever `bCurveCountChanged` is true (:508-510, which happens every frame count changes under curve filtering; see SND-3) and whenever `bDescriptorSent` is reset.
  - The comment at :540-549 says an unchanged-topology resync "would correctly preserve the existing one", which is false because of the `clear()`.
- Recommendation: In the quantized path (and ideally the residual path), keep existing `Transform` objects when the names and parents are unchanged: update the values in place and only rebuild when the descriptor hash changes. Alternatively, copy `mQuantAnchorTranslation`/`mQuantAnchorSet` by name before `clear()`, mirroring model.cpp:1125. Add a round-trip test: sender full sync, delta, resync, delta, receiver parse, assert translations within tolerance.
- Effort: M
- Owner: coding
- Status: closed in #267

### SND-3: With curve filtering on, the curve set changes every frame, which misaligns curves in residual/quantized mode and triggers constant resyncs
- Category: bug
- Severity: high
- Location: Private/O3DSenderCurveProcessor.cpp:145-180; Private/O3DSenderSerializer.cpp:390-392, :454-462, :508-510, :557-561
- Evidence:
  - `BuildFilteredCurves` removes curves per frame by epsilon and delta (`continue` at :166 and :178), so `Frame.CurveNames` and `Frame.CurveValues` change membership from frame to frame.
  - The residual and quantized steady-state branches write `SubjectObject->mCurveValues[Index] = Frame.CurveValues[Index]` purely by index, against `mCurveNames` captured at the last full sync. Only a count change is detected (:390-391, :508-509).
  - When the count stays the same but membership differs (curve A drops out and B comes in), values go to the wrong curve names on the wire. When the count differs, a full sync is forced, which in quantized mode triggers SND-2 and on every path costs a full topology packet.
  - The legacy path is unaffected because it re-sends names every frame.
- Recommendation: Decouple "which curves exist" from "which values changed". Keep a stable curve list per subject (from the curve cache revision) and let the core `SerializeUpdate`/`SerializeCurveUpdates` delta logic do the change suppression. Alternatively, disable epsilon/delta curve filtering whenever residual or quantized coding is on and document that. Also compare curve name identity (hash), not just count, before the steady-state write.
- Effort: M
- Owner: design
- Status: closed in #267

### SND-4: The curve epsilon filter never sends a curve's return to zero, so receivers can hold stale non-zero values
- Category: bug
- Severity: high
- Location: Private/O3DSenderCurveProcessor.cpp:159-166, :168-180, :186-190
- Evidence: `if (FMath::Abs(Value) < Config.CurveEpsilon) { continue; }` runs before the delta-vs-last-sent check and does not update `LastSentCurveValues`. A blend shape going from 0.8 to 0.0 is never sent at 0; its last transmitted value stays 0.8. Whether the receiver then holds 0.8 or treats a missing curve as 0 depends on the receiver and transport path. In residual/quantized mode it also changes the curve count (see SND-3).
- Recommendation: Apply epsilon only to suppress curves that were already at about 0 when last sent: `if (|Value|<eps && (!bHasLast || |Last|<eps)) continue;`. Otherwise send the zero once and record it as last-sent. Add a unit test for a 0.8 to 0 to 0 sequence.
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-5: The capture rate limiter drops about half the frames when CaptureRateHz is close to the tick rate (default 60 Hz)
- Category: bug
- Severity: high
- Location: Private/O3DSenderComponent.cpp:912-935, :1136; Public/O3DSenderComponent.h:143-145
- Evidence: `ConsumeCaptureBudget` rejects any tick where `Now - Last < 1/Rate` and then sets `InOutLastCaptureTime = NowSeconds`. At a 60 fps tick with normal jitter (for example 16.60 ms < 16.667 ms), the frame is skipped, and the effective rate falls to about 30-45 Hz with irregular spacing. The limiter also runs on `FPlatformTime::Seconds()`, so it ignores pause, time dilation and fixed-timestep / Movie Render Queue rendering. The existing test (Tests/O3DSenderComponentTests.cpp:25-42) only checks the basic gate.
- Recommendation: Use an accumulator: `Last += MinDelta`, re-anchoring to `Now` if it falls more than one interval behind, and add a small tolerance (for example 0.5 ms). Consider world or app time (`FApp::GetCurrentTime()` / `World->GetTimeSeconds()`, needs-UE-verification) for the cadence. Extend the test with a jittered 60 Hz sequence and assert about 60 accepted frames per second.
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-6: Audio capture callbacks read and write UObject state from audio threads without synchronization
- Category: thread-safety
- Severity: high
- Location: Private/O3DSenderAudioCaptureComponent.cpp:26-36, :310-314, :384-395, :416-431, :516-524; Private/O3DSenderComponent.cpp:603-606
- Evidence:
  - `FO3DSenderSubmixTap::OnNewSubmixBuffer` runs on the audio render thread and calls `Owner.Get()` on a `TWeakObjectPtr` there. Resolving a weak pointer off the game thread is not GC-safe (needs-UE-verification).
  - `ProcessAndSubmitAudio` reads `Config.GameGain`, `Config.MicGain` and `CaptureMode` (:394) under `SinkMutex`, but the game-thread writers do not take that lock: `ConfigureAudioCaptureComponent` assigns `AudioCaptureComponent->Config = CaptureConfig` (O3DSenderComponent.cpp:604), and `StartCaptureWithMode` writes `CaptureMode` (:160).
  - `WorkingBuffer` (a member) is resized on the audio thread with no guard. `LastRejectedLogTime` and the function-local statics (:401, :500, :528) are also written from audio threads.
  - The mic lambda captures a raw `this` (:310). Its safety depends on `CloseStream()` joining the callback before `MicCapture.Reset()` (needs-UE-verification).
- Recommendation: Take an immutable snapshot of the capture parameters (gain, channels, rate, label) into a thread-safe shared state object (`TSharedPtr<FAudioState, ThreadSafe>`) that the tap and mic lambda hold instead of the UObject. Swap it atomically on the game thread. Give each producer its own scratch buffer. Never touch the `UObject` from the audio or capture threads.
- Effort: M
- Owner: coding
- Status: closed in #269

### SND-7: The submix listener is unregistered from the new submix instead of the one it was registered on, which leaks a listener
- Category: bug
- Severity: high
- Location: Private/O3DSenderComponent.cpp:603-606; Private/O3DSenderAudioCaptureComponent.cpp:154-164, :263-266, :288-291, :68-84
- Evidence:
  - `ConfigureAudioCaptureComponent` first overwrites `AudioCaptureComponent->Config` (including `SubmixToTap`), then calls `StartCaptureWithMode`. That in turn calls `TeardownSubmixTap()`, which computes `TargetSubmix` from the new `Config.SubmixToTap`.
  - If the user changed the submix, the listener stays registered on the old submix and keeps delivering buffers. A new one is also added, so audio can double up.
  - `BeginPlay` (:82) also calls `RebuildSubmixTap()` without tearing down first. The sender component may already have registered it via `StartCaptureWithMode` during its own BeginPlay, which gives a possible double registration (dedupe behaviour needs-UE-verification).
- Recommendation: Store the `USoundSubmix*` (as a weak pointer) that the listener was registered on, and always unregister from that. Make `RebuildSubmixTap` idempotent: tear down if already registered.
- Effort: S
- Owner: coding
- Status: closed in #269

### SND-8: Serialization and transport dispatch run synchronously on the game thread every tick
- Category: architecture
- Severity: high
- Location: Private/O3DSenderComponent.cpp:1133-1170, :363-385; Private/O3DSenderSerializer.cpp:183-228, :313-363, :588-599
- Evidence: `TickComponent`, then `HandleBoneTransformsFinalized`, then `OnPoseFrameReady.Broadcast`, then `FO3DSenderSerializer::OnPoseFrameReady`. That builds the FlatBuffer (`SubjectList::Serialize`, `CalcMatrices`, CRC/finalize), copies it into a `TArray`, and calls `SenderInstance->SendSerialized(...)`, all inline on the game thread. .github/copilot-instructions.md §2 requires "Network I/O and encoding run async". With several subjects or MetaHuman-sized skeletons (more than 800 bones plus more than 250 curves), this is measurable game-thread time, and any transport whose `SendSerialized` blocks stalls the frame.
- Recommendation: Keep only the sampling on the game thread: copy local transforms and curves into a pooled, immutable frame. Hand that to a per-sender worker (a `UE::Tasks` pipe or an `FRunnable` with an SPSC queue) that owns the serializer state and calls `SendSerialized`, and bound the queue with a drop-oldest policy. Document on `IOpen3DSender` which thread each method is called from.
- Effort: L
- Owner: design
- Status: closed in #318

### SND-9: Heavy per-frame allocation in capture and serialization
- Category: performance
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:969-975, :1124-1129; Private/O3DSenderSerializer.cpp:218-225, :231-252, :317-340, :595-597; Private/O3DSenderCurveProcessor.cpp:120
- Evidence: Every captured frame does the following:
  - A new `FO3DSPoseFrame` (heap arrays for transforms, names and values, plus the Subject FString).
  - Two array copies into `DescriptorSnapshot`.
  - In the legacy default path, a fresh `SubjectList` plus `Subject`, with N `new Transform` (each holding `std::vector`s), N `std::string` bone names and M curve-name strings via `TCHAR_TO_UTF8`.
  - A `std::vector<char>` buffer plus a second full copy into `TArray<uint8> Payload`.
  - `Name.ToString()` for every curve in `BuildFilteredCurves` even when nothing is logged.
  - The default (legacy) mode also sends full topology and names every frame, which is the largest possible wire format.
- Recommendation: Reuse a member frame and member arrays (`Reset()` rather than reconstruct). Keep a persistent `O3DS::Subject` per subject in legacy mode too, and only rebuild it on a descriptor change. Cache UTF-8 names per descriptor or curve revision. Serialize straight into a reusable buffer and pass a `TArrayView`/`TConstArrayView<uint8>` to the delegate. Build `NameString` only when logging. Consider making delta updates with a periodic full sync the default.
- Effort: M
- Owner: coding
- Status: closed in #316

### SND-10: Transport credentials (the WebRTC access token) are stored in a serialized UPROPERTY and saved into level and Blueprint assets
- Category: fab-readiness
- Severity: high
- Location: Public/O3DSenderComponent.h:159-161; Private/O3DSenderComponent.cpp:329-335; Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:32 (`webrtc.token`)
- Evidence: `TMap<FString, FString> TransportOptions` is `UPROPERTY(VisibleAnywhere)`, which is non-transient and therefore saved with the asset. Transport widgets write the token into it (key `webrtc.token`). `BuildTransportConfig` resets `Config.Token` and sets `bPersistToken=false`, then copies all options into `AdvancedParams`, so the token's persistence policy is bypassed. This breaks the "no hard-coded credentials in source control" rule, since assets are usually committed.
- Recommendation: Split options into a persisted `TransportOptions` and a `Transient` secret store (or read secrets from config or environment through the customization, respecting `bPersistToken`). Mark secret keys through the customization API and have `SetTransportOption` route them to the transient store. Redact them in `ToDebugString` and logs.
- Effort: M
- Owner: design
- Status: closed in #275

### SND-11: The build hard-fails on every platform except Win64, and the plugin does not declare a platform allow-list
- Category: fab-readiness
- Severity: high
- Location: Open3DSender.Build.cs:5, :25-32, :34-45; Open3DBroadcast.uplugin (Modules entry "Open3DSender" has no PlatformAllowList)
- Evidence: `throw new BuildException("Open3DSender does not define third-party binaries for platform ...")` for any non-Win64 target. The uplugin module entry has no `PlatformAllowList`, so enabling the plugin on a Mac or Linux editor breaks the whole project build rather than skipping the module. `[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]` also excludes Client and Server targets.
- Recommendation: Add `"PlatformAllowList": ["Win64"]` to every module of the plugin until other binaries exist, or ship Mac and Linux libraries. Include the Client and Server target types, or document why they are excluded. Replace the throw with a clear message once the allow-list protects the build.
- Effort: S
- Owner: coding
- Status: closed in #283

### SND-12: Capture tick is not ordered after the target mesh's animation evaluation
- Category: bug
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:58-61, :734-746, :1155-1160
- Evidence: The constructor sets `bCanEverTick` but no `TickGroup` and no `AddTickPrerequisiteComponent(TargetMesh)`. Capture reads `GetComponentSpaceTransforms()` in the default tick group. The code comments say `RegisterOnBoneTransformsFinalizedDelegate` "was removed in UE 5.4+". The project rule (§2) requires capture after animation evaluation. Without a prerequisite, ordering with parallel animation evaluation and post-physics (ragdoll or physical animation) results is not guaranteed, so the capture can be one frame stale or miss physics blending (needs-UE-verification).
- Recommendation: Verify against UE 5.7 whether `USkeletalMeshComponent::RegisterOnBoneTransformsFinalizedDelegate` (or an equivalent) exists and bind to it if it does. Otherwise set `PrimaryComponentTick.TickGroup = TG_PostUpdateWork` (or TG_PostPhysics) and call `AddTickPrerequisiteComponent(TargetMesh)` in `BindToTarget`, removing it in `UnbindFromTarget`.
- Effort: S
- Owner: coding
- Status: closed in #317

### SND-13: Residual and quantized modes never re-send a full descriptor, so late joiners or a lost first packet leave receivers without topology
- Category: bug
- Severity: medium
- Location: Private/O3DSenderSerializer.cpp:390-392, :508-510; Public/O3DSenderComponent.h:236-239
- Evidence: A full `Subject::Serialize()` happens only on the first frame, on a descriptor change or on a curve-count change. Residual keyframes are `SubjectUpdate`s, not topology. A receiver that connects mid-stream (UDP, NNG pub/sub, WebRTC reconnect) or loses the first packet cannot parse updates. The header says quantization is "safe on unreliable transports too", which is only true once topology has arrived.
- Recommendation: Add `FullSyncIntervalSeconds` (or frames) and force `bNeedFullSync` periodically (with the anchor preservation from SND-2). Optionally, let transports request a resync, for example on a new peer connection, through an `IOpen3DSender` callback.
- Effort: M
- Owner: design
- Status: closed in #267

### SND-14: Changing encoding settings at runtime is not applied consistently
- Category: bug
- Severity: medium
- Location: Private/O3DSenderSerializer.cpp:298-309, :433-445, :370-376, :500-506; Private/O3DSenderComponent.cpp:1185-1195
- Evidence:
  - The serializer reads `Component->bEnableResidualCoding` and `bEnableQuantization` every frame, but switching modes reuses the same `PersistentSubjects` with `bDescriptorSent==true` and does not force a resync.
  - `ResidualPredictor` and `ResidualKeyframeIntervalFrames` only take effect when an encoder is created at a full sync, so edits are silently ignored until a topology change.
  - `RestartProps` covers none of the Residual, Quantization or curve-pattern properties.
- Recommendation: Fingerprint the encoding settings per subject and force a full sync plus a fresh encoder when they change. Alternatively, add these properties to the restart set. Document which settings are live.
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-15: The UE sender never writes tx_seq, tx_wallclock_us or frame_epoch, so the receiver's reorder gate can never engage
- Category: architecture
- Severity: medium
- Location: Private/O3DSenderSerializer.cpp:340, :447, :468, :550, :571; Private/O3DSenderComponent.cpp:973; Open3DReceiver/Private/O3DReceiverSource.cpp:571-589; src/o3ds/model.h:370-381
- Evidence: Every Serialize call passes the default `seq=0`. `FrameCounter` / `FO3DSPoseFrame::FrameIndex` is computed but never reaches the wire. The receiver treats `TxSeq==0` as "legacy sender" and skips A1 ordering and loss detection.
- Recommendation: Keep an `O3DS::SequenceCounter` (src/o3ds/sequencing.h) per outbound stream in the serializer and pass `tx_seq`, `tx_wallclock_us` and `frame_epoch` to every Serialize, SerializeUpdate or SerializeUpdateResidual call. Bump the epoch on Stop/Start. Add a test.
- Effort: S
- Owner: coding
- Status: closed in #341

### SND-16: The audio stream label does not match the pose subject name
- Category: bug
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:467-468, :963-967; Private/O3DSenderAudioCaptureComponent.cpp:393
- Evidence: Audio uses the raw `SubjectName` property, which is unsanitized and empty by default, so the label becomes `"o3ds:audio"`. Pose frames use `CachedSubjectName`, which is sanitized or auto-generated as `World/Actor/Comp`. The comment says the label "matches mocap subject", but it only does when `SubjectName` is set and already sanitized.
- Recommendation: Pass `ResolveSubjectName(TargetMesh.Get())` (or `CachedSubjectName`) to `SetAudioSink`, and refresh the label whenever the subject name cache changes.
- Effort: S
- Owner: coding
- Status: closed in #279

### SND-17: Pose and audio timestamps use unrelated clock domains
- Category: architecture
- Severity: medium
- Location: Private/O3DSenderSerializer.cpp:339, :394, :512; Private/O3DSenderAudioCaptureComponent.cpp:33, :313
- Evidence: Pose frames are stamped with `FPlatformTime::Seconds()` at serialization time. Submix audio uses the mixer's `AudioClock`, and mic audio uses the device `StreamTimeSec`. Receivers cannot align audio with pose, and the pose stamp is the encode time rather than the capture or game time (no timecode).
- Recommendation: Define one sender clock: capture `FPlatformTime` (or engine timecode through `FApp::GetTimecode()`, needs-UE-verification) at sampling and carry it in `FO3DSPoseFrame`. Map audio clocks to that domain with an offset measured when the stream starts. Document the timestamp semantics on `SendSerialized` and `SubmitPcm`.
- Effort: M
- Owner: design
- Status: closed in #319

### SND-18: Audio device enumeration and open/close run synchronously on the game thread, repeatedly
- Category: performance
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:65, :409-422, :485-498, :629-650, :195-196 with :313; Private/O3DSenderAudioCaptureComponent.cpp:299-348
- Evidence:
  - `ResolveAudioDeviceIndex` builds an `Audio::FAudioCapture` and calls `GetCaptureDevicesAvailable` on every `BuildAudioCaptureConfig()` in Input mode. That is called from `BuildTransportConfig` and `UpdateAudioCaptureBinding`, and also from the constructor, PostLoad, OnRegister and PostInitProperties through `SyncAudioConfigSource`.
  - `StartCapture` calls `InitializeTransport()`, which calls `UpdateAudioCaptureBinding()` and so `StartCaptureWithMode()`, and then calls `UpdateAudioCaptureBinding()` again. The microphone stream is therefore closed and reopened twice per start (`OpenAudioCaptureStream`/`CloseStream`, which may block on OS device calls, needs-UE-verification).
  - The `GetOptions` callbacks also enumerate devices every time the editor queries them.
- Recommendation: Cache the device list, refresh it on demand or on a device-change notification, and resolve the index only when capture starts. Remove the duplicate `UpdateAudioCaptureBinding` call. Only restart capture when the audio config actually changed (compare with the previous config). Consider opening devices off the game thread.
- Effort: M
- Owner: coding
- Status: closed in #319

### SND-19: If StartCapture fails partway, the transport and serializer stay running
- Category: bug
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:172-200, :223-226, :230-235
- Evidence: With no valid mesh and `bEnableAudio=false`, the serializer is attached and `InitializeTransport()` starts a transport (which may open sockets). Then `bIsCapturing=false` and only a warning is logged. `StopCapture()` returns early because `!bIsCapturing`, so the transport stays up until EndPlay, and a later `StartCapture` restarts it.
- Recommendation: Validate the preconditions (mesh or audio) before creating the serializer and transport, or call `TeardownTransport()` and `Serializer->Detach()` on the failure branch. Surface the failure to users (on-screen or message log, plus a Blueprint-visible result or delegate).
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-20: Include/Exclude curve patterns apply even when filtering is disabled, and pattern edits are not picked up at runtime
- Category: bug
- Severity: medium
- Location: Private/O3DSenderCurveProcessor.cpp:246-263, :111-115, :103-115; Public/O3DSenderComponent.h:203-209; Private/O3DSenderComponent.cpp:834-841
- Evidence: `RefreshCurveCache` removes curves by pattern regardless of `bEnableCurveFiltering`. The UI greys the patterns out (`EditCondition="bEnableCurveFiltering"`) but they still drop curves. The cache is only rebuilt when the mesh or skeleton changes, so removing a pattern at runtime never brings the curves back, even though `UpdatePatternCacheIfNeeded` hashes the patterns.
- Recommendation: Do not filter in `RefreshCurveCache`. Rely only on the mask in `UpdatePatternCacheIfNeeded` (gated by `bEnableCurveFiltering`). Invalidate the cache when patterns change.
- Effort: S
- Owner: coding
- Status: closed in #267

### SND-21: The audio resampler is stateless per buffer, has no anti-aliasing and drifts over time; one audio source option is not implemented
- Category: bug
- Severity: medium
- Location: Private/O3DSenderAudioCaptureComponent.cpp:416-484, :486-496; Public/O3DSenderAudioCaptureComponent.h:22-28
- Evidence: Linear interpolation restarts at every callback buffer. `OutFrames = Round(NumFrames*Ratio)` per buffer accumulates rounding drift and clicks at buffer boundaries, and there is no low-pass filter before downsampling (for example 48 kHz to 16 kHz). The enum value `EO3DSenderAudioSource::GameAndMic` is exposed but no code path handles it. There is also an empty branch at :486-489.
- Recommendation: Use UE's `Audio::FResampler` or a stateful polyphase resampler per stream with a fractional-position carry (needs-UE-verification for the API). Remove or implement `GameAndMic`. Delete the dead branch.
- Effort: M
- Owner: coding
- Status: closed in #279

### SND-22: UO3DSenderComponent is a god class, and the serializer is tightly coupled to it
- Category: architecture
- Severity: medium
- Location: Private/O3DSenderComponent.cpp (whole file); Public/O3DSenderComponent.h:117, :263, :306-316; Private/O3DSenderSerializer.cpp:298-304, :434-444, :467, :566-570; Public/O3DSenderSerializer.h:81
- Evidence: One class handles:
  - pose capture and skeleton caching
  - subject naming
  - curve config
  - audio device enumeration and audio component lifecycle
  - transport lifecycle and the editor restart policy
  - on-screen notifications

  The serializer holds a raw `UO3DSenderComponent*` and reads its UPROPERTYs directly, so the coupling is bidirectional and the serializer cannot be unit-tested without a UObject. `friend class FO3DSenderTransportController` (h:117) is unused. `GetSerializer()` dereferences a possibly null `TUniquePtr` (h:263): the serializer only exists between StartCapture and EndPlay.
- Recommendation:
  - Pass an `FO3DSenderEncodingSettings` value struct into the serializer (at Attach or per frame) instead of the component pointer.
  - Move audio binding into an `FO3DSenderAudioBinding` helper.
  - Let `FO3DSenderTransportController` own frame dispatch (`HandleSerializedFrameForward`).
  - Make `FO3DSenderSerializer` private, or give it a narrow public interface.
  - Make `GetSerializer()` return a pointer, or create the serializer in the constructor.
  - Drop the unused friend.
- Effort: L
- Owner: design
- Status: closed in #332

### SND-23: Two parallel transport registries; the picker lists customizations rather than factories, and the customization lookup returns an unlocked pointer
- Category: architecture
- Severity: medium
- Location: Public/O3DSenderRegistry.h:6-18; Private/O3DSenderTransportCustomization.cpp:13-30; Private/O3DSenderComponent.cpp:715-732, :341-347; Private/O3DSenderComponentCustomization.cpp:388-389; Open3DShared/Public/O3DTransportRegistry.h:5-11
- Evidence:
  - Transports must register twice, once as a factory and once as a customization, and the two can drift apart. `EnsureValidTransportName` and the combo box only list customization names, so a transport with a factory but no customization cannot be picked, and a customization without a factory fails at Start.
  - `FindTransportCustomization` returns a raw pointer into `GSenderCustomizations` after releasing the lock (:29). A concurrent register or unregister (module load or unload) can rehash the map, leaving the pointer dangling.
  - The Shared umbrella header `__has_include`s Sender headers, which inverts the module dependency, and its paths (`Open3DSender/O3DSenderRegistry.h`) do not match the Public layout.
- Recommendation: Keep one registry entry per transport: `{Factory, ConfigureTransport, BuildTransportWidget, DisplayName, Capabilities}`. Return copies (`TOptional`/`TSharedPtr`) rather than raw pointers. Remove the Shared umbrella header.
- Effort: M
- Owner: design
- Status: closed in #289

### SND-24: Residual and Quantization settings are not integrated into the custom details layout
- Category: usability
- Severity: medium
- Location: Private/O3DSenderComponentCustomization.cpp:84-90, :178-337; Public/O3DSenderComponent.h:215-257
- Evidence: The customization hides the `Open3DStream|Sender*` categories and rebuilds groups for Sender, Transport, Curves and Audio. The `Open3DStream|Sender|Residual` and `|Quantization` properties are never added to a group. Depending on how `HideCategory` treats nested categories (needs-UE-verification), they are either hidden entirely or shown as stray categories outside the "Open3DStream" layout. There is also no warning when both modes are enabled (Residual silently wins) or when Residual is used on an unreliable transport.
- Recommendation: Add an "Encoding" group containing a mode enum (Legacy, Residual, Quantized) that replaces the two booleans, and show each mode's settings with EditCondition. Warn in the UI when the selected transport is unreliable and Residual is chosen.
- Effort: S
- Owner: coding
- Status: closed in #389

### SND-25: The default configuration captures and encodes but sends nowhere, with no warning
- Category: usability
- Severity: medium
- Location: Public/O3DSenderComponent.h:151-157; Private/O3DSenderComponent.cpp:279-282; Private/O3DSenderComponentCustomization.cpp:576-586, :616-626
- Evidence: `bAutoCreateTransport=false` by default. The transport combo is disabled and the transport widget is collapsed until it is ticked. A newly added component therefore serializes every frame (costing CPU) and transmits nothing. `InitializeTransport` returns silently.
- Recommendation: Default `bAutoCreateTransport=true` (with Loopback), or log a one-time warning or message-log entry ("no transport, frames only available via OnSerializedFrame"). Skip serialization entirely when there is neither a transport nor an `OnSerializedFrame` listener.
- Effort: S
- Owner: design
- Status: closed in #389

### SND-26: Large gaps in the Blueprint API
- Category: usability
- Severity: medium
- Location: Public/O3DSenderComponent.h:48-106, :124-129, :155-157, :259-266, :326-340
- Evidence:
  - `OnDescriptorReady`, `OnPoseFrameReady` and `OnSerializedFrame` are native multicast delegates, and `FO3DSSkeletonDescriptor`/`FO3DSPoseFrame` are not `BlueprintType`, so Blueprint cannot observe frames or state.
  - `Get/SetTransportOption`, `ClearTransportOptions` and `SetTransportName` are not UFUNCTIONs, so runtime configuration (for example a server URL typed in a UI) is impossible from Blueprint.
  - `TransportName` is BlueprintReadWrite, which bypasses the normalization and option-clearing done in `SetTransportName`.
  - `StartCapture`/`StopCapture` are `CallInEditor`, but StartCapture returns immediately outside a game world (cpp:163-170), so the editor button does nothing.
  - There are no state-change events (started, stopped, transport failed).
- Recommendation: Add dynamic delegates for capture state and transport errors. Expose the option accessors and the transport setter as BlueprintCallable, make `TransportName` BlueprintReadOnly with a setter, and remove `CallInEditor` (or make it work in the editor).
- Effort: M
- Owner: coding
- Status: closed in #386

### SND-27: Missing clamps, tooltips and validation on properties
- Category: usability
- Severity: low
- Location: Public/O3DSenderComponent.h:143-149, :245-257; Public/O3DSenderAudioCaptureComponent.h:42-64
- Evidence:
  - `CaptureRateHz` has no `ClampMin`/`UIMin`/`Units`. A value of 0 or below silently means "unlimited", which is undocumented.
  - `QuantizationHalfRange` is not validated to be at least `QuantizationByteRange`.
  - `FO3DSenderAudioCaptureConfig` fields have no tooltips and no clamps (`SampleRate`, `NumChannels` (1-2?), `BitrateKbps`, the gains).
  - The `bAutoStartCapture` doc says "as soon as the component is registered / BeginPlay", but it only runs at BeginPlay.
- Recommendation: Add `ClampMin`/`UIMin`/`Units="Hz"`/`ForceUnits`, document the meaning of 0, validate ranges in PostEditChangeProperty, add tooltips to the struct fields, and fix the doc text.
- Effort: S
- Owner: coding
- Status: closed in #389

### SND-28: The auto-created audio component keeps capturing after audio is disabled
- Category: performance
- Severity: medium
- Location: Private/O3DSenderComponent.cpp:441-447, :583-593; Private/O3DSenderAudioCaptureComponent.cpp:68-84, :397-414
- Evidence: With `bEnableAudio=false`, `UpdateAudioCaptureBinding` only clears the sink. The submix tap registered in the audio component's BeginPlay (or the open mic stream) keeps running and invokes `ProcessAndSubmitAudio` every audio buffer, only to drop the data. `EnsureAudioCaptureComponent` also calls `OnComponentCreated()` and `RegisterComponent()` manually, then `AddInstanceComponent` at runtime; the lifecycle ordering needs-UE-verification.
- Recommendation: Stop the submix tap and mic when the sink is null, or when audio is disabled or capture stops, and restart them on bind. Do not tap in the audio component's BeginPlay unless a sink is bound.
- Effort: S
- Owner: coding
- Status: closed in #279

### SND-29: Local transforms are recomputed from component space instead of read from the bone-space pose
- Category: performance
- Severity: low
- Location: Private/O3DSenderComponent.cpp:977-1035, :1045-1085
- Evidence: For each bone and frame, `ComponentTransform.GetRelativeTransform(Parent)` performs an inverse and multiply. Under non-uniform parent scale, `FTransform` relative math is lossy. `USkeletalMeshComponent::GetBoneSpaceTransforms()` already holds the evaluated local pose (though possibly pre-physics; needs-UE-verification).
- Recommendation: Measure. If the bone-space pose is equivalent for the supported cases, read locals directly and keep the component-space path only for physics-driven meshes.
- Effort: S
- Owner: review

### SND-30: Target mesh selection is fragile for multi-mesh actors, and the property type is not picker-friendly
- Category: usability
- Severity: low
- Location: Private/O3DSenderComponent.cpp:74-80; Public/O3DSenderComponent.h:135-137
- Evidence: The auto-bind uses `FindComponentByClass<USkeletalMeshComponent>()`, which returns an arbitrary first mesh (for example the MetaHuman Body rather than Face, or a leader-pose follower whose own transforms may not be evaluated; needs-UE-verification). `TWeakObjectPtr<USkeletalMeshComponent>` with EditAnywhere generally cannot be set on Blueprint defaults (needs-UE-verification).
- Recommendation: Use `FComponentReference` with `meta=(UseComponentPicker, AllowedClasses="/Script/Engine.SkeletalMeshComponent")`. Prefer the leader-pose component when resolving the target, and log which mesh was chosen.
- Effort: S
- Owner: coding
- Status: closed in #389

### SND-31: Dead code, stale paths and triplicated logic
- Category: code-quality
- Severity: low
- Location: Public/O3DSenderComponent.h:308; Private/O3DSenderComponent.cpp:387-407, :734-751, :767-770, :1172-1175; Public/O3DSenderSerializer.h:19-20, :43-44, :70-71; Private/O3DSenderSerializer.cpp:161-169, :191-196, :255-267, :323-334 / :407-418 / :525-536; Private/O3DSenderCurveProcessor.h:35-36
- Evidence:
  - `BoneTransformsFinalizedHandle` is unused, `BindToTarget`/`UnbindFromTarget` are mostly no-ops, and `UpdateEditConditionHelpers` is empty.
  - `OnSubjectListReady` is bound but, as the code itself says, never broadcast.
  - `FSubjectCache::CurveNames`/`CurveIndex` are never read, and `GetCurveNames`/`GetCurveValues` are unused.
  - The size-mismatch fallback in `FillFrameValues` builds a fake chain hierarchy (`Index-1` parents, empty names).
  - The curve copy block is duplicated three times, and the residual and quantized functions are about 80% identical.
- Recommendation: Delete the dead members and paths. Extract `WriteCurvesToSubject()` and a shared persistent-subject full-sync helper. Replace the fake-hierarchy fallback with drop plus warning.
- Effort: S
- Owner: coding

### SND-32: Console command and global instance list lifetimes
- Category: code-quality
- Severity: low
- Location: Private/O3DSenderSerializer.cpp:38, :56-67, :82, :632-647
- Evidence: `o3ds.Sender.DumpStats` is registered lazily inside a function-local static and never unregistered, so a module unload or reload leaves a dangling delegate into unloaded code. `GInstances` is a static `TArray` of raw pointers with no lock. That is fine only while every caller stays on the game thread, which is undocumented.
- Recommendation: Use `FAutoConsoleCommand` at file scope, or register and unregister in `StartupModule`/`ShutdownModule`. Document game-thread-only use, or protect the list with a lock.
- Effort: S
- Owner: coding
- Status: closed in #334

### SND-33: Hot-path logging and log category hygiene
- Category: code-quality
- Severity: low
- Location: Private/O3DSenderSerializer.cpp:199-215; Private/O3DSenderCurveProcessor.cpp:96-99; Private/O3DSenderTransportController.cpp:16-17; Private/O3DSenderComponentCustomization.cpp:24, :43-54, :558, :578, :590, :618
- Evidence: An empty or NaN pose logs a `Warning` every frame (60 per second) with no rate limit. The curve processor and transport controller log under `LogO3DSenderComponent`. The details customization logs from Slate attribute getters that are polled every paint (VeryVerbose, so cheap when compiled out, but noisy when enabled) and uses its own static category.
- Recommendation: Rate-limit the per-frame warnings (log once on transition, then count them in stats). Add `LogO3DSenderCurves` and `LogO3DSenderTransport` categories. Drop the getter logs.
- Effort: S
- Owner: coding

### SND-34: Editor code and dependencies live in the runtime module
- Category: fab-readiness
- Severity: low
- Location: Private/Open3DSenderModule.cpp:4-8, :19-45; Open3DSender.Build.cs:79-89; Private/O3DSenderComponentCustomization.*; Public/O3DSenderTransportCustomization.h:249-255
- Evidence: The runtime module registers the detail customization and links PropertyEditor, Slate and `EditorStyle` (EditorStyle has been deprecated in favour of AppStyle since 5.1, needs-UE-verification) when `bBuildEditor` is set. `FO3DSenderTransportCustomization` changes layout under `WITH_EDITOR`, which is an ODR hazard across modules compiled with different settings.
- Recommendation: Move the customization into an `Open3DSenderEditor` module (Type Editor). Keep the transport-widget registration in an editor-only registry. Replace EditorStyle with AppStyle.
- Effort: M
- Owner: coding
- Status: closed in #285

### SND-35: Editing transport options is not undoable, and switching transports silently wipes options
- Category: usability
- Severity: low
- Location: Private/O3DSenderComponent.cpp:667-713, :1200-1204; Private/O3DSenderComponentCustomization.cpp:364-375
- Evidence: `SetTransportOption` and `ClearTransportOptions` call `Modify()` without an `FScopedTransaction`, so edits made through custom transport widgets have no undo entry. Changing the transport clears every option in two places (the customization handler and PostEditChangeProperty), so switching away and back loses the configured endpoints without warning.
- Recommendation: Wrap option edits in `FScopedTransaction`. Keep options namespaced per transport (the keys already carry prefixes like `webrtc.`) instead of clearing them, or confirm before clearing.
- Effort: S
- Owner: coding
- Status: closed in #389

### SND-36: Test coverage covers two pure helpers and none of the risky logic
- Category: tests
- Severity: high
- Location: Private/Tests/O3DSenderComponentTests.cpp:1-91
- Evidence: Only `ConsumeCaptureBudget` and `BuildLocalBoneTransforms` are tested. There are no tests for:
  - descriptor delivery or restart (SND-1)
  - residual or quantized resync and anchor consistency (SND-2, SND-3)
  - curve processing (SND-4, SND-20)
  - serializer mode switching (SND-14)
  - the audio resample and mix math (SND-21)
  - the registries
  - the transport controller failure paths

  Test names are inconsistent (`Open3DBroadcast.O3DSender.*` vs `Open3DBroadcast.Open3DSender.*`). The pure-logic tests use `EditorContext` rather than an application-wide context. `#endif // WITH_DEV_AUTOMATION_TESTS` (:91) does not match the `WITH_AUTOMATION_TESTS` guard.
- Recommendation: Add the following, and fix the names, flags and the comment:
  - serializer round-trip tests: sender to `O3DS::SubjectList::Parse`, covering legacy, residual and quantized, with stop/start, rename, curve add/remove and the resync cases
  - curve processor unit tests using a fake name/value source (extract an interface so no `USkeletalMeshComponent` is needed)
  - audio DSP tests on a pure function
  - a lifecycle test with a mock `IOpen3DSender` registered in the registry
- Effort: L
- Owner: coding

### SND-37: Public API documentation gaps and stale references
- Category: docs
- Severity: medium
- Location: Public/O3DSenderSerializer.h:17-20, :35-44, :83-92; Private/O3DSenderSerializer.cpp:286-289; Public/O3DSenderInterface.h:19-23, :41-45, :79-80; Public/O3DSenderComponent.h:156, :259-266; ThirdParty/README.md:8-10
- Evidence:
  - Comments still refer to the removed cvar `o3ds.Sender.Residual.Enabled` (Serializer.h:84, .cpp:287).
  - The `OnSubjectListReady` doc says "Emitted with a shared SubjectList prior to serialization", but it is never emitted.
  - The `IO3DSenderAudioSink::SubmitPcm` and `IOpen3DSender` docs do not say which thread calls each method. `SubmitPcm` is called from audio render or capture threads, `Send`/`SendSerialized`/`Tick` from the game thread.
  - The public delegates and `GetSerializer`/`Get/SetTransportName` on the component have no doc comments.
  - `meta=(HideInDetailPanel)` on `TransportName` (h:156) is a delegate-property specifier (needs-UE-verification).
  - ThirdParty/README describes a `Lib/<Platform>` layout that Build.cs does not use; the libraries come from the plugin-level ThirdParty folder.
- Recommendation: Add threading and lifetime contracts to every public interface method and delegate, remove the stale references, document the timestamp semantics (see SND-17), and fix the README and the meta tag.
- Effort: S
- Owner: coding
- Status: closed in #423
