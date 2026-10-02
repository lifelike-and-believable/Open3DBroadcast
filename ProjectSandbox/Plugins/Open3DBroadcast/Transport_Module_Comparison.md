# In-Depth Comparison of Open3DTransport Modules

## Executive Summary

Open3DBroadcast has **5 transport modules** in a modular architecture: Loopback, NNG, Sockets and MoQ ship in the Open3DBroadcast plugin, and WebRTC ships in the free **Open3DBroadcastWebRTC** add-on plugin, installed next to it (ADR 0002, WP-F11). Each implements the `IOpen3DSender` and `IOpen3DReceiver` interfaces. All modules support both motion capture data streaming and audio transmission, but differ significantly in their network topologies, threading models, and intended use cases.

> **Available in:** every WebRTC section below applies only when the Open3DBroadcastWebRTC
> add-on is installed. The other transports are always present.

> **Naming:** *Open3DBroadcast* is the Unreal Engine plugin. *Open3DStream* is the
> streaming protocol and core C++ library (`o3ds`) it is built on.

---

## 1. Module Overview

| Module | Transport ID | Primary Use Case | Network Dependency | Third-Party Dependencies |
|--------|-------------|------------------|-------------------|-------------------------|
| **Loopback** | `loopback` | In-process testing/validation | None (in-memory) | None |
| **NNG** | `nng` | Advanced messaging patterns | Network (TCP) | NNG library (static) |
| **Sockets** | `tcp`, `udp` | Direct peer-to-peer | Network (TCP/UDP) | Unreal Sockets subsystem |
| **WebRTC** (add-on) | `webrtc` | Cloud/NAT traversal | Network (WebRTC) | LiveKit FFI library |
| **MoQ** | `moq` | Cloud/NAT traversal over QUIC | Network (QUIC/WebTransport, via relay) | moq-ffi library |

---

## 2. Architecture & Interface Compliance

### Common Interface (100% Parity)

All modules implement the same interfaces with identical method signatures
(transport API version 4, WP-A1 PR 3; ADR 0007 items 3 and 4):

**IOpen3DSender** (`Open3DShared/Public/Transport/O3DSenderInterface.h`):
- `Initialize(Config)` / `Start()` - return `FO3DTransportResult` (an error code such as `InvalidConfig`, `NotRunning` or `AddressInUse`, and a message)
- `Stop()` - Cleanup (idempotent)
- `SendSerialized(FO3DSendPayload&&)` - Send one serialized frame; returns `EO3DSendResult` (`Queued`, `DroppedBackpressure`, `NotRunning`, `NotConnected`, `Invalid`, `TooLarge`)
- `SendControl(Envelope, Len)` - Send one control envelope; returns `EO3DSendResult` (`Unsupported` by default)
- `Tick(DeltaSeconds)` - Lightweight upkeep
- `GetStats()` - Performance metrics, including the connection state
- `GetCapabilities()` - `FO3DTransportCapabilities` (audio, control, delivery guarantee, payload limit)
- `GetConnectionState()` / `SetStateChangedCallback()` - `Idle`, `Connecting`, `Connected`, `Reconnecting`, `Failed`
- `CreateAudioSink(AudioConfig)` - Audio sink factory

**IOpen3DReceiver** (`Open3DShared/Public/Transport/O3DReceiverInterface.h`):
- `Initialize(Config)` / `Start()` - return `FO3DTransportResult`; `Start()` without a consumer is `NoConsumer`
- `SetConsumer(Consumer)` - Frame consumer registration
- `Stop()` - Cleanup
- `Poll()` - Process incoming data
- `GetStats()` - Performance metrics
- `GetCapabilities()`, `GetConnectionState()`, `SetStateChangedCallback()` - as on the sender
- `SetAudioSink(Sink, AudioConfig)` - Audio sink registration

The registry answers the same capability question before an instance exists:
`FO3DTransportRegistry::GetCapabilities(Name, Config, Out)` (from the descriptor's
`GetCapabilities`).

**Audio Interfaces**:
- `IO3DSenderAudioSink` - PCM float submission
- `IO3DReceiverAudioSink` - PCM16 delivery

---

## 3. Detailed Module Comparison

### 3.1 **Loopback Transport**

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportLoopback/`

**Architecture**:
- In-process, shared-memory channel communication
- Channel registry with ref-counted channels; each channel is one shared `FO3DSendQueue` (Open3DShared, WP-A1 step 4) carrying mocap, audio and control items, each kind with its own limit
- The receiver's `Poll` hands each item to the shared `FO3DUnifiedReceiveDemux`; the audio sink is the shared `FO3DQueuedSenderAudioSink`
- Lock-free MPSC (Multi-Producer Single-Consumer) queue with atomic item and byte accounting

**Key Classes**:
- `FO3DLoopbackSender` (`Sender/LoopbackSender.h`)
- `FO3DLoopbackReceiver` (`Receiver/LoopbackReceiver.h`)
- `O3DLoopback::AcquireChannel` (`Shared/LoopbackChannel.h`) - the channel's shared queue

**Threading Model**:
- **Synchronous** - No background threads
- Sender enqueues to shared channel on `Send()` call
- Receiver dequeues on `Poll()` call
- Thread-safe via atomic counters and an MPSC queue

**Configuration** (`LoopbackChannel.cpp`):
- `channel` - Channel key (default: URI or "default")
- `loopback.maxqueue` - Mocap frames the channel holds; a full channel refuses new frames (default: 64)
- `loopback.maxaudioqueue` - Audio frames the channel holds (default: 32)

**Audio Support**:
- ✅ Full support with configurable codec
- Uses `O3DAudio::FFrameEncoder`/`FFrameDecoder`
- Audio has its own limit on the shared channel queue (overflow refuses the newest audio frame)
- PCM16 and Opus codecs supported

**Unique Characteristics**:
- ✨ **Zero network overhead** - Ideal for local testing
- ✨ **Deterministic latency** - Predictable in-process behavior
- ✨ **Channel isolation** - Multiple independent channels per process
- ✨ **Backpressure handling** - Queue overflow detection with drop logging
- ⚠️ **Single-process only** - Cannot communicate across processes

**Use Cases**:
- Unit testing transport layer
- Validation of serialization/deserialization
- Performance benchmarking without network variance
- Development/debugging

**Dependencies**:
- No external libraries
- Only core Unreal modules

---

### 3.2 **NNG Transport**

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportNNG/`

