# Transport review: Sockets (TCP/UDP), Loopback, NNG

Summary (architecture and health):
1. Each module registers factories with `O3DTransport::Register{Sender,Receiver}` and per-transport config/widget customizations in `StartupModule`. Senders implement `IOpen3DSender`; receivers implement `IOpen3DReceiver`, and the game thread drives `Poll()` from `FO3DReceiverSource::Tick`.
2. TCP sender: server-mode listen socket, one client, an MPSC queue with a worker `FRunnable`, and a custom 14-byte-magic + LE length framing. TCP receiver: client-mode, non-blocking, reassembles frames in `Poll()` on the game thread.
3. UDP sender: sends synchronously from the caller thread and fragments through `src/o3ds/udp_fragment` only above `udp.maxdatagram` (64000 by default). UDP receiver drains `RecvFrom` on the game thread and reassembles through a global `UdpMapper`.
4. Loopback: a process-global, name-keyed channel map of MPSC `TQueue`s with atomic pending counts. NNG: pub/pair/push senders with a worker thread and MPSC queue; sub/pair/pull receivers call `nng_recv` NONBLOCK on the game thread. Mode and role parsing is in `NngHelpers.cpp`, and prebuilt NNG 1.3.0 is Win64-only.
5. All three wire formats reuse `O3DS::ParseUnifiedMessage` to demux audio from mocap. Audio sinks encode on the audio render thread (`O3DSenderAudioCaptureComponent.cpp:516`).
6. Overall health: **fair to poor for production**. The happy path works on localhost, and the tests cover only that path plus one queue-limit case.
7. Critical correctness bugs: the TCP receiver throws away coalesced bytes after every frame (frames lost and resync on every burst). UDP reassembly has no per-message memory bound or age-out, so one spoofed datagram can make the receiver allocate about 50 MB.
8. Lifetime and thread-safety are inconsistent. The Sockets senders protect sinks with an `OwnerGuard`, but the NNG and Loopback audio sinks hold raw `Owner&` references (use-after-free). The TCP and NNG queue byte counters use non-atomic read-modify-write across three threads. The NNG sender's `Socket` pointer is deleted and replaced from two threads with no lock.
9. There is heavy copy-paste. Three receivers carry the same demux/audio code, three senders carry the same audio-encode/queue/worker code, host:port parsing is duplicated, and there are 7 near-identical Slate panels. Much of this should move into `Open3DShared`. The Slate/editor code sits in Runtime modules under `WITH_EDITOR`.
10. Build flags and platforms: disabling sockets, sender or receiver through the env flags breaks compilation. NNG throws a `BuildException` on every non-Win64 platform while enabled by default. Docs and UI promise options that have no effect (audio.port, udp.mtu for payloads under 64 KB, "Pull (client dial)", NNG_OPT_SENDBUF, SENDTIMEO).

Severity counts: critical 2, high 12, medium 23, low 10 (total 47).

---

## Sockets: TCP

### TRB-1: TCP receiver discards every byte after the first complete frame in the buffer
- Category: bug
- Severity: critical
- Location: ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:316-334, :388-399
- Evidence: `ReadFramed` calls `Recv` for up to `Buffer.Num()-InOutBytesBuffered` bytes (256 KB or more). When one frame is complete it copies the payload out and then does `InOutBytesBuffered = 0; InOutExpectedPayloadSize = 0;` (line 397). Everything after that frame that was already read is dropped: the next frames, or part of one. TCP routinely coalesces several frames into one `Recv`, especially with mocap and audio interleaved at 50+ msg/s. The next read then starts mid-frame, which triggers the magic resync path and loses at least one more frame. `ReadFramed` also returns false whenever `Recv` returns 0 bytes (lines 330-333), even if the buffer already holds a complete frame, so buffered frames wait for more network data.
- Recommendation: Rewrite as a proper stream parser. Append received bytes to a ring or compacting buffer. Loop "parse header → if `Buffered >= Header+Payload` emit → `Memmove` the remainder (or advance a read offset)" without calling `Recv` again. Call `Recv` only when no complete frame is buffered. Add a unit test that writes 3 frames in one `Send` plus a split header across two sends.
- Effort: M
- Owner: coding
- Status: closed in #272

### TRB-2: TCP sender treats a partial or EWOULDBLOCK non-blocking send as a fatal error and drops the client
- Category: bug
- Severity: high
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:374-390, :533-547
- Evidence: The client socket is `SetNonBlocking(true)` (line 358). `SendFramed` does a single `InSocket->Send(Data, Size, BytesSent)` and returns false if `BytesSent != Size`. On false, `RunWorker` destroys the client ("TCP send failed, dropping client."). A momentarily full kernel send buffer (a slow receiver or a Wi-Fi hiccup) causes a disconnect. It can also leave a partially written frame on the wire before the RST. needs-UE-verification: that `FSocket::Send` returns false with `SE_EWOULDBLOCK` on a non-blocking socket.
- Recommendation: Loop on partial sends in the worker. On `SE_EWOULDBLOCK`, wait with `Socket->Wait(ESocketWaitConditions::WaitForWrite, timeout)` or retry after the `WakeEvent` timeout, keeping the remaining offset. Disconnect only on a real error or after a configurable stall timeout. Drop whole queued frames, never partial ones.
- Effort: M
- Owner: coding
- Status: closed in #272

