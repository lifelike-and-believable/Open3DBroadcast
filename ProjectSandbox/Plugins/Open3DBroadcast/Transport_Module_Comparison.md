# In-Depth Comparison of Open3DTransport Modules

> This document describes how the transports behave, for integrators who choose a transport, tune it, or write their own. To set one up, start with the [USER_GUIDE](USER_GUIDE.md) ("Transport Modules" and "Transport Options Reference").

## Executive Summary

Open3DBroadcast has six transports in five modules. Loopback, TCP, UDP, NNG and MoQ ship in the Open3DBroadcast plugin; WebRTC ships in the free **Open3DBroadcastWebRTC** add-on plugin, installed next to it. Each implements the `IOpen3DSender` and `IOpen3DReceiver` interfaces and registers under a name that the sender's **Transport Name** and the LiveLink source's **Transport** list. Every transport carries motion capture, audio and control. They differ in network topology, delivery guarantee, threading and intended use.

> **Available in:** every WebRTC section below applies only when the Open3DBroadcastWebRTC
> add-on is installed. The other transports are always present.

> **Naming:** *Open3DBroadcast* is the Unreal Engine plugin. *Open3DStream* is the
> streaming protocol and core C++ library (`o3ds`) it is built on.

---

## 1. Module Overview

| Module | Registered name | Primary Use Case | Network Dependency | Third-Party Dependencies |
|--------|-----------------|------------------|--------------------|--------------------------|
| `Open3DTransportLoopback` | **Loopback** | In-process testing | None (in-memory) | None |
| `Open3DTransportSockets` | **TCP**, **UDP** | Direct streaming between two machines on a LAN | Network (TCP or UDP) | Unreal `Sockets` and `Networking` modules |
| `Open3DTransportNNG` | **NNG** | Several receivers on a LAN (Pub/Sub), or a reliable link (Pair, Push/Pull) | Network (TCP) | NNG library (static) |
| `Open3DTransportMoQ` (Experimental) | **MoQ** | Streaming through a relay | Network (QUIC, via a relay) | moq-ffi library (`moq_ffi.dll`) |
| `Open3DTransportWebRTC` (add-on) | **WebRTC** | Internet, NAT traversal, LiveKit rooms | Network (WebRTC, via a LiveKit server) | LiveKit FFI library (`livekit_ffi.dll`) |

Every module is Win64 only, for editor and game targets (no Server or Program targets).

---

## 2. Architecture & Interface Compliance

### Common Interface

All transports implement the same interfaces, declared in the `Open3DShared` module under `Public/Transport/`. Senders take serialized frames only; there is no `Send(SubjectList)`.

**IOpen3DSender** (`O3DSenderInterface.h`):
- `Initialize(Config)` / `Start()` - return `FO3DTransportResult` (an error code such as `InvalidConfig`, `NotRunning` or `AddressInUse`, and a message)
- `Stop()` - Cleanup (idempotent)
- `SendSerialized(FO3DSendPayload&&)` - Send one serialized frame; returns `EO3DSendResult` (`Queued`, `DroppedBackpressure`, `NotRunning`, `NotConnected`, `Invalid`, `TooLarge`)
- `SendControl(Envelope, Len)` - Send one control envelope; returns `EO3DSendResult` (`Unsupported` by default)
- `Tick(DeltaSeconds)` - Lightweight upkeep
- `GetStats()` - Counters, including the connection state
- `GetCapabilities()` - `FO3DTransportCapabilities` (audio, control, delivery guarantee, payload limit)
- `GetConnectionState()` / `SetStateChangedCallback()` - `Idle`, `Connecting`, `Connected`, `Reconnecting`, `Failed`
- `CreateAudioSink(AudioConfig)` - Audio sink factory
- `SetPeerJoinedCallback(Callback)` - Called, on any thread, when a receiver connects or a pipe or subscriber is added, so the next frame is a full sync. TCP and NNG implement it; the default does nothing

**IOpen3DReceiver** (`O3DReceiverInterface.h`):
- `Initialize(Config)` / `Start()` - return `FO3DTransportResult`; `Start()` without a consumer is `NoConsumer`
- `SetConsumer(Consumer)` - Frame consumer registration. The receiver calls the consumer's `SubmitFrame(Subject, TConstArrayView<uint8>, Time)` with a view of the received bytes (valid for the call), or `SubmitFrameOwned(Subject, TArray<uint8>&&, Time)` when it can give its own buffer away (Loopback, MoQ, WebRTC); the owned form defaults to the view form
- `Stop()` - Cleanup
- `Poll()` - Process incoming data
- `GetStats()` - Counters
- `GetCapabilities()`, `GetConnectionState()`, `SetStateChangedCallback()` - as on the sender
- `SetAudioSink(Sink, AudioConfig)` - Audio sink registration

**Registration:** a transport module registers one `FO3DTransportDescriptor` per transport name with `FO3DTransportRegistry` (`O3DTransportRegistry.h`): its sender and receiver factories, the functions that turn the options into a transport config, its capabilities, and its option schema (which the editor panels show). The registry answers the capability question before an instance exists: `FO3DTransportRegistry::GetCapabilities(Name, Config, Out)`. A descriptor built against another transport API version (`O3D_TRANSPORT_API_VERSION`) is refused. The USER_GUIDE's "Custom Transport Implementation" has an example.