**Architecture**:
- Based on [NNG (Nanomsg-Next-Generation)](https://nng.nanomsg.org/) library
- Multiple messaging patterns with flexible topology
- Shared transport blocks (ADR 0007 item 7, WP-A1 PR 4d): `FO3DSendQueue` + `FO3DTransportWorker` for sends, `FO3DUnifiedReceiveDemux` for receives
- Pipe event callbacks for connection tracking

**Key Classes**:
- `FO3DNngSender` (`NngSender.h`)
- `FO3DNngReceiver` (`NngReceiver.h`)
- `FNngSocketWrapper` - RAII socket management

**Supported Messaging Patterns** (`NngHelpers.h`):
| Pattern | Sender Mode | Receiver Mode | Topology |
|---------|------------|---------------|----------|
| **Pub/Sub** | Pub (Publisher) | Sub (Subscriber) | 1:N broadcast with topic filtering |
| **Pair** | Pair | Pair | 1:1 exclusive connection |
| **Push/Pull** | Push | Pull | N:M load balancing |

**Roles** (`NngHelpers.h`):
- **Server** - Listen/bind mode (accepts connections)
- **Client** - Dial/connect mode (initiates connections)

**Threading Model**:
- **Sender**: `SendSerialized`, `SendControl` and the audio sink only enqueue on one `FO3DSendQueue`; an `FO3DTransportWorker` owns the socket and calls `nng_send` with `NNG_FLAG_NONBLOCK`
  - Frames: `RefuseNewest`, `nng.qmax` bytes (4 MiB default). The worker drops the oldest frame when NNG cannot take it (no peer, NNG send buffer full)
  - Audio (`nng.qmax` bytes) and control (1,024 envelopes) have budgets of their own
  - NNG redials dropped connections itself; `FO3DReconnectPolicy` (0.1 s to 5 s) only paces reopening a socket that failed or was closed
- **Receiver**: Synchronous polling (NNG's own threads do the I/O)
  - Non-blocking `nng_recv()` on `Poll()`, at most 16 messages per call
  - Messages go to `FO3DUnifiedReceiveDemux` (consumer, audio sink, control sink)

**Configuration** (`NngHelpers.h`; hosts and ports parsed strictly with `O3DTransportOptions`):
- `nng.mode` - Messaging pattern: "pub", "sub", "pair", "push", "pull"
- `nng.role` - Connection role: "server", "client"
- `host` - Hostname or IP (IPv6 in brackets in URIs: `tcp://[::1]:17700`)
- `port` - Port number, 1 to 65535 (digits only)
- `nng.qmax` - Max queue bytes (default: 4MB, range: 64KB-512MB)
- `nng.topic` - Topic filter for pub/sub (UTF-8 prefix matching)

**Audio Support**:
- ✅ Full support (`SupportsAudio() = true`)
- Unified message format with type discrimination
- Audio frames sent through same queue/socket as mocap
- Configurable codec (PCM16, Opus)

**Unique Characteristics**:
- ✨ **Multiple messaging patterns** - Flexible topology options
- ✨ **Topic-based filtering** - Pub/Sub with subscriber-side filtering
- ✨ **Automatic pipe management** - Connection tracking via NNG callbacks
- ✨ **Load balancing** - Push/Pull pattern distributes across receivers
- ✨ **Queue-based backpressure** - Byte-based limits prevent memory exhaustion
- ✨ **Reconnection** - NNG redials dropped connections; failed sockets are reopened with backoff
- ⚠️ **Platform limitation** - Currently Win64 only (`Open3DTransportNNG.Build.cs:20-27`)
- ⚠️ **External dependency** - Requires NNG library

**Connection State Tracking** (`NngSender.cpp`):
- Pipe add/remove callbacks, through an opaque token
- Pipe count in an atomic; the sender's worker and the receiver's `Poll` turn it into connection-state changes
- Automatic reconnection for client-mode Pair/Push patterns

**Use Cases**:
- **Pub/Sub**: One mocap source broadcasting to multiple receivers
- **Pair**: Reliable 1:1 connection between sender/receiver
- **Push/Pull**: Load distribution across multiple processing nodes

**Dependencies**:
- NNG library (static linkage)
- `Sockets`, `Networking` modules

---

### 3.3 **Sockets Transport** (TCP + UDP)

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportSockets/`

**Architecture**:
- **Dual implementation**: Separate TCP and UDP transports
- Both use Unreal's `ISocketSubsystem` abstraction
- Platform-agnostic socket API

#### 3.3.1 **TCP Implementation**

**Key Classes**:
- `FO3DSocketsTcpSender` (`Sender/SocketsTcpSender.h`)
- `FO3DSocketsTcpReceiver` (`Receiver/SocketsTcpReceiver.h`)
- Built on the shared transport blocks in Open3DShared (WP-A1 PR 4b): `FO3DSendQueue`, `FO3DTransportWorker`, `FO3DReconnectPolicy`, `FO3DUnifiedReceiveDemux`, `FO3DQueuedSenderAudioSink`, `O3DTransportOptions`

**Threading Model**:
- **Sender**: Async worker thread (`FO3DTransportWorker`)
  - The listen socket is created on the game thread in `Start()`; the worker is started only if bind and listen succeed. The bind address must be an IP address or a wildcard (`0.0.0.0`, `*`)
  - The worker owns the client socket: it accepts, sends from the shared `FO3DSendQueue` (frames, audio and control in order, each written as one TCP frame), writes keepalives and notices a closed receiver. The game thread never waits on a send
  - Partial sends and `EWOULDBLOCK` are retried until the frame is written; the client is dropped only on a socket error or after `tcp.stalltimeout` without progress. Frames are dropped whole, never partly written
  - One client at a time; the next receiver is accepted as soon as the current one closes (checked every 250 ms while idle)
- **Receiver**: Async worker thread (`FO3DTransportWorker`); `Poll()` on the game thread delivers
  - The worker resolves the host (names included, never on the game thread), connects, reads and frames; connection states Connecting → Connected → Reconnecting are reported from it
  - Framing is parsed by the core `O3DS::TcpStreamParser` (`src/o3ds/tcp_stream_parser.h`): every complete frame in a read is delivered, resync after garbage is one pass, and lengths above `tcp.maxframe` are rejected without allocating them
  - Payloads wait in a bounded hand-off queue (8 MiB, or one `tcp.maxframe` if larger); when it is full the worker stops reading, so TCP flow control holds the sender back
  - Each `Poll()` handles at most 256 payloads or 8 MiB through `FO3DUnifiedReceiveDemux` (frames to the consumer, audio, control); the rest waits in the queue
  - Reconnects with exponential backoff with ±20% jitter (`tcp.backoff` doubling up to `tcp.maxbackoff`, `FO3DReconnectPolicy`); the backoff resets only after a connection delivers data

**Connection Model**:
- **Sender** = Server (listens for connections)
- **Receiver** = Client (connects to sender)

**Framing Protocol** (`src/o3ds/tcp_stream_parser.h`):
```
[14 bytes: magic 00 FF 03 FE "O3DS-START"] [4 bytes: payload size (little-endian)] [N bytes: payload]
```
While idle for `tcp.keepalive` ms the sender writes a keepalive frame whose payload is a 20-byte unified-envelope header (kind Audio, payload size 0). Receivers ignore it as data but it resets their idle timer. Receivers built before WP-S6 also ignore it silently.

**Configuration**:
- `host` - Hostname or IP (IPv6 in brackets in URIs: `tcp://[::1]:17700`)
- `port` - Port number, 1 to 65535 (digits only)
- `bind` - Bind address for sender
- `tcp.timeout` - Receiver: seconds without any data (frames or keepalives) before it reconnects (default: 5)
- `tcp.connecttimeout` - Receiver: seconds a connect may stay pending before it is abandoned and retried (default: 5)
- `tcp.maxframe` - Receiver: largest frame payload accepted, in bytes (default: 4194304, min 1024, max 52428800)
- `tcp.backoff` - Receiver: first reconnect delay in ms, doubled after each failed attempt (default: 500)
- `tcp.maxbackoff` - Receiver: longest reconnect delay in ms (default: 5000)
- `tcp.maxqueue` - Sender: payload bytes of queued frames, and separately of queued audio; a frame that does not fit is refused (`DroppedBackpressure`). Control has its own cap of 1,024 envelopes (default: 4194304, min 65536)
- `tcp.maxqueueage` - Sender: frames and audio that waited longer than this many ms are dropped before sending; control never expires; 0 disables (default: 1000)
- `tcp.stalltimeout` - Sender: ms a frame may make no progress on a full socket before the client is dropped (default: 2000, min 100)
- `tcp.keepalive` - Sender: ms of idleness before a keepalive is sent; 0 disables (default: 1000). Keep it below the receiver's `tcp.timeout`

**Audio Support**:
- ✅ Full support with unified messaging
- Same socket/connection as mocap data
- Ordered, reliable delivery

**Unique TCP Characteristics**:
- ✨ **Reliable ordered delivery** - No packet loss
- ✨ **Automatic reconnection** - Receiver auto-reconnects on disconnect
- ✨ **Stateful framing** - Progressive read state machine
- ✨ **Backpressure handling** - Queue limits prevent unbounded memory growth
- ⚠️ **Single client** - Sender accepts only one receiver at a time
- ⚠️ **Buffering overhead** - Requires buffering for framing

#### 3.3.2 **UDP Implementation**

**Key Classes**:
- `FO3DSocketsUdpSender` (`SocketsUdpSender.h`)
- `FO3DSocketsUdpReceiver` (`SocketsUdpReceiver.h`)

**Threading Model** (shared transport blocks, ADR 0007 item 7, WP-A1 PR 4c):
- **Sender**: `SendSerialized`, `SendControl` and the audio sink only enqueue on one `FO3DSendQueue`; an `FO3DTransportWorker` owns the socket and sends every item (TRB-20)
  - Frames: `DropOldest`. While the worker is behind, at most 4 frames (16 MiB) wait and older ones are discarded so the newest are sent; callers are refused (`DroppedBackpressure`) only at twice that
  - Audio (1 MiB) and control (1,024 envelopes) have budgets of their own and are never dropped for frames
  - Automatic fragmentation for large payloads, on the worker
  - A host name resolves on the worker (`O3DTransportOptions::ResolveHostPort`, retried with `FO3DReconnectPolicy`); the state is `Connecting` until then. An IP literal resolves in `Initialize`
- **Receiver**: Synchronous polling
  - Non-blocking receive on `Poll()`
  - Fragment reassembly state machine
  - Complete messages go to `FO3DUnifiedReceiveDemux` (consumer, audio sink, control sink)

**Fragmentation System** (`o3ds/udp_fragment.h`):
- **MTU awareness**: Default 1200 bytes (configurable)
- **Max datagram**: 64KB default (configurable)
- **Fragment header**: Sequence ID + fragment index + total fragments
- **Reassembly**: `UdpMapper` tracks in-flight fragment sets, keyed on (sender address, message id); at most 8 in flight and 4x `udp.maxframe` bytes in total, oldest evicted first
- **Timeout**: Incomplete fragment sets are discarded after 500 ms, or as soon as a newer message from the same sender completes
- **Poll bound**: each `Poll()` reads at most 1024 datagrams / 8 MiB; the rest stays in the socket buffer for the next poll

**Configuration**:
- `host` - Hostname or IP (IPv6 in brackets in URIs: `udp://[::1]:17800`); the receiver's bind host must be an IP address, `*`, empty or `localhost`
- `port` - Port number, 1 to 65535 (digits only)
- `bind` - Bind address for receiver
- `udp.broadcast` - Enable broadcast mode (default: false)
- `udp.mtu` - MTU size in bytes (default: 1200)
- `udp.maxdatagram` - Max datagram size before fragmentation (default: 64000)
- `udp.maxframe` - Receiver: largest reassembled message accepted, in bytes (default: 4194304, max 52428800)

**Broadcast Support** (`SocketsUdpSender.cpp`):
- Special hostname handling: `*` or empty → `255.255.255.255`
- Automatically enables `SetBroadcast(true)` on socket
- One-to-many distribution without explicit receiver addresses

**Audio Support**:
- ✅ Full support
- Unified message format (same as NNG)
- Fragments audio frames if needed

**Unique UDP Characteristics**:
- ✨ **Connectionless** - No handshake or connection state
- ✨ **Low latency** - No TCP overhead
- ✨ **Broadcast capable** - One-to-many without multicast
- ✨ **Automatic fragmentation** - Transparent large frame handling
- ✨ **MTU configurable** - Tune for network characteristics
- ⚠️ **Unreliable** - Packet loss possible
- ⚠️ **Unordered** - Frames may arrive out of order
- ⚠️ **Fragmentation overhead** - Large frames incur header overhead

**Use Cases**:
- **TCP**: Reliable point-to-point streaming, production deployments
- **UDP**: Low-latency streaming, local networks, broadcast scenarios

**Dependencies**:
- `Sockets`, `Networking` modules (built-in Unreal)
- No external libraries

---

### 3.4 **WebRTC Transport** (Open3DBroadcastWebRTC add-on)

**Location**: `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/Source/Open3DTransportWebRTC/` (the add-on plugin)

**Architecture**:
- Based on [LiveKit FFI](https://github.com/livekit/client-sdk-rust) library
- Cloud-signaling with NAT traversal (STUN/TURN)
- Room-based communication model
- Opus encoding/decoding handled by LiveKit internally

**Key Classes**:
- `FO3DWebRTCSender` (`WebRTCSender.h`)
- `FO3DWebRTCReceiver` (`WebRTCReceiver.h`)
- `LkClientHandle` - Opaque LiveKit FFI handle

**Threading Model**:
- **Event-driven** via LiveKit FFI callbacks
- LiveKit manages internal thread pool
- Callbacks invoked on FFI threads:
  - `OnConnectionState` - Connection state changes
  - `OnDataReceived` - Incoming mocap data (receiver)
  - `OnAudioReceived` - Incoming PCM16 audio (receiver)
- **Sender**: `SendSerialized` and `SendControl` hand each message to LiveKit on the caller's thread; LiveKit's refusal is the backpressure (`DroppedBackpressure`). No send queue or worker (WP-A1 PR 4f, ADR 0007 addendum)
- **Receiver** (shared transport blocks, WP-A1 PR 4f): the data callback puts frames (`RefuseNewest`, 16 MiB) and control (1,024) on `FO3DSendQueue` hand-offs; `Poll` delivers them through `FO3DUnifiedReceiveDemux`. Audio goes from the audio callback straight to the sink
- **Reconnect**: LiveKit's own (sender); the receiver's no-data watchdog recreates the client

**Connection Model**:
- **Room-based**: Both sender and receiver join a LiveKit room
- **Token-based auth**: JWT tokens for room access
- **Signaling**: LiveKit server coordinates WebRTC peer connections
- **NAT traversal**: Automatic STUN/TURN for firewall traversal

**Configuration**:
- `Uri` - LiveKit server URL (e.g., `wss://myserver.livekit.cloud`)
- `Token` - JWT authentication token
- `StreamId` - Subject name filter (receiver only)

**Audio Support**:
- ✅ **Built-in Opus encoding** - LiveKit handles codec internally
- ✅ **PCM16 API boundary** - Float→Int16 conversion in audio sink
- ✅ **Automatic synchronization** - LiveKit handles A/V sync
- Sender: `lk_publish_audio_pcm_i16()` (`WebRTCSender.cpp:52-58`)
- Receiver: PCM16 callback delivers decoded audio

**LiveKit FFI Callbacks** (`WebRTCSender.h:62`):
```cpp
static void OnConnectionState(void* user, LkConnectionState state,
                             int32_t reason_code, const char* message);
```

**Connection States**:
- `LkConnConnecting` - Establishing connection
- `LkConnConnected` - Active connection
- `LkConnReconnecting` - Temporary disconnection
- `LkConnDisconnected` - Clean shutdown
- `LkConnFailed` - Connection error

**Unique Characteristics**:
- ✨ **Cloud-native** - Works across WAN/Internet
- ✨ **NAT traversal** - Automatic firewall/NAT handling
- ✨ **Room-based topology** - N:M communication in single room
- ✨ **Managed infrastructure** - LiveKit handles signaling/TURN servers
- ✨ **Built-in Opus** - No manual audio encoding needed
- ✨ **Automatic reconnection** - LiveKit handles connection recovery
- ⚠️ **External service dependency** - Requires LiveKit server
- ⚠️ **Authentication required** - JWT token management
- ⚠️ **Platform limitation** - Currently Win64 only (`Open3DTransportWebRTC.Build.cs:20-27`)
- ⚠️ **DLL dependency** - `livekit_ffi.dll` runtime requirement

**Use Cases**:
- Remote streaming over Internet
- Cloud-based mocap services
- Multi-user collaborative sessions
- Firewall/NAT traversal scenarios

**Dependencies**:
- LiveKit FFI library (DLL)
- LiveKit server infrastructure

---

### 3.5 **MoQ Transport**

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/`

**Architecture**:
- Media over QUIC (MoQ) transport, backed by the `moq-ffi` Rust library over Cloudflare's [moq-rs](https://github.com/cloudflare/moq-rs)
- Data travels over **QUIC / WebTransport** to a **relay**, rather than peer-to-peer
- Publish/subscribe model addressed by **track namespace + track name**
- Relay-mediated connections traverse NAT the same way WebRTC does, without STUN/TURN

**Key Classes**:
- `FO3DMoQSender` (`MoQSender.h`)
- `FO3DMoQReceiver` (`MoQReceiver.h`)
- `FMoQSessionWrapper` — session lifecycle over the FFI boundary
- `FMoQPublisherHandle` / `FMoQSubscriberHandle` — RAII handles for FFI objects
- `FMoQAsyncDispatcher` (`MoQAsyncDispatcher.h:17`) — `FRunnable`, a shared dispatcher thread
- `FMoQFfiSupport` — DLL load and export validation

**Threading Model**:
- **Dedicated dispatcher thread** (`FMoQAsyncDispatcher`, an `FRunnable` singleton) marshals async FFI work
- FFI callbacks deliver connection-state and subscriber-data events, run on the game thread
- **Sender** (shared transport blocks, WP-A1 PR 4e): `SendSerialized`, `SendControl` and the audio sink enqueue on one `FO3DSendQueue` (`RefuseNewest`, `queue_bytes` for frames, audio 1 MiB, control 1,024); an `FO3DTransportWorker` publishes each item on its track and drops items whose publisher is not ready
- **Receiver**: data callbacks go to a bounded hand-off queue; `Poll` routes them through `FO3DUnifiedReceiveDemux`
- **Reconnect**: MoQ's own jittered backoff and connect timeout, on the game thread (moq-ffi does not reconnect)
- Handle types own FFI lifetime explicitly, so teardown ordering is enforced rather than incidental

**Audio**:
- Uses the **shared `FO3DQueuedSenderAudioSink`** (bare audio payloads for the audio track) — the same sink as Loopback, NNG and Sockets
- Encodes to **PCM16 or Opus** via `O3DAudio::FFrameEncoder`, then publishes on a separate audio track
- Receiver subscribes to the audio track independently of the mocap track

> Note: MoQ follows the standard audio path. **WebRTC is the outlier** here, because LiveKit performs Opus encoding internally behind its FFI.

**Advantages**:
- ✨ **NAT traversal without STUN/TURN** — outbound QUIC to a relay
- ✨ **No per-session signalling service** — the relay is the rendezvous point
- ✨ **Standard audio pipeline** — same encoder and codec options as the LAN transports
- ✨ **Automatic reconnection** — configurable via `ReconnectDelaySeconds`

**Limitations**:
- ⚠️ **Relay dependency** — requires a reachable MoQ relay
- ⚠️ **Platform limitation** — currently Win64 only; other platforms auto-disable the transport
- ⚠️ **DLL dependency** — `moq_ffi.dll` runtime requirement
- ⚠️ **Draft protocol** — MoQ is an evolving IETF draft, so relay interoperability tracks a specific draft revision

**Use Cases**:
- Remote streaming over the Internet where a relay is preferable to a full WebRTC stack
- Cloud/WAN delivery via Cloudflare relays
- Deployments wanting NAT traversal without LiveKit server infrastructure

**Dependencies**:
- `moq_ffi` library (DLL)
- A MoQ relay endpoint

---

## 4. Functional Parity Matrix

| Feature | Loopback | NNG | TCP | UDP | WebRTC | MoQ |
|---------|----------|-----|-----|-----|--------|-----|
| **Core Interfaces** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Send SubjectList** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Audio Support** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Control (events and values)** | ✅ Own queue | ✅ In-band | ✅ In-band | ✅ In-band | ✅ `__o3d.ctl` data label | ✅ `control/` track |
| **Stats Reporting** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Backpressure Handling** | ✅ Queue | ✅ Queue | ✅ Queue | ⚠️ None | ✅ LiveKit | ✅ Relay/QUIC |
| **Reconnection** | N/A | ✅ Auto | ✅ Auto | N/A | ✅ Auto | ✅ Auto |
| **Multiple Receivers** | ✅ Many | ✅ Pattern | ❌ Single | ✅ Broadcast | ✅ Room | ✅ Relay fan-out |
| **Reliable Delivery** | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **Ordered Delivery** | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **NAT Traversal** | N/A | ❌ | ❌ | ❌ | ✅ | ✅ |
| **Cross-Process** | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Platform Support** | All | Win64 | All | All | Win64 | Win64 |

**Audio Support is universal.** Every transport reports `bAudioSend` and
`bAudioReceive` in `GetCapabilities()` on **both** the sender and receiver side, no
exceptions. Audio is a plugin-level capability, not a property of any one
transport. (`SupportsAudio()` still exists and forwards to `GetCapabilities()`.)

**Delivery guarantee** (`FO3DTransportCapabilities::Delivery`, ADR 0005 (iii)):
Loopback, TCP, NNG pair and push/pull, and WebRTC report `ReliableOrdered`; UDP,
NNG pub/sub, MoQ and WebRTC with `webrtc.prefer_lossy` report `Unreliable`.

**Control** (ADR 0011; USER_GUIDE "Control Channel") is a one-way stream of
events and values from a sender to its receivers. Every transport reports
`bControl` in `GetCapabilities()` on both sides (`SupportsControl()` forwards to it);
`SendControl` returns `Unsupported` on a transport that does not override it. Every
control message is a unified envelope of kind `Control` (2), at most 1,100
bytes, so it is never fragmented. Control is never counted as a mocap frame.
Delivery per transport:

| Transport | Carriage | Delivery | Notes |
|-----------|----------|----------|-------|
| **Loopback** | Control items on the channel's shared queue, in order with frames and audio, with a limit of their own | Reliable, ordered | Up to 1,024 envelopes wait; further sends are refused and the publisher retries. A full frame queue never refuses control |
| **TCP** | Envelope in a TCP frame, a control item on the shared send queue with mocap and audio, with a cap of its own | Reliable, ordered with frames | `SendControl` is refused while no client is connected; events retry until their TTL, values are repaired by the next snapshot. A full frame budget never refuses control |
| **UDP** | One datagram per envelope, queued and sent by the worker | Unreliable, unordered | Never fragmented; refused if `udp.maxdatagram` is below the envelope size. Events rely on redundant copies, values on snapshots |
| **NNG** | Envelope on the same socket and queue as frames | Pair and push/pull: reliable, ordered. Pub/sub: treated as unreliable | Covered by tests in pub/sub, pair/pair and push/pull |
| **MoQ** | Separate publisher on `control/<session>` (track name as for mocap), announced on every connect with stream delivery whatever `delivery_mode` says | Treated as unreliable; not ordered against mocap (MoQ orders nothing across tracks) | `SendControl` is refused until the control track exists. A custom `track_namespace` without a `mocap/` or `audio/` prefix gets `control/` prepended. Receivers subscribe whenever a control sink is set; failures log at Verbose. Control never moves a frame, byte or drop counter |
| **WebRTC** (add-on) | Reliable, ordered LiveKit data on the `__o3d.ctl` label | Receiver queue capped at 1,024 envelopes, delivered on Poll | Refused while not connected; classified by envelope bytes, so plain mocap on `__o3d.ctl` stays mocap |

**Redundancy:** each event is sent `ControlEventRedundancy` times (default 3,
range 1–5) on consecutive sender ticks, on every transport, and receivers
de-duplicate by event id. **Values** are re-sent as a full snapshot every
`ControlSnapshotIntervalSeconds` (default 1 s), so lossy links and late
joiners converge within about one interval.

---

## 5. Audio Implementation Comparison

### Audio Codec Support

| Transport | Codec Options | Encoding Location | API Boundary |
|-----------|--------------|------------------|--------------|
| **Loopback** | PCM16, Opus (via `O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **NNG** | PCM16, Opus (via `O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **TCP** | PCM16, Opus (via `O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **UDP** | PCM16, Opus (via `O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **MoQ** | PCM16, Opus (via `O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **WebRTC** | **Opus (LiveKit internal)** | **LiveKit FFI** | **PCM float→int16** |

**WebRTC is the only outlier.** Every other transport — including MoQ — encodes
on the sender through the shared `O3DAudio::FFrameEncoder` and exchanges PCM16 at
the API boundary. WebRTC differs only because LiveKit performs Opus
encode/decode internally behind its FFI.

### Audio Sink Implementations

**Sender Side** (`IO3DSenderAudioSink`):
- **Input**: PCM float (normalized -1.0 to 1.0)
- **Loopback/NNG/Sockets/MoQ**: Use `FO3DSenderAudioSinkBase` helper
  - Encodes to PCM16 or Opus
  - Wraps in unified message format
  - Enqueues/sends through transport
  - MoQ uses the same sink with bare audio payloads and publishes them on a dedicated audio track
- **WebRTC**: Direct conversion to int16 + `lk_publish_audio_pcm_i16()`

**Receiver Side** (`IO3DReceiverAudioSink`):
- **Input**: PCM16 (via `O3DS::FAudioFrameMeta`)
- **All transports**: Decode incoming audio frames
- **MoQ**: Subscribes to the audio track independently of the mocap track
- **WebRTC**: Receives pre-decoded PCM16 from LiveKit callback

### Receive-side playback

`O3DRemoteAudioComponent` (`Open3DReceiver/Public/O3DRemoteAudioComponent.h`)
provides Unreal-side playback — volume/pitch, attenuation, submix sends, source
effect chains, concurrency and spatialization.

**It is transport-agnostic.** It lives in the `Open3DReceiver` module and
contains no reference to any specific transport, so it applies equally to every
transport in this document — it is not a WebRTC feature.

---

## 6. Error Handling & Reliability

### Loopback
- **Queue overflow**: Drop with rate-limited logging
- **Channel shutdown**: Weak pointers prevent dangling references
- **No network errors**: In-process only

### NNG
- **Send errors**: Counted in `SendErrors` (and `DroppedFrames` for frames); a closed socket is reopened with backoff. No peer or a full NNG buffer drops the oldest frame
- **Queue overflow**: The newest frame is refused (`DroppedBackpressure`); queued frames are never discarded
- **Receive errors**: Counted in `ReceiveErrors`; a dialing socket is reopened with backoff
- **Pipe events**: Track connection count for availability
- **Socket errors**: Graceful socket closure on Stop()

### TCP
- **Connection loss**: Receiver auto-reconnects with exponential backoff; a pending connect times out after `tcp.connecttimeout`
- **Slow receiver**: Sender waits for socket space; drops the client only after `tcp.stalltimeout` without progress
- **Send errors**: Drop the frame in progress, close the client, wait for a new connection
- **Framing errors**: Skip to the next frame magic in one pass; oversize frames are counted in `DroppedFrames`
- **Queue overflow**: New frames dropped whole and counted; frames older than `tcp.maxqueueage` dropped before sending

### UDP
- **Packet loss**: Silent drop (unreliable transport)
- **Fragment timeout**: Discard incomplete reassembly
- **Send errors**: Log, count in `DroppedFrames` and `SendErrors`, continue (best-effort)
- **Queue overflow**: Oldest waiting frames dropped and counted; callers refused only at twice the soft cap
- **No connection state**: Stateless operation (`Connecting` only while a host name resolves)
- **Windows**: `WSAECONNRESET` from an ICMP port-unreachable only affects `recv`; the sender never reads, and the receiver ends that `Poll` and reads again on the next

### WebRTC
- **Connection failures**: LiveKit automatic reconnection
- **Send refused by LiveKit**: `DroppedBackpressure`, counted in `DroppedFrames` and `SendErrors`
- **Receiver backlog**: frames over 16 MiB waiting for `Poll` are refused and counted in `DroppedFrames`
- **Network changes**: LiveKit ICE restart
- **Audio publish errors**: Log and return false
- **Token expiration**: User must refresh token

### MoQ
- **Connection failures**: automatic reconnection with capped, jittered backoff and a connect timeout
- **Queue overflow**: the newest frame is refused (`DroppedBackpressure`); frames whose track is not ready are dropped at the worker
- **Relay unreachable**: session reports failure; publisher/subscriber handles torn down in order
- **Subscribe failures**: retried on reconnect; mocap and audio tracks resubscribe independently
- **Missing DLL/exports**: `FMoQFfiSupport` validates exports at load and disables the transport rather than failing later

---

## 7. Performance Characteristics

| Transport | Latency | Throughput | CPU Usage | Memory Overhead |
|-----------|---------|-----------|-----------|----------------|
| **Loopback** | <1ms | Unlimited | Minimal | Queue size × frame size |
| **NNG** | ~1-5ms LAN | High | Low-Medium | 4MB queue default |
| **TCP** | ~1-10ms LAN | High | Medium | Framing buffers + queue |
| **UDP** | <1ms LAN | High | Low | Up to 4 frames waiting (oldest dropped) |
| **WebRTC** | 20-100ms+ | Medium | High | LiveKit internal |
| **MoQ** | 20-100ms+ (relay RTT) | Medium | Medium | moq-ffi internal + track buffers |

**Notes**:
- Latency varies significantly with network conditions
- WebRTC latency includes signaling, encoding, and jitter buffering
- TCP/UDP latency depends on round-trip time (RTT)
- MoQ latency is dominated by the client→relay→client path, so relay placement matters more than raw bandwidth
- MoQ and WebRTC figures are indicative only; neither has been benchmarked in this repository

---

## 8. Testing & Validation

Transport tests live in the editor-only `Open3DBroadcastTests` module (`Source/Open3DBroadcastTests/Private/Transport/<Name>/`, ADR 0006), except WebRTC, whose tests stay in its module until the add-on's test module exists (WP-F11). Every transport also runs the conformance suite, `Open3DBroadcast.Conformance.<Transport>.<Case>` (lifecycle, rejected sends, backpressure, concurrent sends, monotonic stats, byte-exact round trip).

| Transport | Test File | Coverage |
|-----------|-----------|----------|
| **Loopback** | `LoopbackAudioTests.cpp`, `LoopbackLifetimeTests.cpp` | Audio roundtrip, start/stop lifetime |
| **NNG** | `NngTransportTests.cpp`, `NngLifetimeTests.cpp`, `NngModeRoleTests.cpp`, `NngSharedBlocksTests.cpp` | Pub/sub round trip, queue limit, receive demux, mode and role pairs, start/stop lifetime, refuse-newest under backpressure, audio and control independent of the frame budget, Stop under load |
| **Sockets** | `SocketsAudioTests.cpp`, `SocketsLifetimeTests.cpp`, `SocketsTcpTransportTests.cpp`, `SocketsTcpSharedBlocksTests.cpp`, `SocketsUdpSharedBlocksTests.cpp` | TCP/UDP audio, start/stop lifetime, TCP burst, slow reader, reconnect, keepalive, audio and control independent of the frame budget (TCP and UDP), receiver backoff on its worker, UDP drop-oldest under backpressure, Stop under load (TCP and UDP) (framing parser: core `test/tcp_stream_parser_tests.cpp`) |
| **WebRTC** | `WebRTCTransportTests.cpp`, `WebRTCPerSubjectTests.cpp`, `WebRTCFunctionalTests.cpp`, `WebRTCControlTests.cpp`, `WebRTCSharedBlocksTests.cpp` (fake LiveKit) | Transport + per-subject routing, token fetch, control, receiver hand-off limit and order, audio independent of the frame queue and the data channel, Stop under load |
| **MoQ** | `MoQSenderTests.cpp`, `MoQReceiverTests.cpp`, `MoQSessionWrapperTests.cpp`, `MoQTrackNamespaceTests.cpp`, `MoQFunctionalTests.cpp`, `MoQLifetimeTests.cpp`, `MoQControlTests.cpp`, `MoQSharedBlocksTests.cpp` (fake moq-ffi); `Network/MoQ/MoQRelayNetworkTests.cpp` (real relay, opt-in) | Session lifecycle, track naming, reconnect and backoff, control, refuse-newest under backpressure, audio and control independent of the frame budget, Stop under load, relay integration |

**Common Test Patterns**:
- Initialize sender/receiver
- Start both endpoints
- Send test frames
- Poll receiver
- Validate received data
- Check stats counters

---

## 9. Configuration Examples

### Loopback
```cpp
FO3DTransportConfig Config;
Config.Transport = "loopback";
Config.Uri = "loopback://test-channel";
Config.AdvancedParams.Add("loopback.queue", "128");
Config.AdvancedParams.Add("loopback.audioqueue", "64");
```

### NNG (Pub/Sub)
```cpp
FO3DTransportConfig SenderConfig;
SenderConfig.Transport = "nng";
SenderConfig.Uri = "tcp://0.0.0.0:9000";
SenderConfig.AdvancedParams.Add("nng.mode", "pub");
SenderConfig.AdvancedParams.Add("nng.role", "server");

FO3DTransportConfig ReceiverConfig;
ReceiverConfig.Transport = "nng";
ReceiverConfig.Uri = "tcp://localhost:9000";
ReceiverConfig.AdvancedParams.Add("nng.mode", "sub");
ReceiverConfig.AdvancedParams.Add("nng.role", "client");
ReceiverConfig.AdvancedParams.Add("nng.topic", "mocap/");
```

### TCP
```cpp
FO3DTransportConfig SenderConfig;
SenderConfig.Transport = "tcp";
SenderConfig.Uri = "tcp://0.0.0.0:8000";

FO3DTransportConfig ReceiverConfig;
ReceiverConfig.Transport = "tcp";
ReceiverConfig.Uri = "tcp://localhost:8000";
```

### UDP (Broadcast)
```cpp
FO3DTransportConfig SenderConfig;
SenderConfig.Transport = "udp";
SenderConfig.Uri = "udp://*:7000";  // Broadcast

FO3DTransportConfig ReceiverConfig;
ReceiverConfig.Transport = "udp";
ReceiverConfig.AdvancedParams.Add("bind", "0.0.0.0");
ReceiverConfig.AdvancedParams.Add("port", "7000");
```

### WebRTC
```cpp
FO3DTransportConfig SenderConfig;
SenderConfig.Transport = "webrtc";
SenderConfig.Uri = "wss://myserver.livekit.cloud";
SenderConfig.Token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";

FO3DTransportConfig ReceiverConfig;
ReceiverConfig.Transport = "webrtc";
ReceiverConfig.Uri = "wss://myserver.livekit.cloud";
ReceiverConfig.Token = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...";
ReceiverConfig.StreamId = "subject-name-filter";
```

---

### MoQ
```cpp
FO3DTransportConfig Config;
Config.Transport = "moq";
Config.AdvancedParams.Add("relay_url", "https://relay.example.com");
Config.AdvancedParams.Add("track_namespace", "mocap/session-1");
Config.AdvancedParams.Add("track_name", "characterA");
Config.AdvancedParams.Add("audio_namespace", "audio/session-1");
Config.AdvancedParams.Add("delivery_mode", "datagram");
```

---

## 10. Key Differences Summary

### Threading Philosophy
- **Loopback**: Pure synchronous (no threads)
- **NNG**: Async send thread (shared `FO3DTransportWorker`), sync receive polling over NNG's own I/O threads
- **TCP**: Async send thread and async receive thread (shared `FO3DTransportWorker`); delivery from `Poll`
- **UDP**: Async send thread (shared `FO3DTransportWorker`), sync receive polling
- **WebRTC**: Event-driven FFI callbacks; sends on the caller's thread, receive hand-off to `Poll`
- **MoQ**: Async send thread (shared `FO3DTransportWorker`), dispatcher thread (`FMoQAsyncDispatcher`) for FFI callbacks, delivery from `Poll`

### Network Topology
- **Loopback**: In-process only
- **NNG**: Flexible (1:1, 1:N, N:M depending on pattern)
- **TCP**: 1:1 (sender accepts single receiver)
- **UDP**: 1:N (broadcast capable)
- **WebRTC**: N:M (room-based)
- **MoQ**: N:M (relay fan-out, addressed by track namespace + name)

### Reliability Trade-offs
- **Loopback**: Reliable, in-memory queues
- **NNG**: Reliable, TCP-based with queue backpressure
- **TCP**: Reliable, ordered, with framing overhead
- **UDP**: **Unreliable**, low-latency, best-effort
- **WebRTC**: Reliable with managed retry/jitter buffering
- **MoQ**: Reliable, ordered within a track; QUIC handles loss recovery

### Platform Coverage
- **Loopback**: ✅ All platforms
- **NNG**: ⚠️ Win64 only (extensible to other platforms)
- **TCP/UDP**: ✅ All platforms (Unreal Sockets abstraction)
- **WebRTC**: ⚠️ Win64 only (extensible to other platforms)
- **MoQ**: ⚠️ Win64 only (other platforms auto-disable the transport)

---

## 11. Recommendations by Use Case

| Use Case | Recommended Transport | Rationale |
|----------|----------------------|-----------|
| **Local testing** | Loopback | Zero network overhead, deterministic |
| **LAN production** | TCP | Reliable, simple config, no external deps |
| **Low-latency LAN** | UDP | Minimal overhead, acceptable packet loss |
| **One-to-many LAN** | NNG (Pub/Sub) or UDP (Broadcast) | Efficient distribution |
| **Cloud/WAN** | WebRTC | NAT traversal, managed infrastructure |
| **Development/debugging** | Loopback or TCP | Loopback for unit tests, TCP for integration |
| **Firewall traversal** | WebRTC or MoQ | STUN/TURN, or outbound QUIC to a relay |
| **Cloud/WAN without a LiveKit server** | MoQ | Relay-mediated; no signalling service to operate |
| **Load balancing** | NNG (Push/Pull) | Automatic distribution across workers |

---

## 12. Future Extensibility

All modules follow consistent patterns for future enhancement:

**Common Extension Points**:
- ✅ Audio codec plugins (via `O3DAudio::FFrameEncoder` registry)
- ✅ Transport registry (dynamic registration in module startup)
- ✅ Stats collection (standardized `FO3DTransportStats`)
- ✅ Advanced params (key-value override mechanism)

**Platform Expansion**:
- NNG, WebRTC and MoQ currently Win64-only but architecturally ready for Linux/Mac
- Build.cs files have placeholder platform detection

**Protocol Versions**:
- Unified message format supports future extensions
- Version fields in serialized payloads

---

## Conclusion

The Open3DTransport architecture demonstrates excellent **functional parity** across all modules while providing **unique characteristics** tailored to different deployment scenarios:

- **Loopback** excels at testing with zero external dependencies
- **NNG** provides flexible messaging patterns for sophisticated topologies
- **TCP** offers reliable LAN streaming with broad platform support
- **UDP** delivers ultra-low latency for local networks
- **WebRTC** enables cloud-scale deployments with NAT traversal
- **MoQ** delivers relay-mediated cloud streaming over QUIC without operating a signalling service

All modules share 100% interface compatibility, making them **drop-in replacements** for each other, allowing developers to choose the optimal transport based on deployment requirements without changing application code.

---

## Document Metadata

**Generated**: 2025-11-15
**Last revised**: 2026-07-25 — added the MoQ transport, corrected the audio and
testing sections, and refreshed platform coverage.
**Codebase Version**: originally commit `ec7551b`; revision verified against `2a953bc`
**Modules Analyzed**:
- Open3DTransportLoopback
- Open3DTransportNNG
- Open3DTransportSockets (TCP + UDP)
- Open3DTransportWebRTC
- Open3DTransportMoQ

**File Locations Referenced**:
- Core interfaces: `Source/Open3DSender/Public/`, `Source/Open3DReceiver/Public/`
- Transport implementations: `Source/Open3DTransport*/Private/`
- Build configuration: `Source/Open3DTransport*/*.Build.cs`
