# 0009: Protocol versioning, byte order and wire compatibility

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** every open question below was accepted with the recommended default given next to it. Needs-verification items stay open and are resolved in the implementing WPs; a result that invalidates a default is handled by a superseding ADR.
- **Date:** 2026-09-29
- **Plan decision:** D8 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0005](0005-wire-resync-and-loss-contract.md) (depends on this ADR for old-reader rejection; `ref_seq`), [ADR 0003](0003-core-library-delivery-to-plugin.md) (mirror and flatc checks, `SYNC_STAMP.txt`), [ADR 0007](0007-transport-abstraction-and-registry.md) (shared receive demux), [ADR 0008](0008-sender-pipeline-threading.md) (sender clock); [`resilient-streaming-and-motion-prediction.md`](../roadmap/resilient-streaming-and-motion-prediction.md) §0 and §2; `.github/copilot-instructions.md` §3; feeds WP-A4, WP-S2, WP-S10, WP-T1, WP-D3

**Recommendation in one line:** reuse the 4-byte word that already starts every O3DS frame as the **minimum reader protocol version**. Every deployed reader rejects any value other than 1 (`src/o3ds/model.cpp:987`), so new writers stamp 2 on residual and quantized frames and old readers drop them cleanly, while plain frames stay readable. Add a `protocol_version` field (what the writer speaks) and the FlatBuffers `file_identifier "O3DS"` (checked only on version-2 frames). Everything on the wire is little-endian. The audio envelope gets a v2 with its own magic, and UDP fragments get a magic and version. There is one CHANGELOG with a mandatory "Schema/Protocol" section, and a CI matrix tests old and new readers against old and new writers.

## Context

**Frame header.** `finalize` writes a 4-byte `flags` word (always 1, host byte order), a 4-byte CRC-32 (host byte order) and the FlatBuffer (`src/o3ds/model.cpp:859-877`). All six serialize paths pass 1 (`:778`, `:801`, `:828`, `:854`, `:908`, `:936`). `SubjectList::Parse` reads both words by type-punning and **rejects any `flags != 0x0001`** with "Invalid data structure" (`:984-990`), then checks the CRC and runs the Verifier (`:992-1006`). `PeekMeta` skips both checks (`:941-966`; CORE-15). The UE receiver calls `PeekMeta` and then `Parse` (`Plugin/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:577`, `:972`). CORE-22 notes the host-endian, unaligned reads.

**Schema (CORE-16).** `src/o3ds.fbs` has no `file_identifier` (`:213` is `root_type SubjectList;`). The generated verifier passes `nullptr` as the identifier (`src/o3ds_generated.h:1380-1382`). The C2 residual fields (`:159-160`) change what the existing `translations`/`rotation`/`curves` vectors mean, so a pre-C2 reader applies residuals as absolute values. The D1 quantized vectors (`:188-195`) are invisible to old readers, which freeze those channels. ADR 0005 adds `SubjectUpdate.ref_seq` and changes anchor semantics, so D1-era readers misdecode after the first periodic resync. Fields are appended in order and the append-only rule holds (resilient-streaming §0).

**FlatBuffers identifier behaviour, read from the vendored 2.0.6 headers** (`Plugin/ThirdParty/flatbuffers/include/flatbuffers/base.h:141-143`):
- `Finish(root, file_identifier)` writes the four bytes **after** the root offset (`flatbuffer_builder.h:1097-1111`), so the root offset still points at a valid table.
- `Verifier::VerifyBufferFromStart` checks the identifier only when one is passed (`verifier.h:175-179`).
- Therefore: **an old reader accepts a buffer that carries an identifier**, because it passes `nullptr`. **A new reader using the regenerated `VerifySubjectListBuffer` rejects an old buffer**, because flatc emits the identifier check once the schema declares one. FlatBuffers' own docs say the identifier is optional and compatible with buffers that lack it (References).
- `model.cpp` calls `builder.Finish(root)` directly (`:776`, `:799`, `:826`, `:852`, `:906`, `:934`), so declaring the identifier alone would not write it.

