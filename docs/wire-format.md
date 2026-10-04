# Open3DStream wire format

What goes on the wire between an Open3DStream sender and receiver, and the rules for changing it.
The decisions are in [ADR 0009](adr/0009-protocol-versioning.md) (versioning, byte order) and
[ADR 0005](adr/0005-wire-resync-and-loss-contract.md) (resync and loss); the constants and codecs
live in the core, in `src/o3ds/wire_format.h`, `src/o3ds/udp_fragment.h` and
`src/o3ds/tcp_stream_parser.h`, where CTest covers them.

**Wire protocol: 2. Core version (`O3DS_VERSION_TAG`): 1.1.0.**

Everything on the wire is **little-endian**. Every read and write goes through explicit byte
helpers, never a type-punned pointer (CORE-22). Nothing from before protocol 2 is accepted except
plain version-1 frames: there are no old receivers or senders to stay compatible with.

## 1. Mocap frame

A mocap frame is an 8-byte header followed by a FlatBuffer (`src/o3ds.fbs`, root `SubjectList`).

| Bytes | Field |
|---|---|
| 0 | `min_reader_version`: the lowest protocol a reader needs to apply this frame |
| 1 | flags, 0 |
| 2-3 | reserved, 0 |
| 4-7 | CRC-32 of the FlatBuffer (u32) |
| 8- | the `SubjectList` FlatBuffer, with file identifier `"O3DS"` at bytes 12-15 |

- **Writers** stamp `min_reader_version` from what the frame contains: **2** when any update is
  residual (`predictor_id != 0`) or carries a quantized vector (`*_q8`, `*_q16`), **1**
  otherwise. Full snapshots and plain deltas, which is what default settings send, are
  version 1, so readers from before protocol 2 still apply them. Writers also set
  `SubjectList.protocol_version` to the protocol they implement (2; 0 means a writer from
  before protocol 2). That field is for diagnostics and captures only.
- **Scale in updates** (CORE-11, ADR 0005 (v)): `SubjectUpdate.scale` carries absolute
  values in every update, including residual ones (`predictor_id != 0`), where translations,
  rotations and curves are residuals. A writer sends a transform's scale when it moved more
  than the delta threshold since it was last sent, and on every residual keyframe.
- **`SubjectUpdate.ref_seq`** is the `tx_seq` of the full Subject the update is relative to
  (topology, curve list, quantization anchors, residual history); 0 is unset. A receiver
  parsing sequenced frames with a `ParseContext` drops an update whose `ref_seq` names a full
  Subject it did not apply, and residual updates after a sequence gap, until the subject's
  next full Subject (ADR 0005 (ix)). It does not change `min_reader_version`.
- **Readers** (`O3DS::CheckFrame`, used by `SubjectList::Parse`, `PeekMeta` and
  `PeekPacketMeta`) accept `min_reader_version` 1 to `O3DS_PROTOCOL_VERSION` with bytes 1-3
  zero, check the CRC, and verify the FlatBuffer (the `"O3DS"` identifier is required on
  version-2 frames and optional on version 1). A frame that needs a newer protocol is rejected
  with "sender requires protocol N; update this receiver". Every rejection has a reason
  (`O3DS::Wire::FrameCheck`).

## 2. Unified envelope (audio, control)

Audio and control travel in an envelope; mocap travels as a raw frame (section 1).

**Envelope v2**, 24 bytes:

| Bytes | Field |
|---|---|
| 0-3 | magic `'O','3','D','U'` (a byte string) |
| 4 | envelope version, 2 |
| 5 | kind: 0 Mocap, 1 Audio, 2 Control |
| 6 | codec: 0 O3DS, 1 Opus, 2 PCM16, 3 O3DControl |
| 7 | flags, 0 |
| 8-15 | `timestamp_us` (u64), sender clock (section 6) |
| 16-19 | payload size (u32) |
| 20-23 | `seq` (u32): per stream and kind, wraps |

Envelope v1 (big-endian, magic `'O','3','D','A'`, 20 bytes) is not accepted.

- A reader passes kind and codec through; each consumer checks the pair it handles (control:
  kind Control with codec O3DControl and a payload of 1 to 1,076 bytes), and a reader ignores
  a kind it does not know (ADR 0011).
- A control envelope, header included, is at most 1,100 bytes, so it is never fragmented
  (ADR 0011 item 4).
