# 0013: Timecode on LiveLink frames

- **Status:** Accepted (maintainer sign-off 2026-10-04)
- **Implemented:** #366 (PR 1, core), #367 (PR 2, sender), #368 (PR 3a, receiver frames), #369 (PR 3b, control alignment in Timecode mode). The manual test at the desk (Verification) remains.
- **Accepted with defaults:** the open questions below are accepted with the default given next to each. The needs-verification item stays open and is resolved in PR 3; a result that invalidates the decision is handled by a superseding ADR.
- **Date:** 2026-10-03
- **Plan decision:** RCV-8 in WP-A4 of [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md)
- **Related:** [ADR 0008](0008-sender-pipeline-threading.md) (sampling clock; its open question 4 said "no engine timecode in v1", which this ADR replaces for the wire and LiveLink), [ADR 0009](0009-protocol-versioning.md) (appended wire fields), [ADR 0005](0005-wire-resync-and-loss-contract.md) (mapped presentation time, A2.c)

**Decision in one line:** the sender stamps its engine timecode, when it has one, on every frame in a new optional `SubjectList` field; the receiver sets LiveLink's `SceneTime` from it, and when a frame has none, derives one from the frame's mapped presentation time at the engine's timecode rate.

## Context

**What the receiver does today.** `FO3DLiveLinkPublisher::PushFrameData` sets `MetaData.SceneTime = FQualifiedFrameTime()` on every frame, which is frame 0 at 24 fps, and adds two `Printf` string metadata entries per frame (`CurveHash`, `SubjectListTime`) (`Plugin/Source/Open3DReceiver/Private/O3DLiveLinkPublisher.cpp:229-232`). RCV-8 (`docs/review/2026-09-plugin-review/receiver.md:94-101`) cites the older location in `O3DReceiverSource.cpp`.

**What that does in LiveLink's Timecode mode (UE 5.7, verified in engine source).**
- On insert, Timecode mode rejects a frame only when `SceneTime.Time.FloorToFrame() < 0`, orders by `SceneTime`, and warns once about equal timecodes; a change of `SceneTime.Rate` flushes the subject's buffer (`Engine/Plugins/Animation/LiveLink/Source/LiveLink/Private/LiveLinkSubject.cpp:423-441, 589-690`).
- The snapshot reads at `FApp::GetCurrentFrameTime()` minus the configured offsets (`LiveLinkSubject.cpp:162-187`), and the closest-frame lookup compares `SceneTime` (`:896-959`).
- With every frame at frame 0, the lookup always returns the newest frame: Timecode mode silently behaves like Latest, with no synchronization.

**What the wire carries.** `SubjectList.time` is the sender's `FPlatformTime::Seconds()` at sampling (ADR 0008 item 7), and `tx_wallclock_us` is UTC at transmit (`src/o3ds.fbs:205-223`). There is no timecode field. The sender never reads engine timecode: `FO3DSPoseFrame` has no timecode (`Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:160-220`), and ADR 0008 deferred it to WP-A4 (`docs/adr/0008-sender-pipeline-threading.md:111, 158`).

**What the engine offers.** `FApp::GetCurrentFrameTime()` returns a `TOptional<FQualifiedFrameTime>`, set each engine frame only while the `UTimecodeProvider` is synchronized (`Engine/Source/Runtime/Core/Public/Misc/App.h:737`; `Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp:2984-2997`). The sender samples on the game thread (`HandleBoneTransformsFinalized`, `Plugin/Source/Open3DSender/Private/O3DSenderComponent.cpp:1078-1106`), where it can read it.

**How Unreal's own network sources do it.** LiveLink Face, the ARKit face source, Master Lockit, Preston MDR and OpenTrackIO carry the sender's `FQualifiedFrameTime` on their wire and set `SceneTime` from it; the MessageBus source passes `SceneTime` through unchanged. Local-device sources (OpenVR, LiveLinkInputDevice, the Blueprint virtual subject) stamp the receiving engine's timecode at push. (Paths in the RCV-8 research notes; for example `LiveLinkFaceSource.cpp:374`, `AppleARKitLiveLinkSource.cpp:227-228`.)

**Protocol state.** Protocol 2 is unreleased (`docs/wire-format.md` section 8), so an appended field joins it without a version change.

## Decision drivers

1. Frames must line up with other timecoded sources (cameras, audio, other machines) in Take Recorder and Sequencer when the sender has timecode.
2. Timecode mode must work, never silently act as Latest.
3. A sender without a synchronized timecode provider must keep working, with nothing to configure.
4. One wire change at most, while protocol 2 is unreleased.

## Options considered

### Option A: receiver stamps its own engine timecode at push
- Pros: receiver only, no wire change.
- Cons: the timecode includes network and gate delay; frames arriving in one engine tick share a timecode; it records when the frame arrived, not when it was performed. Fails driver 1.

### Option A': receiver derives timecode from the mapped presentation time
`SceneTime = EngineTC(now) + (mappedWorldTime - FApp::GetCurrentTime()) × rate`, fractional.
- Pros: receiver only; keeps the sender's frame spacing and the gate's de-jitter.
- Cons: still receiver timecode, not the performance's. A fallback, not a sync.

### Option B: sender timecode on the wire, with A' as the fallback
- Pros: meets every driver; matches Unreal's own network sources.
- Cons: a wire field, sender and receiver changes, and a new ADR (this one) because ADR 0008 said no.

### Option C: map `SubjectList.time` to a timecode at a configured rate
RCV-8's recommendation as written.
- Cons: `SubjectList.time` is the sender's seconds since boot, so the result lines up with nothing; it only gives Timecode mode monotonic ordering, which A' also gives without a setting.

