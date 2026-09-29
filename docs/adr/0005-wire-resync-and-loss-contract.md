# 0005: Wire-coding resync and loss contract

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** Q2 (D1-era receivers are not kept compatible; D1 is unreleased and D8 will version the wire) and Q6 (no receiver keyframe request in v1; L2 stays deferred) were accepted as this ADR proposes. Needs-verification items stay open for the implementing WPs.
- **Date:** 2026-09-29
- **Plan decision:** D7 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [`resilient-streaming-and-motion-prediction.md`](../roadmap/resilient-streaming-and-motion-prediction.md) (owns the schema fields and workstreams A, C, D); D5 (sender threading), D8 (protocol versioning, not decided here); ADR 0003 (core compiled as `Open3DStreamCore`); feeds WP-S3, WP-S4, WP-A4, WP-T1

**Recommendation in one line:** every stateful update names the full sync it depends on (one new append-only field, `SubjectUpdate.ref_seq`). The sender always stamps `tx_seq`/`tx_wallclock_us`/`frame_epoch` through a core `StreamWriter`, sends a full descriptor on start, on change, on a new peer and at least once a second, and re-anchors quantization at each full sync. A receiver applies an update only when its `ref_seq` matches the last full sync it applied for that subject and, for residual updates, only when no sequence gap has occurred since. Otherwise it drops the update and waits for the next full sync. Residual coding is allowed only when the transport reports reliable, ordered delivery.

## Context

The sender has three encodings: legacy (a full `Subject` every frame), residual (C2) and quantized (D1), chosen in `FO3DSenderSerializer::SerializeFrame` (`Plugin/Source/Open3DSender/Private/O3DSenderSerializer.cpp:290-309`). Residual and quantized keep a persistent `O3DS::Subject` and send a full `Subject` only on the first frame, a skeleton-hash change or a curve-count change (`:390-392`, `:508-510`).

**Descriptor delivery is edge-triggered (SND-1).** The serializer learns bone names and parents only from `OnDescriptorReady` (`O3DSenderSerializer.cpp:69`, `:156-159`). `StopCapture` detaches and clears the serializer (`O3DSenderComponent.cpp:247-248`) but the component re-broadcasts only when the hash changes (`:877`, `:888-892`). The next frame is then built from an empty cache padded to the bone count with parent 0 and `NAME_None` (`O3DSenderSerializer.cpp:218-227`).

**Quantization anchors diverge (SND-2) and are never on the wire.** The sender anchors each `Transform` once, at its first `Serialize` (`src/o3ds/model.cpp:356-360`). `BuildSubjectFromDescriptor` calls `OutSubject.clear()` (`O3DSenderSerializer.cpp:235`), so every sender resync creates new transforms with new anchors. The receiver deliberately keeps old anchors by name across a resync (`model.cpp:1125-1130`, `:1187-1196`). Quantized deltas decode relative to the receiver's anchor (`model.cpp:1266-1285`). Two further consequences the findings do not state:
- A receiver that joins late anchors to the first full sync it sees (`model.cpp:1192-1196`, a transform with no preserved entry), which differs from the sender's first-ever anchor. Periodic full syncs alone would not fix late joiners.
- The sender anchors to its double-precision value, while the receiver anchors to the float32 value on the wire (`src/o3ds.fbs:24-28`). At large coordinates the float rounding is comparable to the byte tier's step (default range 0.01 over 127 steps, `O3DSenderComponent.h:244-247`).

**Curves are matched by index (SND-3).** Steady-state updates write curve values by index against the names from the last full sync (`O3DSenderSerializer.cpp:458-462`, `:557-561`); only a count change triggers a resync.

**No periodic resync (SND-13, CORE-10).** Residual keyframes are `SubjectUpdate`s and carry no topology; `ParseUpdateResidual` drops updates for unknown subjects (`model.cpp:1385`). The legacy delta and quantized paths mark a channel sent at serialize time, so a lost update for a channel that then stops moving is never repaired. The component header nonetheless calls quantization "stateless-across-loss and safe on unreliable transports" (`O3DSenderComponent.h:236-238`).