### TRB-3: Queue byte accounting uses non-atomic read-modify-write across producers and consumer (TCP and NNG senders)
- Category: thread-safety
- Severity: high
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:516-518, :564-580; Open3DTransportNNG/Private/Sender/NngSender.cpp:523-525, :541-543, :573-592
- Evidence: `const uint64 Pending = QueueBytes.Load(); ... QueueBytes.Store(Pending + TotalSize);` runs on the producer threads (the game thread via `Send`/`SendSerialized` and the audio render thread via the sink). `QueueBytes.Store(Current > PayloadSize ? Current - PayloadSize : 0)` runs on the worker. Interleaved updates are lost. If the counter drifts upward, the queue looks permanently full and every frame is dropped until `Stop()`. If it drifts downward, the queue becomes unbounded. The NNG EAGAIN path (lines 542-543) re-derives from the stale `Current` and overwrites concurrent updates.
- Recommendation: Use `fetch_add` before enqueueing and roll back with `fetch_sub` on failure, and use `fetch_sub` on dequeue. Better, move the "bounded MPSC byte queue + worker" into one shared class in `Open3DShared` (see TRB-38) and fix it once.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-4: TCP receiver reconnect backoff never grows
- Category: bug
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:229, :251-261
- Evidence: `TickConnection` increments `ConnectBackoffAttempt` and then calls `ConnectToServer()`, which unconditionally sets `ConnectBackoffAttempt = 0;` (line 229). The exponential backoff formula therefore always evaluates to 0.5 s, and an unreachable sender gets a new socket plus a log line twice per second forever.
- Recommendation: Reset the attempt counter only on reaching `SCS_Connected` (as line 270 already does), and remove the reset in `ConnectToServer`.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-5: TCP receiver can stay stuck in the Connecting state forever
- Category: bug
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:264-278, :287-296
- Evidence: While `State == EState::Connecting` the only exits are `SCS_Connected` or `SCS_ConnectionError`. The data-timeout check (`ConnectionTimeoutSeconds`) runs only in the `Connected` state. A SYN black-holed by a firewall or a wrong subnet leaves the socket pending indefinitely, and the reconnect/backoff logic never runs. needs-UE-verification: that `GetConnectionState()` returns `SCS_NotConnected` for a pending non-blocking connect.
- Recommendation: Record the connect start time and treat `Now - ConnectStart > ConnectionTimeoutSeconds` in the Connecting state as an error. Tear down and back off.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-6: Idle sender makes the TCP receiver flap every 5 s, and the sender detects dead clients late
- Category: bug
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:279-296; Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:344-372
- Evidence: The receiver force-reconnects when no data arrives for `ConnectionTimeoutSeconds` (default 5). The sender sends only when frames exist and has no heartbeat, so a paused sender or one with no subjects causes endless reconnects. On the sender side `TickAcceptClient` returns early while `ClientSocket` is set (lines 349-351), and a dead client is noticed only when a send fails. A reconnecting receiver therefore sits in the listen backlog without being served until the old socket errors.
- Recommendation: Add a small keep-alive frame (for example a zero-length or unified "ping" kind) sent by the worker when idle for about 1 s. Detect peer close on the sender with a periodic zero-byte `Recv` or `Wait(WaitForRead)` check. Consider accepting new clients and replacing the stale one.
- Effort: M
- Owner: design
- Status: closed in #272

### TRB-7: TCP sender serves one client only; extra receivers hang connected but receive nothing
- Category: usability
- Severity: medium
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:300, :349-371
- Evidence: `Listen(8)` accepts up to 8 in the kernel backlog, but only one `ClientSocket` is ever served. Additional receivers complete the TCP handshake (kernel accept queue), report `SCS_Connected`, and then time out and flap (TRB-6). Nothing is documented or logged about this.
- Recommendation: Either support fan-out (a `TArray<FSocket*>` with a per-client send offset) or reject extra connections explicitly: accept them, log a warning, and close. Document the single-client limit in the UI tooltip.
- Effort: M
- Owner: design

### TRB-8: TCP receiver resync discards one byte per Poll and can stall on garbage
- Category: security
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:339-377
- Evidence: If no magic is found in the buffered bytes, the code removes exactly one byte and returns false (`Memmove(..., +1, ...)`). The same happens for an invalid length (line 372, "Skip this byte"). Each call costs an O(n) scan plus an O(n) memmove. Garbage that fills the 256 KB buffer drains at one byte per call, and `Poll` breaks on false, so effectively one byte per game frame. A malfunctioning or malicious peer can wedge the stream.
- Recommendation: When no magic is found, keep only the last `FrameMagicSize-1` bytes. On an invalid length, skip to the next magic occurrence. Keep scanning within the same `Poll`, bounded by work per tick.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-9: The TCP length field is trusted up to 50 MiB, with a grow-only buffer
- Category: security
- Severity: low
- Location: Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:22, :368-386
- Evidence: `MaxPayloadSizeBytes = 50 MiB`. Any valid header makes the receiver `SetNum` up to 50 MiB and wait for that much data, and the buffer is never shrunk (`DisconnectSocket` uses `Reset()`, which keeps capacity). Typical mocap frames are about 4-50 KB.
- Recommendation: Make the cap configurable (for example `tcp.maxframe`, default 4 MiB). Shrink the buffer after large frames. Grow incrementally as bytes arrive rather than allocating up front.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-10: The audio thread takes the socket lock and contends with the network worker and the game thread
- Category: thread-safety
- Severity: high
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:60-73, :315, :347, :523; Open3DTransportSockets/Private/Sender/SocketsUdpSender.cpp:36-49, :162, :217, :471-525
- Evidence: `FSocketsTcpSenderAudioSink::OnSubmitPcmInternal` holds `OwnerGuard->Lock` for the whole Opus/PCM encode and enqueue, and it runs on the audio render thread (`O3DSenderAudioCaptureComponent.cpp:516`). The TCP worker holds the same lock across `Socket->Send` (line 523), and the game thread takes it in `TickAcceptClient`/`DestroySocket`. On UDP, `Send`/`SendSerialized` (game thread) and the audio sink both hold the lock across encode and `SendTo`, including fragment loops. As a result the audio render thread can block on socket I/O (glitches), and the game thread can block on Opus encoding. This violates the "never block the game thread" rule.
- Recommendation: Split the concerns. Use a tiny lock or atomic shared pointer only for the "owner alive" check, do encoding lock-free on the audio thread into its own scratch, and hand bytes to an MPSC queue drained by a network worker (the same worker as mocap). Never hold a lock across a syscall that the game or audio thread also needs.
- Effort: M
- Owner: design
- Status: closed in #269

### TRB-11: Audio encoder state is reconfigured on the game thread while the audio thread uses it
- Category: thread-safety
- Severity: medium
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:249-266, :392-399; Open3DTransportSockets/Private/Sender/SocketsUdpSender.cpp:261-276; Open3DTransportNNG/Private/Sender/NngSender.cpp:645-678
- Evidence: `CreateAudioSink` writes `ActiveAudioConfig` and calls `RefreshAudioEncoder()` → `AudioEncoder.Initialize(...)` without taking `OwnerGuard->Lock` (TCP/UDP) or any lock (NNG). An existing sink may be encoding on the audio thread at the same moment. `UnifiedAudioScratch` is also shared.
- Recommendation: Take the same lock used by the sink path, or give each sink its own encoder instance (preferred, since the sink already carries its config).
- Effort: S
- Owner: coding
- Status: closed in #269