- **Audio payload** (inside an audio envelope, and alone on envelope-less channels such as
  MoQ's audio track): a little-endian header with channels, sample rate, source GUID, stream
  label and subject, then the samples. Version 1 is PCM16; version 2 names its codec (Opus).
  **PCM16 samples are little-endian.** The payload keeps its codec and timestamp because
  envelope-less channels read them from it (ADR 0009 item 4's payload v3 was not adopted).

## 3. UDP datagram

A datagram's first 4 bytes say what it is (`udpClassifyDatagram`):

| First bytes | Kind |
|---|---|
| `'O','3','D','F'` | a fragment (below) |
| `'O','3','D','U'` | an envelope (section 2) |
| byte 0 non-zero, bytes 1-3 zero | a mocap frame (section 1) |

The UDP receiver reassembles fragments and passes every other datagram on whole; the receiver's
demux and frame checks reject what is not an envelope or a frame.

**Fragment header v2**, 24 bytes: magic `'O','3','D','F'`, version 2, flags 0, two reserved zero
bytes, then message id, fragment index, total reassembled size and fragment payload size (u32
each). A message larger than `udp.maxdatagram` is fragmented. The 16-byte fragment header from
before protocol 2 (no magic) is not accepted.

## 4. TCP stream

Each message is framed as a 14-byte magic (`00 FF 03 FE "O3DS-START"`), a u32 payload length,
then the payload (a frame or an envelope). Unchanged by protocol 2: everything it carries has
its own version. When the sender is idle it sends a keepalive: an envelope header of kind Audio
with a zero payload size.

## 5. Capture files

`.o3dscap` (`src/o3ds/capture.h`) is little-endian and versioned (format version 1); each record
holds the verbatim wire bytes of one frame and its receive time.

## 6. Clocks

| Field | Clock | Comparable across hosts? |
|---|---|---|
| `SubjectList.time` (seconds) | sender clock: `FPlatformTime::Seconds()` when the pose was sampled (ADR 0008) | no |
| envelope `timestamp_us` | the same sender clock, in microseconds | no |
| `SubjectList.tx_wallclock_us` | UTC at transmit (`O3DS::NowUtcMicros`); can step under NTP | roughly: latency and clock-offset estimates only |
| capture `recv_wallclock_us` | UTC at receive | roughly |

Receivers map sender time to their own with the clock-offset estimator; frames are ordered by
`tx_seq` only, never by a clock.

Senders stamp `tx_seq`, `tx_wallclock_us` and `frame_epoch` through `O3DS::StreamWriter`
(`src/o3ds/stream_writer.h`), one per stream. A receiver keys streams by the subject names a
frame carries, so the UE sender, which writes one subject per frame, keeps one writer per
subject. Epochs are strictly increasing within a process, across writers (ADR 0005 (iv)).

## 7. Name hashes

Topology and curve-name hashes (`O3DS::Wire::HashNames`, `HashParents`) are 64-bit FNV-1a over
the name count (u32), then each name's UTF-8 length (u32) and bytes, case preserved; parents
follow as their count and each index (i32). They are local (change detection) and never on the
wire. A topology hash that ever goes on the wire must use this definition and bump the protocol.

## 8. Changing the wire format

| Change | `O3DS_PROTOCOL_VERSION` | `min_reader_version` on affected frames | `O3DS_VERSION_TAG` |
|---|---|---|---|
| Bug fix, no wire change | no | no | patch |
| Appended field or enum value that old readers may safely ignore | +1 | unchanged | minor |
| New semantics an old reader would misapply | +1 | the new version, on frames that use it | minor, or major if **default** settings produce such frames |
| Envelope, fragment or audio header layout change | +1 | n/a (new magic or version byte) | minor |
| Dropping support for an old frame or envelope version | no | n/a | major |

- **Until a protocol version is first released, wire changes join it** instead of adding
  another: protocol 2 is the release that ships ADR 0005 (ADR 0009 item 2), so
  `SubjectUpdate.ref_seq` is part of protocol 2. The table applies once a version has shipped.
- The schema is append-only: never reorder, remove or retype a field. Regenerate
  `src/o3ds_generated.h` with the pinned `flatc` (`flatc --cpp -o src src/o3ds.fbs`) and sync
  the plugin's core mirror (`python Build/Scripts/sync_o3ds_core.py`); CI checks both.
- **Compatibility window:** none for the formats before protocol 2, which nothing deployed
  uses. From the first release on, readers keep accepting an older frame or envelope version
  for at least two minor releases after a newer one ships; dropping one after that is a major
  bump.
- Every wire change adds an entry to the `### Schema/Protocol` section of the root
  [`CHANGELOG.md`](../CHANGELOG.md) stating the protocol version, the `min_reader_version` of
  affected frames, compatibility in both directions (old reader and new writer, new reader and
  old writer), and the migration steps. The review checklist blocks a wire change without one
  (roadmap §7 item 8).
- **Tests:** `core.wire_format_tests` covers the stamp of every serialize path, reader
  acceptance and rejection, and the envelope and hash codecs; a wire change adds cases there.