**Residual coding is not loss-tolerant (CORE-5, CORE-6).** The decoder's history advances on every applied update. With no history it falls back to a zero/identity reference even when the wire carries residuals (`src/o3ds/predict/residual_codec.cpp:104-124`), producing garbage that then feeds the predictor.

**Sequencing is dormant (SND-15, CORE-29).** The schema already has `tx_seq`, `tx_wallclock_us` and `frame_epoch` (`src/o3ds.fbs:203-209`). The plugin calls the per-`Subject` wrappers `Subject::Serialize(outbuf, t)` and `Subject::SerializeUpdate(...)`, which cannot write these fields at all (`model.cpp:762-781`, `:783-804`); `Subject::SerializeUpdateResidual` writes only `tx_seq` (`:827`), and the plugin passes none (`O3DSenderSerializer.cpp:340`, `:447`, `:468`, `:550`, `:571`). The frame counter exists but never reaches the wire (`O3DSenderComponent.cpp:973`). The receiver treats `tx_seq == 0` as legacy and bypasses the gate (`Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:577-585`).

**Epochs.** `NewSessionEpoch()` is wall-clock seconds; two restarts within one second reuse the epoch (`src/o3ds/sequencing.h:82-90`). `PostEditChangeProperty` does Stop then Start (`O3DSenderComponent.cpp:1226-1229`), so a counter reset on restart would look like a backward jump within one epoch and be dropped as stale.

**Receiver gaps are invisible after the gate.** `ReorderGate` gives up on a hole after `max_window`/`max_delay_s` and emits what it has (`src/o3ds/reorder_gate.h:70-74`); `EmitGatedFrame` then parses without knowing a frame was skipped (`O3DReceiverSource.cpp:679-688`). The legacy path parses, mutating codec state, before `ShouldProcessFrame` rejects the frame (`:605-619`).

**Scale (CORE-11).** `Subject::SerializeUpdate` has scale commented out (`model.cpp:579-587`) and the residual parser skips it (`:1479-1481`), while `ParseUpdate` already applies `scale` updates (`:1352`).

**Receiver skeleton cache (RCV-4, RCV-5).** The fast path fingerprints only bone count and parent ids (`O3DReceiverSource.cpp:1188-1193`) and reuses cached names on a match (`:1198-1202`). Every emitted packet re-pushes every subject in the shared scratch list (`:645`, `:740`).

**Transport delivery.** WebRTC is reliable by default and lossy when `webrtc.prefer_lossy` is set, with large packets switched back to the reliable channel (`Plugin/Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:1090`, `:620-760`). MoQ offers `MOQ_DELIVERY_DATAGRAM` and `MOQ_DELIVERY_STREAM` (`moq_ffi.h:87-90`). NNG opens pub, pair or push sockets (`Plugin/Source/Open3DTransportNNG/Private/Sender/NngSender.cpp:389-413`). The TCP sender listens and accepts clients mid-stream (`Plugin/Source/Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:235`, `:268-300`). No transport reports its delivery guarantee (`Plugin/Source/Open3DSender/Public/O3DSenderInterface.h:27-90`).

## Decision drivers

1. Anything the receiver applies is correct, or it is not applied. Silent corruption is worse than a held pose.
2. Recovery is bounded in time without a back-channel, since UDP, NNG pub/sub and MoQ relays are one-way.
3. Append-only schema; unset fields behave exactly like today's stream.
4. Core-first: the state machines live in `src/o3ds` and are tested with CTest (WP-T1).
5. Fit the resilient-streaming plan, and say where this ADR refines it.

## Options considered

### Anchor and topology identity
- **A1. Fixed anchor carried on the wire** (a new `Transform.quant_anchor`), plus a topology id. Keeps the D1 "anchor once" rule and suits old receivers that already saw the first sync. Needs two fields, and a root bone that travels far leaves the byte and half tiers and falls back to float32.
- **A2. Re-anchor at every full sync and have each update name its full sync (`ref_seq`) (chosen).** One field. Deltas stay small because the anchor follows the pose, and a lost full sync is detected instead of corrupting data. Old D1 receivers, which preserve anchors by name, misdecode a new sender's quantized stream after its first periodic resync. That is acceptable only because D1 has not shipped in a release, and D8 must version it (see Consequences).
- **A3. No schema change; rely on sequence gaps.** A gap does not tell the receiver whether the lost packet was a full sync. Rejected.