### TRB-12: TCP sender Stop() returns the WakeEvent to the pool while the audio thread may still trigger it
- Category: thread-safety
- Severity: medium
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:158-177, :582-585, :403-449
- Evidence: `Stop()` calls `ReturnSynchEventToPool(WakeEvent)` (line 170) before `DestroySocket()` nulls `ClientSocket` under the lock (line 174). In that window an audio-thread `ProcessCapturedAudio` still sees `ClientSocket != nullptr`, calls `EnqueuePayload`, and does `WakeEvent->Trigger()` on a pointer that is being returned or reused (a non-atomic read and use of a pooled event). The NNG sender has the same pattern (NngSender.cpp:246-250 vs :594-597).
- Recommendation: Set a "stopping" atomic first, take the owner lock to null the socket, drain the audio path, and only then return the event. Alternatively own the `FEvent` for the lifetime of the object.
- Effort: S
- Owner: coding
- Status: closed in #269

### TRB-13: TCP sender Start() leaks a running worker when bind or listen fails, and restart without Initialize fails silently
- Category: bug
- Severity: low
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:143-177
- Evidence: `Start()` calls `StartWorker()` before `CreateListenSocket()`, and on failure it returns false with the thread still running. `Stop()` sets `SocketSubsystem = nullptr`, so `Stop(); Start();` without re-`Initialize` always fails in `CreateListenSocket` (`!SocketSubsystem`) with no log. The TCP/UDP receivers and UDP sender behave the same way.
- Recommendation: Create the listen socket first and start the worker only on success. Do not null `SocketSubsystem` in `Stop()`, or re-fetch it in `Start()`.
- Effort: S
- Owner: coding
- Status: closed in #272

### TRB-14: TCP sender's 4 MB queue cap is hard-coded and the queue has no age limit, so latency builds up
- Category: performance
- Severity: medium
- Location: Open3DTransportSockets/Private/Sender/SocketsTcpSender.h:110; SocketsTcpSender.cpp:564-567
- Evidence: `uint64 MaxQueueBytes = 4 * 1024 * 1024;` has no config key. At about 5-50 KB per frame, 4 MB is 80-800 frames of backlog, which is seconds of stale mocap. Only new frames are dropped when the queue is full; the old ones are kept.
- Recommendation: Expose `tcp.maxqueue` (bytes and/or frames). For realtime use prefer drop-oldest for mocap (or "latest value wins" per subject), and keep audio in a separate small queue.
- Effort: S
- Owner: design
- Status: closed in #272

## Sockets: UDP

### TRB-15: UDP reassembly has no per-message memory bound or age-out, so one spoofed datagram can exhaust memory
- Category: security
- Severity: critical
- Location: src/o3ds/udp_fragment.cpp:110-135, :173-226; Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:316-346
- Evidence: `IsFragmentPacket` accepts any datagram of 16 bytes or more whose 16-byte header claims `TotalSize <= 50 MiB` and `FrameCount <= 4096`, with a matching slice length. `UdpCombiner::addFragment` then `malloc(bufSz)` (up to 50 MiB) per new `frameId`. `UdpMapper::items` is a `std::map` with no size cap and no timeout. Entries are erased only when a *higher-or-equal* id completes (`j->first <= i.first`), so ids above the legitimate sequence live forever. An attacker on the LAN sending one 1 KB datagram per id with `TotalSize=50MB` makes the receiver allocate 50 MB per packet (the default bind is 0.0.0.0, with no source filtering). A legitimate sender restart (the `MessageCounter` resets to 1) also leaks all incomplete combiners from the previous session.
- Recommendation: Bound the in-flight reassemblies (for example 8 messages) and the total bytes (for example 2x maxdatagram x fragments). Evict by age (for example 100 ms) and by LRU. Allocate lazily and cap `TotalSize` with a configurable `udp.maxframe` (default about 1-4 MiB). Key reassembly by (source addr, message id). Add a magic and version to the fragment header so fragments cannot be confused with raw payloads (TRB-17).
- Effort: M
- Owner: coding
- Status: closed in #264

### TRB-16: `udp.mtu` has no effect for payloads under 64000 bytes, so IP fragmentation is relied on
- Category: bug
- Severity: high
- Location: Open3DTransportSockets/Private/Sender/SocketsUdpSender.cpp:119-121, :396-404, :437-439; Shared/SocketsTransportUdpConfig.cpp:24-27
- Evidence: `SendPayload` sends as a single datagram whenever `Size <= MaxDatagramBytes` (default 64000). Application-level fragmentation at `MtuBytes` (default 1200) is used only for payloads over 64000. Typical frames of 4-50 KB therefore go out as one IP-fragmented datagram: losing any one fragment loses the frame, some networks and NAT/VPNs drop fragments, and the "MTU" setting in the UI (UdpSenderWidget.cpp:125-137) is effectively ignored.
- Recommendation: Fragment whenever `Size + header > MtuBytes`. Keep `MaxDatagramBytes` only as a hard ceiling. Rename or remove one of the two knobs to avoid confusion.
- Effort: S
- Owner: coding

### TRB-17: The fragment header has no magic or version, so non-fragment datagrams can be misclassified
- Category: bug
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:348-387; src/o3ds/udp_fragment.cpp:59-67
- Evidence: A datagram is treated as a fragment when bytes 4-15 happen to form a self-consistent `(seq,total,fragSize)` triple matching the datagram length. Unfragmented FlatBuffer or unified payloads are also accepted "raw", so the classification is heuristic. The header is written in host-endian `uint32_t` and read through `reinterpret_cast<const uint32*>` (unaligned access, strict-aliasing UB on some targets).
- Recommendation: Version the UDP framing. Prefix every datagram with a magic, version and flags byte (fragmented or whole), use explicit little- or big-endian `memcpy` reads, and put it behind a protocol version bump (per the CHANGELOG rules).
- Effort: M
- Owner: design
- Status: closed in #336

### TRB-18: UDP receiver Poll() is an unbounded loop on the game thread
- Category: security
- Severity: high
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:125-169
- Evidence: `while (true)` loops until `RecvFrom` returns EWOULDBLOCK. A sustained stream above the game's per-frame drain rate (a flood, broadcast storm or misconfigured sender) keeps the game thread in `Poll` indefinitely. The TCP and NNG receivers cap work at 16 frames per poll, but UDP does not.
- Recommendation: Cap datagrams and bytes per `Poll`. Better, move the receive loop to a dedicated socket thread (FRunnable with `Socket->Wait`) that pushes complete frames into a bounded SPSC queue drained by `Poll()`.
- Effort: M
- Owner: coding
- Status: closed in #264