## Decision

**Option B, with A' as the fallback.**

1. **Wire.** `SubjectList` gains an optional `scene_time`: a FlatBuffers struct of frame number (int32), sub-frame (float) and rate numerator and denominator (int32), the fields of `FQualifiedFrameTime`. Absent means the sender had no timecode. Appended, part of protocol 2; `min_reader_version` unchanged (a reader that ignores it loses nothing it had). The core API gets a matching optional parameter on the serialize functions and `StreamWriter`.

2. **Sender.** `HandleBoneTransformsFinalized` reads `FApp::GetCurrentFrameTime()` together with the sampling time and stores it, when set, on `FO3DSPoseFrame`; the serializer writes it. Not `FApp::GetTimecode()`, which returns a default when no provider is synchronized. A sender without a synchronized provider writes nothing: no setting, no warning.

3. **Receiver.**
   - A frame with `scene_time` sets `MetaData.SceneTime` from it.
   - A frame without it gets option A': the mapped presentation time (or arrival time on the ungated path) converted to the engine's current timecode rate, as a fractional frame time, when the engine has a timecode; otherwise `SceneTime` stays unset, as today.
   - One rate per subject: a subject keeps the rate of its first frame with a timecode, and frames at another rate are converted to it, because a rate change flushes LiveLink's buffer.
   - Concealment and render-ahead frames (`PublishSyntheticFrame`) get `SceneTime` on the same timeline and rate.
   - The two per-frame `Printf` string metadata entries are removed (RCV-11); the curve hash stays internal.
   - Control alignment in Timecode mode (`GetPresentedSenderTimeUs`, `Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:1053-1058`) uses the frame's `SceneTime` once it is meaningful, and `USER_GUIDE.md:837` is updated.

4. **Docs.** The user guide explains Timecode mode: it synchronizes only when the sender has a synchronized timecode provider (genlock or LTC through `UTimecodeProvider`), and both machines' timecode sources agree; otherwise it orders frames by the derived timecode.

## Consequences

- **Easier:** Take Recorder and Sequencer line received mocap up with other timecoded sources; Timecode mode stops silently acting as Latest; per-frame string allocations on the receiver go.
- **Harder:** a wire field and core API parameter; rate handling per subject; the fallback depends on the receiving engine having a timecode.
- **Unchanged:** EngineTime and Latest modes (they use `WorldTime`, not `SceneTime`); senders without timecode providers.

## Implementation outline

1. **PR 1, core:** the `scene_time` struct and field, core serialize and `StreamWriter` parameters, `PeekPacketMeta` exposure, wire-format docs and CHANGELOG Schema/Protocol entry. Tests: round trip, absent means unset, `min_reader_version` unchanged.
2. **PR 2, sender:** read the frame time at sampling, carry it on the pose frame, write it. Test: a component with a fake synchronized timecode provider stamps it; without one, nothing.
3. **PR 3, receiver:** `SceneTime` from the field, the A' fallback, per-subject rate, synthetic frames, string metadata removed, control alignment, user guide. Tests through the real receiver source: frames with timecode keep it; frames without get a monotonic derived one; a rate change does not reach LiveLink as a rate change.

## Verification / acceptance

- In Timecode mode with a shared timecode provider, received frames are selected by the sender's timecode (manual test at the desk, with the steps in the user guide).
- Without a timecode on either side, behaviour in EngineTime and Latest modes is unchanged; all existing tests pass.

## Open questions for the maintainer

1. **Do you use Take Recorder or Sequencer with timecode today, and with which provider** (genlock card, LTC, `USystemTimeTimecodeProvider`)? It decides the manual test setup. **Accepted default:** test with `USystemTimeTimecodeProvider` on both machines.
2. **Fallback when the receiving engine has no timecode:** leave `SceneTime` unset (as today), or synthesize one at a fixed rate so Timecode mode still orders frames? **Accepted default:** leave it unset; Timecode mode without an engine timecode is already flagged by LiveLink's own warning.
3. **needs-verification:** how `FApp::GetCurrentTime()` relates to `FPlatformTime::Seconds()` under a fixed timestep, which affects the A' conversion; checked in PR 3. **Resolved (PR 3a):** normally `FApp::GetCurrentTime()` is `FPlatformTime::Seconds()` at the start of the frame (`Engine/Source/Runtime/Engine/Private/UnrealEngine.cpp:2725-2726`); under a fixed timestep or `t.OverrideFPS` it advances by the fixed delta instead and drifts from the platform clock (`:2713-2720`, `:2902-2907`). The A' conversion therefore uses only the platform clock (WorldTime is in that domain) and one offset to the engine timecode, taken once and again only when it drifts by more than a frame. Whole-frame providers (`USystemTimeTimecodeProvider`'s default, `bGenerateFullFrame`) give equal timecodes within a frame; LiveLink keeps such frames in arrival order and warns once (`LiveLinkSubject.cpp:640-690`), and the user guide says how to get sub-frames.

## References

- Findings: RCV-8, RCV-9 (resolved by the same research: LiveLink evaluates on the pushed `WorldTime` after a source-wide offset estimate), RCV-11.
- UE 5.7: `Engine/Source/Runtime/LiveLinkInterface/Public/LiveLinkTypes.h:110-244`, `LiveLinkSourceSettings.h:40-198`; `Engine/Plugins/Animation/LiveLink/Source/LiveLink/Private/LiveLinkSubject.cpp`, `LiveLinkClient.cpp:1196-1211`, `LiveLinkTimedDataInput.cpp:206-243`.