**Envelope and audio (SHR-7, SHR-30).**
- The unified envelope is **big-endian**: magic `0x4F334441` ("O3DA"), version, kind, codec, flags, `uint64` µs timestamp, `uint32` size (`Plugin/Source/Open3DShared/Public/O3DUnifiedMessage.h:24-51`, `:85-118`).
- The version is written as a literal 1 (`:141`) and never checked. Kind and codec are not validated.
- The timestamp is `static_cast<uint64>(TimestampSec * 1e6)`, which is UB for negative or NaN input (`:133`).
- Only audio is wrapped (`Open3DShared/Private/O3DAudioFrameCodec.cpp:302` is the one caller). Mocap goes raw, and receivers treat anything that is not a valid envelope as mocap (`SocketsUdpReceiver.cpp:423-435`, `SocketsTcpReceiver.cpp:439-451`).
- The inner audio header is **little-endian** (`O3DAudioSerialization.cpp:17-29`). It repeats the codec and timestamp, and it has its own version byte (1 = PCM16, 2 = encoded) which readers do check exactly (`:155-156`, `:308-309`).
- PCM16 samples are `memcpy`'d in host order (`O3DAudioFrameCodec.cpp:193-194`, `:256`).

**Other framing.**
- The UDP fragment header is four host-endian `uint32` (id, index, total, fragment size) with no magic (`src/o3ds/udp_fragment.cpp:59-67`). The receiver classifies datagrams heuristically (`SocketsUdpReceiver.cpp:348-387`; TRB-17).
- The TCP stream uses a 14-byte magic plus a little-endian length (`Open3DTransportSockets/Private/Shared/SocketsTcpTransport.h:8-27`).
- The capture container is little-endian and versioned (`src/o3ds/capture.h:36-55`).

**Versions and changelogs.** `O3DS_VERSION_TAG` is "1.0.4" (`CMakeLists.txt:13`) after `tx_seq`, residual and quantization were added. `windows.yml` passes `-DVERSION_TAG=...`, which CMake never reads (`.github/workflows/windows.yml:91`). `CHANGELOG.md` and `docs/CHANGELOG.md` both read "Nothing yet" (DOC-8). The repository has no git tags, and D1/C2 exist only on `develop` (ADR 0005, accepted Q2).

**Name hashing (SHR-33).** `HashNames` feeds each name's TCHAR bytes into FNV-1a with no length or count (`Open3DShared/Private/O3DHelpers.cpp:164-173`). Its uses are local change detection (`O3DSenderComponent.cpp:820`, `O3DReceiverSource.cpp:145`, `:150`), not wire fields.

## Decision drivers

1. An old reader must never *apply* data it would misinterpret (CORE-16, ADR 0005 Consequences).
2. Default-configuration streams (legacy full snapshots) keep working with already-deployed readers.
3. One rule per concern: one byte order, one place for each version number, one changelog.
4. Rejection happens before CRC and Verifier work, so it is cheap and safe on hostile input (T1).
5. Core-first: framing constants and codecs live in `src/o3ds` and are tested with CTest.

## Options considered

