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
# Open3DStream — Copilot instructions (concise)

Purpose: get AI coding agents productive quickly. Focus on the protocol schema, connector hierarchy, build/test flows, and project conventions.

Core flow (big picture):
- Data model: FlatBuffers schema -> serialized SubjectList (see `src/o3ds.fbs` and generated `o3ds_generated.h`).
- Transport: Connectors pick a protocol from URL schemes (see `src/o3ds/base_connector.cpp` / `src/o3ds/base_connector.h`).
- Consumers: platform plugins (Unreal LiveLink in `plugins/unreal/.../Open3DStreamSource.cpp`, Maya/Mobu plugins) convert FlatBuffers -> engine frame data.

Quick, essential commands:
- Regenerate schema header: `flatc --cpp src/o3ds.fbs` (always when `.fbs` changes).
- Normal build: `mkdir -p build && cd build && cmake .. && make`.
- Enable WebRTC (optional): `cmake .. -DO3DS_ENABLE_WEBRTC=ON && make` (requires libdatachannel + mbedtls).
- Clone with submodules: `git clone --recurse-submodules <repo>` or `git submodule update --init --recursive`.

Key project conventions (do not change casually):
- Version is defined in `CMakeLists.txt` via `set(O3DS_VERSION_TAG "...")` and propagated to headers/packages.
- No exceptions: connectors return bool state; on error call `getError()` (see connector usage in `src/o3ds/*`).
- Memory: `TransformList` owns `Transform*` — do not delete them manually.
- Curves (morph targets) are per-Subject (`mCurveNames`, `mCurveValues`) — counts must match.

Connector & URL patterns (implementations to inspect):
- `tcp://host:port`, `udp://host:port`, `webrtc://host:port/room`, `nng+ipc://path`, `nng+tcp://host:port`.
- To add a protocol: inherit `AsyncConnector`/`BlockingNngConnector`, implement `start(url)`, `write()`, `read()`, then add scheme handling in `base_connector.cpp` and update `src/CMakeLists.txt`.

# Open3DStream — Copilot instructions (concise)

Purpose: make an AI coding agent productive fast. Focus on the schema, connector layer, build/test flows, and project conventions that are specific to this repo.

Core architecture (high level):
- Data: FlatBuffers schema -> SubjectList messages (`src/o3ds.fbs` -> `o3ds_generated.h`).
- Transport: connector layer picks an implementation from URL schemes (see `src/o3ds/base_connector.cpp` / `src/o3ds/base_connector.h`).
- Consumers: platform adapters convert parsed FlatBuffers to engine frames (Unreal LiveLink in `plugins/unreal/.../Open3DStreamSource.cpp`).

Commands you'll use often:
- Regenerate schema: `flatc --cpp src/o3ds.fbs` (required after any `.fbs` change).
- Build native: `mkdir -p build && cd build && cmake .. && make`.
- With WebRTC: `cmake .. -DO3DS_ENABLE_WEBRTC=ON && make` (requires libdatachannel + mbedtls).
- Ensure submodules: `git submodule update --init --recursive` (or clone with `--recurse-submodules`).

Concrete repo conventions (do not change casually):
- Version is the single source in `CMakeLists.txt` (`O3DS_VERSION_TAG`).
- No exceptions: connectors return bool; call `getError()` for diagnostics.
- `TransformList` owns `Transform*` objects — do not free them manually.
- Curves are per-Subject: `mCurveNames` and `mCurveValues` must have matching counts.

Connector & URL patterns to reference:
- `tcp://host:port`, `udp://host:port`, `webrtc://host:port/room`, `nng+ipc://path`, `nng+tcp://host:port`.
- To add a protocol: inherit the appropriate connector (`AsyncConnector` or `BlockingNngConnector`), implement `start(url)`, `write()`, `read()`, then wire the scheme parser in `base_connector.cpp` and add CMake targets in `src/CMakeLists.txt`.

Where to start (high signal-to-noise files):
- Protocol: `src/o3ds.fbs` (schema), `o3ds_generated.h` (generated), `src/o3ds/model.cpp` / `model.h` (serialize/parse).
- Connectors: `src/o3ds/base_connector.h` / `base_connector.cpp` and implementations in `src/o3ds/`.
- Engine adapter: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp` (LiveLink bridge).
- Packaging: `package.py` and `Build/` (Windows PowerShell helpers).

Quick test flow examples:
- Smoke test (native): build `apps/SubscribeTest`, run `./apps/SubscribeTest/SubscribeTest tcp://localhost:5555` and use `python/listener.py` (in `python/`) as a sender.
- WebRTC dev: start `examples/signaling-server.js` then run SubscribeTest with `webrtc://` (note: WebRTC currently works in C++ tools; Unreal integration is partial).

Debugging tips specific to this codebase:
- Toggle verbose parse logging in `src/o3ds/model.cpp` (look for the DEBUG_PARSE flag).
- If schema changes, always regenerate the header before building; mismatches cause silent runtime errors.
- Watch for mixing debug/release third-party libs (mbedtls/libdatachannel) when building plugins for Unreal.

If you want, I can expand any section (build, WebRTC, Unreal wiring, adding a connector) with exact file edits and a small test. Reply which area to expand.
