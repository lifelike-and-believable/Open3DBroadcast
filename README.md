# Open3DStream

> Lightweight and functional interface for live streaming of 3D animation content

Open3DStream is a standardized protocol and library for real-time streaming of skeletal animation and morph target data. It provides high-performance, low-latency transmission of animation frames over multiple network protocols.

## Features

### Animation Data
- ✅ Skeletal animation (transforms, rotations, scales)
- ✅ **NEW**: Animation curves for morph targets (facial animation)
- ✅ Delta-optimized updates for efficiency
- ✅ Hierarchical transform chains
- ✅ Multiple subjects per stream

### Network Protocols
- ✅ TCP Client/Server
- ✅ UDP Server
- ✅ WebSocket (client/broadcast)
- ✅ NNG (Subscribe, Pair, Publisher)
- ✅ **NEW**: WebRTC peer-to-peer with NAT traversal

### Platform Support
- ✅ Unreal Engine (LiveLink plugin)
- ✅ Maya (plugin)
- ✅ MotionBuilder (plugin)
- ✅ Python API
- ✅ C++ library

## Quick Start

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

### Using in Unreal Engine

1. Copy the `plugins/unreal/Open3DStream` folder to your project's `Plugins` directory
2. Enable the plugin in your project settings
3. Open LiveLink window: **Window → Virtual Production → Live Link**
4. Add source: **+ Source → Open3DStream Source**
5. Configure connection:
   - **URL**: `tcp://localhost:5555` (or other protocol)
   - **Protocol**: TCP Client, UDP Server, WebRTC Client, etc.
6. Click **Create**

## Recent Updates

### Animation Curve Support (October 2025)
Added support for animation curves to enable morph target-based facial animation:
- Curve names and values transmitted alongside skeletal data
- Integrated with LiveLink animation frames
- Delta-optimized curve updates
- See [CURVE_SUPPORT.md](CURVE_SUPPORT.md) for details

### WebRTC Protocol (October 2025)
Implemented WebRTC as a network transport for peer-to-peer streaming:
- Low-latency peer-to-peer connections
- Built-in NAT traversal (STUN/TURN)
- DTLS encryption
- Room-based peer grouping
- See [WEBRTC_QUICKSTART.md](WEBRTC_QUICKSTART.md) for 5-minute setup

## Documentation

- [Animation Curve Support](CURVE_SUPPORT.md) - Morph target streaming
- [Curve Implementation Summary](IMPLEMENTATION_SUMMARY.md) - Technical details
- [WebRTC Quick Start](WEBRTC_QUICKSTART.md) - 5-minute WebRTC setup
- [WebRTC Full Documentation](WEBRTC_SUPPORT.md) - Complete WebRTC guide
- [WebRTC Implementation](WEBRTC_IMPLEMENTATION_SUMMARY.md) - Architecture details

## Protocol Comparison

| Protocol | Latency | Setup | NAT Traversal | Encryption | Best For |
|----------|---------|-------|---------------|------------|----------|
| TCP | Medium | Easy | ❌ | ❌ | Local networks |
| UDP | Low | Easy | ❌ | ❌ | Low latency, lossy OK |
| WebSocket | Medium | Medium | ❌ | Optional | Web apps |
| NNG | Low | Medium | ❌ | ❌ | Microservices |
| **WebRTC** | **Low** | **Medium** | **✅** | **✅** | **Cloud, remote** |

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
from o3ds import SubjectList, AsyncSubscriber

def on_data(data):
    subjects = SubjectList()
    subjects.Parse(data)
    
    for subject in subjects:
        print(f"Subject: {subject.name}")
        print(f"Transforms: {len(subject.transforms)}")
        print(f"Curves: {len(subject.curve_names)}")

subscriber = AsyncSubscriber()
subscriber.set_callback(on_data)
subscriber.start("tcp://localhost:5555")
```

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
- libdatachannel
- nlohmann/json
- Node.js (for signaling server)

## Contributing

We welcome contributions! Areas of interest:
- New protocol implementations
- Plugin improvements
- Performance optimizations
- Documentation
- Bug fixes

## Community

- **Discord**: https://discord.gg/UdEbyFM9wD
- **Issues**: [GitHub Issues](https://github.com/lifelike-and-believable/Open3DStream/issues)
- **Discussions**: [GitHub Discussions](https://github.com/lifelike-and-believable/Open3DStream/discussions)

## License

MIT License - see [LICENSE](LICENSE) file for details

## Credits

**Creator**: Alastair Macleod (http://mocap.ca/)

**Contributors**:
- Darin Velarde (http://singularityhitech.com/)

**Curve Support & WebRTC Implementation**: Added October 2025

---

*For detailed technical documentation, build instructions, and troubleshooting, see the linked documentation files above.*
