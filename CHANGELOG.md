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
- Audio and FFI lifetime (WP-S5; design in the ADR 0007 addendum). Audio sinks no longer hold a reference to their sender. Each sender shares a small publish state with its sinks and closes a lifetime gate at the start of `Stop()`, so a sink kept alive by the capture component returns false instead of touching a destroyed sender, socket, LiveKit track or client (TRF-1, TRB-30, TRB-35). A sink created before a `Stop()` stays inactive after the next `Start()`.
- TCP, UDP, NNG and MoQ senders encode audio on the audio thread into sink-local buffers and hand the bytes to the transport worker through a lock-free queue. The audio thread no longer takes the socket lock, and UDP audio is sent from a new audio worker thread instead of the audio thread (TRB-10). The worker's wake event now lives as long as any sink that can trigger it (TRB-12).
- Each audio sink has its own encoder per stream label, created from the configuration at sink creation. Creating a sink no longer reconfigures an encoder that another thread is using (TRB-11, TRF-10). The WebRTC sink converts PCM in a per-call buffer with rounding instead of truncation (TRF-40).
- Receivers decode Opus with one decoder per `(SourceGuid, StreamLabel)`, so interleaved streams no longer corrupt each other (SHR-15).
- MoQ, NNG (sender) and WebRTC (sender) FFI callbacks receive an opaque token instead of an object address. A callback that arrives after its owner is gone does nothing (TRF-12).
- The receiver's audio sink holds a snapshot of the audio defaults instead of a back-reference to the LiveLink source, and a frame delivered off the game thread is forwarded to the game thread before the source is pinned (RCV-1). The WebRTC receiver reads its audio sink under a dedicated lock.
- The sender audio capture component hands its audio and capture threads an immutable parameter snapshot, and each producer has its own scratch buffer; they no longer read the component (SND-6). The submix listener is unregistered from the submix it was registered on, and registering it again first removes the old registration (SND-7).
- `FO3DAudioBus` is game-thread-only and checks it; publishing with no listener returns early (SHR-10).

### Changed

- The largest reassembled UDP message the receiver accepts drops from 50 MiB to 4 MiB by default; set the new `udp.maxframe` receiver option to raise it (up to 50 MiB).
- Core API: `UdpMapper::addFragment` now takes `(sourceKey, data, size, nowMs)`, and `UdpMapper` takes an optional `UdpReassemblyConfig`.

### Schema/Protocol

- No wire change. The 16-byte UDP fragment header is now read and written as explicit little-endian, which is byte-for-byte identical to what existing little-endian senders and receivers produce. The versioned `O3DF` header (ADR 0009, TRB-17) follows separately.
