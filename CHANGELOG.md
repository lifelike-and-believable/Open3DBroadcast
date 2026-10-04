# Changelog

The one changelog for the core library, the plugins and the transports (ADR 0009 item 10).
Each release gets a `## [x.y.z] - date` section; until then changes collect under
`## Unreleased`. A **Schema/Protocol** entry is required for any wire change and states the
protocol version, the `min_reader_version` of affected frames, compatibility in both
directions, and the migration steps ([docs/wire-format.md](docs/wire-format.md) section 8).

## Unreleased

### Schema/Protocol

- **Updates carry scale** (CORE-11, ADR 0005 (v)). The delta and quantized path sends a
  transform's absolute scale in `SubjectUpdate.scale` when it moved more than the delta
  threshold since it was last sent (the block had been commented out); residual updates send
  absolute scale (not a residual) on every keyframe and when it moved past the threshold, and
  `ParseUpdateResidual` applies it. Before, scale froze at the last full Subject in the
  residual and quantized encodings, so squash-and-stretch and curve-driven scale were lost
  between full syncs.
  - Version: protocol 2, unreleased (`docs/wire-format.md` section 8); no schema change.
  - `min_reader_version`: unchanged (a plain delta with scale stays 1; readers have always
    applied `scale` in plain updates).
  - Compatibility: a reader from before this change ignores `scale` in residual updates and
    keeps the last full Subject's scale, as before. A new reader of an old writer's frames sees
    no scale entries.
  - Tests: `core.scale_update_tests`.

- **`SubjectUpdate.ref_seq`** (ADR 0005 (viii), CORE-5): appended `ulong`, default 0. The
  `tx_seq` of the full Subject an update is relative to; `O3DS::StreamWriter` sets it on every
  update. A receiver that parses with a `ParseContext` drops an update whose `ref_seq` is set
  and names a full Subject it did not apply (ADR 0005 (ix), below).
  - Version: protocol stays **2** and `O3DS_VERSION_TAG` stays 1.1.0. Protocol 2 is "the
    release that ships ADR 0005" (ADR 0009 item 2) and is not released yet, so this field is
    part of it rather than a protocol 3.
  - `min_reader_version`: unchanged. An update with `ref_seq` is stamped exactly as before (1
    for a plain delta, 2 for residual or quantized content); a reader that ignores the field
    applies the update as before.
  - Compatibility: a reader without the field ignores it (old behaviour, no loss detection). A
    new reader given an update with `ref_seq` 0, or parsing without a context, applies it as
    before.
  - Migration: regenerate `src/o3ds_generated.h` (`flatc --cpp -o src src/o3ds.fbs`) and sync
    the core mirror; nothing else.
  - Tests: `core.wire_format_tests` `Wire_RefSeqIsAppendedAndLeavesTheVersionStampAlone`.

- **No compatibility with what came before protocol 2** (WP-A4 follow-up). There are no users of
  old receivers or senders, so what existed only for them is gone:
  - **Envelope v1 is not accepted.** Readers accept only envelope v2 (`O3DU`); the core's v1
    reader and writer, `UnifiedWireHeaderSizeV1` and `FUnifiedHeader::MagicValueV2BE` are
    removed, and `FUnifiedHeader::MagicValueBE()` is now the `O3DU` magic. The UDP classifier no
    longer treats `O3DA` as an envelope.
  - **The TCP keepalive is an envelope v2** (24 bytes; it had stayed v1 for old receivers).
  - **A version-1 frame is no longer checked for residual or quantized content**
    (`FrameCheck::UndeclaredNewContent` is removed); writers stamp version 2 on such frames.
  - **The golden baseline frames (`test/fixtures/wire`), the compat tool (`test/compat`) and the
    old-reader CI step are removed.**
  - Kept: `min_reader_version` stamping and the version check (a frame needing a newer protocol
    is still rejected), and the compatibility-window rule for future releases
    ([docs/wire-format.md](docs/wire-format.md) section 8).
  - Version: still 1.1.0 (unreleased): no release ever accepted the removed formats.
  - Compatibility, old reader and new writer, new reader and old writer: not supported for
    builds from before protocol 2; there are none in use.
  - Tests: envelope and UDP tests now check that v1 is rejected; `Open3DBroadcast.Shared.
    Envelope.V2WrittenV1StillRead` is now `.V2WrittenV1Rejected`.

- **Wire format documented; one changelog** (ADR 0009 item 10, WP-A4 PR 5; DOC-8). New
  [`docs/wire-format.md`](docs/wire-format.md): the frame header and versions, envelope v1 and
  v2, the UDP datagram kinds and fragment header, TCP framing, capture files, clock domains,
  name hashes, and the rules for changing the wire format (bump table, append-only schema,
  compatibility window, the changelog entry and the compatibility tests). `docs/CHANGELOG.md`,
  which only said "Nothing yet", is deleted; this file is the changelog. ADR 0009 gains an
  implementation-notes addendum recording where WP-A4 departed from it. No wire change.

- **One name-hash definition, length-prefixed** (ADR 0009 item 8, WP-A4 PR 4; SHR-33). No wire
  change: the hashes are local (topology and curve-name change detection on the sender and the
  receiver). `O3DS::Wire::HashNames` in the core is 64-bit FNV-1a over the name count (u32 LE),
  then each name's UTF-8 length (u32 LE) and bytes, case preserved;
  `O3DS::Wire::HashParents` continues it with the parent count and each index (i32 LE).
  `O3DHelpers::HashNames` and `HashNamesAndParents` call them. The old hash fed each name's
  TCHAR bytes with no length or count, so `{"ab","c"}` and `{"a","bc"}` collided, and the
  parents were hashed as raw host-order bytes. Hash values change; nothing persists them. A
  topology hash that ever goes on the wire must use this definition and bump the protocol.
  New `core.wire_format_tests` case `NameHash_LengthPrefixedAndCaseSensitive`.