### Late joiners and loss recovery
- **L1. Periodic full sync only.** Simple, one-way, bounded by the interval.
- **L2. Receiver keyframe request (back-channel).** Faster recovery, but only TCP, WebRTC and NNG pair are bidirectional, and the resilient-streaming plan puts back-channels out of scope (C2 risks). Deferred.
- **L3. Periodic full sync plus a sender-local "new peer" trigger (chosen).** A transport that knows a peer joined (TCP accept, NNG pipe add, WebRTC participant) asks the sender for an immediate full sync. No back-channel is involved.

### Residual on lossy transports
- **R1. Block residual unless the transport reports reliable ordered delivery (chosen for v1),** as resilient-streaming C2(a) recommends.
- **R2. Keyframe-relative P-frames (C2(b)).** Later work, if needed.

## Decision

**(i) Pull-based descriptor delivery.** Each `FO3DSPoseFrame` carries a `TSharedPtr<const FO3DSSkeletonDescriptor>` and its hash, taken from the component's `DescriptorCache` after `EnsureSkeletonCache`. The serializer never depends on having seen `OnDescriptorReady` (the delegate stays for other listeners). This also suits D5, whose worker thread cannot call back into the component. `StopCapture` resets `CachedSkeletalMesh`, so the next start rebuilds the cache. A frame whose descriptor is missing or whose bone count differs from the descriptor is dropped with a rate-limited warning. It is never padded.

**(ii) Full-sync policy.** A full `Subject` is sent for a subject when any of these holds:
- first frame after `StartCapture` or a subject rename;
- the descriptor hash changed, or the curve **name list** changed (compare a hash of the names, not the count);
- `FullSyncIntervalSeconds` elapsed since the last full sync. This is a new `UPROPERTY` on `UO3DSenderComponent`: default **1.0 s**, clamp 0.25 to 10 s. It applies to the residual and quantized modes; legacy already sends a full `Subject` every frame;
- the transport signalled a new peer (item vi).

In residual mode a full sync also resets the encoder, so it doubles as a residual keyframe. `ResidualKeyframeIntervalFrames` keeps its meaning for cheaper predictor-only keyframes between full syncs. Per-frame curve filtering by epsilon/delta (SND-3) is disabled in residual and quantized modes; the delta threshold already suppresses unchanged curves.

**(iii) Residual only on reliable, ordered transports.**
- New enum `EO3DDeliveryGuarantee { Unknown, Unreliable, ReliableOrdered }` in `Open3DShared`.
- New member on `FO3DSenderTransportCustomization`: `TFunction<EO3DDeliveryGuarantee(const FO3DTransportConfig&)> GetDeliveryGuarantee`. It is evaluated on a built config, so it works in the details panel and at start. When D4/WP-A1 add the capability query (SHR-14), it moves there with the same values.
- Values for v1: Loopback, TCP → `ReliableOrdered`; UDP → `Unreliable`; NNG pair or push → `ReliableOrdered` and pub → `Unreliable` (pub drops messages for slow subscribers, **needs-verification** against NNG docs, Q3); WebRTC → `ReliableOrdered` unless `webrtc.prefer_lossy` is true (**needs-FFI-verification** that the LiveKit reliable channel is ordered, Q4); MoQ → `Unreliable` in both modes until ordering across objects in stream mode is verified (Q5). A missing function means `Unknown`, treated as `Unreliable`.
- **Runtime:** if residual is enabled and the guarantee is not `ReliableOrdered`, the sender logs one Warning naming the transport and uses quantized mode instead, which item (vii) makes loss-safe.
- **UI:** the residual checkbox shows a warning line with the same text, and the Residual tooltip (`O3DSenderComponent.h:215-219`) and the quantization tooltip (`:236-238`) are corrected.