### TRB-19: UDP receive path allocates and copies per datagram
- Category: performance
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:127, :146-147, :311-313, :329-342, :413-416
- Evidence: For every datagram, `CreateInternetAddr()` makes a shared allocation, `TArray<uint8> Frame` is new with a full memcpy (`ProcessDatagram`), fragments go through a `std::vector<char> Combined` copy and then another copy into `Frame`, and `ProcessReceivedPayload` copies again into `PayloadCopy` for `SubmitFrame`. That is 3-4 copies per frame. The TCP and NNG receivers have similar double copies (SocketsTcpReceiver.cpp:125, :429-432; NngReceiver.cpp:511-514).
- Recommendation: Reuse a member `FInternetAddr` and scratch arrays. Have the demux accept a `TArrayView` and build the consumer array once, or add a `SubmitFrame(TArray<uint8>&&)` overload on `ISerializedFrameConsumer` so the buffer can be moved in.
- Effort: S
- Owner: coding
- Status: closed in #264

### TRB-20: UDP sender does blocking-path work and syscalls on the game and audio threads
- Category: performance
- Severity: medium
- Location: Open3DTransportSockets/Private/Sender/SocketsUdpSender.cpp:160-209, :430-460
- Evidence: `Send`/`SendSerialized` serialize and then call `SendTo` synchronously while holding `OwnerGuard->Lock`. For fragmented frames this loops over up to thousands of `SendTo` calls, and `UdpFragmenter` `malloc`s and copies the whole payload every frame. On EWOULDBLOCK mid-message the remaining fragments are abandoned after partial fragments have already been sent, which wastes bandwidth.
- Recommendation: Use the same queued worker model as TCP/NNG (a shared implementation). Fragment directly from the source buffer without the intermediate `UdpFragmenter` copy (use a scatter-gather header + slice, or a reused scratch).
- Effort: M
- Owner: coding
- Status: closed in #318

### TRB-21: UDP has no multicast support, no source filtering, and receiver options that do nothing
- Category: usability
- Severity: medium
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:57, :215-227; Shared/SocketsTransportUdpReceiverWidget.cpp:117-121, :125-160
- Evidence: The receiver always creates an IPv4 socket, never calls a multicast join, and ignores the sender address returned by `RecvFrom`, so any host can inject frames. The receiver's `MtuBytes` is parsed (line 57) but never used. "Accept Broadcast Packets" calls `SetBroadcast` on a receiving socket, which does not affect reception. The widget still exposes both options.
- Recommendation: Add an optional `udp.multicast` group with TTL/loopback settings (needs-UE-verification: `FSocket::JoinMulticastGroup` signature in 5.7) and an optional allowed-source filter. Remove the MTU and broadcast controls from the receiver UI, or make them meaningful.
- Effort: M
- Owner: design

### TRB-22: SO_REUSEADDR on UDP receive and TCP listen sockets allows silent port sharing or hijack
- Category: security
- Severity: low
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:222; Sender/SocketsTcpSender.cpp:290; Sender/SocketsUdpSender.cpp:359
- Evidence: `SetReuseAddr(true)` on the UDP receiver lets a second process or receiver bind the same port without an error, and unicast delivery then goes to only one of them, which is confusing. On Windows, SO_REUSEADDR permits port stealing. needs-UE-verification: how `FSocketBSD::SetReuseAddr` maps on Win64 (SO_REUSEADDR vs SO_EXCLUSIVEADDRUSE). On the UDP *sender* the flag is meaningless.
- Recommendation: Default to off, and expose it as an explicit opt-in (needed only for multicast or multiple listeners).
- Effort: S
- Owner: review

### TRB-23: UDP receive buffer is sized from the receiver's own `udp.maxdatagram`, so config mismatch truncates
- Category: bug
- Severity: low
- Location: Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:120-123, :300-304
- Evidence: `ReceiveBuffer.SetNum(MaxDatagramBytes + 16)`. If the receiver's maxdatagram is lower than the sender's, `RecvFrom` truncates datagrams silently (OS-dependent). The truncated datagram then passes the size check and is parsed as garbage.
- Recommendation: Always size the receive buffer at 65535 (the maximum UDP payload), and use `udp.maxdatagram` only as a policy check.
- Effort: S
- Owner: coding
- Status: closed in #264

## Sockets: config, common, module

### TRB-24: Build flags are not honored; disabling sockets, sender or receiver breaks compilation
- Category: fab-readiness
- Severity: high
- Location: Open3DTransportSockets/Open3DTransportSockets.Build.cs:14-17, :33-41; Open3DTransportSockets/Private/Open3DTransportSocketsModule.cpp:1-15, :35-39; Open3DTransportLoopback/Open3DTransportLoopback.Build.cs:13-16; Open3DTransportNNG/Open3DTransportNNG.Build.cs:14-17
- Evidence: With `O3D_WITH_TRANSPORT_SOCKETS=0` (or NNG=0, or loopback without sender and receiver), each Build.cs `return`s before adding even `Core`, but the module is still listed in the .uplugin and its .cpp files are still compiled. None of the sources are wrapped in `#if O3D_WITH_TRANSPORT_SOCKETS`. The Sockets module adds `Open3DSender`/`Open3DReceiver` conditionally, yet `Open3DTransportSocketsModule.cpp` unconditionally includes `O3DSenderRegistry.h`/`O3DReceiverRegistry.h` and both sender and receiver classes. The result is compile failures for sender-only or receiver-only builds.
- Recommendation: Either wrap each module's sources in the matching `#if` defines (and keep `Core` deps), or drive module inclusion from the .uplugin or target instead of an early return. Add CI jobs for each flag combination.
- Effort: M
- Owner: coding
- Status: closed in #283

### TRB-25: Configured audio ports are dead config
- Category: code-quality
- Severity: medium
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportTcpConfig.cpp:25-38, :55-67; SocketsTransportUdpConfig.cpp:37-49, :79-91; SocketsTransportCommon.h:8-10; Private/Tests/SocketsAudioTests.cpp:111-114, :130-133, :149-155
- Evidence: `audio.port`, `audio.bind` and `audio.host` default to Port+1 and are written into `AdvancedParams`, but no transport reads them. Audio is multiplexed on the data socket through the unified header. The tests even allocate a separate "AudioPort" and assert that it differs from the data port. Users may open Port+1 in firewalls for nothing.
- Recommendation: Delete the audio port options, config code and test scaffolding, or implement a separate audio channel if that was the intent (a design decision).
- Effort: S
- Owner: design

