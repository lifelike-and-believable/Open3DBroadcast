# 0008: Sender pipeline threading

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** every open question below was accepted with the recommended default given next to it. Needs-verification items stay open and are resolved in the implementing WPs; a result that invalidates a default is handled by a superseding ADR.
- **Date:** 2026-09-29
- **Plan decision:** D5 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0005](0005-wire-resync-and-loss-contract.md) (pull-based descriptor, `StreamWriter`, full-sync policy, new-peer flag), [ADR 0007](0007-transport-abstraction-and-registry.md) (D4: `SendSerialized` contract, audio sink guard), [ADR 0009](0009-protocol-versioning.md) (D8: clock domains), [ADR 0006](0006-test-module-layout-and-fakes.md); feeds WP-A2, WP-A3, WP-S3

**Recommendation in one line:** the game thread only samples the pose into a pooled, immutable frame and pushes it into a small per-sender queue (default depth 2, drop oldest). A per-sender `UE::Tasks` pipe drains the queue and owns everything stateful: curve filtering, the serializer, the `StreamWriter` (ADR 0005), compression, CRC and `SendSerialized`. The worker never touches a UObject: settings and the skeleton descriptor travel with each frame as immutable snapshots. Capture ticks after the mesh's animation and physics. Pose and audio share one sender clock. Audio keeps its own path from the audio thread straight into the transport queue.

## Context

**Everything runs inline on the game thread today (SND-8).** `TickComponent` (`Plugin/Source/Open3DSender/Private/O3DSenderComponent.cpp:1155-1170`) calls `HandleBoneTransformsFinalized` (`:1133-1152`), which samples bones and curves and broadcasts `OnPoseFrameReady` (`:1151`). The serializer is bound to that delegate with `AddRaw` (`O3DSenderSerializer.cpp:69-70`) and, in the same call stack:
- builds a `SubjectList`, calls `CalcMatrices` and `Serialize` (`O3DSenderSerializer.cpp:330-340`), which runs a bitwise CRC (`src/o3ds/model.cpp:872`; CORE-7 measured 548 µs per serialize for 250 bones plus 250 curves, 413 µs of it CRC);
- copies the result into a new `TArray` (`O3DSenderSerializer.cpp:595-597`) and broadcasts `OnSerializedFrame` (`:599`);
- the component forwards it to `SendSerialized` (`O3DSenderComponent.cpp:363-385`).

What `SendSerialized` costs depends on the transport: UDP calls `SendTo` under a lock (TRB-20) and WebRTC calls `lk_send_data_ex` inline (ADR 0007, Context table).

**The serializer reads the component directly (SND-22).** It holds a raw `UO3DSenderComponent*` (`O3DSenderSerializer.h`, member `Component`) and reads its UPROPERTYs every frame (`O3DSenderSerializer.cpp:298-304`, `:436-444`, `:467`, `:566-570`). A worker thread cannot do that.

**Allocation per frame (SND-9, CORE-18).** A new `FO3DSPoseFrame` per capture (`O3DSenderComponent.cpp:969-975`), new filtered curve arrays (`:1124-1128`), a new `SubjectList` in legacy mode, a `std::vector<char>` and a second copy into `TArray` (`O3DSenderSerializer.cpp:339`, `:595-597`).

**Tick ordering (SND-12).** The constructor sets only `bCanEverTick` (`O3DSenderComponent.cpp:60`); there is no tick group and no prerequisite on the target mesh. The comments at `:744` and `:1159` say the bone-transforms-finalized callback was removed in 5.4, but a third-party mirror of the UE 5.7 API lists `USkeletalMeshComponent::RegisterOnBoneTransformsFinalizedDelegate` (References; **needs-verification**, Q2).

**Clocks (SND-17).** Pose frames are stamped with `FPlatformTime::Seconds()` at serialization time (`O3DSenderSerializer.cpp:339`, `:394`, `:512`), not at sampling. Submix audio uses the mixer's `AudioClock` (`O3DSenderAudioCaptureComponent.cpp:25-32`); microphone audio uses the device `StreamTimeSec` (`:309-313`).

**Audio already runs off the game thread.** The audio capture component calls `SubmitPcm` on the audio render or capture thread (`O3DSenderAudioCaptureComponent.cpp:516`). Device enumeration still runs on the game thread, several times per start (SND-18).