- **Unified envelope v2** (ADR 0009 item 4, WP-A4 PR 3; SHR-7, SHR-30). Wire protocol stays 2;
  this is an envelope layout change, so the envelope carries its own magic and version.
  - **Layout:** 24 bytes, little-endian: magic `O3DU`, version 2, kind, codec, flags 0, u64
    timestamp in microseconds (sender clock), u32 payload size, u32 sequence number (per stream
    and kind, wrapping). Envelope v1 was 20 bytes, big-endian (`O3DA`), with no sequence number.
  - **Writers** emit v2 for audio and control. Audio frames are numbered by their stream's
    encoder; control envelopes by the sender's control publisher. The timestamp is written from
    finite, non-negative values only (a NaN or negative value is 0, an overflow saturates; the
    old cast was undefined behaviour). **The TCP keepalive stays envelope v1** during the
    compatibility window: a receiver from before envelope v2 would otherwise read every
    keepalive as a malformed frame and log it.
  - **Readers** accept v1 and v2 (`O3DS::ParseUnifiedMessage`, `TryGetControlPayload`, the
    receive demux, the WebRTC add-on's control checks). The codec moved to the core
    (`o3ds/wire_format.h`: `ReadEnvelopeHeader`, `WriteEnvelopeHeaderV2`, `HasEnvelopeMagic`);
    the plugin functions keep their names and gain an optional sequence number.
    `O3DS::UnifiedWireHeaderSize` is now 24 (the v2 header writers emit) and
    `UnifiedWireHeaderSizeV1` is 20; code that found an envelope's end from the header size now
    uses where the payload ends.
  - **PCM16 samples are little-endian** on the wire, converted explicitly
    (`O3DAudio::Pcm16HostToWire`); the same bytes as before on every shipped (little-endian)
    platform.
  - **Compatibility, old receiver and new sender:** audio and control are dropped: an old
    receiver does not recognise `O3DU`, passes the message to the frame parser, and the parser
    rejects it (ADR 0009 Q1, accepted). Mocap and the TCP keepalive are unaffected.
  - **Compatibility, new receiver and old sender:** v1 envelopes are read as before.
  - **Migration:** update receivers and senders together where audio or control is used.
  - **Not done, deviation from ADR 0009 item 4:** audio payload v3 (dropping the codec and
    timestamp the payload repeats from the envelope). Envelope-less audio channels, MoQ's audio
    track (TRF-37), read the codec and timestamp from the payload itself, so the payload keeps
    them.
  - **Tests.** New `core.wire_format_tests` cases for the v2 bytes, v1 big-endian reading and
    writing, rejection of a wrong version or flags, the payload-fit check, and the timestamp
    clamp; new `Open3DBroadcast.Shared.Envelope.V2WrittenV1StillRead` and
    `.AudioFramesAreNumbered` (sequence numbers, little-endian samples, the control envelope
    still within the 1,100-byte budget).

- **UDP fragment header v2** (ADR 0009 item 5, WP-A4 PR 2; TRB-17). Wire protocol stays 2; this
  is a header layout change, so fragments carry their own magic and version.
  - **Layout:** 24 bytes, little-endian: magic `O3DF`, version 2, flags 0, two reserved zero
    bytes, then message id, fragment index, total size and fragment size (u32 each). It was 16
    bytes (the four u32 without a magic).
  - **Classification.** The UDP receiver treats a datagram as a fragment only when it starts
    with `O3DF`; a fragment whose header is inconsistent is dropped and counted in the
    transport's receive errors. Every other datagram is passed on whole, as before and as on
    every other transport. Before, a datagram was a fragment if its first 16 bytes happened to
    describe a consistent fragment. `udpClassifyDatagram` in `o3ds/udp_fragment.h` also names
    envelopes (`O3DA`, `O3DU`) and frames (a frame word). **Deviation from ADR 0009 item 5,**
    which asked the UDP receiver to drop and count any other datagram: the transports stay
    byte-opaque (the conformance suite sends arbitrary payloads through all of them), and the
    receiver's demux and frame check already reject and count what is not an envelope or a
    frame.
  - **Compatibility, old receiver and new sender:** only fragmented messages are affected (larger
    than `udp.maxdatagram`). An old receiver does not recognise a v2 fragment, passes it on as a
    frame, and the parser rejects it ("Invalid data structure"): the message is dropped, never
    misassembled. Messages sent in one datagram are unchanged.
  - **Compatibility, new receiver and old sender:** legacy fragments are rejected (ADR 0009 Q3,
    accepted default); there is no option to accept them. Unfragmented datagrams are unchanged.
  - **Migration:** update UDP receivers and senders together if messages exceed
    `udp.maxdatagram` (large skeletons, low `udp.maxdatagram`).
  - **Tests.** `core.udp_reassembly_hardening_tests` checks the v2 bytes, rejects a wrong magic,
    version, flag or reserved byte and the legacy header, and checks the classification of every
    kind. The UDP fuzz seeds are written with the real header writer.

- **Wire protocol 2; core 1.1.0** (D8, [ADR 0009](docs/adr/0009-protocol-versioning.md), WP-A4
  PR 1; CORE-15, CORE-16, CORE-22).
  - **Frame header.** The first 4 bytes of a frame are now `min_reader_version` (byte 0) and
    three zero bytes, little-endian, followed by the little-endian CRC-32 as before. Version 1
    has exactly the bytes of the old flags word.
  - **Which frames are version 2.** Writers stamp 2 on a frame that carries residual content
    (`predictor_id != 0`) or a quantized vector (`*_q8`, `*_q16`), decided from what the frame
    contains, and 1 on everything else: full snapshots and plain deltas, which is what default
    settings send.
  - **Schema.** `file_identifier "O3DS"` is written by every writer, and `SubjectList` gets the
    appended field `protocol_version:ushort` (the writer's protocol, 2; 0 = before D8). Both are
    append-only; `src/o3ds_generated.h` is regenerated.
  - **Readers** (`Parse`, `PeekMeta`, `PeekPacketMeta`, through the new `O3DS::CheckFrame`)
    accept `min_reader_version` 1 or 2 with the reserved bytes zero, check the CRC (`PeekMeta`
    and `PeekPacketMeta` did not, CORE-15), require the identifier only on version-2 frames, and
    reject anything else with a reason (`O3DS::Wire::FrameCheck`). A frame stamped 3 or higher is
    rejected as "sender requires protocol N; update this receiver", so the next breaking change
    is safe too. Reads go through explicit little-endian helpers (`o3ds/wire_format.h`), not
    type-punned pointers (CORE-22).
  - **Compatibility, old reader and new writer:** full snapshots and plain deltas are read as
    before (pre-D8 readers ignore the identifier and the new field). Residual and quantized
    frames are dropped with "Invalid data structure"; they are never misapplied. Checked in CI
    against the baseline reader (`develop@7aea235`).
  - **Compatibility, new reader and old writer:** full snapshots and plain deltas from any
    pre-D8 writer parse as before (golden frames in `test/fixtures/wire/v1`). Residual and
    quantized frames from pre-D8 `develop` builds, which stamped 1, are rejected: their anchor
    and resync semantics differ from ADR 0005. D1 and C2 were never released.
  - **Migration:** update receivers before turning on residual coding or quantization on a
    sender; default senders need nothing. The UE receiver logs which side to update.
  - `O3DS_VERSION_TAG` is 1.1.0 and can be set with `-DO3DS_VERSION_TAG=` (the release
    workflows passed `-DVERSION_TAG`, which CMake never read; an empty value falls back to the
    default). The plugin's core mirror records it in `SYNC_STAMP.txt`.
  - Not in this change (later WP-A4 PRs, ADR 0009 items 4, 5, 8 and 10): the audio envelope v2
    and little-endian PCM, the UDP fragment header v2, the length-prefixed name hash, and the
    merged changelog and `docs/wire-format.md`. ADR 0005's `ref_seq` and stream writer are
    separate.
- **Tests.** New `core.wire_format_tests` (frame word helpers, the stamp of every serialize path,
  reader acceptance and rejection, the CRC in the peeks, the baseline golden frames); a new
  CI step builds the baseline reader from a worktree and reads this writer's frames;
  `test/compat/compat_tool.cpp` writes and reads the four frame kinds for both. The `peek_meta`
  fuzz target's invariant now accounts for the peeks checking the CRC.
- **UE plugin.** The receiver logs "Sender on '...' requires wire protocol N; this receiver
  implements 2. Update this receiver." (or that a pre-D8 sender should be updated) instead of
  the generic malformed-packet warning, with the same throttle. The sender's residual and
  quantization tooltips say these streams need protocol-2 receivers.

### Core library (`src/o3ds`)

- **Resync contract on the receiver** (ADR 0005 (ix); CORE-5, CORE-6). `SubjectList::Parse`
  takes an appended, defaulted `const ParseContext*` (`tx_seq`, `frame_epoch`, `gap_before`).
  With a context for a sequenced frame:
  - a new `frame_epoch` clears every subject's sync state;
  - a full Subject records its `tx_seq` as the subject's sync reference (`Subject::mSyncRef`);
  - an update whose `ref_seq` is set and differs from that reference is dropped;
  - after `gap_before`, every subject's residual updates are dropped until its next full
    Subject (`Subject::mAwaitingFullSync`).
  Dropped updates are not errors: they are left out of `outTouched` and counted in
  `SubjectList::mUpdatesDroppedUnsynced`. Without a context, or with `ref_seq` 0, updates
  apply as before. `MakeParseContext` and `NoteFrameApplied` (`receiver_streams.h`) compute
  `gap_before` per `ReceiverStream`.
- **`ResidualDecoder::BeginFrame` returns `bool`** (CORE-6): false, and the update is dropped
  and counted, when the update is not a keyframe and the decoder has no history to predict
  from. It used to decode against a zero reference, which applied a wrong pose.
- `StreamWriter::LastFullSeq`; `WriteUpdate` and `WriteResidual` set `ref_seq`.
  `Subject::SerializeUpdate` and `SerializeUpdateResidual` (buffer and builder overloads) take
  a trailing, defaulted `ref_seq`.

- **`O3DS::StreamWriter`** (new `src/o3ds/stream_writer.h`; ADR 0005 (iv), WP-A4): one logical
  sender stream. Its `WriteFull`, `WriteUpdate` and `WriteResidual` stamp every frame with
  `tx_seq` (one counter per writer, not reset by `StartSession`), `tx_wallclock_us` (UTC at the
  write) and `frame_epoch`. Each session takes `max(NewSessionEpoch(), last epoch issued in the
  process + 1)`, so a writer created or restarted within the same second as another still gets
  a strictly larger epoch. `Subject::Serialize` and `Subject::SerializeUpdate` take the same
  trailing, defaulted stamp parameters, and `Subject::SerializeUpdateResidual` takes
  `tx_wallclock_us` and `frame_epoch` after its `seq`; unstamped calls still write 0 (unset).

- **Faster Serialize and Parse (WP-A2e, ADR 0008 outline item 6; CORE-7,
  CORE-18).** For one 250-bone, 250-curve subject (MSVC Release, the new
  `o3ds_core_bench`), a full Serialize went from about 500 µs to 96 µs, Parse
  from about 405 µs to 162 µs, and SerializeUpdate from 81 µs to 14 µs:
  Serialize plus Parse is about 3.5 times faster.
  - The buffer header's CRC-32 is computed by `O3DS::Crc32` (new
    `src/o3ds/crc32.h`), slicing-by-8 with tables built once, instead of
    CRCpp's bit-by-bit loop (16 µs instead of 278 µs for 34.7 KB). Same
    polynomial and parameters, so the value, and the wire format, are
    unchanged; old and new readers and writers interoperate.
  - Every `Serialize*` call that writes a whole buffer reuses one
    `FlatBufferBuilder` per thread (cleared, keeping its memory) instead of
    building a new one, and `finalize()` writes the header and payload in
    place with one `resize` instead of appending byte by byte, so a reused
    output vector keeps its capacity.
  - The core no longer includes CRCpp's `CRC.h`; it is still used by the
    tests to check `Crc32` against the bitwise result. The plugin's core
    mirror no longer carries `CRC.h` or the CRC++ licence, and
    `Open3DStreamCore` no longer adds its include path or
    `CRCPP_USE_NAMESPACE`; `THIRD_PARTY_LICENSES.md` drops the CRC++ row.
  - Tests: new suite `core.crc32_tests` (the CRC-32 check value; every length
    0 to 100 at every alignment, large and constant buffers, all against the
    bitwise CRC; the serialized header). New benchmark
    `test/bench/serialize_bench.cpp` (`o3ds_core_bench`, CTest
    `core.bench.serialize`, label `bench`): it prints the timings and checks
    only correctness.
- **Parse reuses its objects on a full sync (CORE-18, the Parse half; WP-A2
  follow-up).** A full sync used to delete every subject (with
  `clearInactive`, the plugin's setting) and every transform and allocate
  them again. Now a subject of the same name takes its object back, reset to
  the state of a new one, its transforms are reused in node order (each
  assigned a freshly constructed `Transform`, so no old state survives) and
  curve names are assigned in place. Subjects the buffer does not carry are
  still dropped, the list is still in buffer order, and a rejected subject
  still leaves the list as a fresh parse would. Parse of one 250-bone,
  250-curve subject: about 160 µs to about 85 µs (MSVC Release), so
  Serialize plus Parse is about 905 µs before WP-A2e and about 190 µs now.
  Pointers to a resynced subject's `Subject` and to its first transforms now
  stay valid across a full sync of the same subject.
  - Tests: new suite `core.parse_reuse_tests`: a sequence of buffers through
    one list (topology growing and shrinking, curves dropped and emptied,
    nameless nodes, subjects reordered, dropped and duplicated, an update for
    a dropped subject, a rejected subject, an empty list) must leave exactly
    the state, result and touched-subject report of a fresh parse after every
    buffer; state the parser never sets (`mReference`, `mJoints`, transform
    tiers) does not survive a resync; objects are reused.
  - Not changed: the second FlatBuffers verification when `PeekMeta` ran
    first (CORE-18 suggests a `Parse` overload that skips it).
- `SubjectList::Parse()` now rejects buffers it used to accept or crash on
  (WP-S1: CORE-1, CORE-8, CORE-9, CORE-23). It returns false with `mError`
  set when a buffer has:
  - a matrix component on a transform without a matching matrix;
  - a parent cycle, a self-parent or an out-of-range parent id;
  - a NaN or Inf translation, rotation, scale, matrix, curve value,
    quantization range or timestamp;
  - more than 256 subjects or updates, more than 4096 transforms or curves
    in a subject, or more than 64 components or matrices on a transform
    (`src/o3ds/parse_limits.h`).

  A rejected subject or update is not applied. The hierarchy is solved in
  one O(N) pass instead of O(N * depth).
- `ParseUpdate()` skips an out-of-range (including negative) index and still
  applies the valid entries after it. It used to stop at the first bad index.
- New `SubjectList::mComputeWorldMatrices` (default `true`, the old
  behaviour). Set it to `false` to skip world-matrix computation when only
  local TRS is consumed; validation runs either way.
- `ClockOffsetEstimator::Observe()` ignores a sample whose timestamps are more
  than one day apart or larger than `kMaxTimestampUs`, as it already did for
  `tx_wallclock_us == 0`. This removes signed-overflow UB on hostile input
  (CORE-26). After a backward step in the sender clock, the next sample no
  longer gets extra slew budget.
- `SubjectList` is now move-only. Copy assignment used to delete every
  subject twice, and the copy constructor silently produced an empty list
  (CORE-17, partial).

- Quantization anchors are re-captured at every full sync on both ends
  (ADR 0005 (vii), WP-S3). `Subject::Serialize()` stores the float32 value it
  writes and `ParseSubject()` stores the value it parses. The old
  anchor-once rule only held for receivers that parse with
  `clearInactive=false`, so senders and receivers could disagree after a
  resync, and a receiver that joined late always did.
- New `src/o3ds/sender_sync.h`: `ConsumeCaptureBudget` (accumulator capture
  rate limiter), `FilterCurveValue` (curve epsilon/delta filter) and
  `FullSyncTracker` (per-subject full-sync policy), used by the UE sender.

- New `src/o3ds/tcp_stream_parser.h`: `TcpStreamParser`, the TCP stream
  frame parser the UE Sockets receiver uses, moved out of the socket code so
  it runs under CTest and the fuzz harness (WP-S6, ADR 0006). New tests in
  `test/tcp_stream_parser_tests.cpp` and a `tcp_stream` fuzz target.

### Fixed

- **One forged or corrupted `tx_seq` no longer blackholes a sequenced stream** (CORE-15).
  `ReorderGate` holds a frame more than `Config::max_forward_jump` (default 4096) ahead of the
  last applied seq as an unconfirmed candidate; it is applied only when the next frame
  confirms it (same epoch, seq + 1). The gate then releases what it buffered, re-baselines and
  counts the skipped range as lost, so a legitimate stream resuming after a long outage loses
  at most one frame of latency. Before, such a frame became the baseline at its gap timeout
  and every later legitimate frame was dropped as stale (0 of 90 delivered in the review's
  PoC); the UE sender has stamped `tx_seq` since #341, so this was live. An unconfirmed
  candidate that is replaced counts as `stale_dropped`. The CRC half of CORE-15 (`PeekMeta`
  checks the CRC) was done in #335. Tests: `core.reorder_gate_tests`
  `ReorderGate_ForgedForwardJump_DoesNotBlackholeTheStream`, `_ConfirmedForwardJump_Rebaselines`,
  `_UnconfirmedJumpCandidates_AreReplacedAndCounted`.

- UDP fragment reassembly (`src/o3ds/udp_fragment.*`, WP-S2) is hardened against hostile or malformed datagrams: fragments that disagree with a message's first fragment are rejected (previously a heap overflow), in-flight state is bounded (8 messages, 16 MiB by default) with age-based expiry, message ids use wrapping comparison, messages are keyed per sender, rejected fragments no longer yield empty frames, and a use-after-free in `UdpMapper::getFrame` is gone.
- UE UDP receiver: `Poll()` is bounded per call (1024 datagrams / 8 MiB), the receive buffer is always 65,507 bytes regardless of `udp.maxdatagram`, and receive scratch buffers are reused.
- Receiver: renamed bones are republished. The skeleton cache now keys on bone names as well as parents, and a full descriptor always rebuilds it, so a rig swap with the same hierarchy no longer keeps the old names (WP-S4, RCV-4).
- Receiver: each packet publishes only the subjects it carried. Subjects missing from a packet are no longer re-pushed with their last pose, so inactive subjects now time out (RCV-5).
- Receiver: several senders on one channel no longer interfere. Each sender gets its own parse state, reorder gate, clock estimator and timestamp ordering, found by the subject names it sends. Before, one sender's full descriptor deleted the others' subjects, and their sequence numbers, epochs and clocks were mixed (RCV-5).
- Receiver: the LiveLink subject is created only on the first push of a session, and only if LiveLink doesn't already have it. Hierarchy or curve changes re-push static data only, so user subject settings are kept (RCV-7).
- Receiver: with render-ahead enabled, one predicted frame is pushed per real frame instead of the same future frame on every tick (RCV-10).
- A transform sent without a name no longer shifts the parent of every later bone. The core keeps it under a placeholder name (`o3ds_unnamed_<index>`), and the receiver treats a null transform as a malformed frame instead of skipping it (RCV-14).
- Receiver: a legacy-path silence or timestamp-jump reset no longer wipes the reorder gate, clock estimator and concealment state. Stopping the transport now also clears the bone-name cache (RCV-34).
- NNG receiver: a mocap frame wrapped in a unified message is passed to the consumer without the 20-byte header, as the TCP and UDP receivers already did. It used to fail to parse (TRB-37).
- Audio and FFI lifetime (WP-S5; design in the ADR 0007 addendum). Audio sinks no longer hold a reference to their sender. Each sender shares a small publish state with its sinks and closes a lifetime gate at the start of `Stop()`, so a sink kept alive by the capture component returns false instead of touching a destroyed sender, socket, LiveKit track or client (TRF-1, TRB-30, TRB-35). A sink created before a `Stop()` stays inactive after the next `Start()`.
- TCP, UDP, NNG and MoQ senders encode audio on the audio thread into sink-local buffers and hand the bytes to the transport worker through a lock-free queue. The audio thread no longer takes the socket lock, and UDP audio is sent from a new audio worker thread instead of the audio thread (TRB-10). The worker's wake event now lives as long as any sink that can trigger it (TRB-12).
- Each audio sink has its own encoder per stream label, created from the configuration at sink creation. Creating a sink no longer reconfigures an encoder that another thread is using (TRB-11, TRF-10). The WebRTC sink converts PCM in a per-call buffer with rounding instead of truncation (TRF-40).
- Receivers decode Opus with one decoder per `(SourceGuid, StreamLabel)`, so interleaved streams no longer corrupt each other (SHR-15).
- MoQ, NNG (sender) and WebRTC (sender) FFI callbacks receive an opaque token instead of an object address. A callback that arrives after its owner is gone does nothing (TRF-12).
- The receiver's audio sink holds a snapshot of the audio defaults instead of a back-reference to the LiveLink source, and a frame delivered off the game thread is forwarded to the game thread before the source is pinned (RCV-1). The WebRTC receiver reads its audio sink under a dedicated lock.
- The sender audio capture component hands its audio and capture threads an immutable parameter snapshot, and each producer has its own scratch buffer; they no longer read the component (SND-6). The submix listener is unregistered from the submix it was registered on, and registering it again first removes the old registration (SND-7).
- `FO3DAudioBus` is game-thread-only and checks it; publishing with no listener returns early (SHR-10).
- WebRTC (WP-S7): auto-fetch token mode now connects. The transport tracks a pending connect separately from the token and connects on the next `Tick()`/`Poll()` after the token arrives (TRF-3). Fetch results are stored by the token manager and read on the game thread; the HTTP callback no longer writes transport members (TRF-15).
- WebRTC: refreshed tokens are passed to LiveKit with `lk_refresh_token`. If the receiver's refresh call fails it reconnects with the new token; the sender keeps its session and uses the new token on its next connect (TRF-23). An expired token is logged once instead of every frame.
- WebRTC: token fetch callbacks and retries hold the fetcher's state weakly instead of `this`, retries use the core ticker instead of the `GWorld` timer manager, completion delegates are unbound before a request is cancelled, and the 30 s fetch timeout is enforced (TRF-24).
- WebRTC sender: `Send(SubjectList)` serializes the caller's list once. It no longer copies the caller's transform pointers into a pooled subject, which could delete them twice on a failed send (TRF-4). The serializer pool is gone (TRF-19).
- WebRTC sender: the estimated pending-frame counter no longer drops frames or logs warnings on a healthy link. A send fails only when `lk_send_data_ex` reports an error (TRF-5).
- WebRTC: audio track names, data channel labels, URLs and tokens are converted to UTF-8 with a converter that lives until the LiveKit call returns (TRF-2). The receiver decodes labels and track names as UTF-8, so non-ASCII subject names match on both sides, and the CRC-keyed label cache is removed (TRF-31).
- WebRTC receiver: only the labeled data callback is registered. The unlabeled callback is registered only if the labeled one cannot be, so a packet is no longer queued twice or under a phantom `default` subject (TRF-16).
- WebRTC receiver: LiveKit callbacks receive an opaque registry token instead of the receiver's address, and the state they touch moved into a separate object. This removes the WP-S5 exception for the receiver. The receiver no longer logs part of the token.
- WebRTC: each sender and receiver uses its own LiveKit identity (`sender-<pid>-<id>`, `receiver-<pid>-<id>`), so two in one process no longer disconnect each other (TRF-25).

- UE sender (WP-S3): bone names and parents are correct after Stop/Start,
  after a details-panel edit during PIE, and after a subject rename. Each
  frame now carries its skeleton descriptor, and a frame whose bone count
  does not match it is dropped with a rate-limited warning instead of being
  padded with empty names and parent 0 (SND-1).
- UE sender: quantized translations stay correct across full syncs,
  including for receivers that join mid-stream (SND-2).
- UE sender: in residual and quantized modes, curve values no longer land on
  the wrong curve names when the curve set changes at the same count, and
  per-frame curve filtering no longer forces a full sync every frame
  (SND-3).
- UE sender: a curve that returns to zero is sent once as 0, so receivers no
  longer hold its last non-zero value (SND-4).
- UE sender: at a tick rate close to `CaptureRateHz` (default 60 Hz) the
  sender now captures every tick instead of about half of them (SND-5).
- UE sender: changing the encoding mode, residual predictor, keyframe
  interval or quantization ranges during capture takes effect on the next
  frame with a full sync (SND-14).
- UE sender: `StartCapture()` with no valid mesh and audio disabled no
  longer leaves a transport and serializer running. The reason is available
  from the new `GetLastStartCaptureError()` (SND-19).
- UE sender: curve include/exclude patterns are ignored while curve
  filtering is disabled, and pattern edits apply on the next frame
  (SND-20).

- TCP receiver (WP-S6): every frame in a read is delivered. The receiver used to keep only the first complete frame and discard the bytes after it, so frames were lost whenever TCP coalesced them, for example during a burst or with audio and mocap interleaved (TRB-1). Frames already buffered are delivered before the socket is read again.
- TCP receiver: after garbage or a bad header it skips to the next frame header in one pass instead of dropping one byte per poll (TRB-8). A header announcing a frame above `tcp.maxframe` is rejected and counted in `DroppedFrames`, and the buffer grows only as bytes arrive and shrinks after a large frame (TRB-9).
- TCP receiver: the reconnect backoff now grows from 0.5 s to 5 s. It was reset on every attempt and stayed at 0.5 s (TRB-4). It resets once a connection delivers data. A connect that is still pending after `tcp.connecttimeout` seconds is abandoned and retried, so an unanswered connect no longer leaves the receiver in the connecting state forever (TRB-5). A sender that closes or restarts is noticed on the next poll instead of after the idle timeout.
- TCP sender: a partial send or a full socket buffer no longer drops the client. The worker waits for space and finishes the frame, and drops the client only on a socket error or when a frame makes no progress for `tcp.stalltimeout` (TRB-2). A frame is never left half-written on a connection that stays open.
- TCP sender: accepting, sending and closed-peer detection moved to the worker thread, which owns the client socket. The game thread no longer takes a socket lock that the worker held during sends. A receiver that closes its connection is noticed within about 250 ms, so a reconnecting receiver no longer waits in the listen backlog until a send fails (TRB-6).
- TCP sender: an idle sender writes a keepalive every second, so receivers no longer reconnect every `tcp.timeout` seconds while nothing is being sent (TRB-6).
- TCP sender: `Start()` creates the listen socket before starting the worker, so a failed bind or listen no longer leaves a worker thread running. `Stop()` keeps the socket subsystem, so `Stop()` then `Start()` works without `Initialize()`. The TCP receiver behaves the same way (TRB-13).
- TCP transport: `tcp.timeout` set in the receiver's transport options now reaches the receiver. It was stored but never passed on.
- MoQ (WP-S8): after a dropped connection and a reconnect, the sender
  announces its namespaces again. Each connect attempt now uses a new
  moq-ffi client, and the session's announce cache belongs to that client,
  so a reconnect used to skip the ANNOUNCE and publish on a namespace the
  relay did not know (TRF-8).
- MoQ: the send worker no longer copies publisher handles while the game
  thread resets them. Both sides go through a lock, and the worker publishes
  on its own snapshot (TRF-9).
- MoQ: a connect attempt always ends. When `moq_connect` returns an error
  without a FAILED callback, the session reports FAILED itself, and an
  attempt that has not finished within the new `connect_timeout` option
  (default 15 s) is abandoned and retried on a new client. The sender and
  receiver used to stop retrying for good in both cases (TRF-11).
  `moq_connect` also no longer holds a lock that `Disconnect()` needs, so
  stopping during a slow connect no longer blocks the game thread.
- MoQ: FFI callbacks reach the game thread through a queue drained by a
  core ticker instead of a dedicated dispatcher thread. The module stops it
  before unloading moq-ffi, drops callbacks that arrive afterwards, and no
  longer restarts it lazily during shutdown (TRF-13).
- MoQ receiver: a failed subscribe is retried with capped, jittered
  exponential backoff instead of on every `Poll()` (TRF-20). Reconnects use
  the same backoff with jitter.
- MoQ receiver: audio is decoded with the codec written in each frame's
  header. The receiver used its own codec setting, so every frame failed
  when it differed from the sender's (TRF-37).
- MoQ: `moq_last_error()` is read only on the thread of the call that
  failed, right after it, and its text is kept with the connect error. The
  connection callback no longer reads it, since that thread's value belongs
  to a different call. The `catch (...)` around `moq_connect` is removed: it
  cannot catch a Rust panic, which moq-ffi already converts into an error
  result (TRF-39).

- Loopback: `SendSerialized()` and `Send()` return false before `Start()` and
  after `Stop()`. They used to queue frames on an initialized sender that was
  not running. Its stats counters are now updated under a lock, so concurrent
  senders no longer lose updates or make `GetStats()` go backwards (WP-T2
  conformance suite).
- UDP: `Start()` after `Stop()` works again without `Initialize()`, as it does
  for TCP. `Stop()` cleared the socket subsystem, so the restart failed
  (WP-T2 conformance suite).
- WebRTC: an address with a port, such as `127.0.0.1:7880`, now gets `ws://`
  like the bare host. Only the host part is compared with the localhost list,
  which also accepts `[::1]`. It used to get `wss://`
  (`Open3DBroadcast.Transport.WebRTC.Token.SenderAutoFetchConnects`).
- Receiver: the unused `LastObservedSubjectName` field is removed. Audio
  metadata falls back to the stream label, which is what
  `FinalizeAudioMeta` already did (RCV-2, ADR 0006 Q6).

- NNG: the send and receive buffers are now set as message counts (1024), which is what
  NNG expects. The sender passed a byte count with the wrong type, so the call failed
  silently and a pub socket kept its small default queue, dropping frames from any burst
  (TRB-36, found by the new NNG conformance round-trip test).
- NNG fixes (WP-S11):
  - Sender: a worker thread owns the socket while the sender runs. `Start()` opens it
    before the worker exists, the worker closes and reopens it, and `Stop()` closes it
    after joining the worker. `Tick()` no longer reopens it from the game thread, which
    could free the socket while the worker was sending (TRB-33). `Initialize()` now stops
    a running sender first.
  - Sender: when NNG returns `NNG_EAGAIN` (no peer ready, or its send buffer is full) the
    oldest queued frame is dropped and counted in `DroppedFrames`. It used to be put back
    at the tail of the queue, which reordered frames, spun the worker while no peer was
    connected and replayed a stale backlog on reconnect (TRB-34).
  - Sender: `NNG_OPT_SENDTIMEO` is no longer set. Every send is non-blocking, so the
    30-second timeout never applied (TRB-36).
  - A host in the `Uri`, its `?host=` query or the stream id is used when the `host`
    option is empty. The default host used to win every time (TRB-39).
  - Mode and role defaults and allowed combinations live in one place (`NngHelpers`).
    Pair defaults to listen on the sender and dial on the receiver; the receiver module
    used to make both ends listen. The receiver's "Pull (client dial)" now dials, and
    the sender has a new "Push (listen)" choice; push and pull used to be forced to dial
    and listen (TRB-40).
  - Receiver: `NNG_OPT_RECVMAXSZ` is set to the 50 MiB cap, so NNG's smaller default no
    longer drops large frames before `Poll()` sees them. An audio frame is counted once in
    `FramesReceived`, not twice. Pipe callbacks get an opaque token instead of the
    receiver's address. A dialing socket reads as connected only after a pipe event, not
    right after the non-blocking dial (TRB-42).
  - Open, listen, dial, parse and send failures, and a full send queue, are logged at
    Warning (rate-limited where they repeat). Drops while no peer is ready are logged at
    Log, at most every 2 s. Connection changes are logged at Log (TRB-43).

- Audio codec (WP-S10). A frame's codec label now always matches its payload.
  When Opus is compiled out (every platform but Win64 with `opus.lib`), cannot
  run at the stream's format (for example 44.1 kHz or more than 2 channels), or
  fails, the frame is sent as PCM16 and labelled PCM16. It used to be labelled
  Opus while carrying PCM16 (SHR-1).
- Opus is now actually used when selected. Capture buffers of any size are
  collected into exact 20 ms Opus frames, so one buffer yields zero or more
  packets, each stamped with the capture time of its first sample. The first
  encode error no longer switches the stream to PCM16 for good: failures are
  counted and logged at Warning (throttled), the packet is sent as PCM16, the
  encoder is recreated after 3 failures in a row, and a failed initialisation
  is retried (SHR-2).
- Opus: encoder settings (rate, channels, frame duration) are validated,
  `opus_encoder_ctl` results are checked, the encoder's packet buffer is the
  4000 bytes libOpus recommends, and the decoder accepts 120 ms packets
  (SHR-31).
- Audio parsing range-checks untrusted metadata: 1 to 8 channels (1 or 2 for
  Opus), a known sample rate (Opus rates for Opus), a finite timestamp, and
  stream label and subject names of at most 256 bytes (SHR-8).
- Performance metrics: transport counters live in heap entries that never
  move, so registering a new transport name no longer invalidates pointers
  other threads write through (a use-after-free). Rolling averages and peaks
  use compare-exchange, so concurrent updates are not lost and a peak never
  goes down. `o3d.DumpMetrics` copies the counters and logs without holding a
  lock (SHR-3, SHR-17, SHR-26).
- UE sender: the audio stream label is the pose subject name (sanitized, or
  generated as World/Actor/Component), and it follows renames. It used to be
  the raw `SubjectName`, or `o3ds:audio` when that was empty (SND-16).
- UE sender: audio resampling keeps its state across capture buffers, so the
  output length no longer drifts and there is no discontinuity at buffer
  boundaries, and it low-pass filters (8th-order Butterworth at 0.45 x the
  output rate) before downsampling instead of aliasing (SND-21).
- UE sender: the submix tap is registered only while an audio sink is bound,
  so a disabled or unbound audio path no longer processes every audio buffer
  (SND-28).
- Audio send path allocates and copies less: PCM16 is converted straight into
  the frame, Opus packets are encoded into a reused buffer, the unified
  envelope is written in place in front of the audio payload, decode scratch
  buffers keep their allocation, and the audio bus no longer copies each
  frame (SHR-18).

### Changed

- **A new receiver gets a full Subject on the next frame** (ADR 0005 (vi)). `IOpen3DSender`
  gained `SetPeerJoinedCallback(FO3DPeerJoinedCallback)` (default: no-op), called on any thread
  when the sender gains a peer that has seen nothing yet. The sender pipeline sets the callback
  when it attaches a sender; it only sets an atomic flag, which the worker consumes before the
  next frame and answers with a full sync of every subject
  (`FO3DSenderSerializer::RequestFullSyncAll`). A receiver joining mid-stream no longer holds
  the subject until the periodic full sync (`FullSyncIntervalSeconds`).
  - **TCP** calls it when it accepts a receiver; **NNG** for each added pipe (a subscriber, the
    pair peer, a pull socket). Both now report `bPeerJoinSignal`. UDP and MoQ have no join
    event; WebRTC (participant join) waits for verification against the LiveKit FFI (ADR 0005
    Q4).
  - Transport API: stays `O3D_TRANSPORT_API_VERSION` 5 (no release since v0.9.6 carried it).
    A third-party sender needs no change; one that can detect peers overrides the new method
    and sets `bPeerJoinSignal`.
  - Tests: `Open3DBroadcast.Sender.Pipeline.PeerJoinedForcesFullSync`,
    `Open3DBroadcast.Transport.Sockets.Tcp.PeerJoinedOnAccept`, the NNG data round trip (the
    subscriber's pipe is reported), and the capability expectations for TCP and NNG.

- **Residual coding is used only on transports that deliver reliably and in order** (ADR 0005
  (iii)). With `bEnableResidualCoding` on a transport whose capabilities report `Unreliable`
  or `Unknown` (UDP, NNG pub, MoQ, WebRTC with `webrtc.prefer_lossy`), the sender sends
  what it would with residual coding off (full snapshots, or quantized updates only when
  `bEnableQuantization` is set itself) and logs one Warning per capture and transport. An
  earlier version of this change fell back to quantized frames, which switched quantization on
  without the user enabling it; that was changed before release because quantization has known
  rotation artifacts (CORE-12, #247). The decision uses the running sender's `GetCapabilities()`, so a
  transport attached or switched mid-capture changes the mode, and the encoding fingerprint
  makes the next frame a full sync. Loopback, TCP, NNG pair or push, and WebRTC's reliable
  channel keep residual coding.
  - **Details panel:** the Residual category shows the same warning while residual coding is
    on and the selected transport, with its current options, is not `ReliableOrdered`
    (`UO3DSenderComponent::GetConfiguredResidualFallbackWarning`, from the transport
    registry's capabilities; no secrets are read). The Residual and quantization tooltips now
    describe this and the receivers' hold until the next full sync.
  - API: `UO3DSenderComponent::ResolveEncodingMode`, `GetResidualFallbackWarning`,
    `GetConfiguredDeliveryGuarantee`, `GetConfiguredResidualFallbackWarning`.
  - Tests: `Open3DBroadcast.Sender.Encoding.ResidualFallsBackOnUnreliableTransports` (the
    decision table, the configured warning, and a capture on the fake transport made
    unreliable by the new `fake.delivery` option: quantized updates and one warning).

- **Receivers hold a subject instead of applying an update they cannot decode correctly**
  (ADR 0005 (ix); CORE-5, CORE-6). For sequenced frames (every UE sender since #341), an update
  relative to a full Subject the receiver missed, or a residual update after a lost frame, is
  dropped, and the subject is not pushed to LiveLink until its next full Subject (at most
  `FullSyncIntervalSeconds`, 1 s by default; concealment or LiveLink's hold covers the gap).
  Before, such updates were applied to the wrong anchors or residual history and the pose was
  wrong until the next full Subject. A receiver that joins mid-stream also waits for a full
  Subject. New receiver metric `UpdatesAwaitingFullSync` counts the dropped updates.
  - Residual streams recover only at a full Subject, not at a residual cadence keyframe: a
    cadence keyframe does not reset the predictor history on either end, so decoding from it
    after a loss would still diverge (a deviation from ADR 0005 (ix), recorded in its
    implementation notes).
  - **The sender sends a full Subject on the next frame after a full Subject or residual update
    it did not deliver** (refused by the transport for any reason, or serialized with no
    transport attached; before, only a full Subject refused with `DroppedBackpressure` was
    followed by one), so a hold caused by the sender lasts one frame instead of up to
    `FullSyncIntervalSeconds`. A refused quantized update stays applicable across the gap and
    changes nothing. Network loss and a receiver joining mid-stream still wait for the next
    periodic full Subject (until the peer-joined trigger, ADR 0005 (vi)).
  - Unsequenced residual streams (parsed without a context) have no safe resync point either
    except a full Subject: the decoder refuses updates it has no history for, but rebuilds
    history from later cadence keyframes, which does not match the sender. No current sender
    writes unsequenced residual frames.
  - Tests: core `resync_contract_tests` (missed full Subject, residual gap with the pose
    matching the sender on every frame after the resync, new epoch, unset `ref_seq`, decoder
    without history, gap detection); `Open3DBroadcast.Receiver.Correctness.
    ResidualGapHoldsUntilFullSync` through the real receiver source;
    `Open3DBroadcast.Sender.Pipeline.RefusedResidualUpdateForcesFullSync` and
    `.UndeliveredFullSyncIsSentAgain`. The decoder tests
    that asserted the zero-reference fallback now assert the drop.

- **The UE sender stamps its frames** (SND-15, CORE-29; ADR 0005 (iv)). `FO3DSenderSerializer`
  writes every frame through a per-subject `O3DS::StreamWriter`, in the legacy, residual and
  quantized encodings, so frames carry `tx_seq`, `tx_wallclock_us` and `frame_epoch`. Receivers
  now take UE senders' frames through the reorder gate (reordering, duplicate drop), map their
  clock from `tx_wallclock_us`, and can conceal starved subjects; before, only the legacy
  timestamp check ran. The counter is per subject because a receiver keys streams by subject
  names and each frame carries one subject. Stop clears a subject's writer with its cache; the
  next frame starts a newer epoch, which the gate takes as a restart. A payload serialized while
  no transport is attached is not sent, so the receiver sees that `tx_seq` as lost. Test:
  `Open3DBroadcast.Sender.Wire.FramesTakeTheGatedPath`; `LegacyReuseKeepsBytes` now builds its
  expected frame with the same stamp and checks the per-subject numbering.

- Editor categories renamed from Open3DStream to Open3DBroadcast; CreatedBy updated. The Details panel, Blueprint action menu and Add Component list now group the plugin's properties, functions and components under **Open3DBroadcast** (for example `Open3DBroadcast|Sender|Control`) instead of **Open3DStream**. Only display categories changed: property names, config sections and ini keys are the same, so saved levels, Blueprints, LiveLink presets and project settings load unchanged. The LiveLink source is still listed as "Open3DStream Receiver". Both `.uplugin` files now say `"CreatedBy": "Lifelike & Believable and Open3DStream Contributors"`; `CreatedByURL` is unchanged.
- The largest reassembled UDP message the receiver accepts drops from 50 MiB to 4 MiB by default; set the new `udp.maxframe` receiver option to raise it (up to 50 MiB).
- Core API: `UdpMapper::addFragment` now takes `(sourceKey, data, size, nowMs)`, and `UdpMapper` takes an optional `UdpReassemblyConfig`.
- Core API: `SubjectList::Parse()` takes an optional `std::vector<ParsedSubjectInfo>*` that reports the subjects a packet touched and whether each got a full descriptor. `ParseUpdate()` and `ParseUpdateResidual()` return `bool` (applied or not) instead of `void`. A node with a null entry in a subject's node list is rejected.
- Core API: new `src/o3ds/receiver_streams.h` with `PeekPacketMeta`, `SkeletonFingerprint`, `LegacyOrdering` and `ReceiverStreamTable`, the UE-independent receiver state used by `FO3DReceiverSource`.
- Core API: `ConcealmentEngine::TryRenderAhead()` returns true at most once per `ObserveRealFrame()`.
- Receiver: legacy (no `tx_seq`) timestamp ordering now runs before the packet is parsed, so a dropped duplicate or out-of-order frame no longer changes parse state. A packet that fails verification is rejected before either path.
- WebRTC: new `webrtc.room` option, shown as **Room** in the sender and receiver panels. Auto-fetch mode requests tokens for this room on both sides and fails to initialize when it is empty. `StreamId` is no longer used as the room name, so existing auto-fetch setups must set a room (TRF-25).
- WebRTC: the transports call LiveKit through a per-instance function table (`FLkFfiApi`, ADR 0006 option F2) and take an optional token fetcher factory, so tests can run them against a fake. The mock token server takes a `LIVEKIT_API_KEY` issuer so its tokens work with `livekit-server --dev`. Manual test steps are in `docs/testing/webrtc-manual-test.md`.

- UE sender: new `FullSyncIntervalSeconds` property (default 1.0 s, 0.25 to
  10 s). In residual and quantized modes a full skeleton and pose is sent at
  least this often, and also on start, rename, and any change to the
  skeleton, the curve list or the encoding settings (SND-13, ADR 0005 (ii)).
- UE sender: per-frame curve epsilon/delta filtering is off in residual and
  quantized modes; include/exclude patterns still apply.
- UE sender: a NaN or Inf curve value is sent as 0 when "Drop NaN and
  Infinity" is on, as the property describes. It used to drop the curve for
  that frame.
- UE sender: `FO3DSenderSerializer` no longer listens to `OnDescriptorReady`
  and no longer reads component properties. Frames carry the descriptor and
  an `FO3DSenderEncodingSettings` snapshot; `SerializePoseFrame()` is public.

- TCP transport options (documented in `Transport_Module_Comparison.md`): new receiver options `tcp.connecttimeout` (default 5 s), `tcp.maxframe` (default 4 MiB, up to 50 MiB), `tcp.backoff` (default 500 ms) and `tcp.maxbackoff` (default 5000 ms); new sender options `tcp.maxqueue` (default 4 MiB, the old fixed cap), `tcp.maxqueueage` (default 1000 ms), `tcp.stalltimeout` (default 2000 ms) and `tcp.keepalive` (default 1000 ms) (TRB-9, TRB-14).
- The largest TCP frame the receiver accepts drops from 50 MiB to 4 MiB by default; raise it with `tcp.maxframe`.
- TCP sender: frames that waited in the send queue longer than `tcp.maxqueueage` are dropped before sending and counted in `DroppedFrames`, so a slow link no longer builds up seconds of stale mocap (TRB-14). Set it to 0 to keep every frame. The queue byte accounting uses the atomic queue from WP-S5 (TRB-3); WP-A1 replaces it with the shared `FO3DSendQueue`.
- TCP receiver: each `Poll()` handles up to 256 frames or 8 MiB, up from 16 frames.
- MoQ: new `connect_timeout` (alias `moq.connect_timeout`) transport option,
  in seconds, default 15, clamped to 1-120 (WP-S8).
- MoQ: library validation checks every moq-ffi export the module binds,
  and `moq_version` is required. A missing version used to be treated as
  optional and then fail the Draft 07 check anyway (TRF-29).
- MoQ: every moq-ffi call goes through a per-instance function table,
  `FMoQFfiApi` (ADR 0006 option F2). The session wrapper, sender and
  receiver accept a table at construction, which the new fake-FFI tests use.

- Shared audio API (WP-S10): `O3DAudio::FFrameEncoder::BuildEncodedFrame` is
  replaced by `EncodeBuffer`, which appends zero or more frames.
  `FO3DSinkAudioEncoder::Encode` now returns `TArray<O3DAudio::FEncodedFrame>`
  and `EncodeUnified` returns `TArray<TArray<uint8>>`; each message carries its
  own frame's timestamp.
- `FO3DOnAudioPcm16` (the audio bus delegate) passes `TConstArrayView<uint8>`
  instead of `const TArray<uint8>&`. The view is valid only during the
  broadcast; listeners that keep the bytes must copy them.
- `O3DAudio::DeserializePcm16Frame` and `DeserializeEncodedAudioFrame` take an
  optional `EAudioParseError*` that says why a buffer was rejected. New
  `ValidateAudioMeta`, `IsSupportedSampleRate` and
  `SerializeEncodedAudioFrameAfterPrefix`; new `O3DS::WriteUnifiedHeaderInPlace`.
- `FO3DPerformanceMetrics`: transports get a stable handle with
  `AcquireTransportMetrics(FName)` and update it without a lock (the MoQ, NNG
  and WebRTC senders now do). `GetOrCreateTransportMetrics` and
  `GetAllTransportMetrics` are removed; use `AcquireTransportMetrics`,
  `FindTransportMetrics` (returns a shared pointer) and
  `GetTransportMetricsSnapshot`. `GetAllocationRecords` returns a copy. The
  metric fields are `std::atomic`, and the unused `LastCaptureTime` and
  `LastApplyTime` fields are removed.
- `FO3DAudioOpusEncoder` and `FO3DAudioOpusDecoder` are no longer copyable.
  New `GetFrameSizeSamples`, `GetLookaheadSamples` and
  `FO3DAudioOpusDecoder::DecodeLost` (packet-loss concealment).
- New `FO3DAudioResampler` (Open3DShared), used by the sender's capture path.
- `EO3DSenderAudioSource::GameAndMic` was never implemented; it is now hidden
  in the editor. The capture source always follows the capture mode.

### Credentials (WP-S9, ADR 0004)

Transport credentials are no longer saved with levels, Blueprints, `GameUserSettings.ini` or LiveLink connection strings and presets, and are no longer logged (SND-10, RCV-3, TRF-21, TRF-22, SHR-11, LIC-1).

- Each transport customization declares its secret option keys (`SecretOptionKeys`) and, optionally, an environment variable per key (`SecretEnvVars`). WebRTC declares `webrtc.token` and the new `webrtc.tokenEndpointAuth`.
- A secret is resolved when the transport starts, in this order: the value set in this session (editor panel, or the new Blueprint node **Set Transport Secret**), then an environment variable, then a value remembered on this machine (editor only). The variables are `O3DB_WEBRTC_TOKEN` and `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`; for a credential profile other than `default`, `<NAME>__<PROFILE>` is tried first.
- New `<transport>.credentialProfile` option (for WebRTC, `webrtc.credentialProfile`, default `default`). It is saved with the component or source and selects which stored credentials apply.
- "Remember on this machine" stores the value in the per-user `EditorPerProjectUserSettings.ini` under `Saved/Config`, Base64-encoded but not encrypted. It is never written to the project's `Config/` folder and is not available in packaged games.
- WebRTC panels: the Access Token and the new Token Endpoint Credential are password fields that open empty. Each shows where its value comes from ("Not set", "Set for this session", "Remembered on this machine", "From environment variable ..."), with a **Clear** button and a "Remember on this machine" checkbox. A warning appears when Auto Token Fetch is off and no token is available. There is a new Credential Profile field.
- New `UO3DCredentialLibrary` with `SetTransportSecret` and `ClearTransportSecret` for Blueprints and C++ in packaged games. There is no getter.
- New C++ API in Open3DShared: `FO3DSecretStore`, `O3DRedact::Value`, `O3DRedact::Url` and `O3DHelpers::IsHttpsOrLoopbackHttpUrl`; `FO3DTransportConfig::Secrets` carries resolved secrets to the transport. `UO3DSenderComponent` gains `SetTransportSecret`, `ClearTransportSecret`, `SetTransportSecretPersistence`, `GetTransportSecretStatus`, `IsTransportSecretKey` and `GetCredentialProfile`. Open3DShared now contains a `UCLASS` and runs UHT.
- `FO3DTransportConfig::ToDebugString()` prints secrets as key and `<set>` only, redacts values whose key looks sensitive (`token`, `secret`, `password`, `auth`, `apikey` and similar), and prints URLs without query values, user-info or fragment.
- MoQ relay, NNG, WebRTC and receiver source URLs are logged through `O3DRedact::Url`. The WebRTC token fetcher no longer logs response bodies; it logs the status code and response length.
- WebRTC token requests send `Authorization: Bearer <value>` when `webrtc.tokenEndpointAuth` resolves, and no longer send a `grants` object. The token endpoint must authenticate callers and decide grants itself; see "Token endpoint requirements" in the WebRTC `USER_GUIDE.md`.
- `Tests/mock-token-server.py` (WebRTC module) exits unless `API_SECRET` is set, ignores grants sent by the client, rejects roles other than `publisher` and `subscriber`, and computes token times in UTC (TRF-35).
- `FO3DTransportConfig::bPersistToken` is deprecated. Nothing reads or sets it; WP-A1 removes it.

**Breaking changes**

- **Migration warning and resave.** When a level, Blueprint, `GameUserSettings.ini` or LiveLink connection string saved by an older version still contains a declared secret, it is moved into this session's store and removed from the loaded data, and one Warning names the asset or source and the key (never the value). The file on disk still contains the secret until you resave the asset or recreate the LiveLink source. Nothing is saved automatically. Migration only happens when the transport module is loaded.
- **Tokens no longer survive an editor restart by default.** Tick "Remember on this machine", set an environment variable, or use Auto Token Fetch.
- **Plain `http://` token endpoints are refused** unless the host is `localhost`, `127.0.0.1` or `::1`. Use `https://` for any other host.
- **Token endpoints no longer receive `grants`.** A token server that relied on client-sent grants must decide them from the caller's role or identity.
- **The mock token server needs `API_SECRET`.** It no longer defaults to `test-secret`.
- `UO3DSenderComponent::GetTransportOption` returns an empty string for a declared secret key, and `SetTransportOption` stores such a key in the secret store instead of `TransportOptions`. `webrtc.token` is no longer copied into `FO3DTransportConfig::AdvancedParams`; read it from `Secrets` (or `Token`, filled by the WebRTC customization).
- `FO3DTokenFetchRequest::AdditionalGrants` is removed.

### Binaries, symbols and DLL loading (WP-F3)

- The committed `moq_ffi.pdb` and `livekit_ffi.pdb` are removed from the
  plugin, and `Open3DTransportMoQ.Build.cs` no longer stages `moq_ffi.pdb`
  into packaged games (FAB-4). Symbols go to release assets; see
  "Debug symbols" in `Build/README.md`, which also shows how to get the
  removed files from git history. `fab-package.py` now fails when git tracks
  a `.pdb` anywhere under the plugin, including in an excluded module; the
  `**/*.pdb` exclude rule that used to drop them silently is gone.
- New `FO3DFfiLibrary` in Open3DShared (`O3DFfiLibrary.h`, ADR 0007 item 7).
  The MoQ and WebRTC modules load `moq_ffi.dll` and `livekit_ffi.dll` through
  it; the three separate loaders are gone, including the WebRTC receiver's,
  which probed two paths that do not exist (TRF-28). The DLL locations are
  unchanged.
- WebRTC no longer registers its transport (factories and editor panels) when
  `livekit_ffi.dll` fails to load. It used to register anyway, and the first
  LiveKit call then failed on delay-load (TRF-14). MoQ already behaved this
  way.
- Module shutdown in MoQ and WebRTC stops every live transport instance
  before unloading the FFI DLL. If a component or LiveLink source still holds
  an instance after that, the DLL stays loaded until process exit instead of
  being freed under it, and a warning names the count (TRF-14).
- Third-party includes (o3ds core, moq_ffi, livekit_ffi, nng, opus) are
  wrapped in `THIRD_PARTY_INCLUDES_START`/`END`, and the o3ds, moq_ffi and
  livekit_ffi include directories are `PublicSystemIncludePaths` (BUILD-3).
- New tests `Open3DBroadcast.Shared.FfiLibrary.*` cover path lookup, load
  failures and the shutdown sequence with a live (fake) transport instance.

### Developer material out of the plugin (WP-F6)

- Planning, review and analysis notes move from the plugin's `Source/` to
  `docs/dev/<Module>/` (HYG-1): five MoQ documents and three WebRTC
  documents. The WebRTC mock token server and its README move to
  `docs/dev/Open3DTransportWebRTC/Tests/`; run it from there
  (`docs/testing/webrtc-manual-test.md` is updated). The module READMEs and
  the WebRTC USER_GUIDE stay in `Source/`.
- The root-level `MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md`,
  `MOQ_PLAN_SUMMARY.txt` and `PRODUCTION_READINESS_JWT_TOKEN_AUTO_FETCH.md`
  move to `docs/dev/` (HYG-4).
- `fab-package.py` fails when git tracks a `.py`/`.pyc` anywhere under the
  plugin, or a Markdown file under `Source/` other than a `README.md` or
  `USER_GUIDE.md` outside `ThirdParty/`, including in an excluded module.
  The `.py`, `.pyc` and `Source/*/Tests/**` exclude rules are gone.
- The `o3d.ProfileGuide` console command is removed. It printed internal
  investigation notes at Warning level (SHR-27). `o3d.DumpMetrics` and
  `o3d.ResetMetrics` are unchanged.
- The NNG README no longer contains a public IP address; the repeater
  example uses `<repeater-host>` (TRB-44, IP part).
- ProjectSandbox opens `ReceiverTest` in the editor instead of the missing
  `O3DBroadcastMap`, its `.uproject` is strict JSON, its `.gitignore` no
  longer ignores the tracked `Content/`, and `.ignore` drops the old
  `Open3DStream` paths (HYG-3).
- Deleted, as nothing uses them (CI-7): `package.py`, `o3ds.nsi`,
  `usr/UE_5.4` and `usr/UE_5.5` plugin stubs, `scripts/test_package_layout.py`,
  `build.bat`, `build_env.bat`, `build_doc.bat`, `thirdparty/build.bat`,
  `update_protocol.bat`, and the `create-webrtc-audio-issues.yml` workflow
  with its script. `Setup-UE.ps1` defaults to UE 5.7; `windows.yml`,
  `linux.yml` and `doc.yml` use `actions/checkout@v4`; `doc.yml` installs
  `breathe` and deploys with `GITHUB_TOKEN`.

### One transport registry (WP-A1 PR 1, ADR 0007 step 1)

- **Interfaces moved to Open3DShared.** `IOpen3DSender`, `IOpen3DReceiver`,
  their audio sinks, `ISerializedFrameConsumer` and `FO3DTransportConfig` now
  live in `Open3DShared/Public/Transport/` (`O3DSenderInterface.h`,
  `O3DReceiverInterface.h`, `O3DSerializedFrameConsumer.h`,
  `O3DTransportTypes.h`) and are exported by Open3DShared. Class layouts and
  virtual function tables are unchanged.
- **One registry (SHR-12, SND-23, RCV-28).** New
  `Transport/O3DTransportRegistry.h`: a transport registers one immutable
  `FO3DTransportDescriptor` (sender and receiver factories, the configure
  functions, and per-role secret keys and option schema, which is the WP-F7
  `FO3DTransportOptionSchema`) with `FO3DTransportRegistry::Get().Register()`
  and keeps the returned `FO3DTransportRegistration`, whose destructor
  unregisters. A second registration under a taken name is refused (it used
  to override silently), and so is a descriptor built for another
  `O3D_TRANSPORT_API_VERSION`. `OnTransportsChanged` fires after each change.
  Every transport (TCP, UDP, NNG, MoQ, Loopback, and WebRTC in the add-on)
  now registers once instead of four times.
- **Pickers match what can be created (RCV-28).** The sender component's and
  the LiveLink source's transport lists, the Details panel and the "Add
  Source" panel list exactly the names that have a factory for that role,
  from the same entries `CreateSender` and `CreateReceiver` use. A transport
  that registered only a customization is no longer listed.
- **No pointer into the registry (RCV-27).** `Find()` returns a
  `TSharedPtr<const FO3DTransportDescriptor>` that stays valid after the
  transport unregisters; the component and source call the configure
  function through it, outside the lock.
- **Deleted:** `FSerializedFrameConsumerRegistry` and
  `SerializedFrameConsumerRegistry.cpp`, which nothing ever populated
  (SHR-24). The Loopback and WebRTC receivers no longer fall back to it; a
  receiver without `SetConsumer` drops frames as before.
- **Deprecated, removed in the next minor release** (with an
  `O3D_TRANSPORT_API_VERSION` bump): `O3DSenderInterface.h`,
  `O3DReceiverInterface.h`, `O3DSenderRegistry.h`, `O3DReceiverRegistry.h`,
  `O3DSenderTransportCustomization.h`, the customization part of
  `O3DReceiverTransportCustomization.h`, `Open3DShared/Public/O3DTransportTypes.h`
  and `SerializedFrameConsumerRegistry.h`. They are forwarding shims:
  `O3DTransport::RegisterSender`/`RegisterReceiver` and
  `O3DSender::`/`O3DReceiver::RegisterTransportCustomization` each fill part
  of one legacy descriptor, so an out-of-tree transport still compiles and
  registers. `FindTransportCustomization` returns a copy that stays valid
  until the transport registers again or unregisters. The receiver secret
  helpers in `O3DReceiverTransportCustomization.h` (`IsSecretOptionKey`,
  `ResolveSecrets`, ...) are not deprecated.
- **Compatibility.** `O3D_TRANSPORT_API_VERSION` stays 1 while the shims
  exist (the control channel later raised it to 2; see Schema/Protocol). The interface classes are now exported by Open3DShared instead of
  Open3DSender and Open3DReceiver, so the WebRTC add-on (or any out-of-tree
  transport) must be rebuilt against this release, as for every release.
- **Tests.** New `Open3DBroadcast.Shared.TransportRegistry.*` (register and
  unregister, invalid descriptors, duplicate names, a lookup that survives
  unregister, concurrent lookups while registering, picker list equals the
  creatable set, deprecated functions forward). The fake transports and the
  conformance suite use the new registry.

### Transport lifetime: drain on unregister (WP-A1 PR 2, ADR 0007 step 2)

- **Live instances are tracked (SHR-13).** `FO3DTransportRegistry::CreateSender`
  and `CreateReceiver` keep a weak reference to every instance they hand out,
  per transport name. `GetNumLiveInstances(Name)` counts the ones still
  referenced, including leaks left by an earlier unregister of that name.
- **Unregistering drains the transport (TRF-14).** Resetting a
  `FO3DTransportRegistration` (or the last deprecated unregister call of a
  legacy entry) removes the name, so nothing new can be created, then
  broadcasts the new `OnTransportUnregistering(FName)`. The sender component's
  transport controller and the LiveLink receiver source subscribe while they
  hold an instance: they stop it and release it, together with the audio
  sink, control publisher, consumer and control sink that go with it. The
  registry then stops anything still referenced (a receiver also loses its
  consumer and control sink) and logs an Error naming the transport and its
  module for what is left. A factory call that races its own unregister has
  its new instance stopped and returns null.
- **Module shutdown order.** Every transport module resets its registration
  first (which drains), then frees its FFI handles. `FO3DFfiLibrary` no longer
  tracks instances itself: its descriptor names its transports
  (`FO3DFfiLibraryDesc::TransportNames`) and `Unload()` asks the registry,
  keeping the DLL loaded until process exit while an instance of one of them
  is still referenced. `TrackInstance` and `StopLiveInstances` are removed.
  MoQ and the WebRTC add-on use the new order; TCP, UDP, NNG and Loopback have
  no FFI handle and drain through their registration.
- **Game thread only.** `Register`, unregistering and the deprecated register
  functions now `check(IsInGameThread())`, as ADR 0007 item 4 requires.
- **Compatibility.** `O3D_TRANSPORT_API_VERSION` is now 3:
  `FO3DFfiLibrary` and `FO3DFfiLibraryDesc` changed layout and the add-on
  constructs them, and the registry gained a documented drain contract. A
  WebRTC add-on built for version 2 logs the mismatch and registers nothing;
  rebuild it against this release. A transport that keeps its own references
  to instances it created through the registry must release them on
  `OnTransportUnregistering`, or it is reported as a leak.
- **Tests.** New `Open3DBroadcast.Shared.TransportLifetime.*`: unregister with
  no instances, unregister during a live session (stops and releases, no leak
  logged), a leaked instance is stopped and reported, 200 register/unregister
  cycles, an instance created during its own unregister is discarded, and the
  sender component and receiver source releasing their instance when a fake
  transport unregisters mid-session. `Open3DBroadcast.Shared.FfiLibrary.*`
  now checks that a fake FFI library is not freed while the registry counts a
  live instance, and is freed after a clean drain.

### Results, connection state and capabilities (WP-A1 PR 3, ADR 0007 step 3)

- **Results instead of bools (SHR-14).** `IOpen3DSender::Initialize`/`Start`
  and `IOpen3DReceiver::Initialize`/`Start` return `FO3DTransportResult`: a
  code (`InvalidConfig`, `NoConsumer`, `NotRunning`, `ResourceUnavailable`,
  `AddressInUse`, `ConnectFailed`, `AuthFailed`, `Timeout`, `Unsupported`,
  `Internal`) and a message. It converts to `bool` explicitly, so
  `if (!Sender->Start())` still compiles. The same failure maps to the same
  code on every transport: Start before a successful Initialize is
  `NotRunning`, a config the transport cannot use is `InvalidConfig`, a
  receiver started without a consumer is `NoConsumer` (it used to start and
  drop every frame), a TCP or NNG port already bound is `AddressInUse`, and a
  WebRTC config without a token source is `AuthFailed`. The sender component
  and the LiveLink receiver source log the code and message.
- **Send results.** `SendSerialized` takes one `FO3DSendPayload` (bytes,
  subject, capture time, full-sync flag) by rvalue and returns
  `EO3DSendResult`: `Queued`, `DroppedBackpressure`, `NotRunning`,
  `NotConnected`, `Invalid` (empty or malformed), `TooLarge` (above the
  transport's limit) or `Unsupported`. It is pure virtual. `SendControl`
  returns the same enum (`Unsupported` by default); the control publisher
  retries anything but `Queued`, as before. A TCP sender with no receiver
  connected returns `NotConnected`. Only `DroppedBackpressure`, and the
  not-running and not-connected cases where a transport counted them before,
  move `DroppedFrames`.
- **Connection state (ADR 0007 item 3).** New `EO3DConnectionState` (`Idle`,
  `Connecting`, `Connected`, `Reconnecting`, `Failed`),
  `GetConnectionState()` and `SetStateChangedCallback()` on both interfaces,
  and `FO3DTransportStats::State`. Every in-tree transport keeps an
  `FO3DConnectionStateTracker` (exported from Open3DShared): reads are
  lock-free; a change runs the callback on the thread that made it, in order,
  never two at once; after `Stop()` returns no late change from a worker or
  FFI thread is applied or reported. TCP sender: `Connecting` while
  listening, `Connected` with a receiver, `Reconnecting` when it leaves (worker
  thread). NNG sender: by pipe count (worker thread). MoQ, WebRTC, TCP and NNG
  receivers: on the game thread (session callbacks, `Tick` or `Poll`).
  Loopback and UDP are `Connected` from `Start`. A failed `Start` leaves
  `Failed`; a precondition failure (`NotRunning`, `NoConsumer`) leaves the
  state unchanged. `FO3DTransportStats` also gained `SendErrors`,
  `ReceiveErrors`, `PendingFrames` and `PendingBytes` (zero where a transport
  does not count them yet).
- **Capabilities (ADR 0007 item 4).** New `FO3DTransportCapabilities`
  (`bSend`, `bReceive`, `bAudioSend`, `bAudioReceive`, `bControl`,
  `bBidirectional`, `bPeerJoinSignal`, `Delivery`, `MaxPayloadBytes`) from
  `GetCapabilities()` on both interfaces, and before any instance exists from
  the descriptor's new `GetCapabilities(Config)` through
  `FO3DTransportRegistry::GetCapabilities(Name, Config, Out)`. It absorbs ADR
  0005's delivery guarantee (`EO3DDeliveryGuarantee`: Loopback, TCP, NNG
  pair and push/pull, WebRTC `ReliableOrdered`; UDP, NNG pub/sub, MoQ and
  WebRTC with `webrtc.prefer_lossy` `Unreliable`) and ADR 0011's
  `SupportsControl`. `SupportsAudio()` and `SupportsControl()` remain as
  non-virtual forwarders to `GetCapabilities()`.
- **Upgrade note for add-on and out-of-tree transport authors.**
  `O3D_TRANSPORT_API_VERSION` is now 4, and a transport built for 3 is
  refused at registration. To port a transport:
  1. Return `FO3DTransportResult::Ok()` or
     `FO3DTransportResult::Error(Code, Message)` from `Initialize` and
     `Start`; return `NoConsumer` from a receiver's `Start` when no consumer
     is set (after the `NotRunning` check).
  2. Replace `SendSerialized(const uint8*, int32, const FString&, double)`
     with `EO3DSendResult SendSerialized(FO3DSendPayload&& Payload)`; take
     ownership of `Payload.Bytes` instead of copying.
  3. Return `EO3DSendResult` from `SendControl`.
  4. Implement `GetCapabilities()`, `GetConnectionState()` and
     `SetStateChangedCallback()` (an `FO3DConnectionStateTracker` member
     does the last two), and remove any `SupportsAudio()` or
     `SupportsControl()` override: they are no longer virtual.
  5. Optionally fill `FO3DTransportDescriptor::GetCapabilities` so pickers
     and the registry know the delivery guarantee before an instance exists.

  The deprecated `Open3DSender`/`Open3DReceiver` forwarding headers and
  register functions still compile; removing them moves to API version 5 or
  later.
- **Tests.** New `Open3DBroadcast.Shared.TransportResult.*`,
  `Open3DBroadcast.Shared.ConnectionState.*` (tracker rules, callback thread),
  `Open3DBroadcast.Shared.TransportCapabilities.*` (registry query, every
  built-in transport's values, NNG per mode) and
  `Open3DBroadcast.Transport.Results.*` (NotRunning, InvalidConfig and
  NotConnected per transport). The conformance suite gained
  ReceiverStartWithoutConsumer, SendEmptyPayloadInvalid, CapabilitiesMatch,
  ConnectionStateLifecycle and ConnectionStateConnected, and every profile
  states its expected capabilities. The TCP tests check the
  Connecting/Connected/Reconnecting sequence; the WebRTC add-on tests check
  its send results, state transitions and `prefer_lossy` delivery. Tests
  that asserted bools now assert the exact result.

### Shared transport building blocks; Loopback migrated (WP-A1 PR 4a, ADR 0007 step 4)

- **New in Open3DShared (`Public/Transport/`, exported):**
  - `FO3DSendQueue` (`O3DSendQueue.h`): one bounded MPSC queue of typed items
    (mocap, audio, control) with lock-free `fetch_add`/`fetch_sub` item and
    byte accounting (TRB-3) and limits per kind, so no kind takes another's
    room. Mocap either drops the oldest frames above a soft cap, with
    producers refused at twice the cap (the ADR 0007 default), or refuses the
    newest frame at the cap (for reliable transports). Audio and control are
    never discarded to make room for mocap (ADR 0011); control has its own
    cap (1,024 envelopes by default). An optional age limit discards stale
    mocap and audio, never control (TRB-14).
  - `FO3DTransportWorker` (`O3DTransportWorker.h`): a transport's background
    thread that runs a body in a loop and wakes on an enqueue, `Wake()` or
    `Stop()`; and `FO3DReconnectPolicy`: exponential backoff with jitter,
    reset on success, optional attempt limit (TRB-4, TRF-6, TRF-20).
  - `FO3DUnifiedReceiveDemux` (`O3DUnifiedReceiveDemux.h`): classifies a
    received buffer once and routes mocap to the consumer, audio to the audio
    sink (one Opus decoder per stream, SHR-15), control to the control sink
    through `O3DS::TryGetControlPayload`, and counts keepalives, malformed,
    oversize, unknown-kind and rejected-audio buffers (TRB-38, TRB-37). An
    envelope whose header does not fit its buffer is now malformed, not
    legacy raw mocap. It holds the consumer and sinks until `ReleaseSinks()`
    (TRF-38).
  - `FO3DAudioPublishState` and `FO3DQueuedSenderAudioSink`
    (`O3DSenderAudioSinkBase.h`): the WP-S5 lifetime gate, the transport's
    send queue and the last-subject slot, shared with the audio sinks, which
    encode on the audio thread and enqueue audio items; a sink never
    references its sender and never enqueues after `Stop()` or into a later
    session. `FO3DSenderAudioSinkBase` and `FO3DGatedSenderAudioSink` moved
    here from Open3DSender; `O3DSenderAudioSinkBase.h` in Open3DSender
    forwards for one release.
  - `O3DTransportOptions` (`O3DTransportOptions.h`): strict typed option
    getters (`80abc` is no longer port 80) and `ParseHostPort` (scheme, path
    and query tolerated, bracketed IPv6, ports 1 to 65535, optional default
    port), plus `ResolveHostPort`, which resolves host names through the
    socket subsystem on a worker thread (TRB-26).
  - `O3DAudio::TryGetAudioPayloadCodec`: reads the codec of a bare audio
    payload from its header, beside the shared audio (de)serializers (SHR-35).
- **Removed:** `O3DHelpers::NormalizeTcpUrlHostPort`, which rewrote
  `tcp://192.168.1.10` to `tcp://192.168.1:10` and had no callers (SHR-9).
- **Loopback runs on the shared pieces.** Its channel is one
  `FO3DSendQueue` (mocap refuses the newest frame when full, as before;
  `loopback.maxqueue`, `loopback.maxaudioqueue` and the 1,024-envelope
  control cap keep their meaning), the receiver's `Poll` feeds an
  `FO3DUnifiedReceiveDemux`, and its audio sink is `FO3DQueuedSenderAudioSink`.
  Mocap, audio and control now leave the channel in the order they were sent
  (control used to be delivered before the frames of the same `Poll`); each
  kind keeps its own limit. Audio travels through the channel as a unified
  audio envelope, so Loopback exercises the network audio format, and the
  receiver's `BytesReceived` counts the envelope. The sender's
  `GetStats()` fills `PendingBytes`. The module no longer depends on
  Open3DSender or Open3DReceiver: its configure functions read the channel
  and queue options from the transport config. The queue option's tooltip
  now says that a full channel refuses new frames, which is what it always
  did. Net transport source lines: -311.
- **API version stays 4.** Only standalone types were added and two classes
  no add-on uses changed module; no interface layout, vtable or threading
  rule changed.
- **Tests.** New `Open3DBroadcast.Shared.SendQueue.*` (limits, both mocap
  policies, control and audio never dropped for mocap, age limit, FIFO across
  kinds, four producers against a live consumer),
  `.TransportWorker.*`, `.ReconnectPolicy.*`, `.ReceiveDemux.*` (raw and
  enveloped mocap, control, malformed, oversize, unknown, keepalive, audio,
  sink release), `.AudioSinkBase.*` (wire formats, lifetime gate, 1,000
  close cycles under a fake audio thread), `.HostPort.*` (IPv6 brackets,
  missing and bad ports, bad hosts, resolution) and
  `.TransportOptions.TypedGetters`; `Open3DBroadcast.Transport.Loopback.Audio.IndependentOfFrameQueue`
  and `.Lifetime.StopWhileSending` (four send threads and an audio thread,
  1,000 Stop cycles).

### TCP on the shared transport blocks (WP-A1 PR 4b, ADR 0007 step 4)

- **Wire format unchanged.** Same frame header, keepalive, envelopes, option keys and defaults.
- **Sender.** Frames, audio and control are items on one `FO3DSendQueue`, and an
  `FO3DTransportWorker` writes each one as a TCP frame (the header is written on the worker,
  so the payload is no longer copied on the calling thread). TCP is ReliableOrdered, so a full
  queue refuses the newest frame (`DroppedBackpressure`) and never discards a queued one.
  Changes:
  - `tcp.maxqueue` now bounds the payload bytes of frames and, separately, of audio. Control
    has its own cap of 1,024 envelopes, so a full frame budget no longer refuses audio or control.
  - `tcp.maxqueueage` discards stale frames and audio; control no longer expires there (its
    TTL and snapshots handle staleness, ADR 0011).
  - `DroppedFrames` counts frames only; audio discarded without a client is no longer counted.
  - `PendingFrames` and `PendingBytes` are filled.
  - The audio sink is the shared `FO3DQueuedSenderAudioSink`. It still refuses PCM while no
    receiver is connected, through the new `FO3DAudioPublishState::SetPeerReady`.
  - The bind address must be an IP address or a wildcard, so `Start` never resolves a name on
    the game thread; a host name was never usable there.
- **Receiver.** The socket moved to an `FO3DTransportWorker`. It resolves the host with
  `O3DTransportOptions::ResolveHostPort`, so host names now work (`tcp://mocap-pc.local:17700`)
  without blocking the game thread, and IPv6 senders are reachable (TRB-26). It connects,
  reconnects with `FO3DReconnectPolicy`, reads and frames the stream. Payloads go through a
  bounded hand-off queue (8 MiB, or one `tcp.maxframe` if larger) to `Poll`. When that queue is
  full the worker stops reading, so TCP flow control holds the sender back. `Poll` feeds
  `FO3DUnifiedReceiveDemux` with the same per-call bounds (256 payloads or 8 MiB). Changes:
  - Connection state changes after `Start` are reported from the worker thread, as the TCP
    sender's already were.
  - The receiver connects even when nothing calls `Poll`.
  - `Start` no longer fails with `ConnectFailed` for a host it cannot reach or resolve; it
    keeps retrying with backoff. A malformed endpoint is still `InvalidConfig` at `Initialize`.
  - The reconnect delay has ±20% jitter.
  - The consumer is held strongly and released in `Stop` (TRF-38); it used to be held weakly.
  - A damaged envelope (magic present, header does not fit) is dropped instead of being passed
    on as raw mocap.
  - `ReceiveErrors`, `PendingFrames` and `PendingBytes` are filled.
- **Options.** The endpoint is parsed with `O3DTransportOptions::ParseHostPort` (ports 1 to
  65535 in digits only, bracketed IPv6), and every `tcp.*` value with the strict getters, so
  `8000abc` is no longer port 8000. The TCP configure functions read the config only.
  `SocketsTcpAudio.*` and `O3DSockets::BuildTcpUri` are deleted. `Open3DTransportSockets`
  still depends on Open3DSender and Open3DReceiver for UDP's configure functions; that goes
  with the UDP migration (PR 4c). Net transport source lines: -195.
- **API version stays 4.** `FO3DAudioPublishState` gained `SetPeerReady`/`IsPeerReady`; the
  add-on does not use it.
- **Tests.** New `Open3DBroadcast.Transport.Sockets.Tcp.AudioIndependentOfFrameQueue`,
  `.ReceiverBacksOffWithoutSender` (the worker connects and backs off without `Poll`, and data
  resets the backoff) and `.StopWhileSending` (four send threads and an audio thread, 1,000 Stop
  cycles, 50 of them with a connected client). `SocketsTesting.h` gained
  `TcpReceiverGetFailedConnectAttempts`. The existing TCP, sockets and conformance tests are
  unchanged.

### Metrics, helpers and a console command cleaned up (WP-A6 PR 2: SHR-25, SHR-34, SND-32)

- **`o3d.DumpMetrics` logs at `Display`** instead of `Warning` (about 60 lines of informational
  output), and so does `o3d.ResetMetrics` (SHR-25).
- **`o3d.ResetMetrics` clears everything**: the receiver's per-operation timings (parse, pose
  extraction, LiveLink push, total) and active subject count were left behind (SHR-25).
- **Metrics that nothing recorded are gone** (SHR-25), so the dump no longer prints zeros for
  them. **API removal for C++ users:** `FSenderMetrics::FramesQueued`, `SerializationErrors`,
  `AvgSerializationTimeMs`, `ActiveSubjectCount`, `AllocationCount`, `AllocationBytes` and
  `FrameIntervalMs`; `FReceiverMetrics::AvgDeserializationTimeMs`;
  `FO3DTransportMetrics::AvgPacketLossPercent`, `AvgLatencyMs` and `AvgBandwidthMbps`;
  `RecordFrameQueued`, `RecordSerializationError`, `RecordAllocation`, `SetActiveSubjectCount`,
  `UpdateFrameInterval`, `RecordAllocationsForContext`, `GetAllocationRecords` and
  `FAllocationRecord`; and the unused `O3D_RECORD_*` / `O3D_SET_SUBJECT_COUNT` macros. The CSV
  row `FramesQueued` is gone.
- **`SkeletonUpdates` is recorded now**: one per static data push (a subject new to LiveLink, or
  with new bone or curve names). The dump shows sender `Transport Frames Dropped`.
- **`GetMetricsAsCSV` covers more**: transport rows (`Transport.<name>.FramesSent` and the other
  counters), the per-operation timings, the peak receive-to-apply latency, deserialization
  errors, skeleton and pose updates, and sender transport drops.
- **Unused URL helpers deleted** (SHR-34): `O3DHelpers::UrlSplitQuery` (which lowercased query
  values), `O3DHelpers::StripQuery`, and the `O3DSHelpers` compatibility namespace.
  `SanitizeSubjectName`'s comment now says what it does (spaces become `_`, every other character
  outside `[-._A-Za-z0-9/]`, tabs included, is dropped); its behaviour is unchanged.
- **`o3ds.Sender.DumpStats` is registered for the module's lifetime** (SND-32): the first
  serializer registered it and nothing unregistered it, which left a delegate into unloaded code
  after a module unload. The command now exists from module load.
- **Tests.** New `Open3DBroadcast.Shared.Metrics.ResetClearsEverythingAndCsvListsTransports`.

### Quieter receiver logs; receive-to-apply latency (WP-A6 PR 1: RCV-25, RCV-26, RCV-33)

- **`o3ds.Receiver.DebugParse` defaults to 0** (was 1), so the receiver's per-packet parse logs
  are off unless asked for (RCV-26).
- **Slow LiveLink pushes are logged at most once per 5 s per receiver source**, with the number of
  slow pushes not logged since, instead of a `Warning` on every slow frame; the "PRIMARY SUSPECT"
  wording is gone (RCV-26).
- **`UO3DRemoteAudioComponent` logs per component** (RCV-25): the "Subscribed" line (now naming
  the owner) on every BeginPlay and the first PCM frame of every component, instead of only the
  first component in the process; the debug log counters are per component too.
- **The receiver's "round-trip latency" metric is now named for what it measures** (RCV-33):
  receive-to-apply latency on this machine. It is now recorded on the gated path as well (from the
  packet's local receive time, so the reorder gate's wait is included), not only the legacy path.
  **Rename for C++ and CSV users:** `FReceiverMetrics::AvgRoundTripLatencyMs` is now
  `AvgReceiveToApplyLatencyMs`, and the CSV row `AvgRoundTripLatencyMs` is now
  `AvgReceiveToApplyLatencyMs`; the dump says "Receive-to-Apply Latency".
- **Tests.** New `Open3DBroadcast.Receiver.LiveLinkPublisher.ThrottlesSlowPushWarnings`.

### Sender transport options and secrets split out (WP-A3 step 8, SND-22)

- `UO3DSenderComponent`'s transport option and credential handling moved into
  `FO3DSenderTransportSettings` (private): option reads and writes that route declared secret keys
  to the secret store (ADR 0004), the credential profile, moving secrets out of old saved data,
  switching options between transports (SND-35), the options and secrets a transport config gets,
  and which property edits restart capture in the editor. The option maps stay properties of the
  component, which passes them in with a callback that records the change for undo, so `Modify()`
  is called exactly where it was. Every `UFUNCTION` keeps its signature; output is unchanged.
- **Tests.** New `Open3DBroadcast.Sender.TransportSettings.RoutesSecretsOutOfOptions` and
  `.MigratesAndSwitchesOptions`, through the exported `FO3DSenderTransportSettingsProbe`. Existing
  tests, including the secrets tests, are unchanged.

### Sender audio binding split out (WP-A3 step 7, SND-22)

- `UO3DSenderComponent`'s audio binding moved into `FO3DSenderAudioBinding` (private): the capture
  and transport audio configs built from the audio properties, keeping the capture config's source
  and device index in step with the mode, finding or creating the audio capture component, and
  handing it the transport's sink with the throttled "no sink" log. The capture component stays a
  property of the sender component; no `UPROPERTY` or `UFUNCTION` moved. Output is unchanged
  (the transport audio config the binding update built and never used is no longer built).
- **Tests.** New `Open3DBroadcast.Sender.AudioBinding.BuildsConfigsFromProperties` and
  `.FindsCaptureAndBindsSink`, through the exported `FO3DSenderAudioBindingProbe`. Existing tests
  are unchanged.

### Sender pose sampler split out (WP-A3 step 6, SND-22)

- `UO3DSenderComponent`'s pose sampling moved into `FO3DSenderPoseSampler` (private): the skeleton
  descriptor cache, the subject name (override or World/Actor/Component), the frame index, and
  filling a sampled frame's shell and bones. What reaches outside (the `OnDescriptorReady`
  delegate, the audio stream label, forgetting a renamed subject in the pipeline) goes through two
  callbacks the component sets, in the same order as before. Output is unchanged; no `UPROPERTY`
  or `UFUNCTION` moved. The curve capture stays with the component.
- The test accessor's descriptor, subject name and bone-transform functions are now defined in the
  module (`FO3DSenderComponentTestAccess`, test-only), since they reach the private class.
- **Tests.** New `Open3DBroadcast.Sender.PoseSampler.CachesDescriptorPerMesh` and
  `.NamesSubjectsAndFillsFrames`, through the exported `FO3DSenderPoseSamplerProbe`, on a mesh
  built in code. Existing tests are unchanged.

### Receiver transforms keep double precision; dropped poses are counted (WP-A3 step 5, RCV-13)

- **Behaviour change.** The receiver's frame decoder now passes the core's translation, rotation
  and scale to LiveLink as doubles, instead of casting each component to float first (UE's
  `FVector` and `FQuat` are double). The wire carries floats, so a full-sync value comes through
  exactly as before; what changes is a quantized or residual-coded update, which the core
  rebuilds in double (a quantized translation as anchor plus delta, a quantized rotation
  dequantized, a residual as reference plus residual) and which the float cast used to round (a
  translation by up to about 0.004 units at 100,000 units from the origin). Concealment predicts
  from the decoded transforms, so its synthesized frames carry the same precision. A value that is finite in double but too large for float is
  no longer turned into infinity and rejected; the wire cannot carry one.
- **New metric `InvalidPosesDropped`** (`FO3DPerformanceMetrics::FReceiverMetrics`,
  `RecordInvalidPoseDropped()`, in the metrics dump and CSV as `ReceiverInvalidPosesDropped`): a
  subject whose pose is skipped because a transform is not finite, has a zero rotation or is
  missing. Such a pose used to be dropped without a trace. A subject without transforms is still
  skipped and is not counted.
- **Tests.** New `Open3DBroadcast.Receiver.FrameDecoder.KeepsDoublePrecision` (fails with the old
  float casts) and `.CountsDroppedPoses`; `FO3DReceiverFrameDecoderProbe` gained `DecodeSubject` to
  decode a subject built in memory. Existing tests are unchanged.

### Receiver header without core headers; control routing split out (WP-A3 step 4, RCV-29)

- **`O3DReceiverSource.h` includes no `o3ds/` header any more**, and `Open3DStreamCore` is a
  private dependency of Open3DReceiver: modules that depend on the receiver (the editor module, a
  game module) no longer get the Open3DStream core's include paths. The header forward-declares
  the few core types its private member functions name. **Build note for C++ users:** a module
  that included a core header (`o3ds/...`) only through `O3DReceiverSource.h`, or that relied on
  the receiver's public dependency for the core's include path, must now depend on
  `Open3DStreamCore` itself. Nothing in this repository did.
- The receiver side of the control channel moved into `FO3DReceiverControlRouter` (private):
  parsing control payloads, holding changes for alignment with the presented mocap pose,
  publishing to `FO3DControlBus`, and forgetting sources when control is turned off. Behaviour is
  unchanged.
- `Public/Testing/O3DReceiverTesting.h` (test-only) includes `o3ds/control.h` for the aligner
  stats its accessor returns.
- **Tests.** New `Open3DBroadcast.Receiver.ControlRouter.RoutesAlignsAndDiscards`, through the
  exported `FO3DReceiverControlRouterProbe`. Existing tests are unchanged.

### Receiver stream scheduler and concealment split out (WP-A3 step 3, RCV-29)

- `FO3DReceiverSource`'s packet ordering moved into `FO3DReceiverStreamScheduler` (private): the
  per-sender stream table (RCV-5), the legacy timestamp ordering, the reorder gate's push and
  flush, idle pruning and the gate metric deltas. Released packets come back to the source, which
  parses them, maps the gated path's clock and publishes (`ApplyReleasedFrame`, which replaces the
  two near-identical legacy and gated apply functions).
- Receiver-side concealment moved into `FO3DReceiverConcealment` (private): one engine per subject,
  the starvation poll and its metric deltas; its synthesized-frame arrays are reused across ticks.
- Output is unchanged. The legacy ordering decision now reads the same "now" as the rest of the
  packet's handling (it read the clock a few microseconds later before).
- **Tests.** New `Open3DBroadcast.Receiver.StreamScheduler.LegacyOrderingDropsStaleFrames`,
  `.GateReordersPerSender` and `Open3DBroadcast.Receiver.Concealment.SynthesizesOnlyWhenStarved`,
  through exported probes. Existing tests are unchanged.

### Receiver LiveLink publisher split out (WP-A3 step 2, RCV-29)

- `FO3DReceiverSource`'s LiveLink side moved into `FO3DLiveLinkPublisher` (private): creating a
  subject once per session (RCV-7), re-pushing static data when bone or curve names change,
  pushing real and concealed frames, and removing subjects that stopped sending. Output and the
  order of pushes are unchanged; the WP-S4 test hooks now live in the publisher (the test
  accessor sets them through the source).
- **Tests.** New `Open3DBroadcast.Receiver.LiveLinkPublisher.StaticDataOncePerSessionAndOnChange`
  and `.FramesAndInactiveSubjects`, through the exported `FO3DLiveLinkPublisherProbe`. Existing
  tests are unchanged.

### Receiver frame decoder split out (WP-A3 step 1, RCV-29, RCV-11, RCV-12)

- `FO3DReceiverSource`'s conversion of parsed subjects into LiveLink bone names, parents,
  transforms and curves moved into `FO3DReceiverFrameDecoder` (private). Output is unchanged.
- **Fewer allocations per frame (RCV-11, RCV-12).** Curve `FName`s are built only when a subject's
  curve name strings change (they were built for every curve on every frame), the subject `FName`
  is cached, the skeleton and curve hashes are computed only when the names change, and the
  transform and curve value arrays are reused across frames instead of being copied from the
  cache or allocated per packet.
- **Tests.** New `Open3DBroadcast.Receiver.FrameDecoder.ConvertsPoseAndCurves`,
  `.ReusesTopologyUntilItChanges`, `.RejectsUnusablePoses`, through the exported
  `FO3DReceiverFrameDecoderProbe`. Existing tests are unchanged.

### Sender worker cost within ADR 0008's budget (WP-A2 follow-up)

- **Legacy encoding keeps its core Subject.** The default encoding no longer builds a new
  `SubjectList`, 250 transforms and every bone and curve name as UTF-8 for every frame: the Subject
  is kept per subject and rebuilt only when the skeleton changes (compared by bone names,
  case-sensitive, and parents), curve names are rewritten only when they change, and the core's
  output buffer is reused. The unused `CalcMatrices` call is gone. The bytes on the wire are
  unchanged (tested against a Subject built from scratch for every frame).
- **Curve filter** builds a curve name's text only for a verbose log or an uncached pattern match,
  not for every curve every frame.
- **Result** (250 bones, 250 curves, median per frame on the worker): 0.25 to 0.12 ms with one UDP
  sender, 0.37 to 0.16 ms with ten, within the 0.2 ms budget; the synchronous path's game-thread
  time fell from 0.26 to 0.14 ms. New trace scopes (`O3D.Sender.Pipeline.Filter`,
  `O3D.Sender.Serializer.*`) split a frame's worker time; `Run-SenderBenchmark.py` reports them.
- **Tests.** New `Open3DBroadcast.Sender.Wire.LegacyReuseKeepsBytes`.

### Sender pipeline benchmark and its first numbers (WP-A2 follow-up, ADR 0008 item 11)

- **Benchmark.** `Open3DBroadcast.Bench.SenderPipeline` (test module; registers nothing unless
  `O3DB_BENCH=1`, so CI never runs it) and `Build/Scripts/Run-SenderBenchmark.py`, which runs it
  with Unreal Insights tracing and exports the sender timers per case: 1 and 10 senders of 250
  bones and 250 curves (the code-built test mesh), Loopback and UDP, `o3d.Sender.AsyncPipeline` 0
  and 1, 600 frames at 60 Hz.
- **First numbers** (editor, `-NullRHI`, one workstation; full table in the ADR 0008 addendum
  "Insights numbers"): game-thread time per sender went from about 0.22-0.27 ms median (pipeline
  off, which includes serialization and the send) to about 0.025 ms (pipeline on); the worker
  takes 0.24 ms per frame with one sender and 0.36-0.37 ms with ten (median); capture-to-send p99
  0.6-2.2 ms; no pipeline drops. The worker misses ADR 0008's 0.2 ms budget; the others are met.
  The Loopback cases have no receiver, so their sends are refusals; the UDP cases send.

### Root-bone pose test with a skeletal mesh built in code (WP-A2 follow-up, ADR 0008 Verification)

- **Test mesh.** The test module builds a skeletal mesh in code
  (`O3DTestSkeletalMesh.h`: a bone chain with curve metadata on its skeleton,
  and render data with one LOD and no geometry), because the automation host
  project carries no mesh asset. Its test anim instance moves the root bone to a
  position derived from `GFrameCounter`, so every frame's evaluated pose is known.
  The test module now also depends on RenderCore and RHI (editor-only module; the
  runtime modules and the Fab package are unchanged).
- **Test.** New `Open3DBroadcast.Sender.TickOrder.CapturedRootEqualsEvaluatedPose`:
  in a ticking world, for 30 frames, the root bone the sender captures equals the
  pose the mesh evaluated in the same frame (ADR 0008's acceptance test for the
  tick-order change). With the sender ticking before the mesh it fails, capturing
  the previous frame's pose every frame (checked).

### Sender audio on the sender clock; capture devices enumerated once per start (WP-A2d, ADR 0008 outline item 5)

Fourth step of the asynchronous sender (WP-A2). Audio still does not go through the pose pipeline.
The interface version (`O3D_TRANSPORT_API_VERSION`) stays 5: the transport interface did not
change.

- **One clock for pose and audio (SND-17, ADR 0008 item 7, ADR 0009 item 7).** Audio timestamps
  used to be on the audio source's own clock: the mixer's `AudioClock` (seconds since audio
  rendering started) for the submix tap, the device's stream time for the microphone. They are now
  on the sender clock (`FPlatformTime::Seconds()`), the clock pose frames are stamped with, so a
  receiver can line audio up with the pose. Each capture stream maps its clock with
  `FO3DAudioClockMapper` (new public header): the offset is set by the first buffer, follows
  device drift through a low-pass filter (5 s time constant), and is reset when the source clock
  jumps by more than 100 ms (at once when it goes backwards or gets ahead of the sender clock;
  after 0.5 s when it falls behind, so one late callback does not move the stamps). The stamp
  includes the mean delivery delay (about one buffer) as a constant bias. **Behaviour change:** the
  audio envelope's `timestamp_us` now carries sender-clock time; receivers did not use it to align
  anything yet. `UO3DSenderAudioCaptureComponent::PushFrames` passes its timestamp through
  unchanged; callers give it on the sender clock.
- **Capture devices enumerated once per start (SND-18, ADR 0008 item 8).** Device enumeration used
  to run on construction, load, register, `BeginPlay` and twice per `StartCapture`, and every time
  the Details panel asked for the device list. It now runs into one cached list
  (`FO3DAudioInputDevices`, new public header): once per `StartCapture` that captures from an input
  device, once in the editor after engine init, and on request
  (`UO3DSenderComponent::RefreshAudioInputDevices`, Blueprint-callable, or the console command
  `o3d.Sender.Audio.RefreshDevices`, which also logs the list). The device pickers and the
  name-to-index lookups read the cache. After plugging in a microphone while the editor is open,
  refresh to see it in the picker.
- **The capture device is opened once per start.** `StartCapture` used to configure the audio
  capture component twice (once after the transport started, once more after), closing and opening
  the microphone each time, and a capture component created during play opened the default device
  in its `BeginPlay` as well. It now opens the device once per start; the capture component's
  `BeginPlay` opens it only when a sink is already bound, and binding a sink later opens it if
  nothing has tried to since the capture last started (a failed open is not retried until the next
  start).
- **Tests.** New `Open3DBroadcast.Sender.AudioClock.MapsOntoSenderClock`, `.DriftDoesNotAccumulate`,
  `.JitterBarelyMovesStamps`, `.DiscontinuitiesReset` (a hitch, a restarted and a leaping source
  clock, a lasting offset change; stamps never go backwards), and
  `Open3DBroadcast.Sender.AudioDevices.LookupsReadTheCache`, `.StartEnumeratesAndOpensOnce` (a
  fake device list; one enumeration and one device open per Input start, none in Mix) and
  `.SinkBindOpensAtMostOnce` (in a game world: binding and rebinding a sink after a start, or on a
  standalone capture component, opens the device at most once). Existing
  tests are unchanged, except that a port helper in `SocketsLifetimeTests.cpp` was renamed: the
  new files regrouped the unity build and its name collided with another file's.

### Sender pose pipeline: serialization and sends leave the game thread (WP-A2c, ADR 0008 outline item 4)

Third step of the asynchronous sender (WP-A2). The interface version (`O3D_TRANSPORT_API_VERSION`)
stays 5: the transport interface did not change. `IOpen3DSender::SendSerialized` was already
documented as callable from any thread (ADR 0007); it is now called from a worker.

- **Pipeline (SND-8, TRB-20).** Each `UO3DSenderComponent` owns a pose pipeline
  (`FO3DSenderPipeline`, private). The game thread only samples the pose into a pooled frame and
  hands it over; a `UE::Tasks` task (`ETaskPriority::BackgroundHigh`) filters the curves,
  serializes the frame and calls `SendSerialized`, moving the serializer's buffer into the payload
  instead of copying it. One task runs per sender at a time, so frames and control items are
  processed in order. The worker never touches a UObject.
- **Bounded queue, drop oldest.** At most `o3d.Sender.PipelineDepth` frames (default 2, 1 to 8) wait
  for the worker. When another arrives, the oldest waiting frame goes back to the pool before it is
  serialized, so no full sync is lost and the payloads that are sent are numbered without a gap.
  Stop, start and rename reach the worker through the same queue and are never dropped.
- **Stop and destruction.** `StopCapture` discards frames still waiting and does not wait for the
  network. It waits only for a frame the worker is processing at that moment (one serialize and one
  non-blocking send), so `OnSerializedFrame` never fires after it returns and the transport is never
  stopped during a send. A component destroyed with a task in flight is safe: the task holds the
  pipeline, not the component. `Open3DSender`'s module shutdown waits up to 1 s for running tasks
  and logs an error if any remain.
- **A refused full sync is sent again.** When a transport refuses a full sync with
  `DroppedBackpressure`, the next frame of that subject is a full sync (ADR 0007 item 3).
  `FO3DSendPayload::bFullSync` is now set.
- **Behaviour changes for C++ listeners.** `OnSerializedFrame` now fires on the worker thread (kept
  for one release, then removed if unused; bind it only while capture is stopped).
  `OnPoseFrameReady` still fires on the game thread, but with the sampled frame: raw curves in
  `CurveList`/`RawCurveValues`, and `CurveNames`/`CurveValues` empty, because filtering now runs on
  the worker.
- **Fallback.** `o3d.Sender.AsyncPipeline 0` (read at `StartCapture`) keeps everything on the game
  thread in the WP-A2b order, for one release, so a regression can be isolated.
- **Stats.** `UO3DSenderComponent::GetPipelineStats()` and the console command
  `o3d.Sender.DumpPipelineStats` report submitted, dropped and sent frames, queue high-water mark,
  worker time per frame and capture-to-send latency. Trace scopes `O3D.Sender.Sample`,
  `O3D.Sender.Pipeline.Serialize` and `O3D.Sender.Pipeline.Send` show up in Unreal Insights.
  `o3ds.Sender.DumpStats` now reads each serializer under its lock, so it no longer races with a
  worker.
- **Tests.** New `Open3DBroadcast.Sender.Pipeline.SlowTransportDropsOldest`,
  `.RefusedFullSyncIsSentAgain`, `.StopDiscardsQueuedFrames`, `.StopStartAndRenameUnderLoad`,
  `.QuantizationChangeForcesOneFullSync`, `.OwnerReleasedWithTaskInFlight` (1,000 cycles) and
  `.ComponentDestroyedWithTaskInFlight` (200 components, garbage-collected). The component tests
  run with the console variable on and off. Existing tests are unchanged. The Unreal Insights
  numbers ADR 0008 asks for are not recorded yet; see the ADR 0008 addendum "implementation notes
  (WP-A2c)".

### Sender capture ticks after the target mesh (WP-A2b, ADR 0008 outline item 3)

Second step of the asynchronous sender (WP-A2), still synchronous on the game thread. The interface
version (`O3D_TRANSPORT_API_VERSION`) stays 5: the transport interface did not change.

- **Tick group (SND-12).** `UO3DSenderComponent` now ticks in `TG_PostUpdateWork` (it used to tick
  in the default group, `TG_PrePhysics`, the same group as a skeletal mesh by default, in no fixed
  order), so it samples the target mesh after animation evaluation and physics for that frame.
  Before, a captured pose could be the previous frame's. The transport's `Tick` and the control publisher run in the same tick,
  so they also move later in the frame: control values and events set earlier in a frame now go
  out in that frame.
- **Tick prerequisite.** Starting capture makes the target skeletal mesh's tick a prerequisite of
  the sender's tick (`AddTickPrerequisiteComponent`); stopping removes it. Exactly one mesh is a
  prerequisite at a time. A `TargetMesh` written while capturing (Blueprint) or destroyed while
  capturing moves or removes the prerequisite on the next tick. Prerequisites added by other code
  are not touched.
- **Tests.** New `Open3DBroadcast.Sender.TickOrder.TickGroupIsPostUpdateWork`,
  `.PrerequisiteFollowsTargetMesh` (bind, unbind, unbind when not bound, rebind, a mesh written
  while capturing, a destroyed mesh) and `.SamplesAfterTargetMeshEachFrame` (a ticking game world
  in which the mesh and the sender share a tick group; every frame the sender samples after the
  mesh's tick). ADR 0008's root-bone acceptance test needs a skeletal mesh asset, which the plugin
  does not have; see the ADR 0008 addendum "implementation notes (WP-A2b)". Existing tests are
  unchanged.

### Sender settings snapshot, frame pool and sampling clock (WP-A2a, ADR 0008 outline item 2)

First step of the asynchronous sender (WP-A2). Capture is still synchronous: sampling, curve
filtering, serialization and `SendSerialized` all run on the game thread, in the component's tick,
as before. The interface version (`O3D_TRANSPORT_API_VERSION`) stays 5: the transport interface did
not change.

- **The serializer holds no component (SND-22).** `FO3DSenderSerializer::Attach` and `Detach` are
  gone, with the raw `UO3DSenderComponent*` they kept and the `OnPoseFrameReady` subscription. The
  component now calls `SerializePoseFrame` itself after sampling. The serializer works from the
  frame and its settings snapshot only, so it can be constructed and used without any UObject.
  `SetStatsLabel` names an instance for `o3ds.Sender.DumpStats`, which now lists every live
  serializer.
- **Settings snapshot.** `FO3DSenderEncodingSettings` (carried by every frame as
  `FO3DSPoseFrame::Encoding`) now also holds the curve filtering settings: morph clamp, NaN
  handling, the filtering switch, epsilon and delta, and the include and exclude patterns as shared,
  immutable arrays. The component refreshes it for each sampled frame and copies a pattern list only
  when it changed, so copying the snapshot onto a frame allocates nothing.
- **Curve filtering after sampling.** Sampling now stores raw curve values
  (`FO3DSPoseFrame::RawCurveValues`) against a shared, immutable curve list
  (`FO3DSPoseFrame::CurveList`, type `FO3DSCurveList`). A new `FO3DSenderCurveFilter` (private)
  turns them into `CurveNames`/`CurveValues` with the frame's own settings. The rules and their
  results are unchanged; `OnPoseFrameReady` still fires after filtering, on the game thread.
- **Frame pool (SND-9).** New `FO3DSPoseFramePool` (`Open3DSender/Public/O3DSPoseFramePool.h`):
  frames are reused with their arrays' capacity, and at most `Capacity` frames (default 4) ever
  exist. The component no longer builds a new `FO3DSPoseFrame` and new curve arrays per capture.
- **Sampling-time clock (ADR 0008 item 7).** The time written on the wire, and passed to
  `OnSerializedFrame` and `SendSerialized`, is now the frame's sampling time
  (`FPlatformTime::Seconds()` when the pose was sampled), not the time the serializer ran. The two
  differ by the serialization cost (well under a millisecond). Engine timecode is not used.
- **Tests.** New `Open3DBroadcast.Sender.EncodingSnapshot.SerializerWorksWithoutComponent`,
  `Open3DBroadcast.Sender.FramePool.ReusesFramesAndIsBounded`,
  `Open3DBroadcast.Sender.CurveFilter.AfterSamplingMatchesBefore` and
  `Open3DBroadcast.Sender.Wire.SerializedTimeIsSamplingTime`. The WP-S3 sender tests are unchanged.

### Deprecated transport shims removed (WP-A1 step 6, ADR 0007 item 9)

This completes WP-A1. Runtime behaviour is unchanged. Saved data needs nothing: no property or
option key changed.

- **Interface version stays 5.** ADR 0007 kept the shims for one release. The maintainer decided to
  remove them now instead, because no release has carried them (the last tag, v0.9.6, predates
  5a, and no tag contains c98c92c) and no third-party add-on or project builds from this
  codebase. Step 6 therefore joins version 5, with 5a, 5b and 5c.

#### Removed

Each deleted API, with what to use instead:

- **Sender registry** (`Open3DSender/Public/O3DSenderRegistry.h`):
  - `O3DTransport::RegisterSender` and `UnregisterSender`: register one `FO3DTransportDescriptor`
    with `FO3DTransportRegistry::Get().Register(...)` and keep the returned
    `FO3DTransportRegistration`.
  - `O3DTransport::CreateSender`: `FO3DTransportRegistry::Get().CreateSender(Name)`.
  - `O3DTransport::GetRegisteredSenders`: `FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender)`.
- **Receiver registry** (`Open3DReceiver/Public/O3DReceiverRegistry.h`): `RegisterReceiver`,
  `UnregisterReceiver`, `CreateReceiver` and `GetRegisteredReceivers`. The replacements are the same
  as for the sender registry, with `EO3DTransportRole::Receiver`.
- **Sender transport customization** (`Open3DSender/Public/O3DSenderTransportCustomization.h`):
  - `FO3DSenderTransportCustomization`, `O3DSender::RegisterTransportCustomization` and
    `UnregisterTransportCustomization`: set `FO3DTransportDescriptor::ConfigureSender` and
    `SenderOptions` on the registered descriptor.
  - `O3DSender::FindTransportCustomization`: `FO3DTransportRegistry::Get().Find(Name)`.
  - `O3DSender::GetRegisteredTransportNames`: `GetNames(EO3DTransportRole::Sender)`.
  - `O3DSender::GetTransportSecretDeclaration`:
    `FO3DTransportRegistry::Get().GetSecretDeclaration(Name, EO3DTransportRole::Sender, ...)`.
  - `O3DSender::GetTransportOptionSchema`:
    `FO3DTransportRegistry::Get().GetOptionSchema(Name, EO3DTransportRole::Sender, ...)`.
- **Receiver transport customization:**
  - Removed from `O3DReceiverTransportCustomization.h`: `FO3DReceiverTransportCustomization`,
    `O3DReceiver::RegisterTransportCustomization`, `UnregisterTransportCustomization`,
    `FindTransportCustomization`, `GetRegisteredTransportNames`, `GetTransportSecretDeclaration`
    and `GetTransportOptionSchema`. The replacements are the same as for the sender, with
    `ConfigureReceiver`, `ReceiverOptions` and the Receiver role.
  - The receiver's secret and switching helpers stay in that header: `IsSecretOptionKey`,
    `GetCredentialProfile`, `StripSecretOptions`, `MigrateLegacySecretOptions`,
    `ExportConnectionString`, `ResolveSecrets` and `SwitchTransport`.
- **The legacy configure scopes.** `O3DSenderLegacyShims` (`FScopedConfiguringComponent`) and
  `O3DReceiverLegacyShims` (`FScopedConfiguringSettings`) are removed, together with the
  customization caches the modules started. A configure function gets the options view only (WP-A1
  PR 5a), so it no longer receives the component or the settings.
- **`FO3DTransportRegistry::EditLegacyDescriptor`**, the path the deprecated functions used to edit
  one part of a factory-less descriptor. Register one complete descriptor instead. Every registered
  entry now comes from `Register`.
- **`FO3DTransportRoleOptions::SecretOptionKeys` and `SecretEnvVars`.** Declare each secret with a
  `Secret` entry in the role's `OptionSchema`, and put its environment variable in the entry's
  `SecretEnvVar` (WP-A1 PR 5c). `GetSecretDeclaration` now lists exactly those entries. Nothing in
  the repository filled the lists any more. ADR 0004 behaviour is unchanged: the store, resolution,
  persistence, redaction, and the migration of secrets found in saved data.
- **`IOpen3DSender::SupportsAudio`/`SupportsControl` and `IOpen3DReceiver::SupportsAudio`/`SupportsControl`:**
  - sender: `GetCapabilities().bAudioSend` and `GetCapabilities().bControl`;
  - receiver: `GetCapabilities().bAudioReceive` and `GetCapabilities().bControl`.
- **Forwarding headers.** Include the Open3DShared path instead:

  | Removed header | Use instead |
  |---|---|
  | `Open3DSender/Public/O3DSenderInterface.h` | `Transport/O3DSenderInterface.h` |
  | `Open3DReceiver/Public/O3DReceiverInterface.h` | `Transport/O3DReceiverInterface.h` |
  | `Open3DSender/Public/O3DSenderAudioSinkBase.h` | `Transport/O3DSenderAudioSinkBase.h` |
  | `Open3DSender/Public/Testing/O3DLifetimeTestUtils.h` | `Testing/O3DTransportLifetimeTestUtils.h` |
  | `Open3DShared/Public/O3DTransportTypes.h` | `Transport/O3DTransportTypes.h` |
  | `Open3DShared/Public/SerializedFrameConsumerRegistry.h` | `Transport/O3DSerializedFrameConsumer.h` |

#### Tests

- **Deleted**, because they tested only removed API:
  - `Open3DBroadcast.Shared.TransportRegistry.DeprecatedFunctionsForward`
  - `Open3DBroadcast.Sender.TypedConfig.DeprecatedConfigureGetsComponent`
  - `Open3DBroadcast.Receiver.TypedConfig.DeprecatedConfigureGetsSettings`
- **Assertions removed** that covered only removed API:
  - In `DuplicateNameKeepsFirst`: the legacy edit of a handle-owned name.
  - In `PickersListCreatableSet`: the options-only legacy entry. `Register` refuses such a
    descriptor, and `RejectsInvalidDescriptors` covers that.
  - In the conformance `CapabilitiesMatch` case (`Open3DBroadcast.Conformance.<Transport>.CapabilitiesMatch`) and
    `Open3DBroadcast.Transport.WebRTC.Capabilities.DeliveryFollowsPreferLossy`: the "SupportsX forwards
    to GetCapabilities" checks. The capabilities themselves are still asserted.
  - In `WebRTC.Secrets.SchemaEntriesCarryEnvVars`: the "deprecated lists are empty" checks.
  - In `Shared.OptionSchema.SecretEntryCarriesEnvVar`: the merge-with-lists rules. They became
    schema-only rules: each key once, and an entry's variable.
- **Moved to `FO3DTransportRegistry::Register`, with their assertions unchanged.** These tests
  used the shims only as setup: they now register one descriptor with a fake factory, and declare
  their secrets with `Secret` entries.
  - the typed-config and transport-switch tests (sender and receiver);
  - the WP-S9 secrets tests (sender and receiver);
  - the options-panel tests;
  - `MakeDescriptor` in the registry tests.
- **Moved to the current API, with their assertions unchanged:**
  - The Loopback, MoQ relay and lifetime tests call `FO3DTransportRegistry::Get().CreateSender`
    and `CreateReceiver`, and include the Open3DShared headers.
  - Tests that asked `SupportsAudio()`/`SupportsControl()` ask `GetCapabilities()`.

### Typed config, part 2: registered names, typed secrets, Float, restart-on-change, Validate (WP-A1 PR 5c, ADR 0007 step 5)

This completes ADR 0007 item 8 and with it step 5. Saved data is unchanged: no saved property,
option key or value changed, and nothing needs migrating.

- **Interface version stays 5.** 5a, 5b and 5c ship in the same release (the last tag is v0.9.6,
  which predates 5a), so one number covers them. The WebRTC add-on is built against the change.
  Removing the deprecated shims (step 6) joined version 5 as well (see the step 6 entry).
- **`FO3DTransportConfig::Transport` is an `FName` and `Role` an `EO3DTransportRole`** (TRB-27).
  `Transport` holds the registered name ("TCP", "UDP", "NNG", "MoQ", "Loopback", "WebRTC") and
  `Role` the side, `Sender` or `Receiver`. A new constructor takes both:
  `FO3DTransportConfig Config(TEXT("TCP"), EO3DTransportRole::Sender)`. The sender component and the
  receiver source build their configs with it, and every built-in transport's configure functions
  set both. Before, the config carried free text: TCP's tests used "sockets.tcp", NNG put its
  socket's listen or dial side in `Role`, and WebRTC "publisher" or "subscriber". Nothing read
  `Role`; NNG's socket side stays where the transport reads it, the `nng.role` option. Source
  change for transport code: `*Config.Transport` in a format string becomes
  `*Config.Transport.ToString()`, `Transport.IsEmpty()` becomes `Transport.IsNone()`, and a role
  string becomes `EO3DTransportRole::Sender` or `::Receiver` (`LexToString` gives "Sender" or
  "Receiver").
- **Secret schema entries carry their environment variable** (ADR 0004, ADR 0007 item 8). New
  `FO3DTransportOptionField::SecretEnvVar`. A `Secret` entry now declares its key secret by itself;
  `FO3DTransportRoleOptions::SecretOptionKeys` and `SecretEnvVars` are deprecated inputs that still
  count. `FO3DTransportRegistry::GetSecretDeclaration` returns the union (new
  `FO3DTransportRoleOptions::GetSecretDeclaration`), and an entry's variable wins over
  `SecretEnvVars`. The WebRTC add-on declares `webrtc.token` (`O3DB_WEBRTC_TOKEN`) and
  `webrtc.tokenEndpointAuth` (`O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`) in its schema entries only. The
  keys, variables, store, persistence and redaction are exactly as before.
- **`Float` options.** New `EO3DTransportOptionType::Float` (appended), a number stored as decimal
  text. `Min` and `Max` are now `double` (they were `int32`; every int is exact, and assigning an
  int still compiles) and bound a Float when `Max > Min`:
  `FO3DTransportOptionsView::GetDouble` clamps the value, and the default, of a Float field to
  them. The editor panels show a number box with that range and clamp what they write. No built-in
  transport has a Float option yet.
- **`bRestartOnChange`.** A schema entry can say that a running transport only picks up a change
  when it restarts. The editor panel then restarts the sender's transport after such a commit (only
  while the component captures in a game world, the way the component's own restart properties do)
  and says so in the field's tooltip. New `IO3DOptionTarget::RestartTransport`. Every built-in field
  leaves it off, so behaviour is unchanged.
- **`Validate`.** A schema entry can carry `TFunction<bool(const FString& Value, FText& OutError)>`.
  - The sender's transport controller and the receiver source check the options with
    `O3DTransportOptions::ValidateOptions` before they create the transport. A refused value fails
    the start with `InvalidConfig`, whose message names the key and the error (never the value).
    New `UO3DSenderComponent::GetLastTransportResult()` returns it; the receiver shows it in the
    source status.
  - The editor panel shows the error under the row and does not write the value.
  - Only set, visible values are checked; unset keys use their default. A value's type and range
    are not rejected: they keep falling back to the default or being clamped, as before.
  - No built-in transport declares a `Validate` yet.
- **Removed: `FO3DSenderSerializer::OnSubjectListReady`** and its `FOnO3DSubjectListReady`
  delegate type. Nothing broadcast or bound it.
- **Tests.**
  - New: `Open3DBroadcast.Shared.TypedConfig.ConfigCarriesTransportAndRole`,
    `Open3DBroadcast.Shared.OptionSchema.SecretEntryCarriesEnvVar`, `.FloatHonoursMinAndMax` and
    `.ValidateGivesInvalidConfig`, `Open3DBroadcast.Editor.OptionsPanel.RestartOnChangeRestartsTransport`,
    `.ValidateShowsErrorAndRefuses` and `.FloatIsClampedToRange`, and
    `Open3DBroadcast.Transport.WebRTC.Secrets.SchemaEntriesCarryEnvVars`.
  - Existing tests that build a config now set the registered name and an `EO3DTransportRole` (for
    example "TCP" instead of "sockets.tcp", "NNG" instead of "nng"). No assertion changed.

### Consumer API: SubmitFrame view and owned forms, Send(SubjectList) deleted (WP-A1 PR 5b, ADR 0007 step 5)

- **Interface version stays 5.** 5a and 5b ship in the same release (no release has carried
  version 5), so one number covers both. The WebRTC add-on is built against both in this change.
- **`ISerializedFrameConsumer`** (SHR-16, ADR 0007 item 3):
  - `SubmitFrame(const FString& Subject, TConstArrayView<uint8> Bytes, double TimestampSeconds)`
    is the view form: the bytes belong to the caller and are valid only for the call.
  - `SubmitFrameOwned(const FString& Subject, TArray<uint8>&& Bytes, double TimestampSeconds)` is
    the owned form: the consumer may keep the buffer without a copy. Its default passes a view to
    `SubmitFrame`, so a consumer that implements only the view form gets every frame.
  - The subject stays a case-sensitive `FString` (not the `FName` the ADR sketched), like
    `FO3DSendPayload::Subject`.
  - Source change for a consumer: override `SubmitFrame` with `TConstArrayView<uint8>` instead of
    `const TArray<uint8>&`, and copy the bytes if you keep them (`TArray<uint8>(Bytes.GetData(),
    Bytes.Num())`); override `SubmitFrameOwned` too to keep a handed-over buffer without a copy.
- **Receivers.** The demux hands mocap to the consumer as a view of the received message
  (enveloped or raw) instead of copying it into a scratch buffer first, so TCP, UDP and NNG
  deliver each frame without a copy. Loopback, MoQ and the WebRTC add-on already hold each frame
  in its own buffer (their hand-off queue items) and give it away with the owned form. New
  `FO3DUnifiedReceiveDemux::DeliverMocapOwned`; `DeliverMocap` takes a view (a TArray converts).
- **LiveLink source.** Unchanged for users. It implements both forms; a frame that arrives off
  the game thread is moved into the game-thread task when it comes in the owned form, and copied
  once when it comes as a view.
- **`IOpen3DSender::Send(const O3DS::SubjectList&)` is deleted**, with every transport's
  implementation and the sender component's `OnSubjectListReady` handler, which nothing invoked
  (the serializer never broadcasts it). Use `SendSerialized` with the serialized bytes. The
  transport interface no longer mentions the o3ds core, and the WebRTC add-on's runtime code no
  longer includes it; the add-on keeps the `Open3DStreamCore` dependency only for its tests.
- **Tests.** New `Open3DBroadcast.Shared.FrameConsumer.ViewFormHasNoCopy`, `.OwnedFormMovesTheBuffer`,
  `.ViewOnlyConsumerGetsOwnedFrames` and `Open3DBroadcast.Transport.Loopback.FrameReachesConsumerWithoutCopy`.
  Existing consumer test doubles now override the view form; tests that sent a `SubjectList`
  use a helper that serializes it and calls `SendSerialized`, as the deleted `Send` did.

### Typed config: options view, one configure signature, LiveKit fields removed (WP-A1 PR 5a, ADR 0007 step 5)

- **Interface version 5.** `O3D_TRANSPORT_API_VERSION` is now 5: the descriptor's configure
  functions, `FO3DTransportConfig`'s layout and the `O3DTransportOptions` getters changed. A
  WebRTC add-on built for 4 registers nothing and logs the version error; install the add-on
  build made for this release.
- **`FO3DTransportOptionsView`** (`Transport/O3DTransportOptionsView.h`, new): read access to a
  role's options with the transport's schema. `GetString`/`GetInt`/`GetDouble`/`GetBool` use the
  schema's `Default` when a value is unset or does not parse, `IsVisible` evaluates `VisibleWhen`,
  `FindField`, `IsSet`. A view does not own its data. The `O3DTransportOptions` getters take a view
  (a plain map still converts, so calls with `AdvancedParams` compile unchanged and keep the
  caller's default). New `O3DTransportOptions::TryParseBool`.
- **Configure functions.** `FO3DTransportDescriptor::ConfigureSender` and `ConfigureReceiver` are
  both `TFunction<void(const FO3DTransportOptionsView&, FO3DTransportConfig&)>`
  (`FO3DTransportConfigureFunction`); they no longer receive the sender component or the receiver
  source settings. Source change for a transport: take the view instead and read the options from
  it (they are also in `Config.AdvancedParams`, as before). The deprecated
  `RegisterTransportCustomization` functions keep their old signatures and still receive the
  component or the settings through an adapter, until step 6 removes them.
- **`FO3DTransportConfig`.** Removed: `Token`, `bPersistToken`, `bUseAutoTokenFetch`,
  `TokenEndpointUrl`, `TokenRefreshLeadTimeSec` (SHR-36) and the unused `Backend`. Added:
  `SubjectName` (the sender component's Subject Name, empty for a receiver), `OptionSchema` (the
  role's schema, shared) and `GetOptions()`. `AdvancedParams` keeps its name and meaning. The
  struct was never saved, so no saved data is affected; `ToDebugString` no longer prints a
  `Token=` or `Backend=` part.
- **WebRTC add-on.** The token settings are options again end to end: the sender and receiver read
  `webrtc.useAutoTokenFetch`, `webrtc.tokenEndpointUrl` and `webrtc.tokenRefreshLeadTimeSec` from
  the options and the token from `Config.Secrets` (`WebRTCUtils::ReadTokenSettings`), with the same
  parsing and defaults as before. Keys, defaults and the editor panel are unchanged. Code that
  built a config with `Config.Token = ...` puts the token in
  `Config.Secrets.Add(TEXT("webrtc.token"), ...)` instead. The add-on no longer depends on
  Open3DSender or Open3DReceiver.
- **MoQ.** The default stream id (and so the default track name) is still the sender's Subject
  Name, now read from `Config.SubjectName`; `Open3DTransportMoQ` no longer depends on Open3DSender.
- **Switching transports keeps options (SND-35).** Changing a sender's or a receiver source's
  transport no longer clears the options: the outgoing transport's options are kept (new saved
  property `InactiveTransportOptions` on `UO3DSenderComponent` and `FO3DReceiverSourceConfig`) and
  come back when you switch back. Option keys are not renamed. The kept options never hold a
  secret, and a LiveLink connection string carries only the selected transport's options. Undo
  restores both. New `O3DReceiver::SwitchTransport`. Options of a transport that is not registered
  when you switch away (for example its plugin is not loaded) are dropped, as before, because its
  secret keys are unknown.
- **Saved data.** Nothing to migrate: the LiveKit values users saved were always the namespaced
  `webrtc.*` keys of the component's or source's option map, which load and reach the transport
  unchanged. Assets saved before this release load with no inactive options. The ADR 0004 legacy
  secret migration is unchanged.
- **Test helpers.** `Testing/O3DLifetimeTestUtils.h` moved to Open3DShared as
  `Testing/O3DTransportLifetimeTestUtils.h`; Open3DSender keeps a forwarding header until step 6.
- **Tests.** New `Open3DBroadcast.Shared.TransportOptionsView.TypedGettersAndDefaults`,
  `.VisibleWhen`, `.ConfigCarriesSchema`, `Open3DBroadcast.Shared.TransportOptions.SwitchKeepsOtherTransportsOptions`,
  `Open3DBroadcast.Shared.TransportApiVersion.TypedConfigIsVersion5`,
  `Open3DBroadcast.Sender.TypedConfig.SavedOptionsReachConfigureFunction`,
  `.DeprecatedConfigureGetsComponent`, `Open3DBroadcast.Sender.TransportSwitch.KeepsOtherTransportsOptions`,
  `Open3DBroadcast.Receiver.TypedConfig.SavedOptionsReachConfigureFunction`,
  `.DeprecatedConfigureGetsSettings`, `Open3DBroadcast.Receiver.TransportSwitch.KeepsOtherTransportsOptions`
  and `Open3DBroadcast.Transport.WebRTC.TypedConfig.SavedOptionsReachTransport`. Existing tests were
  changed only where they used a removed field or the old configure signature.

### WebRTC add-on on the shared transport blocks (WP-A1 PR 4f, ADR 0007 step 4)

- **Wire format unchanged.** Same data-channel labels (the subject, `__o3d.ctl` for control),
  reliability (reliable by default, lossy with `webrtc.prefer_lossy`), ordered delivery, audio
  tracks named after the subject, option keys and defaults.
- **Receiver.** Frames and control go from LiveKit's data callback to `Poll` through two
  `FO3DSendQueue` hand-offs instead of locked containers, and `Poll` delivers them through
  `FO3DUnifiedReceiveDemux` (`DeliverMocap`, the label as the subject; `DeliverControlEnvelope`).
  Audio is unchanged: LiveKit hands decoded PCM16 to the audio callback, which calls the audio
  sink on LiveKit's thread. Changes:
  - Frames waiting for `Poll` are now bounded at 16 MiB; a frame over that is refused and
    counted in `DroppedFrames` (they used to queue without a limit). `PendingFrames` and
    `PendingBytes` report what waits. Control keeps its cap of 1,024 envelopes.
  - `Poll` delivers frames in arrival order across subjects; it used to deliver them grouped by
    subject. Each subject's frames are still in order.
  - The consumer and the control sink are held by the demux and released in `Stop`, as before.
- **Sender.** Unchanged: `SendSerialized` and `SendControl` still hand each message to LiveKit on
  the caller's thread, and LiveKit's refusal (`DroppedBackpressure`) is the backpressure. The
  audio sink still publishes PCM16 to a LiveKit track per subject behind the lifetime gate.
  Neither `FO3DSendQueue` nor `FO3DQueuedSenderAudioSink` fits (see the ADR addendum).
- **Reconnect.** Unchanged: the sender relies on LiveKit's own reconnect; the receiver keeps its
  no-data watchdog. `FO3DReconnectPolicy` is not used, so no second loop runs against LiveKit's.
- **Options.** Read with `O3DTransportOptions` (keys case-insensitive, values trimmed):
  - `webrtc.prefer_lossy` and `webrtc.useAutoTokenFetch` accept true/false, 1/0, yes/no and
    on/off; `webrtc.useAutoTokenFetch` used to treat any non-zero number as true.
  - `webrtc.reconnect_timeout` must be a number (it used to accept `2s` as 2); anything else is
    the default, 2 s. `webrtc.tokenRefreshLeadTimeSec` must be an integer; anything else is the
    default, 300 s.
  - `webrtc.url` and `webrtc.room` are trimmed.
  - `WebRTCUtils::ParseBoolOption` and the receiver's `ParseDoubleOption` are deleted.
- **Module dependencies.** The runtime code includes only Open3DShared and Open3DStreamCore
  headers: `ConfigureSender`/`ConfigureReceiver` read `Config.AdvancedParams`, not the sender
  component or the source settings. `Open3DTransportWebRTC.Build.cs` keeps Open3DSender and
  Open3DReceiver for two test files only. Net runtime lines in the add-on: -85 (153 added, 238 removed; tests not counted).
- **API version stays 4.** The add-on uses `FO3DSendQueue`, `FO3DUnifiedReceiveDemux` and
  `O3DTransportOptions`, exported since 4; nothing in Open3DBroadcast changed.
- **Tests.** New `Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverQueuePolicy`,
  `.ReceiverAudioIndependentOfFrameQueue`, `.SenderAudioIndependentOfDataChannel`,
  `.SenderStopWhileSending` and `.ReceiverStopWhileReceiving` (200 cycles each), on a fake LiveKit
  that, like the real one, makes no callback after the call that clears it has returned. None
  needs a LiveKit server. The existing add-on tests are unchanged.

### MoQ on the shared transport blocks (WP-A1 PR 4e, ADR 0007 step 4)

- **Wire format unchanged.** Same tracks (`mocap/`, `audio/`, `control/` namespaces, track name
  as before), the same bytes on each (a bare O3DS frame, a bare audio payload, a control
  envelope), relay URL handling, option keys and defaults.
- **Sender.** Frames, audio and control are items on one `FO3DSendQueue`, and an
  `FO3DTransportWorker` publishes each on its track. The frame policy is `RefuseNewest` with
  `queue_bytes` as the byte limit, as before: a full queue refuses the newest frame and never
  discards a queued one. The worker still drops an item whose publisher is not ready (not
  connected, or the track not announced yet), so a reconnect never replays a stale backlog.
  Changes:
  - Control has its own cap of 1,024 envelopes; it used to share `queue_bytes` with the frames.
    Audio keeps its own 1 MiB.
  - The audio sink is the shared `FO3DQueuedSenderAudioSink` (bare audio payloads, as before).
  - `FramesSent` counts frames only; audio adds to `BytesSent`. Audio used to count as a frame.
  - `DroppedFrames` counts frames only: audio refused by its queue or dropped at the worker is no
    longer counted there. `SendErrors` (failed publishes) and `PendingFrames` are filled.
  - The worker thread runs at normal priority (it was above normal).
  - Reconnecting is unchanged: driven from `Tick` and the session's state callbacks on the game
    thread, with the existing jittered backoff and connect timeout. moq-ffi does not reconnect by
    itself, so there is one loop; `FO3DReconnectPolicy` is not used (see the ADR addendum).
- **Receiver.** moq-ffi's data callbacks still reach the game thread through the session's
  dispatcher, then a bounded hand-off queue (an `FO3DSendQueue`, 16 MiB per kind) that `Poll`
  drains into `FO3DUnifiedReceiveDemux` (`DeliverMocap`, `DeliverAudioPayload`,
  `DeliverControlEnvelope`). Changes:
  - The consumer is held strongly and released in `Stop` (TRF-38); it used to be held weakly.
  - `FramesReceived`/`BytesReceived` count mocap only; audio used to count as a frame.
  - Rejected audio and malformed control count in `ReceiveErrors`, not `DroppedFrames`.
  - The hand-off limit is per kind (mocap, audio, control) instead of one shared 16 MiB.
- **Options.** `queue_bytes` must be digits (anything else is the default, as before) and
  `connect_timeout` a strict number (`O3DTransportOptions::TryParseInt`/`TryParseDouble`); keys
  are read with `O3DTransportOptions::GetString`. `MoQHelpers::GetAdvancedOption`, `ParseUInt64`
  and `TryGetAudioCodecFromFrame` are deleted (the receiver uses `O3DAudio::TryGetAudioPayloadCodec`).
- **Module dependencies.** `Open3DTransportMoQ` no longer depends on Open3DReceiver. It keeps
  Open3DSender for one field: `ConfigureSender` defaults the stream id to the sender component's
  `SubjectName`, which no generic config field carries until ADR 0007 step 5. The relay URL is
  read from the config. `FO3DMoQSenderAudioSink`, `FMoQSenderAudioState` and MoQ's
  `FO3DEncodedPayloadQueue` use are deleted. Net lines under `Open3DTransportMoQ`: -386.
- **API version stays 4.** Nothing the add-on uses changed.
- **Tests.** New `Open3DBroadcast.Transport.MoQ.QueueRefusesNewestUnderBackpressure`,
  `.AudioIndependentOfFrameQueue` and `.StopWhileSending` (four send threads and an audio thread,
  1,000 Stop cycles, every one connected). All run on the fake moq-ffi; none needs a relay.
  `MoQTesting.h` gained `SenderSetWorkerPaused`. The existing MoQ, network-gated relay and
  conformance tests are unchanged.

### NNG on the shared transport blocks (WP-A1 PR 4d, ADR 0007 step 4)

- **Fix: `FO3DSendQueue` pending counters never read above a limit.** `Enqueue` used to add to
  the item and byte counters first and subtract again on overflow, so `GetStats` could briefly
  report pending bytes over the cap (seen as an intermittent
  `Open3DBroadcast.Shared.SendQueue.ConcurrentAccounting` failure). It now reserves with a
  compare-exchange that only succeeds when the result fits. Admission and refusal are unchanged.
- **Wire format unchanged.** Same NNG protocols (pub/sub, pair, push/pull), URL schemes
  (`nng+<mode>://`, `tcp://`), envelopes, option keys and defaults.
- **Sender.** Frames, audio and control are items on one `FO3DSendQueue`, and an
  `FO3DTransportWorker` hands each to `nng_send` with `NNG_FLAG_NONBLOCK`. The frame policy is
  `RefuseNewest` with `nng.qmax` as the byte limit, in every mode, as before: a full queue refuses
  the newest frame and never discards a queued one. The worker still drops the oldest frame when
  NNG cannot take it at once (no peer, or NNG's send buffer full), so pub/sub stays lossy and the
  queue holds no stale backlog. Changes:
  - `nng.qmax` now bounds frames and, separately, audio; control has its own cap of 1,024
    envelopes. Before, all three shared one byte cap, so a full frame queue refused audio and
    control.
  - `FramesSent` counts frames only; audio adds to `BytesSent`. Audio used to count as a frame.
  - `DroppedFrames` counts frames only: audio refused by the queue or dropped at the worker is no
    longer counted there. `SendErrors`, `PendingFrames` and `PendingBytes` are filled.
  - The audio sink is the shared `FO3DQueuedSenderAudioSink`.
  - Reopening a socket that failed to open, listen or dial, or that NNG reported closed, is paced
    by `FO3DReconnectPolicy` (0.1 s doubling to 5 s, as before, now with ±20% jitter). NNG still
    redials a dropped connection by itself; the two never run at once.
- **Receiver.** Still read by `Poll` on the game thread: NNG's own threads do the socket I/O, so
  `Poll` only takes what NNG already received. Each message goes to `FO3DUnifiedReceiveDemux`.
  Changes:
  - The consumer is held strongly and released in `Stop` (TRF-38); it used to be held weakly.
  - A damaged envelope (magic present, header does not fit) is dropped instead of being passed on
    as raw mocap.
  - `FramesReceived` and `BytesReceived` count mocap only; audio used to count as a frame.
  - Rejected audio, malformed and oversize messages and receive errors count in `ReceiveErrors`
    instead of `DroppedFrames`; a control envelope is no longer counted as a dropped frame.
  - A listening receiver whose socket failed to open is retried with the same backoff instead of
    on every `Poll`.
- **Options.** Hosts and ports go through `O3DTransportOptions` (ports 1 to 65535 in digits only,
  bracketed IPv6), so `6000abc` is no longer port 6000: a malformed `host` or `port` option, Uri
  authority, `?host=` or `?port=` is now `InvalidConfig` instead of being read as far as it parses.
  `nng.qmax` must be digits (0 still means the default). A StreamId that is not `host:port` is
  still ignored. The configure functions read the config only.
- **Module dependencies.** `Open3DTransportNNG` no longer depends on Open3DSender or
  Open3DReceiver, and none of its files includes their headers. The NNG publish state, audio sink
  and `FO3DEncodedPayloadQueue` use, the receiver's demux branches and audio decoder, and the
  module's own integer and host:port parsers are deleted. Net lines under `Open3DTransportNNG`:
  -349.
- **API version stays 4.** Nothing the add-on uses changed.
- **Tests.** New `Open3DBroadcast.Transport.NNG.QueueRefusesNewestUnderBackpressure`,
  `.AudioIndependentOfFrameQueue` and `.StopWhileSending` (four send threads and an audio thread,
  1,000 Stop cycles, every 50th with a connected peer). `NngTesting.h` gained
  `SenderSetWorkerPaused`. The existing NNG and conformance tests are unchanged.

### UDP on the shared transport blocks (WP-A1 PR 4c, ADR 0007 step 4)

- **Wire format unchanged.** Same datagrams, the same fragment header and reassembly
  (`o3ds/udp_fragment`), the same envelopes, option keys and defaults.
- **Sender.** Frames, audio and control are items on one `FO3DSendQueue`, and an
  `FO3DTransportWorker` sends each one (fragmenting above `udp.maxdatagram`), so no caller's
  thread calls `SendTo` any more (TRB-20). UDP is Unreliable, so the frame policy is
  `DropOldest`: while the worker is behind, at most 4 frames (16 MiB) wait and older ones are
  discarded so the newest go out; callers are refused (`DroppedBackpressure`) only at twice that.
  Changes:
  - `SendSerialized` returns `Queued` instead of reporting the socket result. A failed `SendTo`
    is counted afterwards in `DroppedFrames` and `SendErrors`.
  - `FramesSent` and `BytesSent` count what was actually sent; audio adds the bytes sent, not
    the bytes queued.
  - `DroppedFrames` also counts frames the queue discarded as too old in the backlog.
    `SendErrors`, `PendingFrames` and `PendingBytes` are filled.
  - Audio has a 1 MiB budget and control a cap of 1,024 envelopes of their own, so neither is
    refused or dropped because frames are waiting. Control is still one datagram per envelope
    and still refused with `TooLarge` above `udp.maxdatagram`.
  - A host name (`udp://mocap-pc.local:17800`) is resolved on the worker with
    `O3DTransportOptions::ResolveHostPort`, retried with `FO3DReconnectPolicy`; the state is
    `Connecting` until it resolves. It used to fail `Initialize` with `InvalidConfig`. IPv6
    destinations work. An IP literal is still checked in `Initialize`.
  - The audio sink is the shared `FO3DQueuedSenderAudioSink`; it refuses PCM while there is no
    socket, as before.
- **Receiver.** Still read by `Poll` on the game thread (ADR 0007 leaves UDP receive threading
  open), with the same per-call bounds and reassembly. Complete messages go to
  `FO3DUnifiedReceiveDemux`. Changes:
  - The consumer is held strongly and released in `Stop` (TRF-38); it used to be held weakly.
  - A damaged envelope (magic present, header does not fit) is dropped instead of being passed
    on as raw mocap. Rejected audio, malformed and oversize messages count in `ReceiveErrors`.
  - The bind address may be an IPv6 literal (`udp://[::]:17800`). A host name other than
    `localhost` is still refused (`InvalidConfig`, now from `Start`), so binding never resolves
    a name on the game thread.
- **Options.** The endpoint is parsed with `O3DTransportOptions::ParseHostPort` (ports 1 to
  65535 in digits only, bracketed IPv6) and every `udp.*` value with the strict getters, so
  `17800abc` is no longer port 17800 and `udp.broadcast=yes` is no longer read as false without
  notice. The UDP configure functions read the config only.
- **Module dependencies.** `Open3DTransportSockets` no longer depends on Open3DSender or
  Open3DReceiver, and none of its files includes their headers. `SocketsTransportConfigCommon.cpp`,
  the `O3DSockets` option parsers and `BuildUdpUri` are deleted. Net lines under
  `Open3DTransportSockets`: -441.
- **API version stays 4.** `O3DTransportOptions::IsIpLiteral` is new; nothing the add-on uses
  changed.
- **Tests.** New `Open3DBroadcast.Transport.Sockets.Udp.QueueDropsOldestUnderBackpressure`,
  `.AudioIndependentOfFrameQueue` and `.StopWhileSending` (four send threads and an audio thread,
  1,000 Stop cycles), and `Open3DBroadcast.Shared.HostPort.IsIpLiteral`. `SocketsTesting.h`
  gained `UdpSenderSetWorkerPaused`. The existing UDP, sockets and conformance tests are
  unchanged.

### WebRTC becomes the Open3DBroadcastWebRTC add-on plugin (WP-F11, ADR 0002)

- **WebRTC is no longer part of Open3DBroadcast.** The `Open3DTransportWebRTC`
  module (same module name), `livekit_ffi` and their notices moved to a new
  plugin, `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/` (Beta, Win64,
  Server and Program excluded, plugin dependency on Open3DBroadcast). It is a
  free add-on installed in a project's `Plugins/` folder next to
  Open3DBroadcast, including a Fab-installed one. Open3DBroadcast alone
  contains no WebRTC module and no livekit file, so its whole tree passes
  `check-no-video-codecs.sh`.
- **Upgrade:** projects that use the WebRTC transport must also install
  Open3DBroadcastWebRTC (the build made for the same Open3DBroadcast
  release) and enable it. Components and LiveLink sources keep their WebRTC
  settings; without the add-on they report that the transport is not
  registered. ProjectSandbox enables both plugins.
- **Transport API version.** New
  `Open3DShared/Public/Transport/O3DTransportApiVersion.h`:
  `O3D_TRANSPORT_API_VERSION` (1), the exported
  `O3DTransport::GetHostApiVersion()` and the inline
  `O3DTransport::CheckApiVersion()` (ADR 0007 item 2, minimal SHR-14). The
  add-on checks it first in `StartupModule` and, on a mismatch, logs
  `WebRTC transport not registered: Open3DBroadcastWebRTC was built for
  Open3DBroadcast transport API version ...` and registers nothing. Raise the
  number with any change to the exported transport interface.
- **Add-on blockers from ADR 0002 fixed.** `livekit_ffi.dll` is loaded from
  the add-on's own plugin folder through `FO3DFfiLibrary` (TRF-28); the
  module's `Build.cs` uses the `Open3DStreamCore` module and no path into
  Open3DBroadcast (BUILD-1); if the version check or the DLL load fails
  nothing is registered, and shutdown stops live instances before the DLL is
  freed and undoes only what startup did (TRF-14).
- **Build flags and console variables (SHR-19).** `O3D_WITH_TRANSPORT_WEBRTC`
  is read by the add-on's own `O3DWebRtcBuildFlags`, no longer by
  Open3DBroadcast's `O3DBuildFlags`; the add-on also reports the ignored
  `O3D_WEBRTC_BACKEND_*` flags. The ten unused WebRTC console variables
  (`o3ds.WebRTC.*`, `o3ds.Broadcast.WebRTC.*`) and both copies of
  `O3DConsoleVars.h` are removed from Open3DShared; nothing read them. The
  LiveKit fields of `FO3DTransportConfig` stay until WP-A1 (SHR-36).
- **Tests.** The WebRTC tests moved with the module (still
  `Open3DBroadcast.Transport.WebRTC.*`), plus
  `Open3DBroadcast.Transport.WebRTC.AddOn.*` (version check, livekit_ffi
  found in the add-on and not in Open3DBroadcast, transport registered) and
  `Open3DBroadcast.Shared.TransportApiVersion.*`. `Open3DBroadcastTests` does
  not depend on the add-on. The WebRTC conformance profile is still deferred
  (now to WP-T2e).
- **Scripts and CI.** `Build/Fab/exclude-modules.txt` no longer lists WebRTC;
  `fab-package.py` instead fails if a WebRTC module folder, `.uplugin` entry
  or livekit file appears anywhere in Open3DBroadcast. The Fab workflow also
  runs `check-no-video-codecs.sh` on the plugin tree (`--addon` scans the
  add-on, which still fails by design). The copyright-header and
  runtime-editor-deps checks cover both plugins. New
  `Build/Scripts/Build-WebRTCAddOn.ps1` builds the add-on against an
  Open3DBroadcast package; PR CI and the nightly run it after the
  main-plugin tests, upload the add-on
  (`Open3DBroadcastWebRTC-Win64-<sha>`) and run every test again with both
  plugins enabled. The release workflow builds it with the same version and
  attaches it to the release only when `O3D_PUBLISH_WEBRTC_ADDON` is `true`
  (counsel question L1). The flag-combination nightly drops `no-webrtc`.
- **Docs.** The WebRTC user guide moved to the add-on (`USER_GUIDE.md`, with
  installation, version matching and removal), which also has a README and
  its own `THIRD_PARTY_LICENSES.md` with the livekit_ffi entries. The main
  USER_GUIDE and README say WebRTC is a free add-on, with a download-link
  placeholder until the support-site URL exists.

### Control channel on WebRTC (CTL-6, ADR 0011)

- The WebRTC add-on now carries control (events and values from
  `UO3DSenderComponent`'s `FireControlEvent` / `SetControlValue`):
  `SupportsControl()` is true on its sender and receiver. It needs no
  API change of its own; with WP-A1 PR 2 the add-on builds against
  transport API version 3.
- **Sender.** `SendControl` sends each control envelope with
  `lk_send_data_ex` on a data channel labelled `__o3d.ctl`, reliable and
  ordered, never through `SendSerialized`. It refuses anything that is not
  exactly one well-formed control envelope of at most 1,100 bytes, and any
  call while not connected (before `Start`, after `Stop`, while LiveKit
  reconnects); the control publisher retries. It enters the sender's
  lifetime gate, so `Stop` never destroys the LiveKit client under a send.
  Control never moves a frame, byte or drop counter, and its failures have
  their own log throttle (one Warning per 2 s).
- **Receiver.** The data callback checks for a control envelope (envelope
  magic and kind Control) **before** the "label = subject" mocap path,
  whatever the label, including the unlabeled fallback callback. A
  well-formed envelope is queued (at most 1,024) and handed to the control
  sink on the game thread in `Poll`; a malformed one, or one that arrives
  with no sink, is dropped. Control never reaches the frame consumer and
  moves no frame, byte or drop counter. Plain mocap bytes on a data channel
  labelled `__o3d.ctl` (a subject with that name) are still mocap, as ADR
  0011 requires. The sink is held strongly and released in `Stop`.
- **Upgrade WebRTC receivers before using control:** a receiver built
  before this release reads control as a malformed mocap frame and logs a
  warning for it (ADR 0011 item 10).
- **Tests.** `Open3DBroadcast.Transport.WebRTC.Control.*` (fake LiveKit
  FFI): round trip interleaved with mocap, label classification, sender
  refusals (oversize, not an envelope, not running, FFI failure), sink
  released on `Stop`, and `Stop` while four threads send. They cover the
  control conformance cases for WebRTC, whose conformance profile is still
  deferred to an add-on test module (WP-T2e). Manual check: case 12 in
  `docs/testing/webrtc-manual-test.md`.

### Editor module split (WP-F7, ADR 0010)

- **New `Open3DBroadcastEditor` module** (Type `Editor`, `PostEngineInit`,
  Win64; ships in the Fab package). It owns the `UO3DSenderComponent`
  Details customization, the LiveLink "Add Source" panel of the receiver and
  one generic transport settings panel (`SO3DTransportOptionsPanel`). The
  runtime modules (Shared, Sender, Receiver and every transport) no longer
  depend on `Slate`, `SlateCore`, `PropertyEditor`, `EditorStyle`,
  `AppFramework`, `ApplicationCore` or `InputCore` (FAB-7, SND-34, TRB-46).
- **Transports declare their options as data.** `FO3DSenderTransportCustomization`
  and `FO3DReceiverTransportCustomization` lose the `WITH_EDITOR`
  `BuildTransportWidget` member and gain `OptionSchema`
  (`FO3DTransportOptionSchema`, `Open3DShared/Public/O3DTransportOptionSchema.h`),
  read with `O3DSender::GetTransportOptionSchema` /
  `O3DReceiver::GetTransportOptionSchema`. Source change for any code that
  set `BuildTransportWidget`: declare the fields in `OptionSchema` instead.
  The per-transport Slate panels of Loopback, Sockets, NNG, MoQ and WebRTC
  are deleted.
- `SO3DTransportConfigPanelBase` moves from Open3DReceiver
  (`OPEN3DRECEIVER_API`) to Open3DBroadcastEditor
  (`OPEN3DBROADCASTEDITOR_API`). `UO3DReceiverSourceFactory` stays in
  Open3DReceiver; its creation panel comes from
  `O3DReceiver::SetSourceFactoryPanelBuilder`, which the editor module sets.
- **Panel behaviour (TRB-45, SND-35).** Opening a panel no longer writes
  defaults into the component or source settings; defaults show as hints.
  Values are written on commit only, not on every spin-box drag step. Each
  commit is one undo step, and changing the sender's or receiver's transport
  is one undo step together with the option clear. The panels hold their
  object weakly. A user's port that equals the other Sockets protocol's
  default is no longer reset. NNG shows Mode and Role as two fields; an
  empty Role uses the default role for the mode.
- Switching transports still clears the option map, because most keys
  (`host`, `port`, `channel`) are not namespaced per transport yet. ADR 0007
  item 8 (WP-A1) namespaces them and removes the clear.
- CI: `Build/Scripts/check-runtime-editor-deps.py` fails when a runtime
  module's `Build.cs` names an editor-only module or its sources include an
  editor-only header; it runs in the new "Runtime modules free of editor
  code" PR job. The nightly packages ProjectSandbox as a Win64 Shipping game
  (`Build/Scripts/Build-ShippingGame.ps1`).
- Tests: `Open3DBroadcast.Editor.OptionsPanel.*` (construction does not
  modify the object, one commit is one undoable transaction, secrets never
  enter the option map, a missing object is harmless, every registered
  transport's schema is well formed and its panel opens clean).

### Platforms and build flags (WP-F2, ADR 0001)

- **The plugin declares Win64 as its only platform.** Every module entry in
  `Open3DBroadcast.uplugin` has `"PlatformAllowList": [ "Win64" ]` and the
  plugin has `"SupportedTargetPlatforms": [ "Win64" ]` (FAB-3, SND-11,
  RCV-30, TRB-41, SHR-21). A target for another platform is meant to leave
  the modules out instead of failing in their `Build.cs`; this has not been
  verified on a Linux target yet (see `Build/README.md`, "Platforms").
- **Target types (FAB-8).** The runtime modules support Editor, Game and
  Client targets: `[SupportedTargetTypes]` now lists Client, and the
  `.uplugin` entries have `"TargetDenyList": [ "Server", "Program" ]`.
  Server was and stays excluded; the plugin README says why.
- **No more platform `throw`s in NNG, WebRTC and MoQ.** Their transport flag
  is forced to 0 on any platform other than Win64 and the module builds as a
  stub (TRB-41, TRF-27). The dead Linux/Mac branches in
  `Open3DTransportMoQ.Build.cs` and `MoQFfiSupport.cpp`, and the
  `#error "Unsupported platform"` in the WebRTC module, are gone (BUILD-2).
  Open3DSender and Open3DReceiver still stop with a `BuildException` on
  other platforms, now with a message pointing at the allow list, until
  WP-F1 compiles the core from source.
- **Transport flags now leave the transport out (TRB-24, TRF-27).**
  With `O3D_WITH_TRANSPORT_SOCKETS`, `_NNG`, `_WEBRTC` or `_MOQ` set to 0,
  every source file of that module is inside `#if O3D_WITH_TRANSPORT_<X>`
  and the module compiles to a stub that registers nothing. It used to
  return early from `Build.cs` and then fail to compile its sources.
- **`O3DBuildFlags` moved** from `Open3DShared.Build.cs` to
  `Source/Open3DBroadcastBuildFlags/Open3DBroadcastBuildFlags.Build.cs`
  and no longer caches anything, so each target in one UBT run gets its own
  platform decision (SHR-22, BUILD-4).
- **Removed flags.** `O3D_BUILD_SENDER=0` and `O3D_BUILD_RECEIVER=0` now
  fail with a clear message; they always failed the build before, because
  both modules are always built. `O3D_WEBRTC_BACKEND_LIVEKIT`,
  `O3D_WEBRTC_BACKEND_LIBDC` and `O3D_ENABLE_LEGACY` are ignored, and their
  preprocessor definitions are gone (no source read them) (BUILD-4).
- **Exceptions only where needed (BUILD-5).** `bEnableExceptions` is set in
  the modules that compile the o3ds core headers (Sender, Receiver,
  Loopback, enabled transports, tests), not in Open3DShared or in a
  transport stub. RTTI stays off.
- **Dependencies (SHR-20, RCV-30).** The transports' public dependencies are
  `Core` only; the rest are private. Open3DReceiver's `LiveLink` and
  `LiveLinkAnimationCore` are private, and its unused `AudioMixer`
  dependency is removed. Open3DShared keeps `CoreUObject` and `Engine`
  public because `O3DCredentialLibrary.h` declares a
  `UBlueprintFunctionLibrary`.
- **Paths (BUILD-1).** The WebRTC and MoQ `Build.cs` no longer add
  `<PluginDirectory>/../../ThirdParty/open3dstream/include`, which pointed
  outside the plugin and was never found; the o3ds headers come from
  Open3DSender and Open3DReceiver.
- The empty umbrella header `Open3DShared/Public/O3DTransportRegistry.h`,
  which tried to include Sender and Receiver headers from the base module,
  is deleted (SHR-4 layering).
- Open3DShared prints a build warning when it is built without Opus
  (SHR-21).
- CI: `fab-package.py` fails when a module has no `PlatformAllowList` or the
  plugin has no `SupportedTargetPlatforms`. The nightly workflow builds each
  transport flag combination (`Build/Scripts/Build-FlagCombinations.ps1`)
  and runs `Build/Scripts/Test-LinuxExclusion.ps1`, which builds the
  ProjectSandbox game for Linux when the runner has the Linux toolchain and
  skips with a notice otherwise. PR CI is unchanged.

### Packaging layout: the core compiled from source (WP-F1, ADR 0003)

- **A clean clone builds with `RunUAT BuildPlugin` alone** (FAB-2, FAB-6).
  The o3ds core is no longer a prebuilt `open3dstreamstatic.lib` produced by
  `Sync-O3DSCore.ps1` before every build. A new `Open3DStreamCore` module
  (listed first in the `.uplugin`, Win64, no Server/Program) compiles it from
  `Source/ThirdParty/Open3DStreamCore/`, a committed copy of the part of
  `src/o3ds` the plugin uses, plus `o3ds_generated.h`, the FlatBuffers 2.0.6
  runtime headers and CRC++'s `CRC.h`. `flatbuffers.lib` is no longer
  linked; the plugin never needed it.
- **The copy is generated and checked.** `Build/Scripts/sync_o3ds_core.py`
  writes it from the headers in `Build/o3ds-core-manifest.txt` and their
  include closure, with a `SYNC_STAMP.txt` (pins, versions, hashes).
  `core-tests.yml` runs it with `--check` on every PR, so a core change
  without a re-sync, or a hand edit of the copy, fails CI (#203). The same
  workflow now also fails when the committed `src/o3ds_generated.h` differs
  from what the pinned `flatc` generates from `src/o3ds.fbs` (CORE-21). The
  module `static_assert`s FlatBuffers 2.0.6.
- **Core warnings.** Each core `.cpp` is compiled through a generated
  `Private/Core/O3DSCore_*.cpp` that switches compiler warnings off for the
  core, which has its own warning checks in `core-tests.yml`. The module is
  built without PCH, unity, exceptions or RTTI.
- **Core source changes (`src/o3ds`).** New `o3ds_export.h` with `O3DS_API`
  (empty in the CMake build; `OPEN3DSTREAMCORE_API` in the plugin) on the
  classes and functions the plugin uses across modules.
  `ReceiverStreamTable` declares its copy operations deleted and its moves
  defaulted, which MSVC needs for an exported class that owns
  `std::unique_ptr`s. `GetTime()` uses `std::chrono::steady_clock` instead of
  `<windows.h>`/`clock_gettime`. `src/CMakeLists.txt`: `FATAL` is now
  `FATAL_ERROR`, and the duplicated `o3ds_version.h` entry is gone.
- **Build.cs.** Sender and Receiver depend publicly on `Open3DStreamCore`;
  Loopback, Sockets, NNG, WebRTC, MoQ and the test module privately. The
  `ThirdParty/open3dstream` and `ThirdParty/flatbuffers` include paths and
  libraries, the missing-library `throw`s and the Sender/Receiver
  `BuildException` on non-Win64 platforms are gone; `PlatformAllowList`
  still limits every module to Win64 (ADR 0001).
- **Everything the build needs is under `Source/`.** The plugin-root
  `ThirdParty/` is gone: `open3dstream/` (its licences are now in the
  copy's `LICENSES/`; the CML licence is dropped because nothing links CML),
  `flatbuffers/` (the library and the compiler-only headers, SHR-23) and the
  orphaned `Include/` Opus headers (SHR-23) are deleted, and `opus/` moved
  to `Source/ThirdParty/opus/`. NNG stays a prebuilt library in its module.
- **`Config/FilterPlugin.ini`** lists the root `README.md`, `USER_GUIDE.md`,
  `Transport_Module_Comparison.md`, `LICENSE` and `THIRD_PARTY_LICENSES.md`,
  which BuildPlugin used to leave out of the package (FAB-2).
- **`CanContainContent` is `false`**: the plugin has no `Content/` folder
  (HYG-2). WP-U5 sets it back when it adds sample content.
- **CI.** The plugin CI, nightly, test and release workflows no longer
  install CMake, set up MSVC or run `Sync-O3DSCore.ps1`, which is deleted.
  The PR CI UE job checks out without submodules, and its path filter no
  longer lists `src/`, `thirdparty/`, `apps/` or CMake files.
  `Build-FabZip.ps1` no longer copies a core into the extracted Fab zip;
  CI passes `-RequireStandalone`, which checks the zip holds the core module
  and its copy. `fab-package.py` fails on a top-level file or folder that is
  neither standard nor listed in `FilterPlugin.ini`, on a prebuilt core or
  FlatBuffers library, and when the core copy or `FilterPlugin.ini` is
  missing.

### Tests

- UE automation tests moved out of the Runtime modules into a new editor-only
  module, `Open3DBroadcastTests` (Type `Editor`, Win64; ADR 0006, WP-T2). The
  Fab package leaves it out (`Build/Fab/exclude-modules.txt`). WebRTC tests
  stay in their module until the WebRTC add-on gets its own test module
  (WP-F11).
- Tests reach private code only through exported `Public/Testing/*.h`
  headers: `O3DSenderTesting.h`, `O3DReceiverTesting.h`, `SocketsTesting.h`,
  `NngTesting.h` and `MoQTesting.h` (`CreateSenderForTest`,
  `CreateReceiverForTest`, the `FMoQTestSession` façade). They compile only
  with `WITH_DEV_AUTOMATION_TESTS`. `MoQFfiApi.h` moved to the MoQ module's
  `Public` folder.
- Open3DShared no longer adds the core include paths and libraries "for
  tests" or the Open3DSender include path (SHR-4, SHR-20).
- New transport conformance suite,
  `Open3DBroadcast.Conformance.<Transport>.<Case>`, over every registered
  transport: lifecycle, sends rejected when not running, backpressure,
  concurrent sends, monotonic stats, byte-exact round trip of recorded
  frames, and callbacks after destroy (fake FFI). Profiles exist for Fake,
  Loopback, TCP, UDP, NNG and MoQ. A registered transport without a profile
  gets a failing `HasProfile` test. It replaces three placeholders that
  asserted `TestTrue(..., true)` (SHR-5).
- New fakes: `FO3DFakeSender`, `FO3DFakeReceiver` and
  `FO3DFakeTransportScope`, which registers them under a unique name per
  test. The fake moq-ffi table now routes published data to subscribers.
- New Shared parser tests for the unified envelope and the audio frame
  formats (SHR-6).
- NNG (WP-S11): one localhost integration test per mode and role pair,
  `Open3DBroadcast.Transport.NNG.ModeRole.*` (pub/sub, pair both ways,
  push/pull both ways), with default settings apart from the role. They use
  127.0.0.1 and an OS-chosen port, and run in the default filter. New option
  parser tests (`Options.UriHostHonoured`, `Options.DefaultRoles`) and
  `Demux.AudioNotCountedTwice`.
- Test names follow `Open3DBroadcast.<Area>.<Unit>.<Case>`; the
  `Open3DBroadcast.Open3DTransport*`, `O3DSender`, `O3DShared` and
  `O3DReceiver` prefixes are gone. Every test file uses
  `WITH_DEV_AUTOMATION_TESTS`. Run everything with the filter
  `Open3DBroadcast`.
- Relay tests moved to `Open3DBroadcast.Network.MoQ.*`. They register only
  when `O3DB_NETWORK_TESTS=1`, take the relay from `O3D_MOQ_RELAY_URL` only,
  and fail when it is unset. The hard-coded public relay is gone (UX-4).
- Fixed tests: `FinalizeAudioMeta` asserts the stream-label fallback (RCV-2);
  `BackpressureByteLimit` runs a started sender and overflows the byte cap
  (TRF-34); the MoQ `SupportsAudio` and `CreateAudioSink` tests assert the
  audio support that shipped; the TCP audio round trip expects the submitted
  stream label, as the Loopback audio test does;
  `AudioSinkOutlivesSource` marks the frame format as unknown before
  expecting the snapshot's rate and channels.
- Gauntlet is retired: `Tests/Gauntlet/` and `Build/Scripts/Run-Gauntlet.ps1`
  are deleted (ADR 0006 Q5). Nothing called them, and their filter matched no
  test.

- WP-S10 audio and metrics tests:
  `Open3DBroadcast.Shared.Audio.Encoder.OpusFraming512` and `.OpusFraming1024`
  (512- and 1024-frame buffers produce exact Opus packets that decode to a
  continuous stream, or labelled PCM16 without Opus),
  `.OpusUnavailableSendsLabelledPcm16`, `.SinkEncoderUnifiedMessages`,
  `Open3DBroadcast.Shared.Audio.Opus.SettingsValidation`,
  `Open3DBroadcast.Shared.Audio.Resampler.ChunkInvariantNoDrift` and
  `.AntiAliasing`, `Open3DBroadcast.Shared.Parsers.AudioMeta.RejectsOutOfRange`
  and `.RandomBytes`, `Open3DBroadcast.Shared.Metrics.ConcurrentRegistrationAndUpdates`
  and `.AtomicMaxAndAverage`, `Open3DBroadcast.Sender.Audio.StreamLabelMatchesSubject`
  and `.ResampledBuffersAddUp`.
- `Open3DBroadcast.Shared.Audio.Opus.RoundTrip` compensates for the encoder
  lookahead and requires an SNR above 20 dB and an average error below 0.02
  (it used to compare phase-shifted samples against a 0.15 average error). A
  decoded frame-count mismatch is now an error, and the debug dumps are gone
  (SHR-32).

### Build and CI

- `Build/Scripts/Build-Plugin.ps1` now fails when `RunUAT BuildPlugin` fails, with UAT's exit code (CI-1).
  It used to fall back to a ProjectSandbox UBT build and exit 0, so plugin CI
  could report success for a plugin that does not build. The fallback is still
  available for local troubleshooting behind the new `-AllowFallback` switch,
  which no workflow passes.
- Plugin PR CI runs the UE automation tests (CI-2). They run on the
  self-hosted UE runner after BuildPlugin, against the package that build
  produced, with the filter `Open3DBroadcast` and `O3DB_NETWORK_TESTS=0`. The
  manual Plugin Tests workflow compared a boolean input with the string
  `'true'`, so its tests never ran; that is fixed, and the workflow no longer
  runs on pushes to feature branches.
- `Build/Scripts/Run-AutomationTests.ps1` reads the automation report (CI-3).
  It fails when a test fails, no test runs, the report is missing or
  unreadable, or the editor exits non-zero. It used to pass whenever the
  editor exited 0. The new `-PluginPackageDir` option runs the tests in a
  throwaway host project that holds only a BuildPlugin package. The nightly
  uses it, so it now tests the binaries it just built instead of a
  ProjectSandbox it never compiled.
- Compiler warnings in plugin sources fail the plugin build (CI-4), through
  the new `-FailOnWarnings` switch of `Build-Plugin.ps1`. PR CI and the
  nightly also run one strict build: BuildPlugin `-StrictIncludes` (no
  precompiled headers, no unity build) with warnings as errors. The two
  existing warnings, uses of the deprecated `EKeys::Virtual_Accept` in the
  receiver's editor UI, now use `EKeys::Virtual_Gamepad_Accept.GetVirtualKey()`.
- New Fab source package job (CI-5, ADR 0002). `Build/Scripts/fab-package.py`
  builds a zip from the git-tracked plugin files without the modules in
  `Build/Fab/exclude-modules.txt` (WebRTC, and the future
  `Open3DBroadcastTests`) or the files in `Build/Fab/exclude-files.txt`
  (`.py`, module-level developer notes; a tracked `.pdb` fails the job, see
  WP-F3 above), and checks its contents. PR
  CI uploads it as `Open3DBroadcast-Fab-Source-<sha>` and runs BuildPlugin on
  its contents. Until WP-F1, that build copies in the o3ds core library built
  for the same commit and reports that the zip does not build on its own.
- `check-no-video-codecs.sh` runs on every binary in the Fab package as a
  required step (CI-8).
- The release notes say the plugin is built for UE 5.7 and Win64 only. They
  used to claim compatibility with UE 5.6 and later.
- The libwebrtc source build (`o3ds-webrtc-windows-native.yaml`, up to six
  hours) runs only when started by hand (CI-6). It ran on every push and PR
  to develop, on the same runner pool as the UE plugin CI.
- Plugin CI's path filter includes the o3ds core and everything else
  `Sync-O3DSCore.ps1` compiles (`src/`, `thirdparty/`, `CMakeLists.txt`,
  `apps/`, `plugins/mobu/`). Core-only PRs used to skip the UE build.
- Draft PRs (CI-9): the GitHub-hosted jobs (path filter, Fab package,
  core tests) run on every commit; the UE build and tests run once the PR is
  ready for review, or on a manual run. See `Build/README.md`.
- Copyright headers (WP-F4: FAB-5, FAB-9). The rights holder is Lifelike &
  Believable. Every `.h`, `.cpp` and `.cs` file under the plugin's `Source/`,
  other than `ThirdParty/` and generated files, now starts with
  a copyright line. The 97 files that had none now start with
  `// Copyright Lifelike & Believable. All Rights Reserved.` The 117 files
  that carried `// Copyright (c) Open3DStream Contributors` keep it
  unchanged. The change is comment-only. The new
  `Build/Scripts/check-copyright-headers.py` enforces it in a "Copyright
  headers" job of plugin CI on every PR, drafts included. Third-party code
  outside `ThirdParty/` goes in `Build/Fab/copyright-allowlist.txt` with a
  reason; the list is empty.

### Plugin descriptor (WP-F9)

- The plugin is marked Beta (`"IsBetaVersion": true`, ADR 0002). The MoQ
  transport is Experimental: UE has no per-module maturity key, so the
  descriptor `Description`, the plugin README and the USER_GUIDE say so and
  name the draft (draft-ietf-moq-transport-07) (FAB-12).
- The plugin's display name (`FriendlyName`) is now **Open3DBroadcast**,
  matching the plugin, README and USER_GUIDE names. It was "Open3D
  Broadcast Suite" (UX-5, partial).
- `SupportURL` points at this repository's GitHub issues instead of
  open3dstream.com (FAB-13). `DocsURL` points at `USER_GUIDE.md` on the
  `develop` branch; the old link used a `main` branch that does not exist
  (FAB-14).
- README and USER_GUIDE state the requirements: Unreal Engine 5.7, Win64
  only, editor and game targets (DOC-6, partial).

### Control channel (WP-CTL, ADR 0011)

- New one-way control stream from a sender to its receivers, beside mocap
  and audio: **values** (keyed, last writer wins, re-sent as a snapshot every
  `ControlSnapshotIntervalSeconds`) and **events** (fire once, de-duplicated,
  2 s time-to-live, `ControlEventRedundancy` copies). See USER_GUIDE
  "Control Channel".
- Sender: `UO3DSenderComponent::FireControlEvent`, `SetControlValue`,
  `ClearControlValue`, `ClearAllControlValues` and `GetControlValue`;
  properties `bAllowControlOnly`, `ControlSnapshotIntervalSeconds`,
  `ControlEventRedundancy` and `ControlMaxValueRateHz`.
- Receiver: off by default. Turn it on with Project Settings > Plugins >
  Open3DBroadcast Control (`UO3DControlSettings`, saved to
  `DefaultGame.ini`), at runtime with
  `UO3DControlLibrary::SetControlReceiveEnabled`, or per source with
  `ControlAccept`. Allowlist and per-sender byte and key limits. Control
  never sets properties by reflection or runs console commands.
- Gameplay: `UO3DRemoteControlComponent` (filters, `OnControlEvent`,
  `OnControlValueChanged`, `OnControlValueCleared`) and the game-thread
  `FO3DControlBus`. Events and value changes are held to play with the
  matching mocap (`bAlignControlToMocap`, on by default).
- Transports: TCP, UDP, NNG, Loopback, MoQ and, in the add-on, WebRTC
  (CTL-6).
- Core: `src/o3ds_control.fbs`, `src/o3ds/control.{h,cpp}`
  (`ControlPublisher`, `ControlReceiver`, `ControlAligner`), limits in
  `src/o3ds/parse_limits.h` (`ControlLimits`); CTest suites
  `control_codec_tests` and `control_state_tests`, and a `fuzz_control`
  target.

### Schema/Protocol

- **Control envelope kind (ADR 0011).** New `EUnifiedKind::Control = 2`, always paired with the new `EUnifiedCodec::O3DControl = 3`; a reader drops any other codec with kind Control. The payload is a new FlatBuffers root, `O3DS.Control.ControlMessage` (`src/o3ds_control.fbs`, `file_identifier "O3DC"`, generated `src/o3ds_control_generated.h`), with its own `protocol_version` (1). Readers reject a higher version. The schema is append-only; a new value type is skipped and counted by older readers, not rejected. `SubjectList` (`src/o3ds.fbs`) is unchanged, and no mocap frame's reader requirements change. A control envelope is at most 1,100 bytes including its header and its payload is never empty, so it is never fragmented and never mistaken for the TCP keepalive. It rides envelope version 1 (`O3DA`). Compatibility: receivers built before this change ignore control silently on TCP, UDP and NNG, and never subscribe to the MoQ `control/<session>` track; their mocap is unaffected. A sender sends no control bytes unless gameplay calls the control API. WebRTC carries control from CTL-6 on; WebRTC receivers built before CTL-6 log a (now throttled) warning per control message, so update WebRTC receivers before using control there. `O3DS_VERSION_TAG` is unchanged (1.0.4).
- **`O3D_TRANSPORT_API_VERSION` 1 → 2 (add-on authors).** `IOpen3DSender` gains `SupportsControl()` and `SendControl(const uint8* Envelope, int32 Len)`; `IOpen3DReceiver` gains `SupportsControl()` and `SetControlSink(...)`, with the new `IO3DReceiverControlSink`. All are appended with defaults (`false`), so a transport that does not carry control needs no code change, but the virtual function tables changed: rebuild every out-of-tree transport and the Open3DBroadcastWebRTC add-on against this release. A transport built for version 1 is refused at registration. Removing the WP-A1 forwarding shims will take version 3.

- No change to the TCP frame format (14-byte magic, little-endian length, payload; ADR 0009 item 6). The TCP sender now also writes a keepalive frame when idle: its payload is a 20-byte unified-envelope header (`O3DA`, version 1, kind Audio, payload size 0). Receivers built before this change parse it as an audio message, reject the empty payload without logging, and count it as received data, so it also stops their idle reconnects. Current receivers recognise it and ignore it. No other sender produces an audio envelope with an empty payload. Set `tcp.keepalive` to 0 to turn it off, for example for a third-party receiver that does not accept it. Protocol version and `O3DS_VERSION_TAG` are unchanged.

- No wire change. The 16-byte UDP fragment header is now read and written as explicit little-endian, which is byte-for-byte identical to what existing little-endian senders and receivers produce. The versioned `O3DF` header (ADR 0009, TRB-17) follows separately.

- No schema change. Behaviour change for D1 quantization: both ends now
  re-anchor at every full sync, and the UE sender sends a full sync at
  least once per `FullSyncIntervalSeconds`. A receiver built before this
  change that parses with `clearInactive=false` keeps its old anchors and
  misdecodes quantized translations after the first periodic full sync.
  D1 has not been released, and ADR 0009 versions the wire. Until
  `SubjectUpdate.ref_seq` lands (WP-A4a), a receiver that misses a full sync
  decodes quantized translations against its previous anchor until the next
  full sync reaches it.