### TRB-26: Port and host parsing is lax and does not resolve hostnames
- Category: bug
- Severity: medium
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportCommon.cpp:7-70, :130-139; SocketsTransportConfigCommon.cpp:5-14; Receiver/SocketsTcpReceiver.cpp:177, :193-211; Sender/SocketsUdpSender.cpp:303-334
- Evidence: `FCString::Atoi` accepts "80abc" and ports above 65535 (only `<= 0` is rejected). `[ipv6]:port` is parsed, but the TCP receiver creates an IPv4-only socket (`CreateSocket(NAME_Stream, ..., false)`) and the UDP receiver uses `FNetworkProtocolTypes::IPv4`. Hostnames like `mocap-pc.local` go through `SetIp` and then fall back to `FIPv4Address::Parse`, and nothing calls `GetAddressInfo`. needs-UE-verification: whether `FInternetAddr::SetIp(const TCHAR*)` resolves DNS names in 5.7; if it does, it blocks the game thread.
- Recommendation: Validate the port range 1-65535 with strict parsing (as `O3DNNG::ParseInt` does). Resolve hostnames asynchronously (`ISocketSubsystem::GetAddressInfoAsync`) during `Start`. Pick the protocol family from the resolved address.
- Effort: M
- Owner: coding
- Status: closed in #306

### TRB-27: Inconsistent transport identity and role fields
- Category: code-quality
- Severity: low
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportUdpConfig.cpp:11, :54; SocketsTransportTcpConfig.cpp:41-51; Private/Tests/SocketsAudioTests.cpp:107, :126
- Evidence: TCP config takes `TransportName` but UDP hard-codes `TEXT("UDP")`. The receiver configurators never set `Config.Role`. The tests use `Transport = "sockets.tcp"`, which differs from the registered name "TCP".
- Recommendation: Pass the registered name everywhere, set Role on the receiver, and align the tests with the registry names.
- Effort: S
- Owner: coding
- Status: closed in #313

### TRB-28: Default ports and addresses are hard-coded in several places
- Category: code-quality
- Severity: low
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportConfig.h:11-12; SocketsTransportTcpConfig.cpp:15, :47; SocketsTransportUdpConfig.cpp:15, :24-27, :57, :66-69; Open3DTransportNNG/Private/Shared/NngHelpers.cpp:388-407; Open3DTransportNNG/Private/Open3DTransportNNGModule.cpp:36-55; all *Widget.cpp files
- Evidence: 17700 and 17800 (sockets) and 6000, 7000 and 8000 (NNG, duplicated in two files) are defined as constants, and "0.0.0.0", "127.0.0.1", 1200 and 64000 are repeated literally in the config code and all widgets. The project rules prohibit hard-coded ports; defaults are acceptable but belong in one place and should be overridable from project settings.
- Recommendation: Put the defaults in a `UDeveloperSettings` (Project Settings > Open3DBroadcast > Transports) with a single constants header as fallback, and have the widgets read from it.
- Effort: M
- Owner: design

### TRB-29: TCP/UDP bind to 0.0.0.0 by default with no authentication
- Category: security
- Severity: medium
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportTcpConfig.cpp:15; SocketsTransportUdpConfig.cpp:57; Open3DTransportNNG/Private/Open3DTransportNNGModule.cpp:52-55
- Evidence: The TCP sender (server) listens on all interfaces, and so do the UDP receiver and NNG listeners. Any host on the network can read the mocap/audio stream (TCP/NNG pub) or inject frames and audio (UDP, NNG pull/pair). No token, TLS or allow-list exists for these transports.
- Recommendation: Default the listen address to 127.0.0.1, or to a user-chosen interface shown in the UI with a warning. Document the security posture. Consider an optional shared-secret HMAC in the unified header, or NNG TLS.
- Effort: M
- Owner: design

## Loopback

### TRB-30: Loopback audio sink holds a raw reference to the sender (use-after-free)
- Category: thread-safety
- Severity: high
- Location: Open3DTransportLoopback/Private/Sender/LoopbackSender.cpp:14-19, :52, :94, :247-250
- Evidence: `FLoopbackSenderAudioSink` stores `FO3DLoopbackSender& Owner` and calls `Owner.EncodeAudioFrame(...)` from the audio render thread. The capture component keeps its own `TSharedPtr` to the sink (O3DSenderAudioCaptureComponent.cpp:98-113), so the sink outlives the sender. The only guard is the weak `Channel`, which stays alive while the receiver holds it. The Sockets senders fixed exactly this with `OwnerGuard`.
- Recommendation: Move the encoder into the sink (it only needs the config and channel), or reuse a shared "owner guard" helper from `Open3DShared`.
- Effort: S
- Owner: coding
- Status: closed in #269

### TRB-31: Receiver silently overwrites the sender's queue capacity on the shared channel
- Category: bug
- Severity: medium
- Location: Open3DTransportLoopback/Private/Shared/LoopbackChannel.cpp:94-100; Open3DTransportLoopback/Private/Open3DTransportLoopbackModule.cpp:279-295, :329; Shared/LoopbackChannel.h:49-50
- Evidence: `AcquireChannel` assigns `Existing->Capacity`/`AudioCapacity` from whoever calls last. The receiver's `ConfigureTransport` never adds `loopback.maxqueue`, so the receiver always passes the default 64 and overrides the sender's UI "Queue Capacity" (up to 4096). `Capacity` is a plain `int32` read concurrently from the audio thread.
- Recommendation: Make capacity sender-owned (set only when the role is sender, or take the max), and store it as `std::atomic<int32>`.
- Effort: S
- Owner: coding

### TRB-32: Loopback lifecycle and diagnostics issues
- Category: code-quality
- Severity: low
- Location: Open3DTransportLoopback/Private/Sender/LoopbackSender.cpp:39-41, :137-140, :149, :162, :182; Receiver/LoopbackReceiver.cpp:33-35, :46-50, :101; Shared/LoopbackChannel.cpp:13
- Evidence:
  - `Stop()` on the sender is a no-op, so `Send` still enqueues after Stop. The receiver's `Stop` keeps `Channel` and `Poll` keeps dequeuing.
  - The check-then-enqueue on `PendingCount` is not atomic, so it can overshoot.
  - `Send` allocates a `std::vector` per frame.
  - The queue-full warning is compiled out under `WITH_DEV_AUTOMATION_TESTS`, which is set in normal editor Development builds, so users never see it.
  - A function-`static double LastAudioLogTime` is shared across instances.
  - `GLoopbackChannels` never prunes expired weak entries.
  - Only this receiver falls back to `FSerializedFrameConsumerRegistry::Create()` and holds a strong Consumer, whereas the Sockets and NNG receivers hold weak ones.
