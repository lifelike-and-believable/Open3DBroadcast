# 0011: Control channel for events and values

- **Status:** Accepted (maintainer sign-off 2026-10-01)
- **Accepted with defaults:** every open question below that was not already answered was accepted with the recommended default given next to it. Needs-verification items stay open and are resolved in the implementing WPs; a result that invalidates a default is handled by a superseding ADR.
- **Date:** 2026-09-30
- **Plan decision:** D11 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3; work package **WP-CTL** (§ Implementation outline).
- **Related:** [ADR 0005](0005-wire-resync-and-loss-contract.md) (full-sync interval, epochs, new-peer trigger, delivery guarantee), [ADR 0007](0007-transport-abstraction-and-registry.md) (interfaces, capability query, send queue "control" items, receive demux), [ADR 0008](0008-sender-pipeline-threading.md) (pose pipeline that control must bypass), [ADR 0009](0009-protocol-versioning.md) (envelope, bump table, CHANGELOG, compatibility matrix), [ADR 0004](0004-credentials-and-secret-transport-options.md) (transport authentication), [ADR 0006](0006-test-module-layout-and-fakes.md) (conformance suite, fakes)

**Recommendation in one line:** add a one-way, sender-to-receivers **control stream** next to mocap and audio. It carries two message classes with different loss contracts. **Values** are keyed, last-writer-wins state that the sender re-sends as a periodic snapshot, so late joiners and lossy links converge. **Events** are fire-once messages with an id, receiver-side de-duplication, a time-to-live and redundant copies on unreliable transports. On the wire, control is a new envelope kind (`Control = 2`) whose payload is its own FlatBuffers root (`file_identifier "O3DC"`, `src/o3ds_control.fbs`). `SubjectList` is untouched. The state machines live in core and are tested with CTest. In UE, the sender component gets `FireControlEvent` and `SetControlValue`. Receivers reach gameplay through a game-thread `FO3DControlBus` and a `UO3DRemoteControlComponent`, which mirrors the audio chain. Cues and value changes are held by default so they play with the mocap they were fired against. Receiving control is off by default, and a client turns it on itself through a project setting that ships in `DefaultGame.ini` or a runtime Blueprint/C++ call. It is filtered by an allowlist and rate-limited, and **never** sets properties by reflection or runs console commands.

## Context

**What it is for (maintainer, 2026-09-30).** From the sender, trigger VFX, lighting and audio cues on remote clients, and change environment and character parameters there. These map onto the two classes in item 1:

| Use | Class | Example key or name | `TargetSubject` |
|---|---|---|---|
| VFX cue | Event | `vfx.muzzle_flash` | the character, or none |
| Lighting cue | Event | `light.cue` with an `Int` cue number | none |
| Audio cue | Event | `audio.sting` with a `Name` payload | none, or the character |
| Environment parameter | Value | `env.fog_density` (`Double`), `env.time_of_day` (`Double`), `env.sky_tint` (`Color`) | none |
| Character parameter | Value | `char.emotion` (`Name`), `char.wetness` (`Double`), `char.prop_visible` (`Bool`) | the character's subject name |

Two needs follow that a generic message bus would not cover. Cues should land close to the motion and audio they accompany (item 9, *Timing*). And parameters are often driven every tick, by a slider, a curve or a timeline, so the sender must coalesce them (item 9, *Coalescing*).

**Direction and reachability.**
- UDP, NNG pub/sub and MoQ relays have no back-channel (ADR 0005, decision driver 2 and option L2; ADR 0009, option V4).
- So the only direction that works on every transport is sender to receivers. "Broadcast clients" are the receivers: `FO3DReceiverSource` (a LiveLink source, `Plugin/Source/Open3DReceiver/Public/O3DReceiverSource.h:30`) and the gameplay components that listen to it.

