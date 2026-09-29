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

- Quantization anchors are re-captured at every full sync on both ends
  (ADR 0005 (vii), WP-S3). `Subject::Serialize()` stores the float32 value it
  writes and `ParseSubject()` stores the value it parses. The old
  anchor-once rule only held for receivers that parse with
  `clearInactive=false`, so senders and receivers could disagree after a
  resync, and a receiver that joined late always did.
- New `src/o3ds/sender_sync.h`: `ConsumeCaptureBudget` (accumulator capture
  rate limiter), `FilterCurveValue` (curve epsilon/delta filter) and
  `FullSyncTracker` (per-subject full-sync policy), used by the UE sender.

### Fixed

- UDP fragment reassembly (`src/o3ds/udp_fragment.*`, WP-S2) is hardened against hostile or malformed datagrams: fragments that disagree with a message's first fragment are rejected (previously a heap overflow), in-flight state is bounded (8 messages, 16 MiB by default) with age-based expiry, message ids use wrapping comparison, messages are keyed per sender, rejected fragments no longer yield empty frames, and a use-after-free in `UdpMapper::getFrame` is gone.
- UE UDP receiver: `Poll()` is bounded per call (1024 datagrams / 8 MiB), the receive buffer is always 65,507 bytes regardless of `udp.maxdatagram`, and receive scratch buffers are reused.

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

### Changed

- The largest reassembled UDP message the receiver accepts drops from 50 MiB to 4 MiB by default; set the new `udp.maxframe` receiver option to raise it (up to 50 MiB).
- Core API: `UdpMapper::addFragment` now takes `(sourceKey, data, size, nowMs)`, and `UdpMapper` takes an optional `UdpReassemblyConfig`.

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

### Schema/Protocol

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
