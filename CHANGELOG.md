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

- New `src/o3ds/tcp_stream_parser.h`: `TcpStreamParser`, the TCP stream
  frame parser the UE Sockets receiver uses, moved out of the socket code so
  it runs under CTest and the fuzz harness (WP-S6, ADR 0006). New tests in
  `test/tcp_stream_parser_tests.cpp` and a `tcp_stream` fuzz target.

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

### Changed

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

### Build and CI

- `Build/Scripts/Build-Plugin.ps1` now fails when `RunUAT BuildPlugin` fails, with UAT's exit code (CI-1).
  It used to fall back to a ProjectSandbox UBT build and exit 0, so plugin CI
  could report success for a plugin that does not build. The fallback is still
  available for local troubleshooting behind the new `-AllowFallback` switch,
  which no workflow passes.

### Schema/Protocol

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