- Recommendation: Implement Stop (release the channel and reject sends). Use `fetch_add` with rollback for capacity. Use a member scratch buffer and member log timestamps. Gate the log with a CVar instead of the test macro. Prune expired channels in `AcquireChannel`. Make consumer ownership consistent across transports.
- Effort: S
- Owner: coding

## NNG

### TRB-33: NNG sender Socket pointer is deleted and replaced from two threads with no lock
- Category: thread-safety
- Severity: high
- Location: Open3DTransportNNG/Private/Sender/NngSender.cpp:342-369, :377-481, :527-535, :612-634
- Evidence: The worker thread reads `Socket` (line 527) and calls `nng_send`. On error, `HandleSendError` (worker) calls `CloseSocket()` → `delete Socket`. Meanwhile the game thread's `Tick` calls `OpenSocket()` (which itself calls `CloseSocket()` and then assigns `Socket = NewSocket`) when it sees `!Socket`. With no mutex the worker can dereference a freed `FNngSocketWrapper`, and two threads can double-delete. `BackoffAttempt` and `LastBackoffAttemptTime` are also written from the worker, the game thread and NNG pipe callback threads (lines 117-122).
- Recommendation: Keep socket lifetime on one thread (the worker owns open, close and reconnect). Alternatively guard `Socket` with `StateMutex` and use `TSharedPtr<FNngSocketWrapper, ThreadSafe>` snapshots. Note that NNG dialers already auto-reconnect (`nng_dial` with NONBLOCK), so the manual reconnect for push/pair-dial is probably unnecessary (needs NNG-doc verification: reconnect-time-min/max semantics in 1.3.0).
- Effort: M
- Owner: coding
- Status: closed in #277

### TRB-34: NNG sender requeues on EAGAIN, which reorders frames, busy-spins, and delivers stale backlog
- Category: bug
- Severity: high
- Location: Open3DTransportNNG/Private/Sender/NngSender.cpp:535-547; Open3DTransportNNG/README.md:55-60
- Evidence: On `NNG_EAGAIN` (no peer, or backpressure) the payload goes back to the *tail* of the MPSC queue, then `Sleep(0.001f)`. Newer frames already queued are sent before it, so frames are reordered. With no connected peer (push/pair-dial) the worker spins at 1 kHz dequeuing and re-enqueuing, and the queue fills with frames up to `MaxQueueBytes` (as high as 512 MB). On reconnect the receiver gets a burst of stale mocap. The README markets this as a feature ("Frames are re-queued ... rather than being dropped").
- Recommendation: For realtime data, drop the oldest data (or keep only the latest per subject) on EAGAIN and count it. Never re-enqueue at the tail. If retry is desired, hold a single "pending" payload in the worker and retry it first. Update the README.
- Effort: S
- Owner: design
- Status: closed in #277

### TRB-35: NNG audio sink holds a raw reference to the sender (use-after-free)
- Category: thread-safety
- Severity: high
- Location: Open3DTransportNNG/Private/Sender/NngSender.cpp:41-57, :661
- Evidence: `FNngSenderAudioSink` stores `FO3DNngSender& Owner` and calls `Owner.ProcessCapturedAudio` on the audio thread. The sink is a `TSharedPtr` that the capture component can hold after the sender is destroyed. This is the same defect as TRB-30, and the Sockets senders already fixed it.
- Recommendation: Extract the Sockets `OwnerGuard` pattern into a reusable `TO3DSinkOwnerGuard<T>` in `Open3DShared` or `Open3DSender`, and use it for NNG, Loopback and MoQ (MoQSenderAudioSink.h:31 has the same "must outlive" contract).
- Effort: S
- Owner: coding
- Status: closed in #269

### TRB-36: `NNG_OPT_SENDBUF` is set with the wrong type and units; `SENDTIMEO` has no effect
- Category: bug
- Severity: medium
- Location: Open3DTransportNNG/Private/Sender/NngSender.cpp:451-467, :535; Open3DTransportNNG/README.md:55-60
- Evidence: `nng_setopt_size(Socket, NNG_OPT_SENDBUF, Options.MaxQueueBytes)` passes a byte count as a `size_t`. In NNG, `send-buffer` is an `int` counting *messages*, and the valid range is small (0-8192 in NNG docs). The call likely fails with EBADTYPE or EINVAL, which is logged only at Verbose. `NNG_OPT_SENDTIMEO` = 30 s has no effect because every send uses `NNG_FLAG_NONBLOCK`. The README claims both behaviours. Needs NNG 1.3.0 doc verification.
- Recommendation: Use `nng_socket_set_int(NNG_OPT_SENDBUF, N messages)` and check its return value. Drop SENDTIMEO or document that sends are non-blocking. Fix the README.
- Effort: S
- Owner: coding
- Status: closed in #277

### TRB-37: NNG receiver passes the whole unified message (header included) to the consumer for mocap
- Category: bug
- Severity: medium
- Location: Open3DTransportNNG/Private/Receiver/NngReceiver.cpp:505-516
- Evidence: In the `EUnifiedKind::Mocap` branch the code copies `Data, Size` (the full message including the 20-byte unified header) instead of `PayloadPtr, PayloadSize`. The TCP and UDP receivers correctly use `PayloadPtr` (SocketsTcpReceiver.cpp:430-431). A unified-wrapped mocap frame therefore fails FlatBuffer parsing only on NNG. This is a symptom of the copy-pasted demux (TRB-38).
- Recommendation: Use `PayloadPtr/PayloadSize`, and move the demux into a single shared helper with a unit test that covers unified-mocap, unified-audio and legacy raw input.
- Effort: S
- Owner: coding
- Status: closed in #268