**(iv) Stamping.**
- New core class `O3DS::StreamWriter` (`src/o3ds/stream_writer.h/.cpp`). It owns a `SequenceCounter`, the current epoch, and per-subject `last_full_seq`. It exposes `WriteFull`, `WriteUpdate` and `WriteResidual`, which always stamp `tx_seq`, `tx_wallclock_us` (via `NowUtcMicros()`) and `frame_epoch`, and set `ref_seq`.
- `FO3DSenderSerializer` owns exactly one `StreamWriter`, so one sender component (one transport) is one logical stream, as resilient-streaming A1.b requires. With fan-out (UDP multicast, NNG pub, MoQ relay) every receiver sees the same sequence, and a gap at any receiver means loss at that receiver.
- **The counter is not reset on Stop/Start within a process.** Each `StartCapture` sets `epoch = max(NewSessionEpoch(), previous_epoch + 1)`. This removes the same-second restart hazard in `sequencing.h:82-89` for in-process restarts.
- The default-zero overloads stay for other callers and are marked deprecated in comments (CORE-29).

**(v) Scale is sent.**
- The legacy delta path restores scale updates behind the delta threshold (uncomment and fix `model.cpp:579-587`), using the existing `scale:[ScaleUpdate]` field (`o3ds.fbs:147`); old readers already apply it (`model.cpp:1352`).
- In residual updates, `scale` carries **absolute** values (not residuals) and new readers apply them as such. Old residual readers ignore scale, as today.
- No schema change.

**(vi) New-peer trigger.** New optional callback on `IOpen3DSender`: `virtual void SetPeerJoinedCallback(TFunction<void()>)`, default no-op. The callback may fire on any thread; the serializer sets an atomic flag that the next frame consumes. TCP calls it on accept, NNG on pipe add (NNG sender already tracks pipe events, `NngSender.cpp:122`), WebRTC on participant join (**needs-FFI-verification**, Q4). MoQ and UDP don't implement it.

**(vii) Quantization anchors.**
- Both sides re-anchor at **every** full sync, and the anchor is the float32 value written on the wire.
- The sender rounds through `float` before storing `mQuantAnchorTranslation`. The receiver removes the preserve-by-name logic (`model.cpp:1125-1130`, `:1187-1196`) and anchors to the parsed value.
- Because every update names its full sync (item viii), a lost full sync is detected rather than silently mis-anchoring. This replaces the "anchor once, forever" rule in `model.cpp:339-355` and `o3ds.fbs:169-175`.
- The sender may keep its `Transform` objects across a resync when names and parents are unchanged (the SND-2 recommendation); that becomes an allocation optimisation, no longer a correctness requirement.

**(viii) Schema: one append-only field.**
```
table SubjectUpdate {
  ... existing fields ...
  // Appended (D7, ADR 0005): tx_seq of the SubjectList whose full Subject
  // this update is relative to (topology, curve list, quant anchors).
  // 0 = unset: the reader behaves exactly as before this field existed.
  ref_seq:ulong = 0;
}
```
Regenerate `src/o3ds_generated.h` with `flatc --cpp`, sync through `sync_o3ds_core.py` (ADR 0003), and add a `CHANGELOG.md` "Schema/Protocol" entry.

