## Unreleased

### Core library (`src/o3ds`)

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

### Fixed

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

### Changed

- The largest reassembled UDP message the receiver accepts drops from 50 MiB to 4 MiB by default; set the new `udp.maxframe` receiver option to raise it (up to 50 MiB).
- Core API: `UdpMapper::addFragment` now takes `(sourceKey, data, size, nowMs)`, and `UdpMapper` takes an optional `UdpReassemblyConfig`.
- Core API: `SubjectList::Parse()` takes an optional `std::vector<ParsedSubjectInfo>*` that reports the subjects a packet touched and whether each got a full descriptor. `ParseUpdate()` and `ParseUpdateResidual()` return `bool` (applied or not) instead of `void`. A node with a null entry in a subject's node list is rejected.
- Core API: new `src/o3ds/receiver_streams.h` with `PeekPacketMeta`, `SkeletonFingerprint`, `LegacyOrdering` and `ReceiverStreamTable`, the UE-independent receiver state used by `FO3DReceiverSource`.
- Core API: `ConcealmentEngine::TryRenderAhead()` returns true at most once per `ObserveRealFrame()`.
- Receiver: legacy (no `tx_seq`) timestamp ordering now runs before the packet is parsed, so a dropped duplicate or out-of-order frame no longer changes parse state. A packet that fails verification is rejected before either path.

### Schema/Protocol

- No wire change. The 16-byte UDP fragment header is now read and written as explicit little-endian, which is byte-for-byte identical to what existing little-endian senders and receivers produce. The versioned `O3DF` header (ADR 0009, TRB-17) follows separately.