### TRB-38: Receiver demux and sender audio/queue code is copy-pasted across transports
- Category: architecture
- Severity: medium
- Location: SocketsTcpReceiver.cpp:405-492 / SocketsUdpReceiver.cpp:389-476 / NngReceiver.cpp:487-582 (ProcessReceivedPayload + ProcessAudioPayload); SocketsTcpSender.cpp:249-266, :392-449, :476-598 / SocketsUdpSender.cpp:261-276, :462-526 / NngSender.cpp:483-610, :645-747 (CreateAudioSink / RefreshAudioEncoder / ProcessCapturedAudio / SendEncodedAudio / worker + queue); SocketsTransportCommon.cpp:7-70 vs NngHelpers.cpp:46-109 (ParseHostPortInternal); SocketsTransportCommon.cpp:118-160 vs SocketsTransportConfigCommon.cpp:5-35 vs NngHelpers.cpp:281-291 (option getters); SocketsAudioTests.cpp:54-102 vs NngTransportTests.cpp:55-103 (test helpers)
- Evidence: The same logic appears 2-3 times, and has already diverged: TRB-37 (NNG passes the wrong pointer), audio stats double-counted only in NNG, `CreateAudioSink` clamps channels in TCP/NNG but not UDP, and a strict parser in NNG against `Atoi` in Sockets.
- Recommendation: Add to `Open3DShared`: an `FO3DUnifiedDemux` (a parse → mocap/audio callback helper with decoder scratch), an `FO3DBoundedSendQueue` plus worker (atomic byte accounting, drop policy, wake event), an `FO3DSenderAudioPipeline` (encoder + owner guard + unified wrap), and `O3DTransportOptions::{ParseHostPort, GetInt/Bool/String}`. Add a shared test-utility header (port probe, pump loop).
- Effort: L
- Owner: design
- Status: closed in #310

### TRB-39: NNG URI host is never used because "explicit host" is always true
- Category: bug
- Severity: medium
- Location: Open3DTransportNNG/Private/Shared/NngHelpers.cpp:293-327, :360-386
- Evidence: `if (Host.IsEmpty()) Host = DefaultHost; const bool bHostExplicit = !Host.IsEmpty();`. Since DefaultHost is "0.0.0.0" or "127.0.0.1", `bHostExplicit` is always true, so the host from the `Uri`, the `?host=` query and the StreamId is never applied. A config that carries only `Uri = "nng+sub://10.0.0.5:6000"` connects to 127.0.0.1. The module path happens to work because it always writes the `host` option.
- Recommendation: Compute `bHostExplicit` from the option before applying the default, and apply DefaultHost only at the end (line 409 already does that). Add a unit test for URI-only configs.
- Effort: S
- Owner: coding
- Status: closed in #277

### TRB-40: NNG Pair defaults make both ends listen; "Pull (client dial)" is forced to listen
- Category: bug
- Severity: medium
- Location: Open3DTransportNNG/Private/Open3DTransportNNGModule.cpp:853-855, :927-930, :626; Open3DTransportNNG/Private/Shared/NngHelpers.cpp:726-769, :612-657
- Evidence:
  - Receiver module config: `DefaultRole = (Pull || Pair) ? Server : Client`. Sender module config: Pair defaults to Server. With no explicit role, both Pair ends listen and never connect. The helper's own receiver default for Pair is Client (line 733), contradicting the module.
  - The receiver UI offers "Pull (client dial)", but `ParseReceiverOptions` forces `Pull → Role=Server, bListen=true` (lines 759-762).
  - Push is forced to Client and Pub to Server regardless of the chosen role. The README states "role: server, client" is configurable.
- Recommendation: Keep a single source of truth for mode/role defaults and allowed combinations (in NngHelpers). Make the widgets list only valid combinations. Either support pull-dial and push-listen (valid NNG topologies, for example a push-listen sender with pull-dial receivers) or remove the option from the UI.
- Effort: S
- Owner: design
- Status: closed in #277

### TRB-41: NNG is Win64-only but enabled by default and throws on other platforms
- Category: fab-readiness
- Severity: high
- Location: Open3DTransportNNG/Open3DTransportNNG.Build.cs:19-27; ProjectSandbox/Plugins/Open3DBroadcast/Open3DBroadcast.uplugin (Open3DTransportNNG entry has no PlatformAllowList); Open3DShared/Open3DShared.Build.cs (O3DBuildFlags `WithNNG = true`)
- Evidence: For any `Target.Platform != Win64`, the Build.cs does `throw new BuildException("Open3DTransportNNG does not define third-party binaries ...")`, while `O3D_WITH_TRANSPORT_NNG` defaults to 1 (MoQ is auto-disabled off Win64, but NNG is not). Linux, Mac and Linux-server targets therefore fail to build out of the box. Only a prebuilt `nng.lib` (1.3.0, 2020) is shipped, with no Linux or Mac libs and no build script.
- Recommendation: Add `"PlatformAllowList": ["Win64"]` to the module in the .uplugin, and/or auto-disable NNG off Win64 in `O3DBuildFlags` as is done for MoQ. Longer term, build NNG from source through CMake for all platforms and upgrade from 1.3.0 (check upstream security advisories for later releases).
- Effort: M
- Owner: coding
- Status: closed in #283

### TRB-42: NNG receiver limits, stats and callback lifetime
- Category: code-quality
- Severity: low
- Location: Open3DTransportNNG/Private/Receiver/NngReceiver.cpp:25, :244-253, :416-422, :555-582; Open3DTransportNNG/Private/Sender/NngSender.cpp:440-449, :556-560, :740-744
- Evidence:
  - `NNG_OPT_RECVMAXSZ` is never set, so NNG's own default applies (verify: it may be 1 MB in 1.3.0) and the 50 MB check is probably unreachable. Frames above the NNG default are dropped silently.
  - Audio frames increment `FramesReceived` twice (inside `ProcessAudioPayload` and again in `Poll`).
  - On the sender, audio `FramesSent` is counted at enqueue and again by the worker.
  - Pipe callbacks receive a raw `this`. Whether `nng_close` waits for in-flight pipe callbacks before the wrapper and object are destroyed needs NNG verification.
  - `bConnected=true` is set immediately after a non-blocking dial.
- Recommendation: Set `NNG_OPT_RECVMAXSZ` to the configured cap. Count each frame exactly once, in one place. Document or verify the callback shutdown ordering, or route callbacks through a shared weak context. Derive "connected" only from pipe events.
- Effort: S
- Owner: coding
- Status: closed in #277

### TRB-43: NNG errors are logged only at Verbose
- Category: usability
- Severity: low
- Location: Open3DTransportNNG/Private/Sender/NngSender.cpp:151, :189, :215, :421, :428, :579, :617
- Evidence: A failed config parse, a failed socket open or listen (for example the port is in use), the queue being full and send errors all use `Verbose`, so a user whose NNG sender cannot bind sees nothing in the default log. The receiver uses Warning for similar cases, so the two sides are inconsistent. The project rules say state transitions and errors must be logged by default.
- Recommendation: Log Warning or Error for open, bind and parse failures, and rate-limited Warning for drops. Keep Verbose for per-frame detail.
- Effort: S
- Owner: coding

