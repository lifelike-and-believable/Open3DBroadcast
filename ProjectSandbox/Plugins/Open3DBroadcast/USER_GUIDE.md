# Open3DBroadcast Plugin - User Guide

## Table of Contents

1. [Introduction](#introduction)
2. [Quick Start](#quick-start)
3. [Core Concepts](#core-concepts)
4. [Sender Setup](#sender-setup)
5. [Receiver Setup](#receiver-setup)
6. [Transport Modules](#transport-modules)
7. [Audio Streaming](#audio-streaming)
8. [LiveLink Integration](#livelink-integration)
9. [Configuration Reference](#configuration-reference)
10. [Performance Tuning](#performance-tuning)
11. [Troubleshooting](#troubleshooting)
12. [Advanced Topics](#advanced-topics)

---

## Introduction

The **Open3DBroadcast Plugin** is a comprehensive Unreal Engine plugin for streaming skeletal animation data and audio in real-time. It enables you to capture motion from skeletal meshes in one Unreal Engine instance and receive it in another (or the same) instance using various network transports.

### Key Features

- **Real-time skeletal animation streaming** at configurable frame rates
- **Audio capture and streaming** from game audio or microphone
- **Multiple transport options**: Loopback (testing), Sockets (TCP/UDP), NNG (pub/sub), WebRTC (cloud-ready)
- **LiveLink integration** for seamless animation retargeting
- **Curve and morph target support** for facial animation
- **Production-ready** with comprehensive error handling and statistics

### Use Cases

- **Virtual production**: Stream motion capture data to rendering workstations
- **Remote collaboration**: Share character animations across the internet
- **Multi-machine setups**: Distribute animation processing across multiple systems
- **Development/Testing**: Test animation pipelines locally with loopback transport

---

## Quick Start

### Installation

1. Copy the `Open3DBroadcast` plugin folder to your project's `Plugins/` directory
2. Open your Unreal Engine project
3. Go to **Edit → Plugins** and search for "Open3D"
4. Enable the **Open3D Broadcast Suite** plugin
5. Restart the editor when prompted

### Your First Stream (5 Minutes)

This example uses **Loopback transport** for local testing (no network required).

#### Step 1: Add a Sender Component

1. Open your level with a character that has a skeletal mesh
2. Select the character actor in the **Outliner**
3. In the **Details** panel, click **Add Component**
4. Search for and add **O3D Sender Component**
5. Configure the sender:
   - **Subject Name**: `MyCharacter`
   - **Capture Rate Hz**: `60`
   - **Transport Name**: `loopback`
   - **Auto Start Capture**: ✓ (checked)

#### Step 2: Add Transport Options

In the **O3D Sender Component** details:
1. Expand **Transport Options**
2. Add a new element:
   - **Key**: `role`
   - **Value**: `sender`
3. Add another element:
   - **Key**: `channel`
   - **Value**: `test_channel`

#### Step 3: Create a LiveLink Source (Receiver)

1. Open **Window → Live Link** to show the LiveLink panel
2. Click **+ Source** button
3. Select **Open3D Receiver Source**
4. In the dialog:
   - **Transport Name**: `loopback`
   - **Enable Audio**: unchecked (for now)
5. Expand **Transport Options** and add:
   - **Key**: `role`, **Value**: `receiver`
   - **Key**: `channel`, **Value**: `test_channel`
6. Click **Create**

#### Step 4: Test the Stream

1. Click **Play** in the editor
2. Open the **LiveLink** panel
3. You should see a subject named `MyCharacter` appear with a green status
4. The skeletal animation from your character is now streaming through the loopback transport!

#### Step 5: Apply to Another Character (Optional)

1. Add a second skeletal mesh actor to your level
2. Select it and add a **Live Link Component**
3. In the component settings:
   - **Subject Representation**: Select your skeleton asset
   - **LiveLink Subject Name**: `MyCharacter`
4. The second character will now mirror the first character's animation

**Congratulations!** You've set up your first animation stream.

---

## Core Concepts

### Architecture Overview

```
┌─────────────────┐         ┌──────────────┐         ┌─────────────────┐
│  Sender Actor   │         │  Transport   │         │ LiveLink Source │
│  ┌───────────┐  │         │              │         │  (Receiver)     │
│  │ Skeletal  │  │         │  ┌────────┐  │         │                 │
│  │   Mesh    │  │ Capture │  │Network │  │ Receive │  ┌──────────┐   │
│  └─────┬─────┘  ├────────>│  │  or    │──┼────────>│  │ LiveLink │   │
│        │        │         │  │In-Proc │  │         │  │  Client  │   │
│  ┌─────▼─────┐  │         │  └────────┘  │         │  └──────────┘   │
│  │O3DSender  │  │         │              │         │                 │
│  │Component  │  │         │   Optional   │         │   ┌──────────┐  │
│  └───────────┘  │         │  ┌────────┐  │         │   │  Audio   │  │
│                 │  Audio  │  │ Audio  │  │  Audio  │   │  Output  │  │
│  ┌───────────┐  ├────────>│  │ Stream │──┼────────>│   └──────────┘  │
│  │   Audio   │  │         │  └────────┘  │         │                 │
│  │  Capture  │  │         │              │         │                 │
│  └───────────┘  │         └──────────────┘         └─────────────────┘
└─────────────────┘
```

### Subject-Based Streaming

Each sender broadcasts data for a **subject** - a named stream of animation data. Multiple subjects can share the same transport channel. Receivers subscribe to all subjects on a channel and expose them as LiveLink subjects.

### Transport Modules

The plugin provides 4 transport modules:

| Transport | Best For | Network | Audio | Latency |
|-----------|----------|---------|-------|---------|
| **Loopback** | Testing, local development | None (in-process) | Yes | Ultra-low |
| **Sockets** | LAN, direct P2P | TCP/UDP | No (V1) | Low |
| **NNG** | Advanced messaging patterns | TCP/IPC/WebSocket | No (V1) | Low-Medium |
| **WebRTC** | Internet, NAT traversal, cloud | WebRTC/TURN | Yes | Medium |

### Unified Message Format

All transports use a unified message format with a 20-byte header:

```
┌──────────┬─────────┬────────┬───────────┬──────────┬─────────────┐
│  Magic   │ Version │  Kind  │   Codec   │Timestamp │Payload Size │
│ (4 bytes)│(2 bytes)│(2 byte)│ (2 bytes) │(8 bytes) │  (4 bytes)  │
└──────────┴─────────┴────────┴───────────┴──────────┴─────────────┘
                             Followed by payload data
```

- **Magic**: `0x4F334441` (identifies Open3D frames)
- **Kind**: Mocap (skeletal data) or Audio
- **Codec**: O3DS (FlatBuffers), PCM16, or Opus

---

## Sender Setup

### Adding the Sender Component

**In Blueprint:**
1. Select your actor
2. Add Component → **O3D Sender Component**

**In C++:**
```cpp
#include "O3DSenderComponent.h"

// In your actor class
UO3DSenderComponent* Sender = CreateDefaultSubobject<UO3DSenderComponent>(TEXT("O3DSender"));
```

### Basic Configuration

#### Required Settings

- **Subject Name**: Unique identifier for this animation stream (e.g., "MainCharacter", "Player1")
- **Transport Name**: Which transport to use (`loopback`, `sockets`, `nng`, or `webrtc`)

#### Capture Settings

- **Capture Rate Hz**: Target frame rate (default: 60.0)
  - Higher = smoother but more bandwidth
  - Actual rate limited by tick rate
- **Auto Start Capture**: Start streaming automatically on BeginPlay
- **Target Mesh**: Skeletal mesh to capture (auto-detected if empty)

#### Transport Configuration

Configure transports using **Transport Options** (key-value pairs):

**Common Options:**
- `role`: `sender` or `receiver`
- `uri`: Connection endpoint (IP:port or WebSocket URL)
- `stream_id`: Room/channel identifier

See [Transport Modules](#transport-modules) for transport-specific options.

### Curve Filtering

Reduce bandwidth by filtering animation curves:

- **Enable Curve Filtering**: Enable delta-based filtering
- **Curve Epsilon**: Ignore changes smaller than this value (default: 0.0001)
- **Curve Delta Threshold**: Only send if change exceeds threshold (default: 0.001)
- **Include Curve Patterns**: Wildcards for curves to include (e.g., `face_*`)
- **Exclude Curve Patterns**: Wildcards for curves to exclude (e.g., `*_unused`)
- **Clamp Morph Curves to Unit**: Clamp morph targets to [0, 1] range
- **Drop NaN and Infinity**: Sanitize curve values (recommended: enabled)

### Controlling Capture

**In Blueprint:**
- Call `Start Capture` to begin streaming
- Call `Stop Capture` to stop streaming
- Use `Is Capturing` to check current state

**In C++:**
```cpp
// Start capturing and streaming
Sender->StartCapture();

// Stop
Sender->StopCapture();

// Check status
bool bIsActive = Sender->IsCapturing();
```

### Events and Delegates

Subscribe to capture events:

**Available Events:**
- `OnDescriptorReady`: Fired when skeleton descriptor is sent
- `OnPoseFrameReady`: Fired before each frame is serialized
- `OnSerializedFrame`: Fired after frame serialization with raw data

**Blueprint Example:**
1. Select your Sender Component
2. In Event Graph, find the event under "O3D Sender Component"
3. Bind to the event

**C++ Example:**
```cpp
Sender->OnPoseFrameReady.AddDynamic(this, &AMyActor::OnPoseReady);

void AMyActor::OnPoseReady(const FO3DSPoseFrame& PoseFrame)
{
    UE_LOG(LogTemp, Log, TEXT("Frame %llu with %d bones"),
           PoseFrame.FrameIndex, PoseFrame.BoneLocalTransforms.Num());
}
```

---

## Receiver Setup

### Creating a LiveLink Source

The receiver is implemented as a **LiveLink Source** and configured through Unreal's LiveLink panel.

#### Step-by-Step Setup

1. **Open LiveLink Panel**
   - **Window → Live Link**

2. **Add Source**
   - Click **+ Source**
   - Select **Open3D Receiver Source**

3. **Configure Source**
   - **Transport Name**: Must match sender (e.g., `webrtc`)
   - **Enable Audio**: Check to enable audio playback
   - **Audio Stream Label**: Filter by label (optional, leave empty for all)
   - **Audio Codec**: Preferred decoder (`PCM16` or `Opus`)

4. **Add Transport Options**
   - Expand **Transport Options**
   - Add entries matching your transport (see examples below)

5. **Create Source**
   - Click **Create**
   - Source should appear in LiveLink list
   - Status will be green when receiving data

### LiveLink Source Configuration Examples

#### Loopback (Testing)
```
Transport Name: loopback
Transport Options:
  - role: receiver
  - channel: test_channel
```

#### Sockets (TCP)
```
Transport Name: sockets
Transport Options:
  - role: receiver
  - uri: 0.0.0.0:9000
  - protocol: tcp
```

#### WebRTC (LiveKit)
```
Transport Name: webrtc
Transport Options:
  - uri: wss://your-livekit-server.com
  - token: <your_jwt_token>
  - stream_id: my_room
  - role: receiver
Enable Audio: ✓
Audio Codec: Opus
```

### Applying Animation to Characters

Once the LiveLink source is receiving data, subjects will appear automatically.

#### Method 1: Live Link Component

1. Add a **Live Link Component** to your target actor
2. Configure:
   - **Subject Representation**: Your skeleton asset
   - **LiveLink Subject Name**: Name from sender (e.g., `MyCharacter`)

#### Method 2: Animation Blueprint

1. Open your Animation Blueprint
2. Add a **Live Link Pose** node
3. Connect to your output pose
4. Set **Live Link Subject Name** to your subject
5. Configure retargeting as needed

#### Method 3: Control Rig

1. Create a Control Rig asset
2. Add **Live Link** input
3. Map to your skeleton
4. Apply Control Rig to your character

---

## Transport Modules

### Loopback Transport

**Purpose:** In-process streaming for testing and development

**Configuration:**
```
Transport Name: loopback
Transport Options:
  - role: sender (or receiver)
  - channel: my_channel_name    # Both sender/receiver must match
  - loopback.queue: 64          # Optional: queue size
```

**Characteristics:**
- No network involved
- Ultra-low latency
- Both sender and receiver in same process
- Supports audio
- Perfect for development and testing

**Use When:**
- Testing animation capture pipeline
- Developing new features
- Debugging serialization issues
- Demo setups on single machine

### Sockets Transport

**Purpose:** Direct peer-to-peer streaming over TCP or UDP

**Sender Configuration:**
```
Transport Name: sockets
Transport Options:
  - role: sender
  - uri: 192.168.1.100:9000     # Receiver's IP:port
  - protocol: tcp                # or udp
```

**Receiver Configuration:**
```
Transport Name: sockets
Transport Options:
  - role: receiver
  - uri: 0.0.0.0:9000           # Listen on all interfaces
  - protocol: tcp                # Must match sender
```

**Characteristics:**
- Low latency on LAN
- Reliable (TCP) or fast (UDP)
- Direct connection required
- No audio support in V1
- Firewall rules may be needed

**TCP vs UDP:**
- **TCP**: Reliable, ordered delivery. Best for critical animation data
- **UDP**: Lower latency, may drop packets. Good for high-frequency updates

**Use When:**
- Local network (LAN/studio)
- Direct machine-to-machine
- Low latency is critical
- No NAT traversal needed

### NNG Transport

**Purpose:** Advanced messaging patterns (pub/sub, push/pull, etc.)

**Publisher (Sender) Configuration:**
```
Transport Name: nng
Transport Options:
  - role: sender
  - uri: tcp://0.0.0.0:9000     # Bind address
  - pattern: pub                # Publishing pattern
```

**Subscriber (Receiver) Configuration:**
```
Transport Name: nng
Transport Options:
  - role: receiver
  - uri: tcp://192.168.1.100:9000  # Publisher address
  - pattern: sub                   # Subscribing pattern
```

**Characteristics:**
- Scalable messaging patterns
- Multiple subscribers per publisher
- Cross-platform (IPC, TCP, WebSocket)
- No audio support in V1
- More complex configuration

**Patterns:**
- **pub/sub**: One publisher, many subscribers (broadcast)
- **push/pull**: Load balancing across receivers
- **req/rep**: Request-response pattern

**Use When:**
- Multiple receivers needed
- Advanced routing required
- Cross-platform IPC needed
- Scalability is important

### WebRTC Transport

**Purpose:** Cloud-ready streaming with NAT traversal and audio support

**Sender Configuration (LiveKit):**
```
Transport Name: webrtc
Transport Options:
  - uri: wss://livekit.yourserver.com
  - token: <sender_jwt_token>
  - stream_id: room_name
  - role: sender
  - backend: livekit            # Optional: explicit backend
Audio Settings:
  - Enable Audio: ✓
  - Audio Codec: Opus
  - Sample Rate: 48000
  - Bitrate Kbps: 64
```

**Receiver Configuration (LiveKit):**
```
Transport Name: webrtc
Transport Options:
  - uri: wss://livekit.yourserver.com
  - token: <receiver_jwt_token>
  - stream_id: room_name
  - role: receiver
  - backend: livekit
Audio Settings:
  - Enable Audio: ✓
  - Audio Codec: Opus
```

**Characteristics:**
- Works across the internet
- NAT/firewall traversal via TURN
- Supports audio streaming
- Higher latency than local transports
- Requires signaling server (LiveKit)

**LiveKit Setup:**

1. **Self-Hosted:** Deploy LiveKit server following their docs
2. **Cloud:** Use LiveKit Cloud service
3. **Generate Tokens:** Use LiveKit API to generate JWT tokens with appropriate permissions

**Token Requirements:**
- Sender: Needs publish permissions
- Receiver: Needs subscribe permissions
- Both: Must have same room name in token claims

**Use When:**
- Remote collaboration over internet
- NAT traversal required
- Audio streaming needed
- Multiple participants in a room

**See Also:** [Source/Open3DTransportWebRTC/USER_GUIDE.md](Source/Open3DTransportWebRTC/USER_GUIDE.md) for detailed WebRTC documentation.

---

## Audio Streaming

### Audio Capture (Sender)

The sender can capture audio from two sources:

1. **Game Audio (Mix Mode)**: Captures from audio submix
2. **Microphone (Input Mode)**: Captures from microphone input

#### Enabling Audio

In **O3D Sender Component**:
1. Check **Enable Audio**
2. Configure **Audio Capture Config**:
   - **Audio Capture Mode**: `Mix` or `Input`
   - **Audio Codec**: `PCM16` (uncompressed) or `Opus` (compressed)
   - **Sample Rate**: 48000 recommended
   - **Num Channels**: 1 (mono) or 2 (stereo)
   - **Bitrate Kbps**: 64 recommended for Opus

#### Audio Capture Modes

**Mix Mode (Game Audio):**
```
Audio Capture Mode: Mix
Submix to Tap: [leave empty for main submix]
Game Gain: 1.0        # Volume multiplier
```

Captures audio from the game's audio output. Useful for:
- Broadcasting game sound effects
- Sharing music/ambience
- Full game audio capture

**Input Mode (Microphone):**
```
Audio Capture Mode: Input
Audio Input Device: [select from dropdown]
Mic Gain: 1.0         # Volume multiplier
```

Captures from microphone input. Useful for:
- Voice chat
- Commentary
- Live narration

#### Audio Stream Label

- **Audio Stream Label**: Tag for audio frames (default: `o3ds:audio`)
- Multiple senders can use different labels
- Receivers can filter by label

#### Audio Codec Selection

**PCM16:**
- Uncompressed 16-bit audio
- High quality, high bandwidth
- ~1.5 Mbps for stereo 48kHz
- Zero latency encoding
- Use for: LAN, testing

**Opus:**
- Compressed, high-quality codec
- Configurable bitrate (16-128 kbps)
- Low latency (~20ms)
- Excellent quality at 64 kbps
- Use for: Internet, WebRTC

### Audio Playback (Receiver)

Audio is played back using the **O3D Remote Audio Component**.

#### Adding Audio Component

1. Add **O3D Remote Audio Component** to an actor
2. Configure:
   - **Receive Mode**: `Mix` or `Subject`
   - **Stream Label**: Filter by label (or empty for all)
   - **Gain**: Output volume multiplier
   - **Attenuation Settings**: Spatial audio (optional)

#### Receive Modes

**Mix Mode:**
- Receives audio tagged with stream label
- Global/ambient audio
- No spatial positioning

**Subject Mode:**
- Receives audio associated with LiveLink subject
- Can be positioned in 3D space
- Follows subject's position

#### Audio Bus

Audio is routed through a centralized **Audio Bus** singleton:
- All receivers publish to the bus
- All audio components subscribe to the bus
- Enables flexible routing and mixing

### Audio Troubleshooting

**No audio output:**
1. Check **Enable Audio** on sender and receiver
2. Verify transport supports audio (Loopback, WebRTC)
3. Check **Audio Stream Label** matches
4. Verify codec compatibility
5. Check Windows audio mixer for Unreal Engine volume

**Audio dropouts/crackling:**
1. Increase Opus bitrate
2. Check network bandwidth
3. Reduce capture rate or resolution
4. Use PCM16 for testing (eliminates codec issues)

**High latency:**
1. Use Opus instead of PCM16 (for network streams)
2. Reduce audio buffer sizes (advanced)
3. Use lower sample rate (32000 instead of 48000)

---

## LiveLink Integration

### Understanding LiveLink

LiveLink is Unreal's system for receiving real-time animation data from external sources. Open3DBroadcast integrates as a LiveLink source, making it compatible with all LiveLink-enabled workflows.

### Subject Management

**Automatic Subject Creation:**
- Subjects appear automatically when sender starts
- Subject name matches sender's `Subject Name`
- Inactive subjects removed after 5 seconds

**Subject Data Includes:**
- Bone transforms (local space)
- Animation curves
- Morph target weights
- Frame timing information

### Retargeting Animation

#### Same Skeleton
If sender and receiver use the same skeleton:
1. LiveLink will apply animation directly
2. No retargeting needed

#### Different Skeletons
If skeletons differ:

**Option 1: IK Retargeting (UE5)**
1. Create an IK Rig for source skeleton
2. Create an IK Rig for target skeleton
3. Create an IK Retargeter asset
4. Map bones between skeletons
5. Use retargeter in Animation Blueprint

**Option 2: Control Rig**
1. Create Control Rig asset
2. Add LiveLink input
3. Map source bones to target rig
4. Drive target skeleton from rig

**Option 3: Animation Blueprint**
1. Add LiveLink Pose node
2. Use Modify Bone nodes to adjust
3. Custom retargeting logic

### LiveLink Presets

Save and load LiveLink configurations:

1. Configure your LiveLink sources
2. Click **Preset** dropdown in LiveLink panel
3. **Save Preset** with a name
4. Load preset later to restore configuration

Useful for:
- Switching between different streaming setups
- Team sharing of configurations
- Quick setup for common scenarios

### Multiple Subjects

You can stream multiple subjects simultaneously:

**Sender Side:**
- Add O3D Sender Component to multiple actors
- Give each a unique **Subject Name**
- All can share the same transport

**Receiver Side:**
- Single LiveLink source receives all subjects
- Each subject appears separately in LiveLink
- Apply to different characters as needed

**Example:**
```
Actor1 → O3DSender(Subject: "Character1") ┐
Actor2 → O3DSender(Subject: "Character2") ├→ WebRTC Transport → LiveLink Source
Actor3 → O3DSender(Subject: "Prop1")      ┘
                                              ↓
                                          LiveLink Subjects:
                                          - Character1
                                          - Character2
                                          - Prop1
```

---

## Configuration Reference

### Sender Component Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **SubjectName** | String | "" | Unique identifier for this stream |
| **CaptureRateHz** | Float | 60.0 | Target capture frame rate |
| **bAutoStartCapture** | Bool | false | Start capturing on BeginPlay |
| **TargetMesh** | Object | null | Skeletal mesh to capture (auto-detect if empty) |
| **TransportName** | Name | "loopback" | Transport module to use |
| **bAutoCreateTransport** | Bool | true | Automatically create transport |
| **TransportOptions** | Map | {} | Key-value transport configuration |

#### Curve Filtering Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableCurveFiltering** | Bool | false | Enable curve filtering |
| **CurveEpsilon** | Float | 0.0001 | Minimum significant change |
| **CurveDeltaThreshold** | Float | 0.001 | Change threshold for emission |
| **IncludeCurvePatterns** | Array | [] | Wildcard patterns to include |
| **ExcludeCurvePatterns** | Array | [] | Wildcard patterns to exclude |
| **bClampMorphCurvesToUnit** | Bool | true | Clamp morphs to [0,1] |
| **bDropNaNAndInfinity** | Bool | true | Sanitize invalid values |

#### Audio Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableAudio** | Bool | false | Enable audio streaming |
| **AudioCaptureMode** | Enum | Mix | Mix (game) or Input (mic) |
| **AudioInputDevice** | String | "" | Microphone device name |
| **AudioStreamLabel** | String | "o3ds:audio" | Audio frame identifier |
| **AudioCodec** | Name | "PCM16" | Audio codec (PCM16/Opus) |

#### Audio Capture Config

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **SampleRate** | Int | 48000 | Audio sample rate (Hz) |
| **NumChannels** | Int | 1 | 1=mono, 2=stereo |
| **BitrateKbps** | Int | 64 | Opus bitrate (16-128) |
| **GameGain** | Float | 1.0 | Game audio volume multiplier |
| **MicGain** | Float | 1.0 | Microphone volume multiplier |
| **SubmixToTap** | Object | null | Custom submix (or null for main) |

### Receiver Source Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **TransportName** | Name | "loopback" | Transport module to use |
| **bEnableAudio** | Bool | false | Enable audio playback |
| **AudioStreamLabel** | String | "" | Filter by label (empty = all) |
| **AudioCodec** | Name | "Opus" | Preferred audio decoder |
| **TransportOptions** | Map | {} | Key-value transport configuration |

### Remote Audio Component Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **ReceiveMode** | Enum | Mix | Mix or Subject mode |
| **StreamLabel** | String | "" | Filter by label (empty = all) |
| **SubjectName** | String | "" | Subject for Subject mode |
| **Gain** | Float | 1.0 | Output volume multiplier |
| **bEnableAttenuation** | Bool | false | Enable spatial audio |
| **AttenuationSettings** | Object | null | Attenuation configuration |

### Transport Options Reference

#### Loopback
| Key | Value | Description |
|-----|-------|-------------|
| `role` | `sender`/`receiver` | Required: endpoint role |
| `channel` | string | Required: shared channel name |
| `loopback.queue` | number | Optional: queue size (default 64) |

#### Sockets
| Key | Value | Description |
|-----|-------|-------------|
| `role` | `sender`/`receiver` | Required: endpoint role |
| `uri` | `host:port` | Required: IP and port |
| `protocol` | `tcp`/`udp` | Optional: protocol (default tcp) |
| `sockets.buffer` | number | Optional: buffer size (bytes) |

#### NNG
| Key | Value | Description |
|-----|-------|-------------|
| `role` | `sender`/`receiver` | Required: endpoint role |
| `uri` | `protocol://host:port` | Required: NNG URL |
| `pattern` | `pub`/`sub`/`push`/`pull` | Optional: messaging pattern |
| `nng.timeout` | number | Optional: timeout (ms) |

#### WebRTC
| Key | Value | Description |
|-----|-------|-------------|
| `role` | `sender`/`receiver` | Required: endpoint role |
| `uri` | `wss://host` | Required: LiveKit server URL |
| `token` | JWT string | Required: authentication token |
| `stream_id` | string | Required: room name |
| `backend` | `livekit`/`libdc` | Optional: explicit backend |
| `webrtc.ice_server` | URL | Optional: custom TURN server |

---

## Performance Tuning

### Optimizing Bandwidth

**Reduce Capture Rate:**
```
Capture Rate Hz: 30    # Instead of 60
```
- 50% bandwidth reduction
- Still smooth for most animations

**Enable Curve Filtering:**
```
Enable Curve Filtering: ✓
Curve Delta Threshold: 0.01
Exclude Curve Patterns: ["*_unused", "*_debug"]
```
- Reduces curve data significantly
- Only sends meaningful changes

**Use Opus for Audio:**
```
Audio Codec: Opus
Bitrate Kbps: 32      # Lower for voice-only
```
- 64 kbps stereo vs 1.5 Mbps PCM16
- Excellent quality at low bandwidth

**Simplify Skeleton:**
- Remove unused bones before streaming
- Reduce LOD for distant characters
- Filter unnecessary curves

### Optimizing Latency

**Increase Capture Rate:**
```
Capture Rate Hz: 90    # Higher refresh
```
- More responsive animation
- Increases bandwidth

**Use UDP for Sockets:**
```
Transport Options:
  - protocol: udp
```
- Lower latency than TCP
- May drop frames under packet loss

**Optimize Network:**
- Use wired connections
- Minimize network hops
- QoS prioritization for animation traffic

**WebRTC Tuning:**
```
webrtc.max_bitrate: 5000000    # 5 Mbps cap
webrtc.min_bitrate: 500000     # 500 Kbps floor
```

### Memory Optimization

**Limit Queue Sizes:**
```
loopback.queue: 32     # Smaller queue
```
- Reduces memory footprint
- May drop frames if consumer is slow

**Subject Cleanup:**
- Inactive subjects auto-removed after 5 seconds
- Stops memory leaks from disconnected senders

### CPU Optimization

**Reduce Serialization Cost:**
- Lower capture rate
- Fewer curves
- Simpler skeletons

**Multi-Threading:**
- Sender serialization is async-safe
- Transport I/O typically on separate threads
- Audio encoding threaded (Opus)

### Monitoring Performance

**Transport Statistics:**

Access via C++:
```cpp
FO3DTransportStats Stats = Sender->GetTransportStats();
UE_LOG(LogTemp, Log, TEXT("Sent %lld frames, %lld bytes, avg latency %.2f ms"),
       Stats.FramesSent, Stats.BytesSent, Stats.AverageLatencyMs);
```

**Available Metrics:**
- `FramesSent` / `FramesReceived`
- `BytesSent` / `BytesReceived`
- `DroppedFrames`
- `AverageLatencyMs` / `MaxLatencyMs`

**LiveLink Status:**
- Green: Receiving recent data
- Yellow: Stale data (1-5 seconds old)
- Red: No data (>5 seconds)

---

## Troubleshooting

### Common Issues

#### "Subject not appearing in LiveLink"

**Symptoms:** Sender is capturing but receiver shows no subjects

**Solutions:**
1. **Check transport configuration:**
   - Verify `Transport Name` matches on both sides
   - Verify transport options match (especially `channel` or `stream_id`)
   - Check `role` is set correctly (sender vs receiver)

2. **Check network connectivity:**
   - For Sockets: Verify IP and port accessibility
   - For WebRTC: Verify server URL and token validity
   - Test with Loopback first to isolate network issues

3. **Check LiveLink source status:**
   - Open LiveLink panel
   - Look for error messages
   - Try removing and re-creating source

4. **Enable debug logging:**
   ```
   Console: "log LogO3DReceiver Verbose"
   ```

#### "Audio not working"

**Symptoms:** Animation works but no audio output

**Solutions:**
1. **Verify transport support:**
   - Loopback: ✓ Supported
   - Sockets: ✗ Not supported in V1
   - NNG: ✗ Not supported in V1
   - WebRTC: ✓ Supported

2. **Check audio settings:**
   - Sender: `Enable Audio` checked
   - Receiver: `Enable Audio` checked
   - Codec matches or receiver supports sender's codec

3. **Check audio component:**
   - Add `O3D Remote Audio Component` to scene
   - Verify `Receive Mode` and `Stream Label`
   - Check `Gain` is not zero

4. **Check audio device:**
   - Windows Sound Settings → Unreal Engine not muted
   - For Input mode: Microphone permissions granted
   - For Mix mode: Game actually producing audio

#### "High latency / lag"

**Symptoms:** Animation is delayed or choppy

**Solutions:**
1. **Optimize capture rate:**
   - Try lower capture rate (30 Hz) first
   - Increase only if smooth enough

2. **Network optimization:**
   - Use wired connection
   - Close bandwidth-heavy applications
   - For WebRTC: Check TURN server performance

3. **Codec selection:**
   - For Audio: Use Opus instead of PCM16
   - Lower Opus bitrate if needed

4. **Transport selection:**
   - LAN: Use Sockets (lowest latency)
   - Internet: WebRTC required, latency expected

#### "Frames dropping / choppy animation"

**Symptoms:** Animation stutters or skips frames

**Solutions:**
1. **Check CPU usage:**
   - High CPU can cause dropped ticks
   - Reduce capture rate
   - Simplify scene

2. **Network bandwidth:**
   - Too much data for connection
   - Enable curve filtering
   - Lower capture rate
   - Use Opus for audio

3. **Queue overflow:**
   - Receiver not processing fast enough
   - Increase queue size (Loopback only)
   - Reduce data rate

4. **Check transport stats:**
   ```cpp
   UE_LOG(LogTemp, Warning, TEXT("Dropped %lld frames"), Stats.DroppedFrames);
   ```

#### "WebRTC connection fails"

**Symptoms:** WebRTC transport shows errors or won't connect

**Solutions:**
1. **Verify server URL:**
   - Must be `wss://` (secure WebSocket)
   - Server must be accessible
   - Check firewall rules

2. **Verify JWT token:**
   - Token must be valid (not expired)
   - Token must have correct room name
   - Sender needs publish permission
   - Receiver needs subscribe permission

3. **Check backend availability:**
   - Plugin built with LiveKit support
   - Check build configuration: `O3D_WEBRTC_BACKEND_LIVEKIT=true`

4. **TURN server:**
   - May need TURN server for NAT traversal
   - Check LiveKit server TURN configuration

See [WebRTC-specific troubleshooting](Source/Open3DTransportWebRTC/USER_GUIDE.md) for more details.

### Debug Logging

Enable verbose logging for troubleshooting:

**Console Commands:**
```
log LogO3DSender Verbose
log LogO3DReceiver Verbose
log LogO3DTransportSockets Verbose
log LogO3DTransportWebRTC Verbose
```

**In DefaultEngine.ini:**
```ini
[Core.Log]
LogO3DSender=Verbose
LogO3DReceiver=Verbose
LogO3DTransportWebRTC=Verbose
```

---

## Advanced Topics

### Custom Transport Implementation

You can implement custom transports by extending the transport interfaces.

**Required Interfaces:**
- `IOpen3DSender`: For sending data
- `IOpen3DReceiver`: For receiving data

**Registration:**
```cpp
// In your transport module's Startup
FO3DSenderRegistry::Get().RegisterFactory(
    FName("mycustom"),
    []() -> IOpen3DSender* { return new FMyCustomSender(); }
);

FO3DReceiverRegistry::Get().RegisterFactory(
    FName("mycustom"),
    []() -> IOpen3DReceiver* { return new FMyCustomReceiver(); }
);
```

**See:** Existing transport implementations in `Source/Open3DTransport*/` for examples.

### Multi-Subject Workflows

**Scenario:** Multiple characters in a scene, each with different update rates

```cpp
// High-priority character: 60 Hz
CharacterA->Sender->CaptureRateHz = 60.0f;
CharacterA->Sender->SubjectName = "Hero";

// Background character: 30 Hz
CharacterB->Sender->CaptureRateHz = 30.0f;
CharacterB->Sender->SubjectName = "NPC1";

// Prop: 15 Hz
Prop->Sender->CaptureRateHz = 15.0f;
Prop->Sender->SubjectName = "MovingProp";
```

All share the same transport, each identified by subject name.

### Runtime Transport Switching

**Scenario:** Start with loopback for testing, switch to WebRTC for production

```cpp
// Stop current capture
Sender->StopCapture();

// Change transport
Sender->TransportName = FName("webrtc");
Sender->SetTransportOption("uri", "wss://production.server.com");
Sender->SetTransportOption("token", ProductionToken);

// Restart
Sender->StartCapture();
```

### Custom Audio Processing

**Scenario:** Process audio before sending

1. Subclass `UO3DSenderAudioCaptureComponent`
2. Override `ProcessAudioBuffer` to apply effects
3. Use custom component instead of standard one

**Example:**
```cpp
void UMyAudioCapture::ProcessAudioBuffer(float* Buffer, int32 NumFrames)
{
    // Apply noise gate
    for (int32 i = 0; i < NumFrames * NumChannels; ++i)
    {
        if (FMath::Abs(Buffer[i]) < NoiseThreshold)
            Buffer[i] = 0.0f;
    }

    // Call base implementation
    Super::ProcessAudioBuffer(Buffer, NumFrames);
}
```

### Bandwidth Estimation

**Uncompressed (no filtering):**
```
Skeleton: 100 bones × 7 floats (Transform) × 4 bytes = 2.8 KB
Curves: 50 curves × 4 bytes = 0.2 KB
Frame size: ~3 KB

At 60 Hz: 3 KB × 60 = 180 KB/s = 1.44 Mbps
```

**With curve filtering (50% reduction):**
```
Frame size: ~2.9 KB
At 60 Hz: ~1.39 Mbps
```

**Audio (Opus 64 kbps stereo):**
```
Audio: 64 kbps

Total: 1.39 + 0.064 = 1.45 Mbps
```

**Audio (PCM16 48kHz stereo):**
```
Audio: 48000 × 2 × 2 bytes × 8 = 1.536 Mbps

Total: 1.39 + 1.536 = 2.93 Mbps
```

### Production Checklist

Before deploying to production:

- [ ] Test with loopback transport first
- [ ] Verify all subjects appear in LiveLink
- [ ] Test audio if using WebRTC or Loopback
- [ ] Measure bandwidth usage
- [ ] Test network disconnection handling
- [ ] Verify retargeting works on target characters
- [ ] Configure appropriate capture rates
- [ ] Enable curve filtering if needed
- [ ] Set up monitoring/logging
- [ ] Document transport configuration for team
- [ ] Test firewall/NAT scenarios (for WebRTC)
- [ ] Prepare fallback configurations

---

## Additional Resources

### Documentation

- **Plugin README**: [README.md](README.md) - Installation and build info
- **Transport Comparison**: [Transport_Module_Comparison.md](Transport_Module_Comparison.md) - Detailed transport comparison
- **WebRTC Guide**: [Source/Open3DTransportWebRTC/USER_GUIDE.md](Source/Open3DTransportWebRTC/USER_GUIDE.md) - WebRTC-specific setup

### Support

- **GitHub Issues**: Report bugs and feature requests
- **Documentation**: This guide and module-specific docs
- **Code Examples**: See `Source/*/Private/` for implementation examples

### Next Steps

1. **Start simple**: Use Loopback transport for learning
2. **Experiment**: Try different transports and settings
3. **Optimize**: Tune for your specific use case
4. **Scale**: Move to WebRTC for remote/multi-user
5. **Customize**: Implement custom transports if needed

---

**Happy Streaming!**
