# Open3DStream Plugin

This plugin provides both receiver and broadcaster capabilities for real-time animation streaming using the Open3DStream protocol.

## Modules

### Open3DStream (Receiver Module)
- **Purpose**: Receives animation data from external sources via LiveLink
- **Status**: Mature, production-ready
- **Key Components**:
  - `FOpen3DStreamSource` - LiveLink source implementation
  - Protocol support: TCP, UDP, WebRTC, NNG
  - Real-time animation curve and transform streaming

### Open3DBroadcast (Sender Module)
- **Purpose**: Broadcasts animation data FROM Unreal TO remote clients
- **Status**: Framework implemented, core functionality pending
- **Planned Components**:
  - `UO3DBroadcastSubsystem` - Broadcast management
  - `UO3DBroadcastComponent` - Per-actor broadcast control
  - `UO3DBroadcastSettings` - Configuration and URL management
- **Build Configuration**: Can be optionally disabled via `O3DS_WITH_BROADCAST=0`

## Installation

1. Copy this plugin folder to your project's `Plugins/` directory
2. Enable the plugin in your project settings
3. Both modules will be available automatically

## Build Configuration

The broadcast module can be conditionally compiled:

### To disable the broadcast module:
Add to your Target.cs file:
```csharp
GlobalDefinitions.Add("O3DS_WITH_BROADCAST=0");
```

Or build with UBT flag:
```bash
-D O3DS_WITH_BROADCAST=0
```

### Default behavior:
Both modules are built and available by default.

## Dependencies

- Open3DBroadcast module depends on Open3DStream module for core networking
- Open3DStream module has no dependencies on Open3DBroadcast
- Both modules share the same core C++ library for protocol implementation

## Usage

### Receiving (Open3DStream module)
1. Open **Window → Virtual Production → Live Link**
2. Add source: **+ Source → Open3DStream Source**
3. Configure connection URL and protocol

#### WebRTC Receiver (Server Role)
To receive animation and audio from a WebRTC broadcaster:

1. Add **Open3DStream Source** in Live Link
2. Set **Protocol** = `WebRTC Server`
3. Set **URL** = signaling server URL (e.g., `ws://localhost:8080`)
4. Set **WebRTC Room** = matching room name (e.g., `room1`)
5. Enable **Enable WebRTC Audio** for audio playback
6. (Optional) Adjust **Audio Playout Delay Ms** for resilience vs. latency tradeoff

**CVars:**
- `o3ds.Receiver.WebRTC.Log` - Enable receiver adapter logging (0/1)
- `o3ds.Receiver.Audio.Log` - Enable Opus decoder logging (0/1)
- `o3ds.Receiver.DebugParse` - Enable packet parsing diagnostics (0/1)

**Audio Routing:**
Decoded audio is published to `FO3DSAudioBus` with metadata (stream label, channels, rate). Components can subscribe to `FO3DSAudioBus::OnPcm16()` for in-world playback.

**Limitations (MVP):**
- No jitter buffer (decode per-packet; may cause artifacts on lossy links)
- Non-48kHz audio is dropped with verbose log
- Reconnect requires manual source restart

### Broadcasting (Open3DBroadcast module)

**Requirements**: Unreal Engine 5.6+, built with `O3DS_WITH_BROADCAST=1` (enabled by default)

The Open3DBroadcast module enables streaming animation data FROM Unreal Engine TO external clients or repeaters.

#### Transport Family + Mode Configuration

The broadcast adapter (`UO3DSBroadcastTransportAdapter`) provides a streamlined UX for selecting transport protocols:

**Transport Families:**
- **NNG**: High-performance scalability protocol with multiple modes
- **TCP**: Traditional TCP client/server
- **UDP**: Unreliable datagram transport
- **WebRTC**: Peer-to-peer with NAT traversal

**NNG Modes:**
- **Publisher**: Broadcasts to multiple subscribers (one-to-many)
- **Pair Client**: Connects to a pair server (one-to-one, client role)
- **Pair Server**: Accepts pair client connections (one-to-one, server role)
- **Push**: Pipeline push to a pull endpoint (many-to-one, ideal for repeaters)

**TCP Modes:**
- **Client**: Connects to a TCP server
- **Server**: Accepts TCP client connections

**WebRTC Modes:**
- **Client**: WebRTC client role
- **Server**: WebRTC server role

#### Example: Broadcasting through a Repeater

A common deployment pattern uses the standalone `Repeater` tool to aggregate multiple broadcasters and fan out to many subscribers:

**Repeater Setup:**
```bash
# Start repeater with PULL input on port 7000, PUB output on port 7001
Repeater tcp://0.0.0.0:7000 tcp://0.0.0.0:7001
```

**Broadcaster Configuration (in Unreal Editor):**
1. Add `UO3DSBroadcastTransportAdapter` component to your actor
2. Set **Transport Family** = `NNG`
3. Set **NNG Mode** = `Push`
4. Set **URL** = `tcp://repeater-host:7000`
5. The adapter will automatically inject `?mode=push` into the URL

**LiveLink Receiver Configuration:**
1. Open **Window → Virtual Production → Live Link**
2. Add **Open3DStream Source**
3. Set **Protocol** = `NNG Subscribe`
4. Set **URL** = `tcp://repeater-host:7001`

The broadcaster pushes frames to the repeater's PULL socket, which then publishes to all subscribers on the PUB socket.

#### Backward Compatibility

The legacy `Transport` property (enum: `Disabled`, `TCP`, `TCPServer`, `UDP`, `NNG`, `WebRTCClient`, `WebRTCServer`) is still supported for existing configurations. When set to `Disabled`, the new `TransportFamily` and mode properties are used instead.

#### Console Commands

- `o3ds.Broadcast.Transport.DumpStats` - Show adapter queue/drop stats
- `o3ds.Broadcast.Transport.DumpTransportStats` - Show transport-level counters (FramesSent, BytesSent, etc.)

#### CVars

- `o3ds.Broadcast.Enable` - Runtime enable/disable (0/1)
- `o3ds.Broadcast.Url` - Override URL from console
- `o3ds.Broadcast.Key` - Override session key
- `o3ds.Broadcast.MaxQueuedBytes` - Override queue limit

## Support

- Website: https://open3dstream.com/
- Repository: https://github.com/lifelike-and-believable/Open3DStream
- Issues: [GitHub Issues](https://github.com/lifelike-and-believable/Open3DStream/issues)

## Design & refactor plans

- WebRTC connector refactor (Issue #134): see `docs/WEBRTC_CONNECTOR_REFACTOR_PLAN_ISSUE134.md` for the architecture, milestones, and test plan.

## Troubleshooting

- Build/Test Sandbox Paths: Editor builds/tests run in `ProjectSandbox`, where `ProjectSandbox/Plugins/Open3DStream` is a symlink to this plugin folder. Compiler/linker errors may reference `ProjectSandbox/Plugins/Open3DStream/...`; map them back to `plugins/unreal/Open3DStream/...` in the repo. See `docs/TROUBLESHOOTING.md` for more tips.