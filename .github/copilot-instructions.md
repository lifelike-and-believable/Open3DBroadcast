# Open3DStream - AI Coding Agent Instructions

## Project Overview

Open3DStream is a **real-time 3D animation streaming protocol and library** that transmits skeletal animation, morph targets (curves), and transform data from motion capture systems to 3D engines (Unreal, Maya, MotionBuilder). Think of it as "network middleware for animation data" with multiple protocol backends.

**Core Architecture**: FlatBuffers serialization → Multiple network connectors (TCP/UDP/WebRTC/NNG) → Platform-specific plugins (LiveLink for Unreal, native plugins for Maya/MotionBuilder).

## Critical Build Requirements

### FlatBuffers Protocol Schema
**The schema is the source of truth** - when you see `o3ds_generated.h`, it's generated from `src/o3ds.fbs`:

```bash
flatc --cpp src/o3ds.fbs  # Regenerates o3ds_generated.h
```

**After schema changes**: Rebuild core library AND Unreal plugin. Schema changes are breaking changes.

### CMake Conditional Compilation
WebRTC support is **optional** via `O3DS_ENABLE_WEBRTC`:

```bash
# Without WebRTC (faster builds)
cmake .. && make

# With WebRTC (requires libdatachannel)
cmake .. -DO3DS_ENABLE_WEBRTC=ON && make
```

The Unreal plugin CMake (`plugins/unreal/Open3DStream/Source/Open3DStream/Open3DStream.Build.cs`) handles WebRTC linking automatically.

### Third-Party Dependencies as Git Submodules
Dependencies live in `thirdparty/` as submodules:
- `flatbuffers/` - Protocol serialization
- `nng/` - High-performance messaging library
- `libdatachannel/` + `mbedtls/` - WebRTC stack (optional)

**When pulling**: Always use `git clone --recurse-submodules` or `git submodule update --init --recursive`.

## Key Architectural Patterns

### Connector Hierarchy (src/o3ds/*.h)
All network protocols inherit from base classes:

```
Connector (base)
├── AsyncConnector (non-blocking with callbacks)
│   ├── AsyncNngConnector → AsyncSubscriber, AsyncPublisher, AsyncPair
│   └── WebRTCClient, WebRTCServer
└── BlockingNngConnector → Subscriber, Publisher, Pair

+ Standalone: TCPConnector, UDPConnector
```

**Pattern**: `start(url)` → `write(data, len)` or callback `OnData.BindRaw(this, &Class::OnPackage)`.

### Protocol URL Patterns
The library uses URL schemes to select connectors:
- `tcp://host:port` - TCP client
- `udp://host:port` - UDP server
- `webrtc://signaling-server:port/room` - WebRTC peer
- `nng+ipc://path` - NNG inter-process
- `nng+tcp://host:port` - NNG over TCP

**Implementation**: `src/o3ds/base_connector.cpp` parses URLs and instantiates the right connector.

### Data Model: Subject → Transform → Curves
The core data structure is hierarchical:

```cpp
SubjectList {
  vector<Subject> subjects;  // Multiple characters/objects
}

Subject {
  vector<Transform> transforms;  // Skeleton bones (parent hierarchy)
  vector<string> mCurveNames;    // Morph target names
  vector<float> mCurveValues;    // Morph target values [0.0-1.0]
}

Transform {
  TransformTranslation translation;
  TransformRotation rotation;      // Quaternion
  TransformScale scale;
  int mParentId;                   // -1 for root
}
```

**Serialization**: `SubjectList::Serialize(buffer)` → FlatBuffers → network → `SubjectList::ParseSubject(buffer)`.

**Delta Updates**: `SerializeUpdate()` sends only changed transforms/curves with indices - critical for performance.

### Unreal LiveLink Integration
The Unreal plugin (`plugins/unreal/Open3DStream/`) is a **LiveLink source**:

1. `FOpen3DStreamSource` wraps a connector (TCP/UDP/WebRTC)
2. `OnPackage()` callback receives FlatBuffers data
3. Parses into `FLiveLinkAnimationFrameData`:
   - `FrameData.Transforms` = skeletal bones
   - `FrameData.CurveNames` + `FrameData.CurveValues` = morph targets
4. LiveLink system routes to animation blueprints

**Key file**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp` - this is where network data becomes Unreal animation.

## Development Workflows

### Testing Protocol Changes
1. Edit `src/o3ds.fbs`
2. `flatc --cpp src/o3ds.fbs` (regenerate header)
3. Update `src/o3ds/model.cpp` (serialization logic)
4. Build test apps: `cd build && make apps/SubscribeTest`
5. Test: `./apps/SubscribeTest/SubscribeTest tcp://localhost:5555`

### Building Unreal Plugin (Windows)
**DO NOT use Unreal's "Rebuild" button** - it won't recompile native libraries. Use PowerShell scripts:

```powershell
# Link plugin into test project (creates junction)
.\Build\Scripts\Link-PluginIntoSandbox.ps1

# Build packaged plugin
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -PluginUPluginPath "$PWD\plugins\unreal\Open3DStream\Open3DStream.uplugin" `
  -OutDir "$PWD\Artifacts\Win64"
