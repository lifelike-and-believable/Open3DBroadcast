# Open3DStream

> Lightweight and functional interface for live streaming of 3D animation content

Open3DStream is a standardized protocol and library for real-time streaming of skeletal animation and morph target data. It provides high-performance, low-latency transmission of animation frames over multiple network protocols.

## Features

### Animation Data

✅ Skeletal animation (transforms, rotations, scales)
✅ Animation curves for morph targets (facial animation)
✅ Delta-optimized updates for efficiency
✅ Hierarchical transform chains
✅ Multiple subjects per stream

### Network Protocols

✅ TCP Client/Server
✅ UDP Server
✅ NNG (Subscribe, Pair, Publisher)
✅ WebRTC: P2P (libdatachannel) and SFU (LiveKit)

### Platform Support

✅ Unreal Engine (LiveLink plugin)
✅ Maya (plugin)
✅ MotionBuilder (plugin)
✅ Python examples
✅ C++ library

## Quick Start

### Initialize Git submodules

This repository uses Git submodules for third-party dependencies (e.g., FlatBuffers, NNG, CRC). Before building locally or creating Docker images, initialize submodules:

```bash
git submodule update --init --recursive
```

### Building the Library

```bash
# Clone repository
git clone https://github.com/lifelike-and-believable/Open3DStream.git
cd Open3DStream

# Build with CMake
mkdir build && cd build
cmake ..
make -j4

# Build with WebRTC support (optional)
cmake .. -DO3DS_ENABLE_WEBRTC=ON
make -j4
```

### Docker: Repeater image

We provide a minimal Docker image for the Repeater app. Ensure submodules are initialized locally before building so the Docker build context contains third-party sources.

```bash
# From repo root (after submodule init)
docker build -f docker/Dockerfile.repeater -t open3dstream-repeater:local .

# Smoke test the image (non-root entrypoint)
docker run --rm --entrypoint /bin/sh open3dstream-repeater:local -c "echo ok"

# Run (example addresses)
docker run --rm open3dstream-repeater:local tcp://0.0.0.0:7000 tcp://0.0.0.0:7001
```

### Using in Unreal Engine

**Installation from Release Package:**
 