**Audio Interfaces**:
- `IO3DSenderAudioSink` - PCM float submission
- `IO3DReceiverAudioSink` - PCM16 delivery

**Shared building blocks** (`Open3DShared/Public/Transport/`): `FO3DSendQueue` (a bounded send queue with separate limits for frames, audio and control), `FO3DTransportWorker` (the thread that owns a socket and drains the queue), `FO3DUnifiedReceiveDemux` (routes received messages to the frame consumer, the audio sink and the control sink) and `FO3DQueuedSenderAudioSink`. Loopback, TCP, UDP, NNG and MoQ use them.

---

## 3. Detailed Module Comparison

### 3.1 **Loopback Transport**

**Location**: `Source/Open3DTransportLoopback/`

**Architecture**:
- In-process channels; a sender and a receiver with the same channel name share one channel
- Each channel is one shared `FO3DSendQueue` carrying mocap, audio and control items, each kind with its own limit
- The receiver's `Poll` hands each item to `FO3DUnifiedReceiveDemux`; the audio sink is the shared `FO3DQueuedSenderAudioSink`

**Threading Model**:
- **Synchronous** - No background threads
- The sender enqueues on the shared channel in `SendSerialized()`; the frame's buffer reaches the consumer without a copy (`SubmitFrameOwned`)
- The receiver dequeues in `Poll()`

**Configuration**:
- `channel` (**Channel Name**) - Channel key (default: `default`). Leading and trailing spaces and letter case are ignored
- `loopback.maxqueue` (**Queue Capacity**) - Mocap frames the channel holds, 1 to 4096; a full channel refuses new frames (default: 64)
- `loopback.maxaudioqueue` - Audio frames the channel holds (default: 32)

**Audio Support**:
- PCM16 and Opus, encoded on the sender (`O3DAudio::FFrameEncoder`)
- Audio has its own limit on the shared channel queue; when it is full the newest audio frame is refused

**Characteristics**:
- No network, so no network delay or loss
- Several independent channels per process
- A full queue refuses new frames and logs it
- Single process only: it cannot connect two processes

**Use Cases**:
- Testing a capture pipeline or a character setup
- Sender and receiver in one editor or game
- Development and debugging without network effects

---

### 3.2 **NNG Transport**

**Location**: `Source/Open3DTransportNNG/`

