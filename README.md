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
✅ WebRTC peer-to-peer with NAT traversal (libdatachannel)

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

## Recent Updates

### Animation Curve Support (October 2025)
 
Added support for animation curves to enable morph target-based facial animation:

### WebRTC Status (October 2025)

libdatachannel is integrated with MbedTLS and enabled by default across the codebase:

- C++ command-line tools support WebRTC (ready) – see [WEBRTC_QUICKSTART.md](WEBRTC_QUICKSTART.md)
- Unreal plugin includes a functional WebRTC connector (beta) using a lightweight WebSocket signaling server – see [WEBRTC_UNREAL_IMPLEMENTATION.md](WEBRTC_UNREAL_IMPLEMENTATION.md)

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

**Audio Integration Planning:**
- [Planning Summary](plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_PLANNING_SUMMARY.md) - Executive overview and next steps
- [Technical Plan](plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_INTEGRATION_PLAN.md) - Comprehensive 5-phase plan (~7 weeks)
- [Implementation Roadmap](plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_ROADMAP.md) - Quick reference with checklists

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
| WebRTC | ✅ Ready (Beta) | Low | Medium | ✅ | ✅ | Cloud, remote |

**WebRTC Status**: Unreal WebRTC path implemented with libdatachannel ([Issue #15](https://github.com/lifelike-and-believable/Open3DStream/issues/15)); uses a simple WebSocket signaling server. Marked Beta pending broader testing.

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