1. Download the latest release from [Releases](https://github.com/lifelike-and-believable/Open3DStream/releases)
2. Extract the zip file
3. Copy the appropriate `UE_X.X/Plugins/Open3DStream` folder to your project's `Plugins` directory
   - Or extract the entire `UE_X.X` folder to your project root (it will merge with your project structure)
4. Restart Unreal Editor
5. Enable the plugin: **Edit → Plugins** → search "Open3DStream" → check the box

**Installation from Source:**
 
1. Copy the `plugins/unreal/Open3DStream` folder to your project's `Plugins` directory
2. Build the native libraries (see Building the Library above)
3. Enable the plugin in your project settings

**Usage:**
 
1. Open LiveLink window: **Window → Virtual Production → Live Link**
2. Add source: **+ Source → Open3DStream Source**
3. Configure connection:
   - **URL**: `tcp://localhost:5555` (or other protocol)
   - **Protocol**: TCP Client, UDP Server, WebRTC Client, etc.
4. Click **Create**

#### Unreal Receiver Audio (WebRTC)

To hear audio from a WebRTC sender, add `O3DSRemoteAudioComponent` to an actor in your level:

- The component is a `USceneComponent` (attach to a parent component or socket as needed)
- It always creates and manages an internal `UAudioComponent` + procedural `USoundWave`
- Mirrored AudioComponent-style settings are available:
   - Volume/Pitch at the top; Attenuation (with override gating); Submix Sends; Source Effect Chain; Concurrency; AutoActivate
- Client-first and reconnect flows are supported (signaling restarts, re-offer on DataChannel close)

See `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md` for step-by-step setup and troubleshooting.

## Recent Updates

### Module layout update (UE 5.6)

We introduced a new shared Unreal module, `Open3DShared`, to host cross‑cutting code and third‑party integration wiring used by both receiver (`Open3DStream`) and sender (`Open3DBroadcast`).

- Open3DShared now contains:
   - WebRTC abstractions and implementations (connector, data channel, signaling)
   - Shared loopback registry for serialized frames (decouples Broadcast from Stream)
   - Logging categories and console variables
   - Helper utilities (subject name sanitize, wildcard matching, URL parsing/normalization)
   - Consolidated third‑party linking (libdatachannel, OpenSSL, Opus)

- Open3DBroadcast changes:
   - No longer depends on `Open3DStream`
   - Audio capture component lives in Broadcast
   - Built‑in transports consume helpers from `Open3DShared`
   - User URL typos like `tcp://0.0.0.0.9000` are auto‑normalized to `tcp://0.0.0.0:9000`

- Open3DStream changes:
   - Registers a LiveLink‑backed consumer via the shared loopback registry
   - Depends on `Open3DShared` instead of `Open3DBroadcast`

These changes preserve prior behavior while making responsibilities clearer and avoiding circular dependencies.

### Animation Curve Support (October 2025)
 
Added support for animation curves to enable morph target-based facial animation:

### WebRTC Status (November 2025)
### Receiver Audio Polish & Reconnect (November 2025)

- `O3DSRemoteAudioComponent` is now a `USceneComponent` with attachment options (parent/socket)
- Always-owned internal `UAudioComponent` + procedural `USoundWave`
- Mirrored AudioComponent features: Submix Sends, Source Effects Chain, Concurrency, AutoActivate (default true)
- Editor experience: categories/tooltips aligned with `UAudioComponent` layout; Volume/Pitch surfaced at the top
- Reconnect/resilience: client-first offer retry, signaling reconnect, re-offer on DataChannel close; receiver resets PC on new offers

WebRTC backends are now unified behind a connector interface:

- LibDataChannel (P2P) and LiveKit (SFU) are selectable backends in Unreal.
- URL assembly and backend-specific semantics live inside connectors; you no longer add `role=` or backend hints to URLs.
- A Token field is available under WebRTC settings when required by the chosen backend (e.g., LiveKit). A dynamic hint guides the expected token format.
- C++ command-line tools support WebRTC – see [WEBRTC_QUICKSTART.md](WEBRTC_QUICKSTART.md)
- Unreal plugin includes WebRTC connectors for both backends – see [LIVEKIT_README.md](LIVEKIT_README.md) and testing guide below.

### QUIC Transport Updates (November 2025)

- The QUIC sender now enforces MsQuic control-payload limits and tears down streams immediately when MsQuic rejects a send, preventing slow memory/handle leaks during long capture sessions.
- Tracks configured as `unreliable` automatically publish via MsQuic datagrams when datagrams are allowed; when datagrams are disabled we log a one-time warning and fall back to reliable streams.
- QUIC transport stats now count frames/bytes once per published frame (instead of once per subscriber) so dashboards stay aligned with other transports. New automation tests cover both the successful fan-out and drop paths.

## Documentation

### Feature docs

- [Animation Curve Support](CURVE_SUPPORT.md) - Morph target streaming
- [Curve Implementation Summary](IMPLEMENTATION_SUMMARY.md) - Technical details

### WebRTC (C++ tools)

- [WebRTC Quick Start](WEBRTC_QUICKSTART.md) - 5-minute WebRTC setup for C++ tools
- [WebRTC Full Documentation](WEBRTC_SUPPORT.md) - Complete WebRTC guide
- [WebRTC Implementation](WEBRTC_IMPLEMENTATION_SUMMARY.md) - Architecture details

### WebRTC (Unreal plugin)

- [Unreal WebRTC implementation notes](WEBRTC_UNREAL_IMPLEMENTATION.md)
- [libdatachannel Integration](LIBDATACHANNEL_INTEGRATION.md)
- [Issue #15](https://github.com/lifelike-and-believable/Open3DStream/issues/15) - Implementation roadmap
- [LiveKit Backend Overview](LIVEKIT_README.md)
- [LiveKit vs. LibDataChannel](plugins/unreal/Open3DStream/docs/WEBRTC_BACKENDS_COMPARISON.md)

### WebRTC Backends (Unreal)

- Backend selection: choose `LibDataChannel` (P2P) or `LiveKit` (SFU) from WebRTC settings in the UI.
- Roles: components determine roles (Broadcaster acts as Publisher; Live Link Source acts as Subscriber). Do not append `role=` to URLs.
- URL inputs: provide the signaling base URL and Room. Connectors assemble the final signaling address internally per backend.
- Token: enter under the WebRTC section when required by the backend. The UI provides a backend-specific hint (e.g., LiveKit JWT).
- Reliability: data channels support reliable/lossy modes internally; no URL flags are required for typical use.

## Docs Refresh and Source of Truth

When in doubt, the FlatBuffers schema is the source of truth for the protocol data model:

- Schema: `src/o3ds.fbs`
- Regenerate header after schema changes: `flatc --cpp src/o3ds.fbs`
- Generated header (checked in for CI): `src/o3ds_generated.h`

If you update the schema, rebuild the core library and the Unreal plugin.


## Protocol Comparison

### Unreal Plugin (Current Release)

| Protocol | Status | Latency | Setup | NAT Traversal | Encryption | Best For |
|----------|--------|---------|-------|---------------|------------|----------|
| TCP | ✅ Ready | Medium | Easy | ❌ | ❌ | Local networks |
| UDP | ✅ Ready | Low | Easy | ❌ | ❌ | Low latency, lossy OK |
| NNG | ✅ Ready | Low | Medium | ❌ | ❌ | Microservices |
| WebRTC | ✅ Ready | Low | Medium | ✅ | ✅ | Cloud, remote |

**WebRTC Status**: Unreal WebRTC path supports both libdatachannel (P2P) and LiveKit (SFU). Backend-specific URL details are handled by connectors; configure URL/Room (and Token if required) in the WebRTC section.

### C++ Command-Line Tools

| Protocol | Status | Best For |
|----------|--------|----------|
| TCP | ✅ Ready | Local networks |
| UDP | ✅ Ready | Low latency |
| NNG | ✅ Ready | Microservices |
| **WebRTC** | **✅ Ready** | **Cloud, remote** |

For C++ WebRTC usage, see [WEBRTC_QUICKSTART.md](WEBRTC_QUICKSTART.md).

## Example Usage

### C++ Sender

```cpp
#include "o3ds/model.h"
#include "o3ds/tcp.h"

O3DS::SubjectList subjects;
auto subject = subjects.addSubject("Character");

// Add skeleton
auto root = subject->addTransform("Root", -1);
root->translation.value.v[0] = 1.0;

// Add facial curves
subject->mCurveNames = {"Smile", "EyeBrowUp_L", "EyeBrowUp_R"};
subject->mCurveValues = {0.8f, 0.5f, 0.3f};

// Serialize and send
std::vector<char> buffer;
subjects.Serialize(buffer);
tcpSocket.Send(buffer);
```

### Python Receiver

```python
# Minimal example: receive TCP data and inspect FlatBuffers
# Note: Python utilities are provided as examples; a full Python API is not bundled.
import socket, struct

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
   s.connect(("127.0.0.1", 5555))
   header = s.recv(18)
   if header[:4] != b"\x00\xff\x03\xfe":
      raise SystemExit("Invalid stream header")
   size = struct.unpack("I", header[14:18])[0]
   payload = b""
   while len(payload) < size:
      chunk = s.recv(size - len(payload))
      if not chunk:
         break
      payload += chunk
   print(f"Received {len(payload)} bytes of O3DS data")
```

## Packaging for Release

The project includes a `package.py` script that creates release packages with proper directory structure:

```bash
python package.py
```

This creates a zip file with the following structure:

```text
UE_5.4/Plugins/Open3DStream/
UE_5.5/Plugins/Open3DStream/
lib/
include/
```

Users can extract the entire version folder (e.g., `UE_5.5`) to their project root, and it will automatically merge with their project's `Plugins` directory.

## Building Plugins

### Unreal Engine

```bash
# Plugin is ready to use - just copy to your project
cp -r plugins/unreal/Open3DStream /path/to/YourProject/Plugins/
```

### Maya

```bash
cd plugins/maya
mkdir build && cd build
cmake ..
make
# Load plugin in Maya
```

### MotionBuilder

```bash
cd plugins/mobu
mkdir build && cd build
cmake ..
make
# Install plugin
```

## Requirements

### Core Library

- CMake 3.13+
- C++17 compiler
- FlatBuffers
- NNG (for NNG protocols)

### WebRTC (Optional)

- libdatachannel (enabled by default)
- nlohmann/json
- Node.js (for signaling server)

## Contributing

We welcome contributions! Areas of interest:

- New protocol implementations
- Plugin improvements
- Performance optimizations
- Documentation
- Bug fixes

### CI expectations for contributors

- Submodules are required for all CI builds. Local development and CI expect `git submodule update --init --recursive` to have been run.
- The Docker Repeater image is built in CI for pull requests that touch code or the Dockerfile and is pushed for `main` and nightly scheduled runs.
- Workflows use a lean trigger set and smoke-test the image by running a no-op command inside the container to catch regressions early.

## Community

- **Discord**: <https://discord.gg/UdEbyFM9wD>
- **Issues**: [GitHub Issues](https://github.com/lifelike-and-believable/Open3DStream/issues)
- **Discussions**: [GitHub Discussions](https://github.com/lifelike-and-believable/Open3DStream/discussions)

## License

MIT License - see [LICENSE](LICENSE) file for details

## Credits

**Creator**: Alastair Macleod (<http://mocap.ca/>)

**Contributors**:

**Curve Support & WebRTC Implementation**: Added October 2025


*For detailed technical documentation, build instructions, and troubleshooting, see the linked documentation files above.*