**(ix) Receiver contract** (core: `SubjectList` gains per-subject sync state; UE: glue).
- `SubjectList::Parse` gains an appended, defaulted `const ParseContext*` holding `tx_seq`, `frame_epoch` and `gap_before`. It returns, through an out-parameter, the subjects touched by this packet and whether each received a full descriptor (fixes RCV-5's "re-push everything").
- `EmitGatedFrame` computes `gap_before = (epoch == last_epoch && seq != last_emitted_seq + 1)` **before** parsing. A new epoch resets every subject in the stream to Unknown.
- When `tx_seq == 0` (legacy sender), or `ref_seq == 0` on an update, the receiver behaves exactly as today. D8 decides whether new receivers should refuse residual or quantized payloads that lack `ref_seq`.
- The legacy path's `ShouldProcessFrame` check moves before `Parse` (resilient-streaming A2, "gate decisions before `Parse()` mutates codec state").
- Skeleton cache (RCV-4): invalidated whenever the packet carried a full descriptor for that subject, and the fingerprint includes a hash of bone names so a legacy stream is also covered.
- While a subject is in NeedsKeyframe, the receiver pushes nothing for it. C1 concealment, or LiveLink's hold, covers the gap, and the gap counts toward the HUD loss metrics (A2.d).

### State machines

Sender, per subject (inside `StreamWriter` plus the serializer):
```
            StartCapture / rename
 [Idle] ─────────────────────────────► [NeedFull]
   ▲                                      │ frame with valid descriptor
   │ StopCapture                          │ → WriteFull(seq=S): re-anchor, reset encoder,
   │                                      │   last_full_seq = S
   │                                      ▼
   └──────────────────────────────── [Streaming] ── frame, nothing due ──► WriteUpdate/WriteResidual
                                          │           (ref_seq = last_full_seq)
          descriptor hash or curve names  │
          changed, interval elapsed,      │
          peer joined ────────────────────┘ → [NeedFull]
 frame with missing/mismatched descriptor: drop + rate-limited warning (any state)
```

Receiver, per subject within a stream (`frame_epoch`):
```
 [Unknown] ── full Subject at seq S ──► [Synced(ref=S)]
 [Synced(ref=R)]
    update, ref_seq == R, (residual: !gap_before) ─► apply, stay Synced
    residual keyframe update, ref_seq == R ─────────► reset decoder, apply, Synced(R)
    update, ref_seq != R ───────────────────────────► drop → [NeedsKeyframe]
    residual update and gap_before ─────────────────► drop → [NeedsKeyframe]
    full Subject at seq S' ─────────────────────────► re-anchor, invalidate skeleton cache → Synced(S')
 [NeedsKeyframe]
    any update ─────────────────────────────────────► drop (count), conceal/hold
    full Subject at seq S' ─────────────────────────► Synced(S')
    residual keyframe with ref_seq == R ────────────► Synced(R)
 frame_epoch increases ─► every subject in the stream → [Unknown]
```

## Consequences

- **Correctness:** worst case after loss or a late join is one full-sync interval (default 1 s) of held or concealed pose, never corrupted pose. WP-A4's acceptance ("bounded error that recovers at the next keyframe") becomes testable.
- **Bandwidth:** one full descriptor per second per subject. For a 100-bone subject that is a few KB per second, small next to 60 Hz updates; measured in WP-T1.
- **Compatibility and D8:** old receivers ignore `ref_seq`. A **D1-era receiver** (preserve-anchor logic) receiving from a new quantized sender misdecodes after the first periodic resync. D1 exists only on `develop`, so D8 must bump the wire protocol version with this change and old readers must reject quantized and residual payloads (CORE-16). D7 depends on D8 for that rejection, not for the rest of the contract.
- **Refinements of `resilient-streaming-and-motion-prediction.md`:**
  - D1 says quantization is "stateless-across-loss" and safe on unreliable transports without further work. This ADR makes it loss-safe only together with periodic full syncs and `ref_seq`, and replaces D1's fixed "anchor once" rule with re-anchoring at each full sync.
  - §2.1 leaned toward "has `subjects`" as the keyframe marker. This ADR keeps that for full syncs and adds `ref_seq` so updates can name theirs.
  - C2(a) lists "MoQ reliable streams" as reliable. This ADR treats MoQ as `Unreliable` until ordering is verified (Q5).
  - A1.b says the counter resets on a new session. This ADR keeps the counter monotonic within a process and makes the epoch strictly increase instead.
  - Nothing here conflicts with A1 (gate), A2 (clock mapping), B (capture) or C1 (concealment); C1 gains a clear trigger (NeedsKeyframe).
- **Not decided here:** a receiver keyframe request (L2), keyframe-relative P-frames (R2), CORE-12 to CORE-15 details (tier choice, normalisation, determinism, forged `tx_seq`), which stay in WP-A4.

## Implementation outline

1. **WP-A4a (core, first):** `ref_seq` in `src/o3ds.fbs`, regenerate `src/o3ds_generated.h`; `StreamWriter` (`src/o3ds/stream_writer.*`); float-rounded re-anchoring in `Subject::Serialize`; remove anchor preservation in `ParseSubject`; `ParseContext`, touched-subject output and per-subject sync state in `SubjectList` (`src/o3ds/model.h/.cpp`); scale in `SerializeUpdate` and absolute scale in the residual path; `ResidualDecoder` returns "needs keyframe" instead of the zero-reference fallback (`src/o3ds/predict/residual_codec.*`). Tests in `test/` (WP-T1).
2. **WP-S3 (sender):** descriptor on the pose frame, drop on mismatch, `CachedSkeletalMesh` reset (`O3DSenderComponent.h/.cpp`, `O3DSenderSerializer.h/.cpp`); `FullSyncIntervalSeconds`; curve-name hash; no curve filtering in persistent modes (`O3DSenderCurveProcessor.cpp`); serializer uses `StreamWriter` with the epoch rule.
3. **WP-S3 / WP-A4b (transports):** `EO3DDeliveryGuarantee` (`Open3DShared/Public`), `GetDeliveryGuarantee` per customization, residual fallback and UI warning; `SetPeerJoinedCallback` on `IOpen3DSender` with TCP and NNG implementations (WebRTC after Q4).
4. **WP-S4 (receiver):** `gap_before` and the `ParseContext` in `EmitGatedFrame`; legacy `ShouldProcessFrame` before parse; push only touched subjects; skeleton-cache invalidation (`O3DReceiverSource.h/.cpp`).
5. **WP-A4c:** D8 version bump shipped in the same release as step 1.

## Verification / acceptance

- **Core (CTest, WP-T1), using `channel_model`:**
  - quantized stream with 5 % loss: no applied translation error above the tier tolerance, and every subject resynchronised within one interval of the first post-loss full sync;
  - residual stream with one dropped update: the receiver enters NeedsKeyframe, applies nothing until the next keyframe, then matches within tolerance (replaces the PoC's growing error);
  - late join at frame 30 on residual and on quantized: nothing applied before the first full sync, then correct;
  - a lost full sync: later updates are dropped by `ref_seq` mismatch, not misdecoded;
  - old-reader and new-writer both ways with `ref_seq = 0`: identical to today.
- **UE (D10 test module):** Stop/Start and rename produce correct names and parents; two Stop/Start cycles within one second produce strictly increasing epochs and no stale drops; the UDP transport with residual enabled logs the fallback warning and emits quantized updates; the receiver republishes renamed bones.
- Review check: every `Serialize*` call in `Plugin/Source` goes through `StreamWriter`.

## Open questions for the maintainer

1. ~~Is 1.0 s the right default full-sync interval, and should it vary per transport?~~ **Answered 2026-09-29:** 1.0 s is fine. It stays a single `FullSyncIntervalSeconds` property on the sender component (clamp 0.25 to 10 s), with no per-transport default.
2. **Accepted default:** yes. Original question: accept that D1-era receivers break against new quantized senders, given D1 is unreleased and D8 will version it?
3. **needs-verification (NNG docs):** does an NNG pub socket drop messages for slow subscribers, and are pair and push delivery ordered and reliable over TCP?
4. **needs-FFI-verification (livekit_ffi):** is LiveKit's reliable data channel ordered, and does the FFI expose a participant-joined event for the new-peer trigger?
5. **needs-FFI-verification (moq-ffi):** in `MOQ_DELIVERY_STREAM`, are objects on one track delivered in order across groups through a relay? Until confirmed, MoQ is `Unreliable`.
6. **Accepted default:** not in v1; L2 stays deferred. Original question: should a receiver keyframe request (L2) be planned for bidirectional transports after v1?

## References

- Findings: SND-1, SND-2, SND-3, SND-13, SND-15, CORE-5, CORE-6, CORE-10, CORE-11, CORE-29, RCV-4, RCV-5; context CORE-12, CORE-13, CORE-14, CORE-15, CORE-16.
- Files: `src/o3ds.fbs`; `src/o3ds/model.h`, `model.cpp`; `src/o3ds/sequencing.h`; `src/o3ds/reorder_gate.h`; `src/o3ds/predict/residual_codec.h/.cpp`; `Plugin/Source/Open3DSender/Private/O3DSenderSerializer.cpp`, `O3DSenderComponent.cpp`, `Public/O3DSenderComponent.h`, `Public/O3DSenderInterface.h`; `Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp`; `Plugin/Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp`; `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/include/moq_ffi.h`; `Plugin/Source/Open3DTransportNNG/Private/Sender/NngSender.cpp`; `Plugin/Source/Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp`.
- Plan: `docs/roadmap/resilient-streaming-and-motion-prediction.md` §2.1, §3 (A1.b, A1.c, A2.a), §5 (C1, C2), §6 (D1).
- External: none fetched for this ADR.