**Restarts.** `PostEditChangeProperty` stops and restarts capture for most property edits (`O3DSenderComponent.cpp:1178-1230`); `StopCapture` detaches and clears the serializer (`:247-248`) and tears down the transport (`:256`).

**Accepted constraints.**
- ADR 0005 (i): each pose frame carries a `TSharedPtr<const FO3DSSkeletonDescriptor>` and its hash; frames with a missing or mismatched descriptor are dropped, never padded. This "suits D5, whose worker thread cannot call back into the component".
- ADR 0005 (iv), (vi): the serializer owns one `StreamWriter`; the counter is not reset on Stop/Start in a process; a new-peer callback sets an atomic flag consumed by the next frame.
- ADR 0007: `SendSerialized` is thread-safe and non-blocking and takes `FO3DSendPayload&&`, including a `bFullSync` flag; `Stop()` is safe while a send is in flight.

## Decision drivers

1. The game-thread cost per sender stays under the WP-A2 budget: **0.1 ms for a 250-bone, 250-curve subject**.
2. Wire correctness from ADR 0005 is preserved: no skipped `tx_seq`, no stale descriptor.
3. No UObject access and no raw back-pointer off the game thread (review checklist §7 item 2).
4. Latency beats completeness for live mocap: a late frame is worth less than the next one.
5. Scales to many senders (crowds, multiple performers) without one OS thread each.

## Options considered

### Worker mechanism
- **W1. `UE::Tasks::FPipe` per sender (chosen).** Tasks launched into a pipe run one at a time, so serializer state needs no lock, and no OS thread is dedicated per sender. Pipes are lightweight and store no task list (Epic docs, References). Con: shares the task-graph workers with engine work; needs a priority that does not compete with frame-critical tasks (**needs-verification**, Q1).
- **W2. `FRunnable` plus SPSC queue per sender.** Predictable latency, but one OS thread per sender that sleeps about 99 % of the time, plus wake-event plumbing. Kept as the fallback if Insights shows W1 missing the budget.
- **W3. One shared `FRunnable` for all senders.** One slow transport delays every sender. Rejected.

### Queue and back-pressure
- **Q1. Bounded queue of sampled frames, drop oldest (chosen).** Dropping a *sample* before serialization never drops a full sync and never skips a `tx_seq`, because the worker decides full versus update and stamps the sequence. The receiver sees only a missing sample time.
- **Q2. Drop newest.** Keeps stale data and discards fresh data. Rejected.
- **Q3. Unbounded.** Latency and memory grow without limit when a transport stalls. Rejected.

### Where serializer state lives
- **S1. Worker-owned, with per-frame immutable snapshots of settings and descriptor (chosen).**
- **S2. Shared with the game thread under a lock.** Contention and a lock on the game thread every frame. Rejected.

## Decision