```

**Linux**: Use `link_plugin_into_sandbox.sh` then build in Unreal Editor.

### WebRTC Development (C++ Only - Not Yet in Unreal)
WebRTC is **functional in C++ command-line tools** but not yet in Unreal plugin:

```bash
# 1. Start signaling server
cd examples && node signaling-server.js

# 2. Build with WebRTC
cd build && cmake .. -DO3DS_ENABLE_WEBRTC=ON && make

# 3. Test connection
./apps/SubscribeTest/SubscribeTest webrtc://localhost:8080/testroom
```

**Unreal Status**: Build infrastructure complete ([Issue #13](https://github.com/lifelike-and-believable/Open3DStream/issues/13)), implementation pending ([Issue #15](https://github.com/lifelike-and-believable/Open3DStream/issues/15)).

### Packaging Releases
The `package.py` script creates **version-specific zip files**:

```bash
python package.py  # Creates Open3DStream_1.0.4.zip
```

**Output structure** (designed for easy extraction):
```
UE_5.4/Plugins/Open3DStream/  # Extract to project root
UE_5.5/Plugins/Open3DStream/
lib/                          # C++ static libraries
include/                      # Headers
```

Users extract `UE_5.X/` to their project root → auto-merges with existing `Plugins/`.

## Project-Specific Conventions

### Versioning
Version is defined **once** in `CMakeLists.txt`:
```cmake
set(O3DS_VERSION_TAG "1.0.4")
```

This propagates to `o3ds_version.h` and package names. **Never hardcode version strings elsewhere**.

### Error Handling Pattern
Connectors use state-based error reporting:
```cpp
if (!connector.start(url)) {
    std::cerr << connector.getError() << std::endl;
}
```

**No exceptions** - check return values and call `getError()`.

### Coordinate System Conversions
Unreal uses left-handed Z-up, but mocap systems vary. See `Open3DStreamSource.cpp:operator>>(Matrix&)` for axis mirroring logic:

```cpp
// This is intentional - DO NOT "simplify"
dst.Mirror(EAxis::X, EAxis::Y);  // Handles coordinate system conversion
```

### Memory Management: Transforms
`TransformList` **owns** its `Transform*` pointers - destructor deletes them. Don't manually delete transforms in a list.

## Integration Points

### Adding New Network Protocols
1. Inherit from `AsyncConnector` or `BlockingNngConnector`
2. Implement: `start(url)`, `write(data, len)`, `read(data, len)`
3. Add URL scheme check in `base_connector.cpp`
4. Update `src/CMakeLists.txt` to link dependencies
5. Add conditional compilation flag if optional (like WebRTC)

### Adding Curve/Morph Target Data
Curves are **per-Subject**, not per-Transform:

```cpp
subject->mCurveNames = {"Smile", "EyeBrowUp_L"};
subject->mCurveValues = {0.8f, 0.5f};  // Must match name count
```

**Unreal mapping**: Curve names must match morph target names in skeletal mesh.

### libdatachannel Pre-Built Libraries
WebRTC uses **vendored pre-built binaries** in `plugins/unreal/Open3DStream/lib/webrtc/`:
- `datachannel.lib` (Windows)
- `libdatachannel.a` (Linux/macOS)

**Rebuild only when updating libdatachannel version** via GitHub Actions workflow (`build-libdatachannel.yml` - manual trigger).

## Testing Strategies

### Quick Smoke Test
```bash
# Terminal 1: Listener
./build/apps/SubscribeTest/SubscribeTest tcp://localhost:5555

# Terminal 2: Sender (Python)
cd python
python listener.py  # Sends test frames
```

### Unreal Automation Tests
```powershell
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DStream.*"
```

### Protocol Debugging
Enable verbose logging in `src/o3ds/model.cpp` by uncommenting `#define DEBUG_PARSE`.

## Common Pitfalls

1. **Forgetting to regenerate FlatBuffers header** after `.fbs` changes → runtime corruption
2. **Mixing debug/release libraries** in Unreal → linker errors with MbedTLS
3. **Using absolute paths in junction links** → breaks on other machines (use relative)
4. **Building WebRTC without MbedTLS DTLS-SRTP enabled** → silent connection failures
5. **Committing generated files** - `o3ds_generated.h` is generated but committed (intentionally, for CI)

## Documentation References

- **Curve Support**: `CURVE_SUPPORT.md` - Morph target streaming
- **WebRTC (C++)**: `WEBRTC_QUICKSTART.md` - 5-minute WebRTC setup
- **WebRTC Architecture**: `WEBRTC_ARCHITECTURE.md` - Signaling flow diagrams
- **Build Scripts**: `Build/README.md` - PowerShell script documentation
- **libdatachannel Integration**: `LIBDATACHANNEL_INTEGRATION.md` - WebRTC build details

## Key Files for AI Agents

- `src/o3ds.fbs` - Protocol schema (source of truth)
- `src/o3ds/model.h` - Core data structures
- `src/o3ds/base_connector.h` - Network abstraction
- `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp` - Unreal LiveLink bridge
- `package.py` - Release packaging logic
- `thirdparty/CMakeLists.txt` - Dependency build configuration