### TRB-44: NNG README is inaccurate and leaks an infrastructure IP
- Category: docs
- Severity: medium
- Location: Open3DTransportNNG/README.md:14-36, :55-62; Open3DTransportNNG/ThirdParty/README.md:6-11
- Evidence:
  - The README documents `nng://host:port?mode=&role=` with `nng://127.0.0.1:5555` as the default, but the code emits `nng+<mode>://` and defaults to ports 6000, 7000 and 8000.
  - It claims configurable roles (see TRB-40) and non-blocking behaviour with a "30-second send timeout" (see TRB-36).
  - It tells users to "increase the `MaxQueueBytes` setting", but the actual key is `nng.qmax` (UI "Queue Capacity (MiB)").
  - It hard-codes a public IP `<redacted public IP>` and refers to "main.cpp line ~47" (apps/Repeater).
  - The ThirdParty README shows `Lib/Win64`, but the path used is `lib/Win64`, which matters on case-sensitive file systems.
  - The Sockets and Loopback modules have no docs at all (framing, ports, single-client TCP, UDP limits).
- Recommendation: Rewrite the README from the code: URI grammar, option keys, defaults, valid mode/role table and security notes. Remove the IP. Fix the path case. Add short READMEs for Sockets and Loopback, including the wire framing spec (magic, LE length, UDP fragment header).
- Effort: S
- Owner: coding

## Editor widgets and tests

### TRB-45: Slate panels hold raw UObject pointers, mutate assets on construction, and fire on every drag tick
- Category: usability
- Severity: medium
- Location: Open3DTransportSockets/Private/Shared/SocketsTransportTcpSenderWidget.cpp:36-47, :165-170; SocketsTransportUdpSenderWidget.cpp:37-64, :271-296; SocketsTransportTcpReceiverWidget.cpp:36-49, :229; SocketsTransportUdpReceiverWidget.cpp:39-66, :336; Open3DTransportLoopback/Private/Open3DTransportLoopbackModule.cpp:145-157, :253-260; Open3DTransportNNG/Private/Open3DTransportNNGModule.cpp:161-187, :389-399
- Evidence:
  - The panels store `UO3DSenderComponent*` / `UO3DReceiverSettingsObject*` raw, with no `TWeakObjectPtr`, so a GC'd component or a PIE teardown leaves the panel dangling.
  - `Construct()` writes defaults into the component (`SetBindHost`, `SetPort`, `SetMtu`, ...), so merely opening the panel dirties the asset, with no `Modify()` or transaction on the sender side.
  - A user who deliberately picks port 17800 for TCP is silently reset to 17700 (`ExistingPort == DefaultUdpPort`), and vice versa.
  - Sender panels call `OnConfigChanged` from `OnValueChanged` on every spin-box drag tick, which may restart the transport dozens of times.
  - There is no validation that MTU is at most maxdatagram, and the indentation is broken (`}			ChildSlot`).
- Recommendation: Use `TWeakObjectPtr`. Do not write defaults on construct (show defaults as hints). Wrap edits in an `FScopedTransaction` and call `Modify()`. Notify on `OnValueCommitted` only. Put a reusable "key/value option row" helper in the shared panel base, because 7 panels repeat the same GetOption/SetOption/Clamp code.
- Effort: M
- Owner: coding
- Status: closed in #285

### TRB-46: Editor-only Slate code lives in Runtime modules
- Category: architecture
- Severity: low
- Location: Open3DTransportSockets/Open3DTransportSockets.Build.cs:45-53; Open3DTransportLoopback/Open3DTransportLoopback.Build.cs:32-40; Open3DTransportNNG/Open3DTransportNNG.Build.cs:68-76; Open3DTransportSockets/Private/Shared/*Widget.cpp; Open3DTransportLoopbackModule.cpp:14-268; Open3DTransportNNGModule.cpp:117-836
- Evidence: Panels are compiled under `WITH_EDITOR` inside Runtime modules and registered from `StartupModule`. This works, but it couples runtime modules to Slate, AppFramework and the editor-panel base, and it bloats module .cpp files (the NNG module is about 1000 lines, 70% UI). The Loopback Build.cs adds Slate/AppFramework and the NNG Build.cs adds `InputCore`, `ApplicationCore` and `HTTP`, and none of these appear to be needed beyond `FGenericPlatformHttp` (Core).
- Recommendation: Move the panels into an `Open3DBroadcastEditor` (Type: Editor) module that registers the `BuildTransportWidget` callbacks, and keep the runtime modules headless. Prune the unused dependencies.
- Effort: M
- Owner: design
- Status: closed in #285

### TRB-47: Tests cover only the localhost happy path; framing, UDP and robustness are untested
- Category: tests
- Severity: high
- Location: Open3DTransportSockets/Private/Tests/SocketsAudioTests.cpp:143-255; Open3DTransportNNG/Private/Tests/NngTransportTests.cpp:153-323; Open3DTransportLoopback/Private/Tests/LoopbackAudioTests.cpp:38-119
- Evidence:
  - Sockets has one audio round-trip test and one test misnamed "QueueOverflow", which only checks that a send without a client fails.
  - There are no mocap TCP tests, no multi-frame coalescing or split-header tests (which would have caught TRB-1), no UDP tests at all (fragmentation, loss, oversize, malformed headers), no reconnect or timeout tests, and no NNG pair/push/pull tests.
  - There are no tests for URI-only config parsing (TRB-39), no stop/restart lifecycle tests, and no tests for sink-outlives-sender.
  - The tests use wall-clock sleeps (flaky) and `EditorContext` only.
  - The guards are inconsistent: `WITH_AUTOMATION_TESTS` in Sockets vs `WITH_DEV_AUTOMATION_TESTS` elsewhere, and the Loopback closing comment does not match.
- Recommendation: Add pure unit tests (no sockets) for the TCP stream parser, the UDP fragment classifier and reassembly (limits, eviction, spoofed totals), NngHelpers option parsing and the unified demux. Add socket-level integration tests for TCP burst and reconnect, UDP fragmentation, and each NNG mode. Add a fuzz-style test feeding random bytes to each parser. Use one guard macro consistently.
- Effort: L
- Owner: coding
