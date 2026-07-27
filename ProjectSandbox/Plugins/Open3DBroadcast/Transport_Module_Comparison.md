# In-Depth Comparison of Open3DTransport Modules

## Executive Summary

The Open3DBroadcast plugin features **5 transport modules** in a modular architecture, each implementing the `IOpen3DSender` and `IOpen3DReceiver` interfaces. All modules support both motion capture data streaming and audio transmission, but differ significantly in their network topologies, threading models, and intended use cases.

> **Naming:** *Open3DBroadcast* is the Unreal Engine plugin. *Open3DStream* is the
> streaming protocol and core C++ library (`o3ds`) it is built on.

---

## 1. Module Overview

| Module | Transport ID | Primary Use Case | Network Dependency | Third-Party Dependencies |
|--------|-------------|------------------|-------------------|-------------------------|
| **Loopback** | `loopback` | In-process testing/validation | None (in-memory) | None |
| **NNG** | `nng` | Advanced messaging patterns | Network (TCP) | NNG library (static) |
| **Sockets** | `tcp`, `udp` | Direct peer-to-peer | Network (TCP/UDP) | Unreal Sockets subsystem |
| **WebRTC** | `webrtc` | Cloud/NAT traversal | Network (WebRTC) | LiveKit FFI library |
| **MoQ** | `moq` | Cloud/NAT traversal over QUIC | Network (QUIC/WebTransport, via relay) | moq-ffi library |

---

## 2. Architecture & Interface Compliance

### Common Interface (100% Parity)

All modules implement the same interfaces with identical method signatures:

**IOpen3DSender** (`Open3DSender/Public/O3DSenderInterface.h:27-55`):
- `Initialize(Config)` - Resource allocation
- `Start()` - Begin networking
- `Stop()` - Cleanup (idempotent)
- `Send(SubjectList)` - Send motion capture frame
- `Tick(DeltaSeconds)` - Lightweight upkeep
- `GetStats()` - Performance metrics
- `SupportsAudio()` - Audio capability flag
- `CreateAudioSink(AudioConfig)` - Audio sink factory

**IOpen3DReceiver** (`Open3DReceiver/Public/O3DReceiverInterface.h:22-42`):
- `Initialize(Config)` - Resource allocation
- `SetConsumer(Consumer)` - Frame consumer registration
- `Start()` - Begin networking
- `Stop()` - Cleanup
- `Poll()` - Process incoming data
- `GetStats()` - Performance metrics
- `SupportsAudio()` - Audio capability flag
- `SetAudioSink(Sink, AudioConfig)` - Audio sink registration

**Audio Interfaces**:
- `IO3DSenderAudioSink` - PCM float submission
- `IO3DReceiverAudioSink` - PCM16 delivery

---

## 3. Detailed Module Comparison