**What the stream carries today.** Mocap and audio only.
- `EUnifiedKind` has `Mocap = 0` and `Audio = 1` (`Plugin/Source/Open3DShared/Public/O3DUnifiedMessage.h:10-14`), and `EUnifiedCodec` has `O3DS`, `Opus` and `PCM16` (`:17-22`).
- The envelope is version 1 (`O3DA`, big-endian; `:24-51`, `:131-159`). Only audio is wrapped (`Plugin/Source/Open3DShared/Private/O3DAudioFrameCodec.cpp:458`).
- ADR 0009 item 4 already says "zero-length payloads are allowed for a future control kind". ADR 0007 item 7 already gives `FO3DSendQueue` a "control" item type that "is never dropped for mocap".
- Neither ADR has been implemented: there is no `src/o3ds/wire_format.h` and no `O3DS_PROTOCOL_VERSION` in `src/`. `O3DS_VERSION_TAG` is still `1.0.4` (`CMakeLists.txt:20`). WP-A1 PR 1 (#289) moved `IOpen3DSender`, `IOpen3DReceiver` and `ISerializedFrameConsumer` into `Plugin/Source/Open3DShared/Public/Transport/` with one registry (`O3DTransportRegistry.h`), leaving forwarding shims in Sender and Receiver. It kept `O3D_TRANSPORT_API_VERSION` at 1 because no layout changed (`O3DTransportApiVersion.h`, History).

**How each transport moves non-mocap data today.**

| Transport | Audio path | Receive classification | Evidence |
|---|---|---|---|
| TCP | envelope in the TCP frame | `ParseUnifiedMessage`, then by kind; an unknown kind falls through and returns false | `Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:437-473` |
| UDP | envelope in a datagram | same; unknown kind falls through | `SocketsUdpReceiver.cpp:469-490` |
| NNG | envelope | same; unknown kind falls through | `Open3DTransportNNG/Private/Receiver/NngReceiver.cpp:524-550` |
| Loopback | separate `AudioQueue` on the in-process channel | per queue | `Open3DTransportLoopback/Private/Shared/LoopbackChannel.h:53-54`, `Receiver/LoopbackReceiver.cpp:54-120` |
| MoQ | separate publisher on an `audio/<session>` track | per subscription | `Open3DTransportMoQ/Private/Sender/MoQSender.h:33-43`, `Shared/MoQHelpers.h:95-98` |
| WebRTC (add-on) | LiveKit audio tracks; mocap on data channels **labelled with the subject name** | the label becomes the subject and the bytes go straight to the mocap consumer | `Open3DBroadcastWebRTC/.../Receiver/WebRTCReceiver.cpp:165-185`, `Sender/WebRTCSender.cpp:532-542` |

**What an old receiver does with a message it does not understand.**
- **TCP, UDP and NNG** drop an envelope of unknown kind without logging (table above).
- **Payload size:** the TCP receiver treats *any* zero-payload envelope as a keepalive before it looks at the kind (`SocketsTcpReceiver.cpp:439-445`). The keepalive itself is a kind-Audio envelope with size 0 (`Open3DTransportSockets/Private/Shared/SocketsTcpTransport.h:30-55`). A control payload must therefore never be empty.
- **WebRTC:** a raw byte string sent to an old receiver reaches `FO3DReceiverSource::HandleSerializedFrame`. `PeekPacketMeta` rejects it, and the receiver then logs an **unthrottled Warning per message** and counts a deserialization error (`Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:626-631`). Bytes that start with the envelope magic can never parse as a frame: `SubjectList::Parse` requires the first word to be 1 (ADR 0009 Context, `src/o3ds/model.cpp`).

**Size limits.**
- WebRTC: "lossy ≤ ~1300 bytes, reliable ≤ ~15 KiB" (`.../ThirdParty/livekit_ffi/include/livekit_ffi.h:330-345`; also `WebRTCSender.cpp:56`).
- UDP: a datagram over the fragment threshold is split, and the reassembly path is being hardened separately (WP-S2).

**Senders.**
- One `UO3DSenderComponent` owns one transport instance (`Plugin/Source/Open3DSender/Private/O3DSenderTransportController.h:16-31`), so one component is one logical stream (ADR 0005 iv).
- The full-sync interval already exists as `FullSyncIntervalSeconds`, default 1.0 s (`Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:95`, `:323`).
- ADR 0005's `SetPeerJoinedCallback` and `EO3DDeliveryGuarantee` are **not yet in code**: a search of `Plugin/Source` finds neither.
- Every in-band sender treats `SendSerialized` bytes as opaque. TCP enqueues them (`SocketsTcpSender.cpp:299-312`). UDP records `LastSubject` and sends (`SocketsUdpSender.cpp:305-322`). NNG records per-frame metrics (`NngSender.cpp:321-342`).

**The audio chain is the model for a game-facing receive path.**
- A receiver audio sink, callable on any thread, hops to the game thread (`O3DReceiverSource.cpp:153`).
- From there it goes to `FO3DAudioBus`, a game-thread-only multicast delegate (`Plugin/Source/Open3DShared/Public/O3DAudioBus.h:13-31`), and then to `UO3DRemoteAudioComponent`, which filters by subject or stream (`Plugin/Source/Open3DReceiver/Public/O3DRemoteAudioComponent.h:35-119`).

**Untrusted input (theme T1).** Every receive path takes bytes from the network. UDP and NNG carry no authentication. WebRTC and MoQ authenticate the connection, not the sender's intent (ADR 0004).

## Decision drivers

1. **Correct or not applied** (ADR 0005 driver 1): a receiver never shows a stale value as current and never fires an event twice.
2. Works on every transport without a back-channel, with recovery bounded by one snapshot interval.
3. A new sender never breaks an old receiver's mocap, and an old sender is invisible to a new receiver.
4. Safe on hostile input: verified, bounded, rate-limited, and no remote code path into reflection or the console.
5. Core first: codec and state machines in `src/o3ds`, tested with CTest and fuzzed (WP-T1).
6. Zero cost when unused: no bytes on the wire and no per-frame work unless gameplay calls the control API.
7. Fits WP-A1 (ADR 0007) without waiting for it: the code is per-transport now and folds into the shared queue and demux later.

## Options considered

### Direction
- **D1. One-way, sender to receivers (chosen).** Works on all six transports.
- **D2. Bidirectional (receiver acks, requests, RPC).** Only TCP, WebRTC and NNG pair can carry it (ADR 0005 L2). Deferred; see Open questions.

### Delivery semantics
- **S1. One "message" class, best effort.** Simple, but a value set once is lost for good on UDP and for every late joiner. That fails driver 1.
- **S2. Values as state plus events as messages (chosen).** Values converge through periodic snapshots, the same mechanism as ADR 0005's full sync. Events are de-duplicated and carry a TTL, and redundant copies make them likely, though not certain, to arrive on lossy links.
- **S3. Reliable delivery with acks.** Needs D2. Rejected for v1.

### Payload encoding
- **P1. A new root FlatBuffer with its own schema and identifier (chosen).** It reuses the Verifier and the vendored runtime. Old readers can never mistake it for a `SubjectList`, and `SubjectList` and its `min_reader_version` (ADR 0009 item 1) are unaffected.
- **P2. New tables or fields on `SubjectList`.** These would ride with mocap frames, which the pose pipeline drops oldest-first (ADR 0008). A `SubjectList` carrying only control would also look to old receivers like an empty mocap frame. Rejected.
- **P3. A hand-written TLV.** Needs a new parser to harden and fuzz, with no gain. Rejected.

### Carriage
- **C1. In-band envelope kind on every transport.** Natural for TCP, UDP and NNG. On WebRTC the data label would still be read as a subject name.
- **C2. Separate channel or track per transport.** Natural for MoQ (it already has an audio track) and Loopback (it already has an audio queue).
- **C3. The envelope everywhere, carried on each transport's natural channel (chosen).** Control bytes are always enveloped, so classification never depends on a label, a track name or a subject name. The channel is the in-band stream for TCP, UDP and NNG, a third queue for Loopback, a `control/` track for MoQ, and a reserved data label for WebRTC.

### Applying values in gameplay
- **G1. Set UObject properties by path or name through reflection.** This is the obvious design for "set values". It is rejected because it gives any peer on the network write access to arbitrary object state, and on UDP and NNG that peer is unauthenticated.
- **G2. Typed values delivered to Blueprint delegates and a queryable cache (chosen).** The game decides what each key means.
- **G3. Console commands.** Rejected for the same reason as G1.

## Decision

**1. Scope.** One control stream per sender component, one-way to every receiver of that stream. Two message classes:

| | Value ("set values") | Event ("fire events") |
|---|---|---|
| Identity | `key` (UTF-8, ≤ 128 bytes), optional `target` subject | `name` (UTF-8, ≤ 128 bytes), optional `target` subject, `event_id` |
| Semantics | last writer wins per `(source, key, target)`; clear removes the key | at most once per `(source, epoch, event_id)` |
| Loss recovery | periodic complete snapshot; new-peer snapshot once ADR 0005 (vi) lands | redundant copies on unreliable transports; none on reliable ones |
| Late joiner | gets the current table within one snapshot interval | never sees events older than the TTL |
| Payload | one typed value | an optional typed value |

**2. Value types.** `None`, `Bool`, `Int` (int64), `Double`, `String` (UTF-8, ≤ 512 bytes), `Name` (≤ 128 bytes), `Vector3` (3 × double), `Quat` (4 × double), `Transform` (translation, rotation as a quaternion, scale), `Color` (linear RGBA, 4 × float) and `Bytes` (≤ 512 bytes). Doubles follow UE's large-world coordinates. UE maps them through `FO3DControlValue` (item 8).

**3. Schema (`src/o3ds_control.fbs`, new, append-only from day one).**
```
namespace O3DS.Control;
file_identifier "O3DC";

struct Vec3 { x:double; y:double; z:double; }
struct Quat { x:double; y:double; z:double; w:double; }
struct Color { r:float; g:float; b:float; a:float; }
table BoolV { v:bool; }        table IntV { v:long; }       table DoubleV { v:double; }
table StringV { v:string; }    table NameV { v:string; }    table Vec3V { v:Vec3; }
table QuatV { v:Quat; }        table ColorV { v:Color; }    table BytesV { v:[ubyte]; }
table TransformV { t:Vec3; r:Quat; s:Vec3; }
union Value { BoolV, IntV, DoubleV, StringV, NameV, Vec3V, QuatV, TransformV, ColorV, BytesV }

table Entry  { key:string (required); target:string; value:Value; version:ulong; }
table Clear  { key:string (required); target:string; version:ulong; }
table Event  { event_id:ulong; name:string (required); target:string; value:Value; ttl_ms:uint = 2000;
               time_us:ulong; }   // fire time, sender clock; 0 = the message's sender_time_us

table ControlMessage {
  protocol_version:ushort = 1;        // control protocol the writer speaks
  source_id:string (required);        // transient per-component FGuid (see below)
  source_name:string;                 // display only (actor/component name)
  epoch:uint;                         // ADR 0005 (iv) rule: max(NewSessionEpoch(), previous + 1)
  seq:ulong;                          // per source, monotonic within the process, never reset
  sender_time_us:ulong;               // sender clock (ADR 0009 item 7)
  tx_wallclock_us:ulong;              // UTC at transmit, diagnostics only
  mocap_subjects:[string];            // subjects this sender streams (≤ 16); used to align to that mocap stream
  set:[Entry];
  clear:[Clear];
  events:[Event];
  snapshot_id:uint;                   // 0 = not part of a snapshot
  snapshot_part:ushort;
  snapshot_parts:ushort;              // > 0 when snapshot_id != 0
  snapshot_seq:ulong;                 // publisher seq at which the table was captured; same in every part
}
root_type ControlMessage;
```
- `version` on `Entry` and `Clear` is the `seq` of the message that first carried that change. Snapshots re-send the original version, so a snapshot never overrides a newer live set. A value that has not gone out live yet (new, or changed and still waiting on the live budget or the per-key rate) goes out in the snapshot with the snapshot's capture point (`snapshot_seq`) as its version. Without that, a sender whose live budget never reaches some keys would never deliver them.
- `Event.time_us` is when the event was fired. Redundant copies travel in later messages, so the TTL and alignment use the fire time, not the time of the message carrying the copy.
- A snapshot is the table captured at one instant (`snapshot_seq`). The publisher copies the table once and splits that copy into parts, so parts never mix two states.
- `source_id` is an `FGuid` that the component creates when it registers. It is `Transient` and never serialized, so a duplicated or copy-pasted actor gets its own id and two senders never merge state. This is the same per-instance approach audio uses (`FGuid::NewGuid()`, `Open3DTransportLoopback/Private/Sender/LoopbackSender.cpp:133`). Stop and Start keep the id; the epoch tells sessions apart.
- `source_id` lives in the payload because LiveKit's data callback passes a label and reliability, not a participant (`livekit_ffi.h:94`). Several senders can share one room, relay or NNG bus.
- Readers verify with the identifier. They reject `protocol_version` greater than they support, logging once per source, and ignore unknown fields as FlatBuffers allows.
- **New value types are append-only and do not need a protocol bump.** The generated verifier accepts an unknown union member. A reader marks that entry or event *unsupported*: it skips the item and counts it, and does not reject the message. A snapshot part still counts an unsupported entry's key as present, so the key is not mistaken for a lost clear. A reader never re-encodes a message holding an unsupported item.
- `flatc --cpp` generates `src/o3ds_control_generated.h`. `Build/Scripts/sync_o3ds_core.py` handles only `o3ds_generated.h` by name (`:147`, `:336`), so it and `Build/o3ds-core-manifest.txt` are extended to cover the new header.

**4. Envelope.**
- Append `EUnifiedKind::Control = 2` and `EUnifiedCodec::O3DControl = 3`. A writer emits only the pair (Control, O3DControl), and a reader drops and counts any other pairing with Control.
- The payload is never empty (the TCP keepalive rule in Context).
- **Size budget:** a control envelope is at most **1,100 bytes** including its header. That fits one UDP datagram below the fragmentation threshold and the WebRTC lossy limit, so control never needs fragmenting on any transport. The writer splits snapshots into parts to stay within it. A single entry or event that cannot fit is refused at the API with an error.
- **Envelope version:** control rides whatever envelope the writer emits: v1 (`O3DA`) today, v2 (`O3DU`) once ADR 0009 item 4 lands. It carries its own `seq`, so it does not depend on v2's per-kind `seq`.

**5. Versioning (ADR 0009 item 9).**
- A new kind that old readers ignore is an "appended enum value": `O3DS_PROTOCOL_VERSION` +1 when that constant exists, and a minor bump of `O3DS_VERSION_TAG` (1.0.4 → 1.1.0, or the next minor if D8 ships first).
- No mocap frame's `min_reader_version` changes.
- `CHANGELOG.md` gets a **Schema/Protocol** entry: "control envelope kind 2 added; old TCP, UDP and NNG receivers ignore it; old WebRTC receivers log a warning per control message (see item 10); MoQ receivers that do not subscribe never see it."
- The vtable changes in item 6 bump `O3D_TRANSPORT_API_VERSION` from 1 to 2 (CTL-2), and the WebRTC add-on is rebuilt (ADR 0007 item 2). WP-A1 PR 1 landed at 1 with forwarding shims; removing those shims later takes 3.

**6. Transport interface (on the WP-A1 PR 1 interfaces in `Open3DShared/Public/Transport/`; folds into ADR 0007 item 3's queue, capabilities and demux later).**

| Addition | Called from | Contract |
|---|---|---|
| `IOpen3DSender::SupportsControl() const -> bool` | any thread | default `false` |
| `IOpen3DSender::SendControl(const uint8* Envelope, int32 Len) -> bool` | game thread in v1; any thread allowed | thread-safe, never blocks, copies the bytes. Bytes are a complete control envelope. Returns false when not running, unsupported or full. Default returns false. **Not** routed through `SendSerialized` or the ADR 0008 pose pipe, so control is never dropped as an old pose and never counted as a mocap frame. |
| `IO3DReceiverControlSink::SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double ReceiveTimeSec)` | any thread (implementations must be thread-safe, like `IO3DReceiverAudioSink`) | the **payload**: the ControlMessage bytes inside the envelope, already checked with `O3DS::TryGetControlPayload`, the one classifier every receive path uses; view valid for the call only |
| `IOpen3DReceiver::SupportsControl() const -> bool`, `SetControlSink(TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>)` | game thread, before `Start` | the receiver holds the sink strongly and releases it in `Stop` (as ADR 0007 does for consumers) |

- **After WP-A1:**
  - `SendControl` becomes `SendSerialized` with a payload kind of Control, which goes into `FO3DSendQueue` as a never-dropped control item;
  - `SupportsControl` becomes `FO3DTransportCapabilities::bControl` (done in WP-A1 PR 3: `SupportsControl()` is now a non-virtual forwarder to it, and `SendControl` returns `EO3DSendResult` instead of `bool`, still as its own method until step 4);
  - `FO3DUnifiedReceiveDemux` routes Control to the sink, replacing the per-transport branches below.
- **Backpressure:** today the TCP queue rejects the newest item when full (ADR 0007 Context). A refused control send is retried by the publisher on the next tick. Events retry until their TTL expires; values need no retry, because the next snapshot repairs them.

**7. Per-transport carriage.**

| Transport | Send | Receive | Delivery class (ADR 0005 iii values) |
|---|---|---|---|
| TCP | enveloped bytes into the existing TCP frame queue | Control branch in `ProcessReceivedPayload` | ReliableOrdered |
| UDP | one datagram, never fragmented (≤ budget) | Control branch | Unreliable |
| NNG | enveloped bytes on the same socket | Control branch | pair and push: ReliableOrdered; pub: Unreliable (ADR 0005 Q3) |
| Loopback | new `ControlQueue` on `FO3DLoopbackChannel` beside `Queue` and `AudioQueue` | drained in `Poll` | ReliableOrdered |
| MoQ | separate publisher on `control/<session>/<track>` with `MOQ_DELIVERY_STREAM` whatever `delivery_mode` says, created on every connect beside the mocap publisher (CTL-5: a receiver can subscribe only to an announced track, so a lazy publisher would lose the first cues). `SendControl` is refused until it exists, and the control publisher retries. A custom `track_namespace` without a `mocap/` or `audio/` prefix gets `control/` prepended, so control never shares the mocap track | subscribe to the control track whenever a control sink is set, using the audio subscribe and retry pattern; failures log at Verbose (an older sender never announces the track); a successful mocap subscribe retries a backing-off control subscribe at once | Unreliable and **unordered** until ADR 0005 Q5 is answered: MoQ orders nothing across tracks, so control is not ordered against mocap, and `event_id` and versions carry correctness |
| WebRTC | `lk_send_data_ex(..., LkReliable, ordered = 1, label = "__o3d.ctl")` (`livekit_ffi.h:337-343`) | `OnDataReceivedEx` checks for the envelope magic **before** `EnqueueFrame`; enveloped bytes go to a control queue drained in `Poll`, whatever their label | ReliableOrdered pending ADR 0005 Q4 |

- The WebRTC sender refuses `SendControl` while it is a subscriber-only participant.
- **Event redundancy:** each event is sent `ControlEventRedundancy` times (default 3, clamp 1–5) on consecutive sender ticks, and receivers de-duplicate by `event_id`. The default is 3 on every transport (CTL-4): no delivery-guarantee query exists yet to pick 1 for reliable transports, and the copies are a few dozen bytes each. Once ADR 0007's `FO3DTransportCapabilities` lands, reliable transports can default to 1.

**8. UE surface.**

*Shared (`Open3DShared`)*
- `FO3DControlValue` is a `USTRUCT(BlueprintType)` with `EO3DControlValueType Type` and one field per type. `UO3DControlValueLibrary` provides `Make*` and `As*` Blueprint functions.
- `FO3DControlMeta` holds `SourceId`, `SourceName`, `StreamId`, `TargetSubject`, `SenderTimeSec` and `Seq`.
- `FO3DControlBus` is game-thread-only, like `FO3DAudioBus` (SHR-10). It has one `OnChange` multicast delegate carrying an `FO3DControlChange` (kind: value changed, value cleared or event), and a value cache keyed by `(SourceId, Key, Target)`, queried with `FindValue`. Keys, event names and targets are case-sensitive `FString`s throughout the engine side: `FName` compares case-insensitively and keeps its first-registered casing only in builds with editor data (`WITH_CASE_PRESERVING_NAME`), while core keys are case-sensitive. The Blueprint functions take `FString` too (CTL-4): converting even once through `FName` could return an earlier-registered casing in a Shipping build.
- **Two receiver sources can hear one sender** (UDP multicast, one MoQ track, a duplicated LiveLink source). Each runs its own `ControlReceiver`, so the bus drops an event it has already published for `(SourceId, Epoch, EventId)`, and applies a value change or clear only when its `(Epoch, Version)` is newer than what it holds. `FO3DControlMeta` carries `Epoch`, `Version` and `EventId` for this.
- `FO3DControlValue` stores rotations as `FQuat` (`BlueprintType` in 5.7): a rotator round trip is lossy, which would break exact comparison and the publisher's "same value is a no-op" coalescing. The value library offers rotator make and read helpers.
- Conversions between the engine and core types live in `O3DControlConvert.h` (Shared). Only its `.cpp` includes the core, through a private `Open3DStreamCore` dependency, so Shared's public headers stay core-free. It also `static_assert`s that `O3DS::UnifiedMaxControlPayloadSize` equals `ControlLimits::kMaxPayloadBytes`.

*Sender (`Open3DSender`)*
- `FO3DControlPublisher` (`Open3DSender/Public/O3DControlPublisher.h`) wraps the core `ControlPublisher` (item 9). It owns the source id (a fresh GUID per instance, never serialized), stamps times on the sender clock (`FPlatformTime`, the clock `SubjectList.time` uses), wraps each message in a control envelope, and hands it to `IOpen3DSender::SendControl`, returning refused messages to the core for retry. It is public so it can be driven against a transport without a world.
- On `UO3DSenderComponent` (`BlueprintCallable`, category `Open3DStream|Sender|Control`; strings are `FString`, optional targets default to empty):
  - `FireControlEvent(EventName, Payload, TargetSubject) -> bool`
  - `SetControlValue(Key, Value, TargetSubject) -> bool` (allowed before capture; sent when the transport starts)
  - `ClearControlValue(Key, TargetSubject)`
  - `ClearAllControlValues()`
  - `GetControlValue(Key, TargetSubject, OutValue) -> bool`
- New properties: `ControlSnapshotIntervalSeconds` (default 1 s, clamp 0.25–10 s), `ControlEventRedundancy` (default 3) and `ControlMaxValueRateHz` (default 30).
- **Control-only senders:** `bAllowControlOnly` lets `StartCapture` start the transport with no skeletal mesh and audio off, for a stage, lighting or environment controller actor that only sends cues and parameters. Control starts and stops with the transport, and ticks with the component.
- `mocap_subjects` is the component's resolved subject name, exactly as it goes on the wire, refreshed every tick, so receivers can find the matching stream.
- The value table survives Stop and Start. On Start, a snapshot goes out at once, and receivers reconcile against it without flicker (item 9, *Epoch*).

*Receiver (`Open3DReceiver`)*
- `FO3DReceiverSource::FControlSink` follows the `FAudioSink` pattern: an immutable snapshot, no back-reference, and a hop to the game thread by value. On the game thread the source runs the core `ControlReceiver` (item 9) and publishes the resulting changes to `FO3DControlBus`.
- **Enabling control on a client (maintainer, 2026-09-30):** off by default, but a project must be able to enable it inside its own client, so that a Shipping build ships with it on. There are three layers, and the most specific one that is set wins.
  1. **Project setting (the shipping default).** `UO3DControlSettings : UDeveloperSettings`, `UCLASS(Config = Game, DefaultConfig)`, shown under Project Settings › Plugins › Open3DBroadcast Control.
     - It holds `bAcceptControl` (default **false**), `ControlAllowlist` (key and event-name prefixes; empty means all), `MaxControlLiveBytesPerSecond` (default 64 KiB/s), `MaxControlSnapshotBytesPerSecond` (default 512 KiB/s), `MaxControlKeysPerSource` (default 1,024), `bAlignControlToMocap` (default **true**, item 9) and `MaxAlignmentHoldMs` (default 500).
     - It is saved to the project's `Config/DefaultGame.ini` under `[/Script/Open3DReceiver.O3DControlSettings]`. That file is staged into packaged builds, so ticking the box in the editor is all a project does to ship a Shipping client with control on.
     - The existing `UO3DReceiverSettingsObject` is not reused. It is `Config = GameUserSettings` (`Plugin/Source/Open3DReceiver/Public/O3DReceiverSourceSettings.h:33`), a per-user file the end user can edit, which is the wrong home for project policy.
     - Open3DReceiver gains a `DeveloperSettings` module dependency. That `UDeveloperSettings` is available in Shipping, and that `DefaultConfig` values load in a packaged Shipping build without a user `.ini`, is **needs-verification** against UE 5.7 (Q11).
  2. **Runtime, from the client's own code.** `UO3DControlLibrary::SetControlReceiveEnabled(bool)`, `ClearControlReceiveOverride()` and `IsControlReceiveEnabled()` are `BlueprintCallable`, backed by `FO3DControlBus` in C++. They give a process-wide override of the project setting. A client can enable control from a menu, a login flow or its own config, after BeginPlay, with no restart.
  3. **Per source.** `UO3DReceiverSourceSettings::ControlAccept`, an `EO3DControlAcceptMode` of `ProjectDefault` (default), `Enabled` or `Disabled`, for projects that run several receiver sources and want control on only some of them. Whether LiveLink presets keep per-source settings in a packaged build is **needs-verification** (Q11). The project setting and the runtime call don't depend on it.
  - **Precedence:** per source (if not `ProjectDefault`), then the runtime override (if set), then the project setting.
  - **When it applies:** the effective value is evaluated on the game thread for each message, so changes apply at once. While disabled, the sink drops messages before parsing them and counts them. Disabling also discards that source's control state silently, with no Cleared delegates. Enabling rebuilds the state from the next snapshot (≤ one interval). The receiver source installs the control sink whenever the transport carries control (CTL-4), so a MoQ receiver subscribes to the control track even while control is disabled, and the sink drops what arrives; this keeps runtime enabling free of a resubscribe (CTL-5).
  - **Not provided:** no console variable or command-line switch turns control on. Enabling it is a decision the client's own code or project config makes.
- `UO3DRemoteControlComponent` mirrors `UO3DRemoteAudioComponent`:
  - filters by `StreamId`, LiveLink subject and name prefix;
  - has `BlueprintAssignable` `OnControlEvent`, `OnControlValueChanged` and `OnControlValueCleared`;
  - has `BlueprintPure` `GetControlValue` and `GetAllControlValues`;
  - binds to the bus in `BeginPlay` and unbinds in `EndPlay`.
- **Not provided (G1 and G3):** applying a key to a UObject property by name, or running a console command. A project that wants that writes it in Blueprint against its own allowlist.

**9. Core (`src/o3ds/control.{h,cpp}`, synced to `Open3DStreamCore`).**
- `ControlValue`: a `std::variant` of the item 2 types.
- `ControlWriter`: builds `ControlMessage`s within the byte budget and splits a snapshot into parts. `ParseControl`: Verifier with the identifier, then the semantic limits below, all in `src/o3ds/parse_limits.h`:
  - entries + clears + events ≤ 64 per message;
  - string and bytes lengths as in items 1 and 2;
  - `snapshot_part < snapshot_parts ≤ 2048` (every entry fits a part on its own, so a full 1,024-key table always fits one snapshot);
  - non-finite doubles rejected.
- `ControlPublisher` (sender state):
  - the table with versions, `seq` and epoch;
  - the snapshot timer, with snapshots sent only once the table has been non-empty in this epoch;
  - the redundancy schedule and retry on refusal;
  - `RequestSnapshot()` for the new-peer trigger.
- `ControlReceiver` (receiver state, per `source_id`):
  - **Values:**
    - A set or clear applies only if its `version` is greater than the stored version for that `(key, target)`.
    - A clear leaves a **tombstone** (the key and the clear's version), so a reordered older set that arrives after the clear is rejected. A tombstone is removed once a complete snapshot with `snapshot_seq` ≥ its version has been applied. Tombstones count toward the key cap.
    - That snapshot's capture point becomes the source's **floor**. A set for a key that has neither a value nor a tombstone is rejected when it is at or below the floor, because the key was absent at capture. This keeps a very late retry of an old set from bringing back a cleared value after its tombstone is gone.
  - **Snapshots:**
    - Parts are collected by `snapshot_id`. Entries apply under the value rule as each part arrives.
    - **Lost clears are repaired part by part.** Parts cover contiguous ranges of the table in `(key, target)` order, and `Validate` requires each part's entries to be strictly increasing; only the snapshot of an empty table has an empty part. So any part says which keys were absent at capture within its own range.
    - On each part, the receiver removes (Cleared) every key in that range that the part lacks and whose version is ≤ `snapshot_seq`. Part 0 also covers everything before its first key, and the last part everything after its last key. The gap between two neighbouring parts is covered once both have arrived.
    - A lost part therefore delays repair only for its own range, and a snapshot does not have to complete. Under 20 % loss many snapshots never complete (CTest: one of seven in a 7.5 s run), so repair that waited for a complete snapshot would stall.
    - An incomplete snapshot is discarded when no new part has arrived for `incomplete_snapshot_timeout_s` (default 2 s). The timeout counts from the **last** part, not the first, because a large table paced at the publisher's snapshot cap can take several seconds to send.
  - **Epoch (no flicker on sender restart):**
    - A higher epoch does **not** clear the table. The table becomes *provisional*: it keeps its values and keeps delivering them.
    - The first complete snapshot of the new epoch reconciles. It emits Changed only for values that differ, emits Cleared only for keys it lacks, and adopts the snapshot's versions.
    - A lower epoch is dropped.
    - The publisher sends a snapshot **immediately** on Start, not one interval later. That matters because `PostEditChangeProperty` does Stop then Start (ADR 0005 Context), and remote lights and parameters must not blink when someone edits the sender's details panel.
  - **Events:**
    - De-duplicated over a ring of the last 512 `event_id`s per epoch.
    - **TTL needs no clock sync across hosts.** An event is dropped when its `sender_time_us` is older than the newest `sender_time_us` seen from that source minus `ttl_ms`. Both values come from the sender's clock. This drops replayed or long-delayed events (for example a MoQ relay replaying to a late subscriber) without comparing wall clocks across machines. `tx_wallclock_us` is kept for diagnostics only.
    - The first message from a source after a receiver starts sets the reference, so a replayed backlog delivered first is the remaining risk. Q5 covers it.
  - **Limits:**
    - A per-source byte-rate bucket (not a message count) plus the key cap. Excess input is dropped and counted.
    - Snapshot parts draw from a **separate** budget, so live traffic can never starve snapshot completion.
    - **Sender worst case fits a default receiver by construction.** The publisher caps its own output at 32 KiB/s live and 256 KiB/s for snapshots (`ControlLimits::kPublisherMax*BytesPerS`). The receiver's default budgets are twice those (64 KiB/s and 512 KiB/s), each with a one-second burst. `static_assert`s in `parse_limits.h` hold the 2:1 relation.
    - Within the live cap, clears and events go before value changes. Values over budget wait, coalesced, and a snapshot delivers any that are still waiting.
    - Snapshot parts are spaced over half the interval and never faster than the snapshot cap. A new periodic snapshot starts only when the previous one has finished; a new-peer request replaces it.
  - **Time:** every clock is injected, so the CTest suites are deterministic.
  - **Alignment:** an optional hold queue releases items once a caller-supplied "current mocap sender time" reaches their `sender_time_us`, with the cap from *Timing* below.
- **Coalescing:** within one sender tick, repeated `SetControlValue` calls for the same `(key, target)` collapse to the last value. All changes from a tick go out as one message, or as more if the budget requires. A parameter driven every frame at 60 Hz therefore costs one entry per tick, not one message per call. `ControlMaxValueRateHz` (default 30, clamp 1–120) bounds how often a single key is re-sent, and the latest value always wins. Receivers that want smooth motion interpolate on their side.
- **Timing:** `sender_time_us` uses the same sender clock as `SubjectList.time` and the audio envelope (ADR 0009 item 7). A receiver can therefore place a cue on the same timeline as the pose and audio it is showing.
  - **Default: aligned (maintainer, 2026-09-30).** `bAlignControlToMocap` is **true**. Events and value changes are held until the pose LiveLink is presenting for the matching mocap stream has caught up with their `sender_time_us`. A cue therefore plays with the motion it was fired against, not ahead of it by the receiver's reorder and jitter delay (A1 gate).
  - **Comparison on the sender's own clock (CTL-4).** `FO3DSenderSerializer` stamps `SubjectList.time` with the sender's `FPlatformTime::Seconds()` (`Plugin/Source/Open3DSender/Private/O3DSenderSerializer.cpp:354-355`), the clock the control publisher stamps `sender_time_us` with. The receiver compares the two directly: the presented time is the newest `SubjectList.time` parsed for the stream (`ReceiverStream::subjects.mTime`), minus `BufferSettings.EngineTimeOffset` when the source's LiveLink mode is `EngineTime` (LiveLink reads its buffer that far behind engine time; `LiveLinkSourceSettings.h:74`). No cross-host clock mapping is involved, so NTP steps and UTC skew do not matter, and the core aligner compares `sender_time_us` unchanged. LiveLink's continuously updated smoothing offsets (`EngineTimeClockOffset`, `SmoothEngineTimeOffset`) are not modelled; the cue-to-pose skew measurement in Verification covers them (**needs-verification**). In `Timecode` mode nothing is held, because this channel carries no timecode.
  - **Finding the matching stream:** streams are keyed by subject names (`ReceiverStreamTable::ResolveKey`, `Plugin/Source/ThirdParty/Open3DStreamCore/o3ds/receiver_streams.h:123-177`). Each control message carries `mocap_subjects` (item 3), the subjects its sender streams, and the receiver resolves the stream from them. Several senders in one WebRTC room each align to their own stream.
  - **Never stall:**
    - If no matching stream exists (a control-only sender, or mocap not yet received), or that stream has had no packet for 200 ms (`ReceiverStream::lastSeenS`; the performer's stream paused), the change is delivered at once and counted as unaligned.
    - A message held longer than `MaxAlignmentHoldMs` (default 500) is delivered late, never dropped.
    - Order within a source is preserved: a later message is never released before an earlier one.
  - **Opt-out:** `bAlignControlToMocap = false` delivers on arrival in `Poll`. This suits environment changes that need no lip-sync accuracy and should not wait.
  - **Cost:** alignment adds the mocap stream's playout delay to cue latency, which is the point, since that is the delay the pose already has. The skew with alignment off and on is **needs-verification** per transport, and the Verification section measures it.

**10. Compatibility with old WebRTC receivers.** Control bytes reach an old receiver's mocap path and log a Warning per message (Context). Nothing in this release can change an old build. The mitigations are:
- A sender emits no control traffic unless gameplay uses the API.
- The CHANGELOG entry tells users to update WebRTC receivers before using control.

Separately, and useful on its own, new receivers rate-limit that Warning to once per stream per 10 s, so malformed input never floods the log.

## Consequences

- **Easier:** cues, scene state, take and slate markers, and per-character parameters travel with the stream they belong to, over the same transport and authentication. Gameplay gets Blueprint events and values without a second network stack.
- **Harder:** the transport API version bump means the WebRTC add-on must be rebuilt (already true per release under ADR 0002). The same parser hardening and fuzzing apply to a second root type. Six transports gain a small branch each until WP-A1 removes the duplication.
- **Bandwidth:** zero when unused. With N keys, a snapshot costs about N × (key + value + 24 bytes) per interval: 50 float keys with 24-byte keys take about 3.6 KB/s at 1 s. Each event costs one message, three on Unreliable transports.
- **Constrains:**
  - WP-A1 must keep the never-drop control item in `FO3DSendQueue` and route kind 2 in `FO3DUnifiedReceiveDemux`.
  - ADR 0005 (vi) `SetPeerJoinedCallback` gains a second consumer (`ControlPublisher::RequestSnapshot`).
  - ADR 0009's compatibility matrix gains the rows in Verification.
- **Not decided here:** receiver-to-sender messages (D2), aligning delivery to the mocap timeline, and exposing control values as LiveLink properties or curves.

## Implementation outline

WP-CTL, P1 · L. Each PR keeps all transports working and the conformance suite green. Paths starting with `Plugin/` are relative to `ProjectSandbox/Plugins/Open3DBroadcast/`.

1. **CTL-1 Core (no UE).**
   - `src/o3ds_control.fbs` and the generated `src/o3ds_control_generated.h`.
   - `src/o3ds/control.{h,cpp}` (`ControlValue`, `ControlWriter`, `ParseControl`, `ControlPublisher`, `ControlReceiver`) and limits in `src/o3ds/parse_limits.h`.
   - `src/CMakeLists.txt`; `Build/o3ds-core-manifest.txt` and `Build/Scripts/sync_o3ds_core.py` for the second generated header; mirror into `Plugin/Source/ThirdParty/Open3DStreamCore` and add `Plugin/Source/Open3DStreamCore/Private/Core/O3DSCore_control.cpp`.
   - Tests: `test/control_codec_tests.cpp`, `test/control_state_tests.cpp`, `test/fuzz/fuzz_control.cpp` with seeds from `make_seeds.cpp`.
2. **CTL-2 Shared and interfaces.**
   - `EUnifiedKind::Control` and `EUnifiedCodec::O3DControl` (`O3DUnifiedMessage.h`), with a `WriteControlEnvelope` helper.
   - `SupportsControl`, `SendControl`, `IO3DReceiverControlSink`, `SupportsControl` and `SetControlSink` on the interfaces (`O3DSenderInterface.h`, `O3DReceiverInterface.h`).
   - `O3D_TRANSPORT_API_VERSION` +1 with a History line.
   - `FO3DControlValue`, `FO3DControlMeta`, `FO3DControlBus` and `UO3DControlValueLibrary` in `Open3DShared`.
   - Control support in `FO3DFakeSender`, `FO3DFakeReceiver` and `FO3DFakeLink` (`Open3DBroadcastTests/Public/O3DTestFakes.h:24-146`).
   - Rate-limit the malformed-packet Warning (`O3DReceiverSource.cpp:626-631`).
3. **CTL-3 In-band transports.**
   - TCP and UDP (`Open3DTransportSockets/Private/{Sender,Receiver}/*`), NNG (`Open3DTransportNNG/Private/{Sender,Receiver}/*`) and Loopback (`LoopbackChannel.h`, `LoopbackSender.cpp`, `LoopbackReceiver.cpp`).
   - Conformance cases (Verification) and per-transport tests.
4. **CTL-4 Gameplay surface.**
   - `FO3DControlPublisher`, plus the functions and properties on `UO3DSenderComponent` (`O3DSenderComponent.h/.cpp`). The control properties appear through the default details layout (the sender customization hides only the categories it rebuilds itself).
   - `FControlSink` in `O3DReceiverSource.cpp`, including the alignment hold queue resolved through `ReceiverStreamTable::ResolveKey`.
   - `UO3DControlSettings` (new `Open3DReceiver/Public/O3DControlSettings.h`, plus `DeveloperSettings` in `Open3DReceiver.Build.cs`), `UO3DControlLibrary` with the runtime enable functions, and `ControlAccept` in `O3DReceiverSourceSettings.h`.
   - `UO3DRemoteControlComponent` (`Open3DReceiver/Public` and `Private`).
   - The packaged Shipping test (Verification).
5. **CTL-5 MoQ.**
   - A `control/` namespace helper in `MoQHelpers.h/.cpp`, a control publisher created on connect in `MoQSender`, and a control subscription in `MoQReceiver`. Control never moves a frame, byte or drop counter on either side.
   - Tests in `Open3DBroadcastTests/Private/Transport/MoQ/MoQControlTests.cpp` using `MoQFakeFfi.h`, and the control conformance cases on the MoQ profile. The opt-in relay case is not written yet: it needs a live relay (Verification, MoQ).
6. **CTL-6 WebRTC add-on.**
   - Rebuild against the new API version. Add the `__o3d.ctl` send path and receive classification in `WebRTCSender.cpp` and `WebRTCReceiver.cpp`.
   - Tests in `Open3DBroadcastWebRTC/.../Private/Tests/`, and a manual step in `docs/testing/webrtc-manual-test.md`.
7. **CTL-7 Docs.**
   - A Control section in `USER_GUIDE.md`; a Control row in `Transport_Module_Comparison.md`; the `CHANGELOG.md` Schema/Protocol entry (item 5); the wire layout in `docs/wire-format.md` when WP-D3 creates it.
   - Optional sample: a level Blueprint that toggles a light from a sender event (WP-U5).

**Dependencies:**
- CTL-1 to CTL-4 depend on no unlanded ADR.
- New-peer snapshots wait for ADR 0005 (vi). Until then, recovery is bounded by the snapshot interval alone.
- If WP-A1 lands first, CTL-2 and CTL-3 target `FO3DSendQueue`, the capability flag and the shared demux directly, and skip the per-transport branches.

## Verification / acceptance

**Core (CTest, `test/`, runs in `core-tests.yml`).**
- **Codec:**
  - every value type round-trips bit-exactly;
  - a message at exactly the budget is accepted, and one byte over is refused by the writer;
  - each semantic limit is rejected with a distinct error;
  - a `ControlMessage` buffer fed to `SubjectList::Parse` is rejected, and so is a `SubjectList` buffer fed to `ParseControl`;
  - `protocol_version = 2` is rejected.
- **State, using `channel_model` (as ADR 0005 Verification does):**
  - 20 % loss with 100 keys and random set and clear traffic, over 20 seeds: after the last change, every receiver matches the sender within six snapshot intervals, and no key ever regresses to an older version. Recovery is per snapshot part: each interval, a key whose last change was lost is repaired with probability 1 − p (p = part loss), so it stays wrong for k intervals with probability p^k (4 % for k = 2 at p = 0.2, 6×10⁻⁵ for k = 6). No one-way scheme can promise convergence within a fixed number of intervals on a lossy link;
  - reordering (window 8): no stale value applied, including a set reordered after its clear (tombstone);
  - a late joiner at t = 5 s gets the full table within one interval and **no** events sent before it joined;
  - a lost clear is repaired by the next complete snapshot;
  - a multi-part snapshot with one part lost removes nothing;
  - an epoch increase with an unchanged table emits **no** Changed or Cleared at the receiver (no flicker), and one with a changed table emits exactly the differences;
  - a lower epoch is ignored.
- **Load:** a maximum-load sender (key cap of maximum-size entries, every key at `ControlMaxValueRateHz`, 10 events/s with redundancy 3) against a receiver with default settings completes every snapshot at 0 % loss and drops no live change.
- **Events:**
  - on a lossless channel, an event with redundancy 3 is delivered exactly once;
  - at 20 % loss and redundancy 3, delivery matches theory (1 − 0.2³ = 99.2 %; the bar is theory minus 4 standard deviations, 98.8 %) over 10,000 events for each of three seeds, with zero duplicates;
  - a TTL-expired event is dropped. The case runs with the receiver's wall clock skewed by ±1 hour and gives the same result;
  - the de-duplication ring survives wrap-around.
- **Limits:** a flood of 10× the rate is capped at the bucket size and counted, and the key cap holds.
- **Coalescing:**
  - 1,000 sets of one key in one tick produce one entry carrying the last value;
  - one key driven at 60 Hz with `ControlMaxValueRateHz = 30` sends ≤ 30 entries/s, and the receiver's final value equals the sender's.
- **Timing (default settings, alignment on):**
  - with a simulated 150 ms receiver jitter delay, an event stamped with a pose's `sender_time_us` is delivered in the same receiver tick as that pose (±1 tick);
  - with the alignment cap exceeded, it is delivered late and counted, never dropped;
  - a control-only sender (no mocap stream) and a stream with no clock estimate yet are delivered at once, not held;
  - two senders resolve to their own streams through `mocap_subjects`;
  - release order within a source matches send order;
  - with `bAlignControlToMocap = false`, delivery happens on arrival.
- **Fuzz:** `fuzz_control` runs for 60 s in CI with no findings (WP-T1 style).

**UE automation (`Open3DBroadcastTests`, ADR 0006).**
- **Conformance suite (`Conformance/O3DConformanceSuite.cpp`)**, gated on `SupportsControl()` for each registered transport:
  - a control envelope sent is delivered to the sink byte-identical;
  - interleaved with 100 mocap frames, control does not change mocap frame counts or content;
  - `SendControl` before `Start` and after `Stop` returns false;
  - `Stop()` while four threads call `SendControl` is clean (1,000 cycles; ASan where available, as in ADR 0007's stress case).
- **Per transport:**
  - TCP: a refused send under backpressure is retried by the publisher;
  - UDP: no control datagram exceeds the budget;
  - NNG: pub, pair and push;
  - Loopback: three queues stay independent;
  - MoQ (fake FFI): the control track is announced on connect with stream delivery, never shares the mocap namespace, and is subscribed only with a sink;
  - WebRTC (add-on tests): enveloped bytes on any label route to control, and raw mocap on `__o3d.ctl` still routes to mocap.
- **Components:**
  - `UO3DSenderComponent` → Loopback → `FO3DReceiverSource` → `UO3DRemoteControlComponent`: events and values reach the Blueprint delegates on the game thread;
  - filters by stream, subject and prefix work;
  - **Enablement:**
    - with the project default, nothing is delivered;
    - `SetControlReceiveEnabled(true)` at runtime delivers from the next message, and the values arrive within one snapshot interval;
    - `SetControlReceiveEnabled(false)` stops delivery at once with no Cleared delegates;
    - a per-source `Disabled` beats a runtime `true`, and a per-source `Enabled` beats a project `false`;
    - `ClearControlReceiveOverride()` returns to the project setting;
  - the allowlist blocks non-matching names;
  - Stop and Start on the sender, as triggered by a details-panel edit, produces a new epoch and no `OnControlValueCleared` or `OnControlValueChanged` for unchanged keys;
  - a duplicated sender actor gets a different `source_id`;
  - modelled on `Receiver/O3DRemoteAudioComponentTests.cpp`.
- **API version:** `Shared/O3DTransportApiVersionTests.cpp` asserts the bumped value and that a mismatched add-on registers nothing.
- **Log hygiene:** 1,000 malformed packets produce at most one Warning per stream per 10 s.

**Compatibility matrix (extends ADR 0009 item 11).**

| Writer → reader | Traffic | Expected |
|---|---|---|
| new → baseline TCP, UDP, NNG | mocap + control | mocap unchanged; control dropped silently |
| new → baseline WebRTC | mocap + control | mocap unchanged; control rejected by `PeekPacketMeta` (bounded warnings documented) |
| new → baseline MoQ | mocap + control | mocap unchanged; control track never subscribed |
| baseline → new (all) | mocap + audio | unchanged; no control events; no warnings |
| new → new (all) | mocap + audio + control | all three delivered; conformance green |
| control `protocol_version` 2 → new | control | rejected, logged once per source |

**Packaged Shipping build (CTL-4 acceptance; resolves Q11).** Use `Build/Scripts/Build-ShippingGame.ps1` on the sandbox project.
- **Project setting:** with `bAcceptControl=True` in `ProjectSandbox/Config/DefaultGame.ini`, a Shipping client receives events and values from a Loopback or TCP sender with no user `.ini` present.
- **Runtime call:** with the setting false, a Blueprint calling `SetControlReceiveEnabled(true)` on BeginPlay achieves the same.
- **Defaults:** with neither, nothing is delivered.

**Manual and network.**
- WebRTC: two senders in one room are distinguished by `source_id` (`docs/testing/webrtc-manual-test.md`).
- MoQ: through a real relay (`docs/dev/Open3DTransportMoQ/CLOUDFLARE_RELAY_TESTING.md`), measure the late-subscriber replay behaviour that the TTL guards against.
- **Cue-to-pose skew:** on each transport, with alignment off and on, measure the receiver-tick difference between an event and the pose frame stamped with the same sender time. This answers Q6 and the needs-verification in item 9, *Timing*.

**Performance.**
- With control unused, sender and receiver per-frame cost is unchanged (WP-A2 budget, ADR 0008 driver 1).
- `SetControlValue` costs ≤ 5 µs on the game thread.
- Receiver control handling costs ≤ 20 µs per message.

**Review checks.**
- No control code reaches a UObject by name or path, or calls `GEngine->Exec` or a console command.
- `grep -rn "EUnifiedKind::Control" Plugin/Source` hits only the envelope writer and the receive branches (later, only the shared demux).

## Open questions for the maintainer

1. ~~Is control receiving off by default?~~ **Answered 2026-09-30:** yes, off by default. A client must be able to enable it itself, including in a Shipping build: the project setting saved to `DefaultGame.ini`, plus a runtime Blueprint and C++ call (item 8, *Enabling control on a client*).
2. Is the size budget 1,100 bytes per control envelope, with snapshots split into parts? **Default: yes.** This avoids UDP fragmentation and the WebRTC lossy limit.
3. Is event redundancy 3 copies on Unreliable transports and 1 on ReliableOrdered ones, with the TTL defaulting to 2 s? **Default: yes.**
4. **needs-FFI-verification (livekit_ffi):** is the reliable data channel ordered across messages (ADR 0005 Q4)? And does the data callback expose the sending participant? Only `label` and `reliability` are visible in `livekit_ffi.h:94`. The reference source (`E:\OtherProjects\livekit-ffi-ue\livekit_ffi`) was not available when this was written. Default: rely on `source_id` in the payload. If participant identity is needed, request a `LkDataCallbackEx2` from the FFI maintainers.
5. **needs-FFI-verification (moq-ffi):** in `MOQ_DELIVERY_STREAM`, does a relay replay earlier objects to a late subscriber, and are objects ordered (ADR 0005 Q5)? Default: treat MoQ as Unreliable. The TTL and version checks make replays harmless.
6. ~~Should cue alignment be on by default?~~ **Answered 2026-09-30:** yes. `bAlignControlToMocap` defaults to true, with an opt-out (item 9, *Timing*).
7. **needs-verification (NNG docs):** does pub drop messages for slow subscribers (ADR 0005 Q3)? This decides whether NNG pub uses redundancy. Default: yes, treat pub as Unreliable.
8. Receiver-to-sender control (acks, requests, a remote "set value on the sender") is deferred to a later ADR, and only for bidirectional transports. **Default: deferred.**
9. Should control values also appear as LiveLink properties or curves on the matching subject? **Default: no.** Gameplay reads them from the component. Revisit if virtual-production users ask for it.
10. Does `TargetSubject` filtering happen on the receiver only (the stream is a broadcast)? **Default: yes.** There is no per-receiver addressing in a one-way stream.
11. **needs-verification (UE 5.7):**
    - `UDeveloperSettings` with `Config = Game, DefaultConfig` is available in Shipping, and its `DefaultGame.ini` values load in a packaged Shipping build.
    - Whether a `ULiveLinkPreset` applied at runtime keeps per-source `ULiveLinkSourceSettings` subclass properties.

    **Default:** the project setting and the runtime call are the supported ways to enable control in Shipping. Per-source override is a convenience. The CTL-4 PR confirms both with a packaged Shipping test (Verification).

## References

- Findings and themes: T1 (untrusted bytes), T5 (duplicated demux); SHR-7, SHR-10, SHR-30, TRB-6, TRB-17, TRF-31.
- Files: `Plugin/Source/Open3DShared/Public/{O3DUnifiedMessage.h,O3DAudioBus.h,Transport/O3DTransportApiVersion.h}`; `Plugin/Source/Open3DSender/Public/{O3DSenderInterface.h,O3DSenderComponent.h}`; `Plugin/Source/Open3DSender/Private/O3DSenderTransportController.h`; `Plugin/Source/Open3DReceiver/Public/{O3DReceiverInterface.h,O3DReceiverSource.h,O3DRemoteAudioComponent.h}`; `Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp`; the `Open3DTransport{Sockets,NNG,Loopback,MoQ}` sender and receiver sources cited above; `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/include/moq_ffi.h`; `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/Source/Open3DTransportWebRTC/{Private/Sender/WebRTCSender.cpp,Private/Receiver/WebRTCReceiver.cpp,ThirdParty/livekit_ffi/include/livekit_ffi.h}`; `src/o3ds.fbs`; `src/o3ds/model.cpp`; `Build/Scripts/sync_o3ds_core.py`; `Plugin/Source/Open3DBroadcastTests/{Public/O3DTestFakes.h,Private/Conformance/O3DConformanceSuite.cpp}`.
- Plans: `docs/dev/Open3DTransportMoQ/MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md` ("Metadata/Control Tracks (Optional)", `meta/<session>/<character>`); this ADR uses `control/` so the track says what it carries.
- External: none fetched for this ADR.