**Architecture**:
- Based on the [NNG](https://nng.nanomsg.org/) library, over TCP (`tcp://` addresses only)
- Three messaging patterns
- `FO3DSendQueue` and `FO3DTransportWorker` for sends, `FO3DUnifiedReceiveDemux` for receives
- NNG pipe callbacks track connections

**Supported Messaging Patterns**:

| Pattern | Sender Mode | Receiver Mode | Topology | Delivery |
|---------|-------------|---------------|----------|----------|
| **Pub/Sub** | `pub` (**Publisher**) | `sub` (**Subscriber**) | 1:N; every subscriber receives every message | Unreliable |
| **Pair** | `pair` (**Pair**) | `pair` (**Pair**) | 1:1 exclusive connection | Reliable, ordered |
| **Push/Pull** | `push` (**Push**) | `pull` (**Pull**) | NNG distributes messages across pullers: each message goes to one | Reliable, ordered |

There are no subscription topics: a subscriber receives everything the publisher sends.

**Roles** (`nng.role`):
- `server` (**Listen (server)**) - Listens for connections
- `client` (**Dial (client)**) - Dials the other end
- Default: the sender listens for Pub/Sub and Pair, the receiver listens for Push/Pull

**Threading Model**:
- **Sender**: `SendSerialized`, `SendControl` and the audio sink only enqueue on one `FO3DSendQueue`; an `FO3DTransportWorker` owns the socket and calls `nng_send` without blocking
  - Frames: a full queue (`nng.qmax` bytes, 4 MiB by default) refuses the newest frame. The worker drops the oldest frame when NNG cannot take it (no peer, NNG's send buffer full)
  - Audio (`nng.qmax` bytes) and control (1,024 envelopes) have budgets of their own
  - NNG redials dropped connections itself; the transport only reopens a socket that failed or was closed, with a backoff of 0.1 s to 5 s
- **Receiver**: Synchronous polling (NNG's own threads do the I/O)
  - Non-blocking `nng_recv()` in `Poll()`, at most 16 messages per call
  - Messages go to `FO3DUnifiedReceiveDemux`

**Configuration**:
- `nng.mode` (**Mode**) - `pub`, `pair` or `push` on the sender (default `pub`); `sub`, `pair` or `pull` on the receiver (default `sub`)
- `nng.role` (**Role**) - `server` or `client`; empty uses the usual role for the mode
- `host` (**Host**) - Address to listen on (default `0.0.0.0`) or to dial (default `127.0.0.1`); IPv6 in brackets
- `port` (**Port**) - 1 to 65535; default 6000 (Pub/Sub), 7000 (Pair), 8000 (Push/Pull)
- `nng.qmax` (**Queue Capacity (MiB)**) - Sender queue in bytes (default 4 MiB)

**Audio Support**:
- PCM16 and Opus, encoded on the sender
- Audio envelopes go through the same socket as mocap, with a queue budget of their own

**Characteristics**:
- Several patterns, so several topologies
- Connection tracking through NNG pipe callbacks
- Byte limits on the send queue
- No encryption and no authentication

**Use Cases**:
- **Pub/Sub**: One mocap source to several receivers on a LAN
- **Pair**: A reliable link to one receiver, opened from either end
- **Push/Pull**: A reliable link where the receiver listens

---

### 3.3 **Sockets Transport** (TCP + UDP)

**Location**: `Source/Open3DTransportSockets/`

**Architecture**:
- Two transports in one module: **TCP** and **UDP**
- Both use Unreal's `ISocketSubsystem`
- No encryption and no authentication

#### 3.3.1 **TCP Implementation**

**Threading Model**:
- **Sender**: worker thread (`FO3DTransportWorker`)
  - The listen socket is created on the game thread in `Start()`; the worker starts only if bind and listen succeed. The bind address must be an IP address or a wildcard (`0.0.0.0`, `*`)
  - The worker owns the client socket: it accepts, sends from the shared `FO3DSendQueue` (frames, audio and control in order, each written as one TCP frame), writes keepalives and notices a closed receiver. The game thread never waits on a send
  - Partial sends are retried until the frame is written; the client is dropped only on a socket error or after `tcp.stalltimeout` without progress. Frames are dropped whole, never partly written
  - One client at a time; the next receiver is accepted as soon as the current one closes
- **Receiver**: worker thread (`FO3DTransportWorker`); `Poll()` on the game thread delivers
  - The worker resolves the host (names included, never on the game thread), connects, reads and frames; it reports Connecting, Connected and Reconnecting
  - Every complete frame in a read is delivered; after garbage it resynchronizes on the next frame marker; lengths above `tcp.maxframe` are rejected without allocating them
  - Payloads wait in a bounded hand-off queue (8 MiB, or one `tcp.maxframe` if larger); when it is full the worker stops reading, so TCP flow control holds the sender back
  - Each `Poll()` handles at most 256 payloads or 8 MiB; the rest waits
  - Reconnects with exponential backoff with ±20% jitter (`tcp.backoff` doubling up to `tcp.maxbackoff`); the backoff resets only after a connection delivers data

**Connection Model**:
- **Sender** = Server (listens)
- **Receiver** = Client (connects to the sender)

**Framing Protocol**:
```
[14 bytes: marker 00 FF 03 FE "O3DS-START"] [4 bytes: payload size (little-endian)] [N bytes: payload]
```
While idle for `tcp.keepalive` ms the sender writes a keepalive frame whose payload is a 24-byte envelope header (`O3DU`, kind Audio, payload size 0). Receivers ignore it as data, but it resets their idle timer.

**Configuration**:
- `bind` (**Bind Address**) - Sender: local address to listen on (default `0.0.0.0`)
- `port` (**Port**) - 1 to 65535 (default 17700)
- `host` (**Remote Host**) - Receiver: the sender's address (default `127.0.0.1`); IPv6 in brackets
- `tcp.timeout` (**Connection Timeout (seconds)**) - Receiver: seconds without any data (frames or keepalives) before it reconnects (default 5)
- `tcp.connecttimeout` - Receiver: seconds a connect may stay pending before it is abandoned and retried (default 5)
- `tcp.maxframe` - Receiver: largest frame payload accepted, in bytes (default 4194304, min 1024, max 52428800)
- `tcp.backoff` - Receiver: first reconnect delay in ms, doubled after each failed attempt (default 500)
- `tcp.maxbackoff` - Receiver: longest reconnect delay in ms (default 5000)
- `tcp.maxqueue` - Sender: payload bytes of queued frames, and separately of queued audio; a frame that does not fit is refused (`DroppedBackpressure`). Control has its own cap of 1,024 envelopes (default 4194304, min 65536)
- `tcp.maxqueueage` - Sender: frames and audio that waited longer than this many ms are dropped before sending; control never expires; 0 disables (default 1000)
- `tcp.stalltimeout` - Sender: ms a frame may make no progress on a full socket before the client is dropped (default 2000, min 100)
- `tcp.keepalive` - Sender: ms of idleness before a keepalive is sent; 0 disables (default 1000). Keep it below the receiver's `tcp.timeout`

**Audio Support**:
- Same connection as mocap, reliable and ordered
- PCM16 and Opus, encoded on the sender

**Characteristics**:
- Reliable, ordered delivery
- The receiver reconnects on its own
- Queue limits bound the sender's memory
- One receiver at a time per sender

#### 3.3.2 **UDP Implementation**

**Threading Model**:
- **Sender**: `SendSerialized`, `SendControl` and the audio sink only enqueue on one `FO3DSendQueue`; an `FO3DTransportWorker` owns the socket and sends every item
  - Frames: drop-oldest. While the worker is behind, at most 4 frames (16 MiB) wait and older ones are discarded so the newest are sent; callers are refused (`DroppedBackpressure`) only at twice that
  - Audio (1 MiB) and control (1,024 envelopes) have budgets of their own and are never dropped for frames
  - Fragmentation of large messages happens on the worker
  - A host name resolves on the worker, with retries; the state is `Connecting` until then. An IP address resolves in `Initialize`
- **Receiver**: Synchronous polling
  - Non-blocking receive in `Poll()`
  - Fragment reassembly
  - Complete messages go to `FO3DUnifiedReceiveDemux`

**Fragmentation**:
- A message larger than `udp.mtu` (1200 bytes by default, fragment header included) is split into fragments of that size; control messages are never split
- **Fragment header**: message id, fragment index, fragment count
- **Reassembly**: tracks in-flight fragment sets per (sender address, message id); at most 8 in flight and 4 × `udp.maxframe` bytes in total, oldest evicted first
- **Timeout**: Incomplete fragment sets are discarded after 500 ms, or as soon as a newer message from the same sender completes
- **Poll bound**: each `Poll()` reads at most 1024 datagrams or 8 MiB; the rest stays in the socket buffer for the next poll

**Configuration**:
- `host` - Sender (**Destination Host**): the receiver's address, or a broadcast address (default `127.0.0.1`). Receiver (**Bind Address**): the local address to listen on (default `0.0.0.0`); it must be an IP address, `*`, empty or `localhost`
- `port` (**Port**) - 1 to 65535 (default 17800)
- `udp.broadcast` - Sender (**Enable UDP Broadcast**): allow sending to a broadcast address. Receiver (**Accept Broadcast Packets**): receive broadcast datagrams (default false)
- `udp.mtu` (**MTU**) - Sender only: largest datagram sent, header included; larger messages are split into fragments of this size (default 1200, 280 to 65507)
- `udp.maxdatagram` (**Max Datagram Bytes**) - Receiver: largest datagram accepted (default 64000, 512 to 65507). The sender reads it as the ceiling for control messages but does not show it
- `udp.maxframe` - Receiver: largest reassembled message accepted, in bytes (default 4194304, max 52428800)

**Broadcast**:
- A `host` of `*` sends to `255.255.255.255` and turns broadcast on; any other broadcast address needs `udp.broadcast` on the sender
- One-to-many on one subnet without listing receivers. There is no multicast

**Audio Support**:
- Same envelope format as the other transports, fragmented when needed
- PCM16 and Opus, encoded on the sender

**Characteristics**:
- Connectionless: no handshake
- Lowest latency of the network transports: nothing waits for a lost datagram
- Broadcast to one subnet
- Unreliable and unordered: a lost datagram, or one lost fragment, is a lost frame

**Use Cases**:
- **TCP**: Reliable streaming to one receiver on a LAN
- **UDP**: Lowest latency on a LAN; broadcast to several receivers on one subnet

---

### 3.4 **WebRTC Transport** (Open3DBroadcastWebRTC add-on)

**Location**: `Source/Open3DTransportWebRTC/` in the Open3DBroadcastWebRTC add-on plugin

**Architecture**:
- Based on the LiveKit FFI library (`livekit_ffi.dll`)
- Room-based: senders and receivers join a LiveKit room through a LiveKit server, which handles signalling and NAT traversal
- Mocap and control travel as LiveKit data; control uses the `__o3d.ctl` label
- Audio is published as LiveKit audio tracks; LiveKit encodes and decodes it with Opus

**Threading Model**:
- **Event-driven** via LiveKit FFI callbacks on LiveKit's threads
- **Sender**: `SendSerialized` and `SendControl` hand each message to LiveKit on the caller's thread; LiveKit's refusal is the backpressure (`DroppedBackpressure`). No send queue or worker
- **Receiver**: the data callback puts frames (refuse-newest, 16 MiB) and control (1,024 envelopes) on hand-off queues; `Poll` delivers them through `FO3DUnifiedReceiveDemux`. Audio goes from the audio callback straight to the sink
- **Reconnect**: LiveKit's own on the sender; on the receiver a no-data watchdog recreates the client

**Connection Model**:
- **Room-based**: senders and receivers are participants of one room
- **Token-based auth**: a LiveKit access token, set as a credential or fetched from a token endpoint
- **Delivery**: reliable and ordered by default; unreliable with `webrtc.prefer_lossy`. A frame of more than 15,000 bytes is refused (`TooLarge`). With `webrtc.prefer_lossy`, frames above 1,300 bytes are sent reliably

**Configuration** (keys; the add-on's USER_GUIDE has the details):
- `webrtc.url` - LiveKit server URL (for example `wss://myserver.livekit.cloud`)
- `webrtc.room` - Room name
- `webrtc.token` - Access token for manual mode. A credential: set it with **Set Transport Secret** or in the panel's credential row, never in saved options
- `webrtc.useAutoTokenFetch`, `webrtc.tokenEndpointUrl`, `webrtc.tokenRefreshLeadTimeSec` - Automatic token fetch; `webrtc.tokenEndpointAuth` is a credential
- `webrtc.prefer_lossy` - Unreliable delivery instead of reliable

**Audio Support**:
- The sender converts float PCM to 16-bit and publishes it on a LiveKit audio track per subject (`lk_audio_track_create`, `lk_audio_track_publish_pcm_i16`); LiveKit encodes it with Opus
- The receiver gets decoded PCM16 from a LiveKit callback

**Characteristics**:
- Works across networks and NAT, through a LiveKit server
- Room-based N:M topology
- Needs a LiveKit server and access tokens
- Runtime dependency on `livekit_ffi.dll`
- Each add-on build works only with the Open3DBroadcast release it was built for

**Use Cases**:
- Streaming over the internet
- Several participants in one room
- Networks with NAT or firewalls between the machines

---

### 3.5 **MoQ Transport** (Experimental)

**Location**: `Source/Open3DTransportMoQ/`

**Architecture**:
- Media over QUIC, draft-ietf-moq-transport-07, backed by the moq-ffi library (`moq_ffi.dll`). The relay must speak draft-07
- Data travels over QUIC to a **relay**; both ends connect out to it
- Publish/subscribe, addressed by **track namespace + track name**
- A sender publishes three tracks under one track name: mocap in its namespace (`mocap/<session>`), audio in `audio/<session>` and control in `control/<session>`

**Threading Model**:
- A shared dispatcher thread handles the FFI's asynchronous work; connection-state and subscriber-data events are handled on the game thread
- **Sender**: `SendSerialized`, `SendControl` and the audio sink enqueue on one `FO3DSendQueue` (refuse-newest; `queue_bytes` for frames, 1 MiB for audio, 1,024 control envelopes); an `FO3DTransportWorker` publishes each item on its track and drops items whose track is not ready
- **Receiver**: data callbacks go to a bounded hand-off queue; `Poll` routes them through `FO3DUnifiedReceiveDemux`
- **Reconnect**: the transport's own jittered backoff (0.5 s to 10 s) and connect timeout (`connect_timeout`, 15 s by default)

**Configuration**:
- `relay_url` (**Relay URL**) - The relay, for example `https://relay.example.com:443` (required)
- `track_namespace` (**Track Namespace (optional)**) - Namespace of the mocap track. Default: `mocap/<Subject Name>` on the sender, `mocap/default` on the receiver
- `track_name` (**Track Name (optional)**) - Default: the **Subject Name** on the sender, `primary` on the receiver
- `delivery_mode` (**Delivery Mode**) - Sender: `stream` (default) or `datagram`
- `queue_bytes` (**Queue Capacity (MiB)**) - Sender queue in bytes (default 8 MiB)
- `connect_timeout` - Seconds before an unfinished connection attempt is abandoned and retried (1 to 120, default 15)
- `moq.session` - The `<session>` part of the default namespace

The defaults differ between the ends, so set the namespace and the track name on both. Start a custom namespace with `mocap/`: the transport then puts audio on `audio/...` and control on `control/...` with the same rest of the namespace.

**Audio**:
- The shared `FO3DQueuedSenderAudioSink`, as on Loopback, TCP, UDP and NNG
- PCM16 or Opus, encoded on the sender, published on the audio track
- The receiver subscribes to the audio track independently of the mocap track

**Characteristics**:
- Outbound connections only: no port to open on either machine
- No signalling service to run; the relay is the meeting point
- Same audio pipeline and codec options as the LAN transports
- Rated unreliable in both delivery modes, so residual coding is not used over it
- Needs a reachable draft-07 relay; the plugin does not include one
- Runtime dependency on `moq_ffi.dll`; if the DLL or one of its functions is missing, the transport is not registered

**Use Cases**:
- Streaming between networks through a relay you run or have access to
- NAT traversal without a LiveKit server

---

## 4. Functional Parity Matrix

| Feature | Loopback | NNG | TCP | UDP | WebRTC | MoQ |
|---------|----------|-----|-----|-----|--------|-----|
| **Core Interfaces** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Send serialized frames** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Audio Support** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Control (events and values)** | Yes, own queue | Yes, in-band | Yes, in-band | Yes, in-band | Yes, `__o3d.ctl` data label | Yes, `control/` track |
| **Stats Reporting** | Yes | Yes | Yes | Yes | Yes | Yes |
| **Backpressure Handling** | Queue, refuses new frames | Queue, refuses new frames | Queue, refuses new frames | Queue, drops oldest frames | LiveKit refuses | Queue, refuses new frames |
| **Reconnection** | N/A | Automatic | Automatic (receiver) | N/A | Automatic | Automatic |
| **Multiple Receivers** | Yes, per channel | Pub/Sub: yes | No, one at a time | Yes, by broadcast | Yes, room | Yes, relay fan-out |
| **Reliable, Ordered Delivery** | Yes | Pair, Push/Pull: yes. Pub/Sub: no | Yes | No | Yes; no with `webrtc.prefer_lossy` | No |
| **NAT Traversal** | N/A | No | No | No | Yes | Yes |
| **Cross-Process** | No | Yes | Yes | Yes | Yes | Yes |
| **Platform Support** | Win64 | Win64 | Win64 | Win64 | Win64 | Win64 |

**Audio Support is universal.** Every transport reports `bAudioSend` and
`bAudioReceive` in `GetCapabilities()` on both the sender and the receiver side.

**Delivery guarantee** (`FO3DTransportCapabilities::Delivery`):
Loopback, TCP, NNG Pair and Push/Pull, and WebRTC report `ReliableOrdered`; UDP,
NNG Pub/Sub, MoQ (both delivery modes) and WebRTC with `webrtc.prefer_lossy` report `Unreliable`.
Residual coding is used only on `ReliableOrdered`.

**Control** (USER_GUIDE "Control Channel") is a one-way stream of
events and values from a sender to its receivers. Every transport reports
`bControl` in `GetCapabilities()` on both sides;
`SendControl` returns `Unsupported` on a transport that does not override it. Every
control message is an envelope of kind `Control`, at most 1,100
bytes, so it is never fragmented. Control is never counted as a mocap frame.
Delivery per transport:

| Transport | Carriage | Delivery | Notes |
|-----------|----------|----------|-------|
| **Loopback** | Control items on the channel's shared queue, in order with frames and audio, with a limit of their own | Reliable, ordered | Up to 1,024 envelopes wait; further sends are refused and the publisher retries. A full frame queue never refuses control |
| **TCP** | Envelope in a TCP frame, a control item on the shared send queue with mocap and audio, with a cap of its own | Reliable, ordered with frames | `SendControl` is refused while no client is connected; events retry until their time-to-live, values are repaired by the next snapshot. A full frame budget never refuses control |
| **UDP** | One datagram per envelope, queued and sent by the worker | Unreliable, unordered | Never fragmented; refused if `udp.maxdatagram` is below the envelope size. Events rely on redundant copies, values on snapshots |
| **NNG** | Envelope on the same socket and queue as frames | Pair and Push/Pull: reliable, ordered. Pub/Sub: treated as unreliable | |
| **MoQ** | Separate publisher on `control/<session>` (track name as for mocap), announced on every connect with stream delivery whatever `delivery_mode` says | Treated as unreliable; not ordered against mocap (MoQ orders nothing across tracks) | `SendControl` is refused until the control track exists. A custom `track_namespace` without a `mocap/` or `audio/` prefix gets `control/` prepended. Receivers subscribe whenever a control sink is set. Control never moves a frame, byte or drop counter |
| **WebRTC** (add-on) | Reliable, ordered LiveKit data on the `__o3d.ctl` label | Receiver queue capped at 1,024 envelopes, delivered on Poll | Refused while not connected; classified by envelope bytes, so plain mocap on `__o3d.ctl` stays mocap |

**Redundancy:** each event is sent `ControlEventRedundancy` times (default 3,
range 1 to 5) on consecutive sender ticks, on every transport, and receivers
drop duplicates by event id. **Values** are re-sent as a full snapshot every
`ControlSnapshotIntervalSeconds` (default 1 s), so lossy links and late
joiners converge within about one interval.

---

## 5. Audio Implementation Comparison

### Audio Codec Support

| Transport | Codec Options | Encoding Location | API Boundary |
|-----------|---------------|-------------------|--------------|
| **Loopback** | PCM16, Opus (`O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **NNG** | PCM16, Opus (`O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **TCP** | PCM16, Opus (`O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **UDP** | PCM16, Opus (`O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **MoQ** | PCM16, Opus (`O3DAudio::FFrameEncoder`) | Sender | PCM16 |
| **WebRTC** | **Opus (inside LiveKit)** | **LiveKit** | **PCM float to int16** |

**WebRTC is the only outlier.** Every other transport encodes on the sender
through the shared `O3DAudio::FFrameEncoder` and exchanges PCM16 at the API
boundary. WebRTC differs because LiveKit encodes and decodes Opus itself.

### Audio Sink Implementations

**Sender Side** (`IO3DSenderAudioSink`):
- **Input**: PCM float (normalized -1.0 to 1.0)
- **Loopback, NNG, TCP, UDP, MoQ**: the shared `FO3DQueuedSenderAudioSink`
  - Encodes to PCM16 or Opus
  - Wraps each frame in the audio envelope
  - Queues it for the transport
  - MoQ publishes the bare audio payloads on its audio track
- **WebRTC**: converts to int16 and publishes on a LiveKit audio track

**Receiver Side** (`IO3DReceiverAudioSink`):
- **Output**: PCM16 with frame metadata (`O3DS::FAudioFrameMeta`)
- **Loopback, NNG, TCP, UDP, MoQ**: decode the received audio envelopes
- **MoQ**: subscribes to the audio track independently of the mocap track
- **WebRTC**: receives decoded PCM16 from a LiveKit callback

### Receive-side playback

`UO3DRemoteAudioComponent` (**O3D Remote Audio Component**, module `Open3DReceiver`)
plays received audio, with volume and pitch, attenuation, submix sends, source
effect chains, concurrency and spatialization.

**It is transport-agnostic.** It contains no reference to any specific transport, so
it applies equally to every transport in this document.

---

## 6. Error Handling & Reliability

### Loopback
- **Queue overflow**: The newest frame is refused, with rate-limited logging
- **No network errors**: In-process only

### NNG
- **Send errors**: Counted in `SendErrors` (and `DroppedFrames` for frames); a closed socket is reopened with backoff. No peer or a full NNG buffer drops the oldest frame
- **Queue overflow**: The newest frame is refused (`DroppedBackpressure`); queued frames are never discarded
- **Receive errors**: Counted in `ReceiveErrors`; a dialing socket is reopened with backoff
- **Pipe events**: Track the connection count

### TCP
- **Connection loss**: The receiver reconnects with exponential backoff; a pending connect times out after `tcp.connecttimeout`
- **Slow receiver**: The sender waits for socket space; it drops the client only after `tcp.stalltimeout` without progress
- **Send errors**: Drop the frame in progress, close the client, wait for a new connection
- **Framing errors**: Skip to the next frame marker in one pass; oversize frames are counted in `DroppedFrames`
- **Queue overflow**: New frames are dropped whole and counted; frames older than `tcp.maxqueueage` are dropped before sending

### UDP
- **Packet loss**: Silent drop (unreliable transport)
- **Fragment timeout**: Incomplete reassembly is discarded
- **Send errors**: Logged, counted in `DroppedFrames` and `SendErrors`; sending continues
- **Queue overflow**: The oldest waiting frames are dropped and counted; callers are refused only at twice the soft cap
- **No connection state**: `Connecting` only while a host name resolves
- **Windows**: an ICMP port-unreachable (`WSAECONNRESET`) affects only receiving; the receiver ends that `Poll` and reads again on the next

### WebRTC
- **Connection failures**: LiveKit reconnects
- **Send refused by LiveKit**: `DroppedBackpressure`, counted in `DroppedFrames` and `SendErrors`
- **Frame larger than 15,000 bytes**: refused (`TooLarge`); the add-on logs an error and the sender component a warning
- **Receiver backlog**: frames beyond 16 MiB waiting for `Poll` are refused and counted in `DroppedFrames`
- **Token expiry**: with automatic token fetch the token is refreshed before it expires; a manual token must be replaced by you

### MoQ
- **Connection failures**: automatic reconnection with capped, jittered backoff and a connect timeout
- **Queue overflow**: the newest frame is refused (`DroppedBackpressure`); frames whose track is not ready are dropped at the worker
- **Relay unreachable**: the session reports failure and reconnects
- **Subscribe failures**: retried on reconnect; mocap and audio tracks resubscribe independently
- **Missing DLL or functions**: checked when the module loads; the transport is not registered rather than failing later

### All transports
- A frame larger than the transport accepts (its queue capacity, or its largest message) is refused with `TooLarge`; the sender component logs "the N-byte frame is larger than the transport accepts". See the USER_GUIDE's troubleshooting.
- An option that fails validation stops the transport from starting, with `InvalidConfig` and a reason that names the option.

---

## 7. Performance Characteristics

| Transport | Latency | Memory Overhead |
|-----------|---------|-----------------|
| **Loopback** | No network delay | Queue capacity × frame size |
| **NNG** | LAN round trip | Send queue (4 MiB by default) |
| **TCP** | LAN round trip; a lost packet holds back the frames after it | Send queue (4 MiB by default) and the receiver's hand-off queue |
| **UDP** | LAN round trip; nothing waits for a lost datagram | Up to 4 frames waiting (oldest dropped) |
| **WebRTC** | Path through the LiveKit server, plus LiveKit's buffering | Inside LiveKit |
| **MoQ** | Path from the sender to the relay to the receiver | Send queue (8 MiB by default) and moq-ffi |

**Notes**:
- Latency varies with network conditions. None of these transports has published benchmark figures; measure on your own network.
- MoQ latency is dominated by the sender-to-relay-to-receiver path, so relay placement matters more than raw bandwidth.

---

## 8. Testing & Validation

The transports' automated tests are in the GitHub repository; they are not part of the Fab package. Every transport in the plugin runs the same conformance suite (lifecycle, rejected sends, backpressure, concurrent sends, monotonic stats, byte-exact round trip), plus tests of its own. A custom transport can be checked against the same cases.

---

## 9. Configuration Examples

Options are key-value pairs. The same keys work in the editor panels (by their panel names), in **Set Transport Option** on the sender component, in the Options map of **Create Open3DStream LiveLink Source**, and in the project-wide defaults (**Project Settings > Plugins > Open3DBroadcast**). An option you leave out takes the project default, then the transport's default. Each example lists what to set on each end; everything else stays at its default.

### Loopback
| End | Transport | Options |
|-----|-----------|---------|
| Sender | `Loopback` | `channel` = `stage1` (optional; default `default`) |
| Receiver | `Loopback` | `channel` = `stage1` |

### TCP
| End | Transport | Options |
|-----|-----------|---------|
| Sender (listens) | `TCP` | none (listens on `0.0.0.0`, port 17700) |
| Receiver | `TCP` | `host` = `192.168.1.10` (the sender's address) |

### UDP (one receiver)
| End | Transport | Options |
|-----|-----------|---------|
| Sender | `UDP` | `host` = `192.168.1.20` (the receiver's address) |
| Receiver (listens) | `UDP` | none (listens on `0.0.0.0`, port 17800) |

### UDP (broadcast to a subnet)
| End | Transport | Options |
|-----|-----------|---------|
| Sender | `UDP` | `host` = `192.168.1.255` (the subnet's broadcast address), `udp.broadcast` = `true` |
| Each receiver | `UDP` | `udp.broadcast` = `true` |

### NNG (Pub/Sub)
| End | Transport | Options |
|-----|-----------|---------|
| Sender (listens) | `NNG` | none (`nng.mode` `pub`, listens on `0.0.0.0`, port 6000) |
| Each receiver | `NNG` | `host` = `192.168.1.10` (the sender's address); `nng.mode` stays `sub` |

### NNG (Push/Pull)
| End | Transport | Options |
|-----|-----------|---------|
| Sender | `NNG` | `nng.mode` = `push`, `host` = `192.168.1.20` (the receiver's address) |
| Receiver (listens) | `NNG` | `nng.mode` = `pull` (listens on `0.0.0.0`, port 8000) |

### MoQ
| End | Transport | Options |
|-----|-----------|---------|
| Sender | `MoQ` | `relay_url` = `https://relay.example.com:443`, `track_namespace` = `mocap/stage1`, `track_name` = `performer1`, `delivery_mode` = `datagram` (optional) |
| Receiver | `MoQ` | `relay_url` = `https://relay.example.com:443`, `track_namespace` = `mocap/stage1`, `track_name` = `performer1` |

### WebRTC (add-on)
| End | Transport | Options |
|-----|-----------|---------|
| Sender and receiver | `WebRTC` | `webrtc.url` = `wss://myserver.livekit.cloud`, `webrtc.room` = `stage` |

The access token is a credential: set it with **Set Transport Secret** (key `webrtc.token`) or in the panel's credential row, or use automatic token fetch (`webrtc.useAutoTokenFetch` = `true`, `webrtc.tokenEndpointUrl`). The add-on's USER_GUIDE has the details.

In C++, the sender component takes the same keys:

```cpp
Sender->StopCapture();
Sender->SetTransportName(TEXT("UDP"));
Sender->SetTransportOption(TEXT("host"), TEXT("192.168.1.20"));
Sender->SetTransportOption(TEXT("port"), TEXT("17800"));
Sender->StartCapture();
```

---

## 10. Key Differences Summary

### Threading Philosophy
- **Loopback**: Synchronous (no threads)
- **NNG**: Send worker thread, synchronous receive polling over NNG's own I/O threads
- **TCP**: Send worker thread and receive worker thread; delivery from `Poll`
- **UDP**: Send worker thread, synchronous receive polling
- **WebRTC**: LiveKit callbacks; sends on the caller's thread, receive hand-off to `Poll`
- **MoQ**: Send worker thread, a dispatcher thread for the FFI, delivery from `Poll`

### Network Topology
- **Loopback**: In-process only
- **NNG**: 1:1 (Pair), 1:N (Pub/Sub), or distributed across pullers (Push/Pull)
- **TCP**: 1:1 (the sender accepts one receiver at a time)
- **UDP**: 1:1, or 1:N by broadcast on one subnet
- **WebRTC**: N:M (room)
- **MoQ**: N:M (relay fan-out, addressed by track namespace and name)

### Reliability Trade-offs
- **Loopback**: Reliable, ordered, in memory
- **NNG**: Pair and Push/Pull reliable and ordered; Pub/Sub unreliable
- **TCP**: Reliable, ordered
- **UDP**: **Unreliable**, lowest latency, best effort
- **WebRTC**: Reliable and ordered by default; unreliable with `webrtc.prefer_lossy`
- **MoQ**: Rated unreliable in both delivery modes. **Stream** delivers every frame in order; **Datagram** drops late frames instead of waiting

### Platform Coverage
- Every transport: Win64 only, editor and game targets

---

## 11. Recommendations by Use Case

| Use Case | Recommended Transport | Rationale |
|----------|-----------------------|-----------|
| **Local testing** | Loopback | No network, nothing to configure |
| **LAN, one receiver** | TCP | Reliable, simple to set up, no external dependency |
| **Low-latency LAN** | UDP | No waiting for lost datagrams; loss is acceptable |
| **One-to-many LAN** | NNG (Pub/Sub) or UDP (broadcast) | One stream to several receivers |
| **Across networks** | WebRTC (add-on) | NAT traversal through a LiveKit server |
| **Across networks without a LiveKit server** | MoQ (Experimental) | Outbound QUIC to a draft-07 relay; no signalling service to run |
| **Development and debugging** | Loopback or TCP | Loopback in one process, TCP between two |
| **Firewalled networks** | WebRTC or MoQ | Outbound connections only |
| **Residual coding** | Loopback, TCP, NNG Pair or Push/Pull, WebRTC (reliable) | Residual coding needs reliable, ordered delivery |

---

## 12. Extensibility

- **Transport registry**: a module registers its transport at startup with `FO3DTransportRegistry`; the pickers, the panels and Blueprint pick it up from there
- **Option schema**: a descriptor declares its options, and the editor builds the panel rows from it
- **Stats**: every transport reports the same `FO3DTransportStats`, shown in Blueprint as `FO3DBroadcastTransportStats`
- **Protocol versions**: the envelope and the frames carry version fields; the byte layout is in [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md)

---

## Conclusion

All transports implement the same interfaces, so a sender component or a LiveLink source switches between them by changing **Transport** and its options, without code changes. They differ where it matters for deployment:

- **Loopback** for testing in one process
- **TCP** for reliable streaming to one receiver on a LAN
- **UDP** for the lowest latency on a LAN, and broadcast to a subnet
- **NNG** for several receivers (Pub/Sub) or a reliable link opened from either end
- **WebRTC** (add-on) for streaming across networks through a LiveKit server
- **MoQ** (Experimental) for streaming across networks through a relay, without a signalling service

---

## Document Metadata

**Last revised**: 2026-10-06, checked against the plugin source of this release.
**Modules covered**:
- Open3DTransportLoopback
- Open3DTransportNNG
- Open3DTransportSockets (TCP + UDP)
- Open3DTransportMoQ
- Open3DTransportWebRTC (Open3DBroadcastWebRTC add-on)

**File locations**:
- Transport interfaces and registry: `Source/Open3DShared/Public/Transport/`
- Transport implementations: `Source/Open3DTransport*/Private/`