### 3.1 **Loopback Transport**

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportLoopback/`

**Architecture**:
- In-process, shared-memory channel communication
- Channel registry with ref-counted channel objects
- Two separate queues: mocap data and audio
- Lock-free MPSC (Multi-Producer Single-Consumer) queues

**Key Classes**:
- `FO3DLoopbackSender` (`LoopbackSender.h:7`)
- `FO3DLoopbackReceiver` (`LoopbackReceiver.h:8`)
- `FO3DLoopbackChannel` (`LoopbackChannel.h:37`) - Shared channel state

**Threading Model**:
- **Synchronous** - No background threads
- Sender enqueues to shared channel on `Send()` call
- Receiver dequeues on `Poll()` call
- Thread-safe via atomic counters and MPSC queues

**Configuration** (`LoopbackChannel.cpp`):
- `channel` - Channel key (default: URI or "default")
- `loopback.queue` - Queue capacity (default: 64 frames)
- `loopback.audioqueue` - Audio queue capacity (default: 32 frames)

**Audio Support**:
- ✅ Full support with configurable codec
- Uses `O3DAudio::FFrameEncoder`/`FFrameDecoder`
- Separate audio queue with overflow protection
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
- Async worker thread for send operations
- Pipe event callbacks for connection tracking

**Key Classes**:
- `FO3DNngSender` (`NngSender.h:13`)
- `FO3DNngReceiver` (`NngReceiver.h:12`)
- `FNngSenderRunnable` - Background send thread
- `FNngSocketWrapper` - RAII socket management

**Supported Messaging Patterns** (`NngHelpers.h:17-24`):
| Pattern | Sender Mode | Receiver Mode | Topology |
|---------|------------|---------------|----------|
| **Pub/Sub** | Pub (Publisher) | Sub (Subscriber) | 1:N broadcast with topic filtering |
| **Pair** | Pair | Pair | 1:1 exclusive connection |
| **Push/Pull** | Push | Pull | N:M load balancing |

**Roles** (`NngHelpers.h:26-31`):
- **Server** - Listen/bind mode (accepts connections)
- **Client** - Dial/connect mode (initiates connections)

**Threading Model**:
- **Sender**: Async worker thread (`FNngSenderRunnable`)
  - Dedicated send thread with wake event
  - MPSC queue for payload buffering
  - Backpressure via queue byte limit (4MB default)
  - Exponential backoff on errors
- **Receiver**: Synchronous polling
  - Non-blocking `nng_recv()` on `Poll()`
  - Auto-reconnect with backoff for client mode

**Configuration** (`NngHelpers.h:10-15`):
- `nng.mode` - Messaging pattern: "pub", "sub", "pair", "push", "pull"
- `nng.role` - Connection role: "server", "client"
- `host` - Hostname/IP
- `port` - Port number
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
- ✨ **Exponential backoff** - Automatic reconnection with increasing delays
- ⚠️ **Platform limitation** - Currently Win64 only (`Open3DTransportNNG.Build.cs:20-27`)
- ⚠️ **External dependency** - Requires NNG library

**Connection State Tracking** (`NngSender.cpp:116-143`):
- Pipe add/remove callbacks
- Connection count maintained via `FThreadSafeCounter`
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
- `FO3DSocketsTcpSender` (`SocketsTcpSender.h:24`)
- `FO3DSocketsTcpReceiver` (`SocketsTcpReceiver.h:15`)

**Threading Model**:
- **Sender**: Async worker thread
  - Listen socket on main thread
  - Background send thread with MPSC queue
  - Frame-based protocol (4-byte length header + payload)
  - Single client connection (new client disconnects old)
- **Receiver**: Synchronous with state machine
  - Connection state machine (`EState` enum: `SocketsTcpReceiver.h:31-38`)
  - States: Disconnected → Connecting → Connected → ReadingHeader → ReadingPayload
  - Automatic reconnection with exponential backoff
  - Buffered reads with progressive framing

**Connection Model**:
- **Sender** = Server (listens for connections)
- **Receiver** = Client (connects to sender)

**Framing Protocol** (`SocketsTcpSender.cpp:SendFramed`):
```
[4 bytes: payload size (little-endian)] [N bytes: payload]
```

**Configuration**:
- `host` - Hostname/IP
- `port` - Port number
- `tcp.timeout` - Connection timeout seconds (default: 5.0)
- `bind` - Bind address for sender

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
- `FO3DSocketsUdpSender` (`SocketsUdpSender.h:21`)
- `FO3DSocketsUdpReceiver` (`SocketsUdpReceiver.h:19`)

**Threading Model**:
- **Sender**: Synchronous (sends on `Send()` call)
  - Direct socket write, no queuing
  - Automatic fragmentation for large payloads
- **Receiver**: Synchronous polling
  - Non-blocking receive on `Poll()`
  - Fragment reassembly state machine

**Fragmentation System** (`o3ds/udp_fragment.h`):
- **MTU awareness**: Default 1200 bytes (configurable)
- **Max datagram**: 64KB default (configurable)
- **Fragment header**: Sequence ID + fragment index + total fragments
- **Reassembly**: `UdpMapper` tracks in-flight fragment sets
- **Timeout**: Incomplete fragment sets are discarded

**Configuration**:
- `host` - Hostname/IP
- `port` - Port number
- `bind` - Bind address for receiver
- `udp.broadcast` - Enable broadcast mode (default: false)
- `udp.mtu` - MTU size in bytes (default: 1200)
- `udp.maxdatagram` - Max datagram size before fragmentation (default: 64000)

**Broadcast Support** (`SocketsUdpSender.cpp:228-271`):
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

### 3.4 **WebRTC Transport**

**Location**: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/`