**1. Game thread: sample only.** Per tick, in this order, and nothing else:
1. Rate gate with the accumulator limiter (SND-5, WP-S3) and `EnsureSkeletonCache`.
2. Take a frame from the pool (item 4). Fill `CaptureTimeSec = FPlatformTime::Seconds()` **at sampling** (item 7), the frame index, the subject `FName`, local bone transforms (SND-29: read the bone-space pose when WP-A2's measurement shows it matches), and **raw** curve values in descriptor order.
3. Attach the current `TSharedPtr<const FO3DSSkeletonDescriptor>` and its hash (ADR 0005 i), and the current `TSharedRef<const FO3DSenderEncodingSettings>` (item 6).
4. Push to the sender's pipeline (item 3) and return.
- Curve **filtering** (include/exclude patterns, epsilon; `O3DSenderCurveProcessor`) moves to the worker. Curve **capture** stays on the game thread, because it reads the mesh.
- `OnPoseFrameReady` still fires on the game thread with the sampled frame.
- The transport's `Tick` stays on the game thread (ADR 0007 contract).

**2. Worker: everything stateful.** A pipe task drains the queue. For each frame, it:
- drops it with a rate-limited warning if its descriptor is missing or mismatched (ADR 0005 i);
- applies curve filtering;
- asks the serializer, which owns the per-subject caches, the persistent `O3DS::Subject`s, one `StreamWriter` and a reusable `FlatBufferBuilder` and output buffer (CORE-18), for a full sync or an update per the ADR 0005 (ii) policy;
- serializes, stamps `tx_seq`/`tx_wallclock_us`/`frame_epoch`/`ref_seq` through `StreamWriter`, and computes the CRC (table-driven, CORE-7);
- calls `SendSerialized(FO3DSendPayload&&)` on a `TSharedPtr<IOpen3DSender>` the pipeline holds;
- if a **full sync** comes back `DroppedBackpressure`, marks the subject NeedFull so the next frame is a full sync (ADR 0007 item 3).

`OnSerializedFrame` is kept for C++ listeners but now fires **on the worker thread**, and its comment says so. No listener exists in the repository (grep). It is not Blueprint-exposed (`O3DSenderComponent.h:261`).

**3. The pipeline object.** `FO3DSenderPipeline` (Open3DSender, private) is created once per component and held in a `TSharedPtr` that every launched task captures, so tasks never outlive their state. It contains:
- `UE::Tasks::FPipe` named `O3DSenderPipe_<Subject>`;
- a bounded queue of `TSharedPtr<const FO3DSPoseFrame>` plus **control items** (Start with epoch, Stop, NewTransport, SettingsChanged). Control items are never dropped;
- an atomic "drain task scheduled" flag. The game thread launches a drain task only when the flag was clear (a single compare-exchange); the task re-checks the queue before clearing it;
- the queue itself is either a lock-free SPSC ring or a mutex held only for pointer moves. Either is acceptable; the PR measures both and states which it chose.

**Depth:** default **2 frames**, console variable `o3d.Sender.PipelineDepth` (clamp 1 to 8). On overflow the oldest *pose* frame is released to the pool and `Stats.PipelineDropped` increments. Rationale: at 60 Hz, 2 frames is 33 ms of slack, enough to absorb a hitch without adding standing latency.

**Priority:** `ETaskPriority::BackgroundHigh`, so the work does not compete with foreground frame tasks (**needs-verification** of the 5.7 enum and its scheduling semantics, Q1).

**4. Frame pool.** `FO3DSPoseFramePool` per pipeline hands out frames whose arrays keep their capacity (`Reset()`, not reallocation). Capacity is depth plus 2. A frame is immutable once pushed; the worker holds it as `const` and returns it to the pool when done. This removes the per-frame `FO3DSPoseFrame` and array allocations (SND-9).

**5. Descriptor, full-sync and new-peer requests reach the worker as data.**
- **Descriptor:** on every frame (ADR 0005 i); the worker compares hashes.
- **Rename:** the frame's subject name differs from the cached one; the worker treats it as ADR 0005's "rename" trigger.
- **Start/Stop:** control items. Start carries the new epoch (`max(NewSessionEpoch(), previous + 1)`, ADR 0005 iv). Stop clears caches and marks every subject NeedFull. The `StreamWriter` lives in the pipeline, which survives Stop/Start, so the counter keeps increasing as 0005 requires.
- **New peer:** the transport callback sets an atomic flag on the pipeline (ADR 0005 vi); the next frame on the worker consumes it.
- **Periodic full sync:** evaluated on the worker from `CaptureTimeSec` (monotonic), so a stalled worker does not cause a burst of full syncs.

**6. Property changes during capture: snapshot semantics.**
- Encoding settings (mode, residual predictor, keyframe interval, thresholds, quantization ranges, curve patterns, `FullSyncIntervalSeconds`) are copied into an immutable `FO3DSenderEncodingSettings` on the game thread whenever a relevant property changes (`PostEditChangeProperty`, Blueprint setters, `PostLoad`). Frames carry the current snapshot, so each frame is encoded with exactly one consistent set of settings (SND-14).
- A change of encoding **mode**, curve patterns or quantization ranges forces a full sync on the next frame.
- Transport name or transport options still restart capture, as today (`O3DSenderComponent.cpp:1226-1229`). The restart goes through Stop and Start control items, so it needs no wait.
- The serializer stops holding `UO3DSenderComponent*` (SND-22). This is the first step of WP-A3's split.

**7. One clock domain (SND-17).** Defined in ADR 0009, applied here:
- The **sender clock** is `FPlatformTime::Seconds()`: monotonic and process-relative. Pose `SubjectList.time` is the sampling time on this clock.
- Audio timestamps are mapped onto the same clock. At stream start the sink records `Offset = FPlatformTime::Seconds() - SourceClock` (source clock is `AudioClock` or `StreamTimeSec`) and stamps `SourceClock + Offset`. The offset is re-estimated with a slow low-pass filter so audio-device drift does not accumulate. A jump of more than 100 ms resets it.
- `tx_wallclock_us` stays UTC at transmit (ADR 0005, `StreamWriter`); receivers use it only to estimate clock offset.
- Engine timecode (`FApp::GetTimecode`) is not used in v1 (Q4).

**8. Audio path.** Audio does **not** go through the pose pipe. It arrives on audio threads, and the ADR 0007 sink guard encodes it into sink-local scratch and enqueues it on the transport's send queue. The two paths meet only in the transport queue, where audio items are never dropped to make room for mocap (ADR 0007 item 7). Audio device enumeration is cached and refreshed on demand. It runs once per `StartCapture` and never from `GetOptions` callbacks. Opening the device is done once per start (SND-18).

**9. Tick ordering (SND-12).** `PrimaryComponentTick.TickGroup = TG_PostUpdateWork`, and `BindToTarget` calls `AddTickPrerequisiteComponent(TargetMesh)` (removed in `UnbindFromTarget`). This samples after animation evaluation and after physics blending (**needs-verification** of both APIs and of the 5.7 guarantee that parallel animation evaluation has completed by then, Q2). If `RegisterOnBoneTransformsFinalizedDelegate` is verified to fire on the game thread after physics blending, WP-A2 may bind to it instead and must say so in the PR.

**10. Stop, EndPlay and flush semantics.** `StopCapture` **discards** queued, unserialized pose frames: they are stale by definition. It does not wait for the in-flight task.
- The game thread pushes a Stop control item, stops the transport (ADR 0007: `Stop()` is safe while a send is in flight), and releases its references.
- A task still running holds the pipeline and the transport by `TSharedPtr`; its last `SendSerialized` returns `NotRunning`.
- Frames already handed to the transport follow the transport's own `Stop()` semantics.
- `Open3DSender`'s `ShutdownModule` waits for all pipes to empty, with a 1 s cap and an error log on timeout (**needs-verification** of `FPipe::WaitUntilEmpty` or its 5.7 equivalent, Q1).

**11. Measurement plan (WP-A2 acceptance).**
- Trace scopes: `O3D.Sender.Sample` (game thread), `O3D.Sender.Pipeline.Serialize`, `O3D.Sender.Pipeline.Send` (worker), `O3D.Transport.Worker` (ADR 0007).
- Test scene: one 250-bone, 250-curve subject at 60 Hz, then 10 such senders, on Loopback and on UDP.
- Record in the PR, before and after, from Unreal Insights: game-thread time per sender (median, p99); worker time per frame; capture-to-`SendSerialized` latency; pipeline and transport drop counts.
- **Budgets:** game thread ≤ 0.1 ms median per sender; worker serialize ≤ 0.2 ms per frame after the CORE-7 table CRC; capture-to-send latency p99 ≤ 2 frames at 60 Hz; zero pipeline drops in steady state.
- Core benchmark (`perf.cpp`) Serialize+Parse improves at least 3x (CORE-7).

## Consequences

- **Easier:** encoding cost leaves the game thread; slow transports can no longer stall the frame; the serializer becomes testable without a UObject (feeds WP-A3); the ADR 0005 state machine has one owner on one thread.
- **Harder:** one more thread boundary to reason about; `OnSerializedFrame` listeners must be thread-safe; tests need to wait for the pipe (0006: poll with a timeout, never sleep).
- **Constrains:** transports must honour ADR 0007's non-blocking, thread-safe `SendSerialized`, and UDP and WebRTC need their shared worker before this lands with them (WP-A1 step 4). WP-S3's descriptor-on-frame work is a prerequisite.

## Implementation outline

1. **WP-S3 (prerequisite, per ADR 0005):** descriptor pointer and hash on `FO3DSPoseFrame`; drop on mismatch.
2. **WP-A2a (still synchronous):** `FO3DSenderEncodingSettings` snapshot; the serializer loses `Component`; curve filtering moves after sampling; frame pool; sampling-time clock (`O3DSenderComponent.h/.cpp`, `O3DSenderSerializer.h/.cpp`, `O3DSenderCurveProcessor.cpp`). A pure refactor: WP-S3 tests pass unchanged.
3. **WP-A2b:** tick group and prerequisite (`O3DSenderComponent.cpp` constructor, `BindToTarget`, `UnbindFromTarget`).
4. **WP-A2c:** `FO3DSenderPipeline` with the pipe, queue, control items and `SendSerialized(FO3DSendPayload&&)` (new `Private/O3DSenderPipeline.h/.cpp`; `O3DSenderComponent.cpp:363-385` replaced; `O3DSenderTransportController.cpp` hands the sender to the pipeline). Behind a console variable `o3d.Sender.AsyncPipeline` (default on) for one release, so a regression can be isolated.
5. **WP-A2d:** audio clock mapping and cached device enumeration (`O3DSenderAudioCaptureComponent.cpp`, `O3DSenderComponent.cpp:409-422`, `:485-498`, `:629-650`).
6. **WP-A2e:** CORE-7 table CRC and CORE-18 builder reuse in `src/o3ds/model.cpp` (core PR, CTest benchmark).
7. Remove the `o3d.Sender.AsyncPipeline` fallback one release later.

## Verification / acceptance

- The Insights numbers in Decision §11 are recorded in the WP-A2c PR and meet the budgets.
- Tests (0006 test module): a scripted slow fake transport shows pipeline drops of the *oldest* frames and no gap in `tx_seq`; Stop/Start and rename under load produce correct descriptors (ADR 0005 acceptance); changing quantization ranges mid-capture forces exactly one full sync; destroying the component with a task in flight is clean under ASan (1,000 cycles).
- A review check finds no UObject pointer or `TWeakObjectPtr` dereference in `FO3DSenderPipeline` or `FO3DSenderSerializer`.
- With the tick-order change, a test that moves the root bone each frame shows the captured transform equals the same frame's evaluated pose.

## Open questions for the maintainer

1. **needs-verification:** the UE 5.7 `UE::Tasks::FPipe` API (`Launch` signature, `ETaskPriority::BackgroundHigh`, a wait-until-empty call) and whether background-priority tasks run on workers separate from foreground frame tasks. Default: use it as described; fall back to W2 (`FRunnable`) if Insights misses the budget.
2. **needs-verification:** `TG_PostUpdateWork` plus `AddTickPrerequisiteComponent(TargetMesh)` samples the final, physics-blended pose in 5.7, and whether `RegisterOnBoneTransformsFinalizedDelegate` exists and fires on the game thread. Default: tick group plus prerequisite.
3. Pipeline depth default 2 with drop-oldest? **Default: yes**, tunable by console variable.
4. Use engine timecode (`FApp::GetTimecode`) as the pose clock? **Default: no for v1**; `FPlatformTime::Seconds()` everywhere, timecode on LiveLink frames is WP-A4 (RCV-8).
5. Keep `OnSerializedFrame` (now on the worker) or delete it? **Default: keep for one release, documented as worker-thread, then delete** if nothing uses it.

## References

- Findings: SND-8, SND-9, SND-12, SND-17, SND-18, SND-22, SND-29, CORE-7, CORE-18, TRB-20, TRF-7, TRF-19; context SND-5, SND-14, SND-6.
- Files: `Plugin/Source/Open3DSender/Private/{O3DSenderComponent.cpp,O3DSenderSerializer.cpp,O3DSenderTransportController.cpp,O3DSenderCurveProcessor.cpp,O3DSenderAudioCaptureComponent.cpp}`; `Plugin/Source/Open3DSender/Public/{O3DSenderComponent.h,O3DSenderSerializer.h}`; `src/o3ds/model.cpp`.
- External (retrieved 2026-09-29, search snippets only; Epic pages not fetched directly):
  - Epic, "Tasks System References in Unreal Engine" and the `FPipe` API page (piped tasks run sequentially and need no extra synchronization; pipes are lightweight): https://dev.epicgames.com/documentation/unreal-engine/tasks-system-references-in-unreal-engine , https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FPipe
  - remiphilippe/mcp-unreal, a third-party UE 5.7 API mirror listing `RegisterOnBoneTransformsFinalizedDelegate`: https://github.com/remiphilippe/mcp-unreal/blob/main/docs/ue5.7/api/USkeletalMeshComponent.md
