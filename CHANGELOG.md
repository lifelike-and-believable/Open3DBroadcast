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