**Architecture**:
- Based on [LiveKit FFI](https://github.com/livekit/client-sdk-rust) library
- Cloud-signaling with NAT traversal (STUN/TURN)
- Room-based communication model
- Opus encoding/decoding handled by LiveKit internally

**Key Classes**:
- `FO3DWebRTCSender` (`WebRTCSender.h:20`)
- `FO3DWebRTCReceiver` (`WebRTCReceiver.h:21`)
- `LkClientHandle` - Opaque LiveKit FFI handle

**Threading Model**:
- **Event-driven** via LiveKit FFI callbacks
- LiveKit manages internal thread pool
- Callbacks invoked on FFI threads:
  - `OnConnectionState` - Connection state changes
  - `OnDataReceived` - Incoming mocap data (receiver)
  - `OnAudioReceived` - Incoming PCM16 audio (receiver)
- Synchronization via mutexes for state access

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
- `FO3DMoQSender` (`MoQSender.h:45`)
- `FO3DMoQReceiver` (`MoQReceiver.h:33`)
- `FMoQSessionWrapper` — session lifecycle over the FFI boundary
- `FMoQPublisherHandle` / `FMoQSubscriberHandle` — RAII handles for FFI objects
- `FMoQAsyncDispatcher` (`MoQAsyncDispatcher.h:17`) — `FRunnable`, a shared dispatcher thread
- `FMoQFfiSupport` — DLL load and export validation

**Threading Model**:
- **Dedicated dispatcher thread** (`FMoQAsyncDispatcher`, an `FRunnable` singleton) marshals async FFI work
- FFI callbacks deliver connection-state and subscriber-data events
- Handle types own FFI lifetime explicitly, so teardown ordering is enforced rather than incidental

**Audio**:
- Uses `FO3DMoQSenderAudioSink`, derived from the **shared `FO3DSenderAudioSinkBase`** — the same path as Loopback, NNG and Sockets
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
| **Stats Reporting** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Backpressure Handling** | ✅ Queue | ✅ Queue | ✅ Queue | ⚠️ None | ✅ LiveKit | ✅ Relay/QUIC |
| **Reconnection** | N/A | ✅ Auto | ✅ Auto | N/A | ✅ Auto | ✅ Auto |
| **Multiple Receivers** | ✅ Many | ✅ Pattern | ❌ Single | ✅ Broadcast | ✅ Room | ✅ Relay fan-out |
| **Reliable Delivery** | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **Ordered Delivery** | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ |
| **NAT Traversal** | N/A | ❌ | ❌ | ❌ | ✅ | ✅ |
| **Cross-Process** | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Platform Support** | All | Win64 | All | All | Win64 | Win64 |

**Audio Support is universal.** The base `IOpen3DSender` and `IOpen3DReceiver`
interfaces default `SupportsAudio()` to `false`, and every transport overrides it
to `true` on **both** the sender and receiver side — twelve overrides, no
exceptions. Audio is a plugin-level capability, not a property of any one
transport.

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
  - MoQ additionally publishes on a dedicated audio track (`FO3DMoQSenderAudioSink`)
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
- **Send errors**: Exponential backoff retry, queue backpressure
- **Receive errors**: Auto-reconnect for client mode, backoff on errors
- **Pipe events**: Track connection count for availability
- **Socket errors**: Graceful socket closure on Stop()

### TCP
- **Connection loss**: Receiver auto-reconnects with backoff
- **Send errors**: Drop frame, close client, wait for new connection
- **Framing errors**: Reset state machine, reconnect
- **Queue overflow**: Drop with backpressure stats

### UDP
- **Packet loss**: Silent drop (unreliable transport)
- **Fragment timeout**: Discard incomplete reassembly
- **Send errors**: Log and continue (best-effort)
- **No connection state**: Stateless operation

### WebRTC
- **Connection failures**: LiveKit automatic reconnection
- **Network changes**: LiveKit ICE restart
- **Audio publish errors**: Log and return false
- **Token expiration**: User must refresh token

### MoQ
- **Connection failures**: automatic reconnection after `ReconnectDelaySeconds`
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
| **UDP** | <1ms LAN | High | Low | Minimal (no buffering) |
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

All transports include test files:

| Transport | Test File | Coverage |
|-----------|-----------|----------|
| **Loopback** | `LoopbackAudioTests.cpp` | Audio roundtrip |
| **NNG** | `NngTransportTests.cpp` | Connection patterns |
| **Sockets** | `SocketsAudioTests.cpp` | TCP/UDP audio |
| **WebRTC** | `WebRTCTransportTests.cpp`, `WebRTCPerSubjectTests.cpp` | Transport + per-subject routing |
| **MoQ** | `MoQSenderTests.cpp`, `MoQReceiverTests.cpp`, `MoQSessionWrapperTests.cpp`, `MoQTrackNamespaceTests.cpp`, `MoQCloudflareRelayTests.cpp` | Session lifecycle, track naming, relay integration |

MoQ currently has the broadest automation coverage of any transport.

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
- **NNG**: Async send thread, sync receive polling
- **TCP**: Async send thread, sync receive with state machine
- **UDP**: Fully synchronous
- **WebRTC**: Event-driven FFI callbacks
- **MoQ**: Dedicated dispatcher thread (`FMoQAsyncDispatcher`, an `FRunnable`) plus FFI callbacks

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