### How old readers are made to reject new semantics
- **V1. Stamp the existing frame word (chosen).** Deployed readers already reject any value other than 1. No reader change is needed to get the rejection.
- **V2. `file_identifier` only.** Old readers ignore it (Context), so it cannot cause rejection. Useful only as a type check for new readers.
- **V3. Move residual and quantized values into new vectors (CORE-16's alternative).** Old readers would see "no update" and freeze. That is safer than garbage but still silently wrong, and it breaks the C2 and D1 layouts. Rejected.
- **V4. Capability negotiation.** Needs a back-channel that UDP, NNG pub/sub and MoQ relays lack (ADR 0005 L2 deferred). Rejected.

### Where the protocol version lives
- **P1. Frame word only.** Cheap, but a reader cannot tell what a newer writer speaks when the frame is compatible.
- **P2. Schema field only.** Old readers never look at it; they cannot reject on it.
- **P3. Both, with different meanings (chosen).** The frame word is the **minimum reader version** needed to apply the frame. The schema field is the **writer's** version, for diagnostics and captures.

### Byte order
- **B1. Little-endian everywhere, new envelope version with its own magic (chosen).** Matches the core header on every shipped host, FlatBuffers, the audio header, TCP framing and the capture format. Only the envelope changes.
- **B2. Big-endian everywhere.** Changes the core header, the audio header and TCP framing. Rejected.
- **B3. Document the mix.** Fails driver 3 and leaves SHR-7 open. Rejected.

## Decision

**1. Frame word = minimum reader protocol version.** The first 4 bytes of an O3DS frame are, little-endian:

| Byte | Meaning |
|---|---|
| 0 | `min_reader_version`: 1 or 2 today |
| 1 | flags, reserved, must be 0 (a later version may define "CRC omitted", CORE-7) |
| 2-3 | reserved, 0 |

The CRC-32 that follows is little-endian. All reads and writes use `memcpy` plus explicit LE conversion (CORE-22). On today's little-endian hosts, version 1 has the same bytes as before.

**2. Protocol versions.** `O3DS_PROTOCOL_VERSION` (in a new `src/o3ds/wire_format.h`) becomes **2** in the release that ships ADR 0005.
- **Writers** stamp `min_reader_version = 2` on any frame that contains an update with `predictor_id != 0` or any `*_q8`/`*_q16` vector. Otherwise they stamp 1: legacy full snapshots and non-quantized, non-residual deltas. Deltas carry `ref_seq`, which old readers ignore, and they behave for old readers exactly as they do today.
- **Readers** accept `1 <= min_reader_version <= O3DS_PROTOCOL_VERSION` and reject anything higher with "sender requires protocol N; update this receiver" (logged once per stream). This makes the next breaking change safe as well.
- **Pre-D8 develop writers.** A new reader **rejects** a version-1 frame that contains residual or quantized content (the question ADR 0005 (ix) left to D8). Such frames come only from unreleased `develop` builds whose anchor and resync semantics differ from ADR 0005.
- `PeekMeta` applies the same frame-word check and the CRC before returning metadata (with CORE-15).
- The sender component's UI states that residual and quantized streams need protocol-2 receivers.

**3. Schema additions (append-only).**
```
file_identifier "O3DS";          // written by every new writer
table SubjectList {
  ... existing fields, then ADR 0005 changes ...
  // Appended (D8, ADR 0009): protocol version the writer implements.
  // 0 = written before D8.
  protocol_version:ushort = 0;
}
```
- Writers call `FinishSubjectListBuffer` (identifier included) instead of `builder.Finish(root)` at the six sites above.
- Readers verify with the identifier when `min_reader_version >= 2`, and with `nullptr` when it is 1, so old buffers keep parsing.
- Regenerate `src/o3ds_generated.h` with `flatc --cpp`, sync with `sync_o3ds_core.py`; the ADR 0003 CI checks catch drift.

**4. Audio envelope v2 (SHR-7, SHR-30).** New magic bytes `'O','3','D','U'` (a byte string, not an integer), little-endian, 24 bytes:

| Offset | Field |
|---|---|
| 0-3 | magic `O3DU` |
| 4 | envelope version = 2 |
| 5 | kind (validated: Mocap, Audio) |
| 6 | codec (validated: O3DS, Opus, PCM16) |
| 7 | flags, 0 |
| 8-15 | `timestamp_us` (u64, sender clock, item 7) |
| 16-19 | `payload_size` (u32) |
| 20-23 | `seq` (u32, per stream and kind, wraps) |

- Inside it, audio payload **v3** drops the duplicated codec and timestamp; channels, sample rate, GUID, label and subject stay.
- PCM16 samples are **little-endian**, converted explicitly (a no-op on LE hosts; `#if !PLATFORM_LITTLE_ENDIAN` swap).
- `timestamp_us` is written only from finite, non-negative values; the writer clamps and the reader rejects out-of-range values.
- Zero-length payloads are allowed for a future control kind.
- Old readers reject `O3DU` by magic, fall through to the raw-mocap path, and `Parse` rejects the frame word, so audio is dropped rather than misplayed.
- New readers accept envelope v1 (`O3DA`, big-endian) and v2 for the compatibility window (item 9). Writers emit v2 only.
- The codec lives in `src/o3ds/wire_format.{h,cpp}`; `O3DUnifiedMessage.h` becomes a thin UE wrapper used by ADR 0007's `FO3DUnifiedReceiveDemux`.

**5. UDP fragment header v2 (TRB-17).** 24 bytes, little-endian: magic `'O','3','D','F'`, version 2, flags (0), 2 reserved bytes, then `msg_id`, `index`, `total_size` and `frag_size` as u32. Classification of a datagram becomes deterministic from its first 4 bytes:
- `O3DF`: fragment;
- `O3DU` or `O3DA`: envelope;
- first byte 1 or 2 with bytes 1-3 zero: raw frame;
- anything else: dropped and counted.

Legacy magic-less fragments are rejected by default (Q3). This lands as the follow-up PR WP-S2 already plans.

**6. TCP framing** is already little-endian, and every payload it carries has its own version. It is unchanged.

**7. Clock domains (SHR-30, SND-17)**, documented in `docs/wire-format.md`:

| Field | Clock | Comparable across hosts? |
|---|---|---|
| `SubjectList.time` (s) | sender clock: `FPlatformTime::Seconds()` at **sampling** (ADR 0008) | no |
| envelope `timestamp_us` | same sender clock, in µs | no |
| `tx_wallclock_us` | UTC at transmit (`system_clock`), may step under NTP | yes, approximately; latency and clock offset only |
| capture `recv_wallclock_us` | UTC at receive | yes, approximately |

Receivers map sender time to local time with the A2 clock-offset estimator. Ordering uses only `tx_seq`.

**8. Name hashing (SHR-33).** One definition, in core as `o3ds::HashNames` (`src/o3ds/wire_format.h`):
- 64-bit FNV-1a over the element count (u32 LE), then for each name its UTF-8 length (u32 LE) and its exact UTF-8 bytes. Case is preserved: a case change is a real change on the wire.
- The UE `O3DHelpers::HashNames` calls it.
- It is local today. If a topology hash ever goes on the wire, it must use this definition and bump the protocol version.

**9. Bump rules.**

| Change | `O3DS_PROTOCOL_VERSION` | `min_reader_version` on affected frames | `O3DS_VERSION_TAG` |
|---|---|---|---|
| Bug fix, no wire change | no | no | patch |
| Appended field or enum value that old readers may safely ignore | +1 | unchanged | minor |
| New semantics an old reader would misapply | +1 | set to the new version on frames that use it | minor, or major if **default** settings produce such frames |
| Envelope, fragment or audio header layout change | +1 | n/a; new magic or version byte | minor |
| Removing support for an old frame version | no | n/a | major |

The D8 release is protocol **2** and `O3DS_VERSION_TAG` **1.1.0**: default streams stay readable. `O3DS_VERSION_TAG` becomes overridable (`-DO3DS_VERSION_TAG=`), `windows.yml` passes that name, and `SYNC_STAMP.txt` (ADR 0003) records both numbers. **Compatibility window:** new readers accept envelope v1 and frame version 1 for at least two minor releases. After that, dropping them is a major bump.

**10. CHANGELOG policy (DOC-8).**
- One file, the repository-root `CHANGELOG.md` (the name `.github/copilot-instructions.md` §3 uses). `docs/CHANGELOG.md` is deleted.
- The WP-F8 Fab packaging job copies it into the staged plugin root (whether a plugin-root `.md` needs a `FilterPlugin.ini` entry is **needs-verification**, Q4).
- Format: one `## [x.y.z] - date` section per release, with `### Schema/Protocol`, `### Plugin`, `### Core` and `### Transports` subsections.
- Every Schema/Protocol entry states: the protocol version; the `min_reader_version` of affected frames; compatibility in both directions; migration steps.
- The review checklist (§7 item 8) blocks wire changes without one.

**11. Compatibility test matrix (WP-A4 acceptance).**

| Writer, reader | Frames | Expected |
|---|---|---|
| baseline, new (golden fixtures) | legacy full, legacy delta | parsed identically to baseline |
| baseline, new (golden fixtures) | D1 quantized, C2 residual (version 1) | rejected, counted, logged once |
| new, baseline (baseline reader built in CI) | legacy full, legacy delta | parsed; identifier ignored |
| new, baseline | quantized, residual (version 2) | rejected with "Invalid data structure" |
| new, new | all modes, plus `min_reader_version = 3` | modes round-trip; version 3 rejected |
| envelope v1 and v2, new demux | audio PCM16 and Opus | both decoded; v2 PCM is LE |
| envelope v2, baseline demux | audio | dropped, no crash |
| fragment v2 and legacy, new UDP receiver | mixed | v2 reassembled; legacy rejected by default |

- The baseline is `develop@7aea235`, the plan's baseline.
- Golden frames are written once with the baseline writer into `test/fixtures/wire/v1/*.o3dscap`, using the existing capture format.
- The "baseline reader" is a small `test/compat/old_reader.cpp` compiled in `core-tests.yml` against a `git worktree` of the baseline commit.
- Core rows run in CTest. Envelope and fragment rows run in CTest once the codec moves to `wire_format.*`. The UE demux rows run in the 0006 test module.

## Consequences

- **Easier:** ADR 0005 can ship; residual and quantized streams can never be misapplied by an older receiver; future breaking changes have a rule and a slot; all wire constants live in one core header with CTest coverage.
- **Harder:** audio between a new sender and an old receiver stops, cleanly, until both are updated. The CHANGELOG entry says so. Two envelope versions are parsed during the window.
- **Constrains:** every future schema PR fills in the bump table and the CHANGELOG. WP-A1's demux implements item 5's classification. The resilient-streaming phases that add fields must declare whether old readers may ignore them.
- **Refines** `resilient-streaming-and-motion-prediction.md` §2.4: a `predictor_version` mismatch is covered by `min_reader_version` for anything an old reader would misapply.

## Implementation outline

1. **WP-A4-D8a (core, lands with ADR 0005 step 1):** `src/o3ds/wire_format.{h,cpp}` (constants, LE helpers, frame word, `HashNames`); `finalize` and `Parse`/`PeekMeta` use them; `file_identifier`, `protocol_version`, `FinishSubjectListBuffer`; regenerate `src/o3ds_generated.h`; sync the mirror; `CMakeLists.txt:13` to 1.1.0 and overridable; fix `windows.yml:91`. CTest: golden fixtures and the old-reader job.
2. **WP-S2 follow-up:** fragment header v2 in `src/o3ds/udp_fragment.*` and `SocketsUdpReceiver.cpp` classification.
3. **WP-S10 / WP-A1:** envelope v2 and audio payload v3 codec in `wire_format.*`; `O3DUnifiedMessage.h` and `O3DAudioSerialization.cpp` wrap it; LE PCM; demux accepts v1 and v2.
4. **WP-D3:** merge the CHANGELOGs, write `docs/wire-format.md` (headers, clocks, bump table), and the first Schema/Protocol entry.
5. **WP-F8:** copy `CHANGELOG.md` into the Fab package.

## Verification / acceptance

- Every row of the matrix in item 11 passes in CI; a deliberately wrong `min_reader_version` on a quantized frame fails the old-reader job.
- `grep -rn "\*(std::uint32_t\*)\|reinterpret_cast<const uint32\*>" src/o3ds Plugin/Source` finds no header reads (CORE-22).
- A fuzz target (WP-T1) over `wire_format` classification finds nothing in 60 s.
- `CHANGELOG.md` has a 1.1.0 Schema/Protocol entry; `docs/CHANGELOG.md` is gone.

## Implementation notes (WP-A4, 2026-10-03)

Implemented in #335 (items 1-3, 9, 11), #336 (item 5), #337 (item 4), #338 (item 8)
and #339 (item 10). Where the implementation departs from the decision above,
and why:

- **Item 5, unknown UDP datagrams are passed on, not dropped.** Fragments are recognised only by
  their `O3DF` magic, as decided. Every other datagram is passed on whole, as on every other
  transport: the transports stay byte-opaque (the conformance and shared-block tests send
  arbitrary payloads through all of them), and the receiver's demux and frame checks already
  reject and count what is not an envelope or a frame. A frame word of any non-zero version
  classifies as a frame, so a newer protocol reaches the parser and is reported as such.
- **Item 4, the TCP keepalive stays envelope v1.** A receiver from before envelope v2 would read
  a v2 keepalive as a malformed frame and log it every few seconds. Readers accept both
  versions, so the keepalive moves to v2 when v1 support is dropped.
- **Item 4, audio payload v3 was not adopted.** MoQ's audio track carries the audio payload with
  no envelope (TRF-37) and reads the codec and timestamp from the payload, so the payload keeps
  them. PCM16 is little-endian, as decided (no bytes change on little-endian hosts).
- **Item 4, kind and codec are not validated by the envelope reader.** Each consumer checks the
  kind and codec pair it handles, and readers ignore a kind they do not know (ADR 0011, which
  added kind Control after this ADR).
- **Item 11, golden fixtures are raw frames**, not `.o3dscap` captures: `test/fixtures/wire/v1`
  holds one frame per file, written by `test/compat/compat_tool.cpp` built against the baseline.
- **Question 6** is confirmed: flatc 2.0.6 emits an identifier-checking `VerifySubjectListBuffer`
  and `FinishSubjectListBuffer`, so version-1 frames are verified with a null identifier.
- **Question 4** (packaging a plugin-root `CHANGELOG.md`) is still open: the Fab package does not
  carry the changelog yet (WP-F8).

## Open questions for the maintainer

1. Is it acceptable that audio from a new sender is dropped by old receivers (no dual-format writing)? **Default: yes**; D1 and C2 were never released and the plugin has no tagged release.
2. Keep new writers stamping version 1 on plain frames so old receivers still get mocap? **Default: yes.**
3. Reject legacy magic-less UDP fragments by default? **Default: yes** (T1); a `udp.acceptLegacyFragments` option is added only if a user needs it.
4. **needs-verification:** does BuildPlugin package a plugin-root `CHANGELOG.md` without a `FilterPlugin.ini` entry? Default: add the entry in WP-F1 if needed.
5. Version 1.1.0 for the D8 release, rather than 2.0.0? **Default: 1.1.0**, because default streams stay compatible.
6. **needs-verification:** flatc 2.0.6 emits `SubjectListIdentifier()`, an identifier-checking `VerifySubjectListBuffer` and `FinishSubjectListBuffer` with the identifier. This was read from FlatBuffers' runtime headers and docs, not from a flatc run. Default: the WP-A4 PR confirms it from the regenerated header.

## References

- Findings: CORE-16, SHR-7, SHR-30, SHR-33, TRB-17, CORE-22, DOC-8; context CORE-15, CORE-7, SND-17.
- Files: `src/o3ds.fbs`; `src/o3ds_generated.h`; `src/o3ds/{model.cpp,udp_fragment.cpp,capture.h,o3ds_version.cpp}`; `CMakeLists.txt`; `src/CMakeLists.txt`; `.github/workflows/windows.yml`; `Plugin/Source/Open3DShared/Public/O3DUnifiedMessage.h`; `Plugin/Source/Open3DShared/Private/{O3DAudioSerialization.cpp,O3DAudioFrameCodec.cpp,O3DHelpers.cpp}`; `Plugin/Source/Open3DTransportSockets/Private/{Receiver/SocketsUdpReceiver.cpp,Shared/SocketsTcpTransport.h}`; `Plugin/ThirdParty/flatbuffers/include/flatbuffers/{verifier.h,flatbuffer_builder.h,buffer.h,base.h}`; `CHANGELOG.md`, `docs/CHANGELOG.md`; `.github/copilot-instructions.md` §3.
- External (retrieved 2026-09-29, search snippet): FlatBuffers docs, "Evolution" and "Schema" pages. The identifier is optional, sits at bytes 4-7, and keeps buffers compatible with buffers that lack one: https://flatbuffers.dev/evolution/ , https://flatbuffers.dev/schema/
