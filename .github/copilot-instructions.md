# Open3DStream — Copilot Instructions

This document is for AI coding agents working on Open3DStream. It captures goals, architecture, conventions, and high‑signal entry points so changes align with the project’s direction.

## Phase focus: O3DBroadcast (Unreal → Remote Clients)

We are now prioritizing O3DBroadcast: streaming final animation pose (bone transforms) and curve values (morphs/attributes) for multiple Skeletal Meshes from a single Unreal Engine instance to remote clients that use the Open3DStream LiveLink Source.

High‑level outcomes:
- UE acts as a sender for multiple SkeletalMeshComponents simultaneously.
- Each Skeletal Mesh becomes one Open3DStream Subject with:
  - Stable bone hierarchy and per‑frame final pose (component space or parent‑relative).
  - Per‑frame curve values (morph targets and animation curves).
- Transport‑agnostic output (TCP/UDP/WebRTC/NNG) via the connector layer.
- Remote clients (e.g., Unreal with the Open3DStream LiveLink Source, Maya/Mobu) subscribe and receive synchronized frames.
- Editor‑friendly configuration, live add/remove of meshes, robust connection lifecycle, and frame‑drop/backpressure handling.

Deliverables (MVP):
- An Unreal module that:
  - Registers SkeletalMeshComponents to broadcast.
  - Captures final evaluated animation pose and curve values each frame.
  - Packages data into Open3DStream Subject messages.
  - Sends them via a configured connector URL (supports webrtc://, tcp://, etc.).
- Minimal editor UI/settings for selecting meshes and connection URL.
- Example map/project settings and a smoke test guide with a second Unreal instance as a client using the existing Open3DStream LiveLink Source.

## Project goals and non‑goals

Goals (what we optimize for):
- Real‑time, low‑latency streaming of skeletal animation, morph curves, and transforms from MoCap/DCC tools and UE to engines.
- Stable, schema‑driven protocol (FlatBuffers) with compatibility strategies.
- Transport‑agnostic connectors (TCP/UDP/WebRTC/NNG) with a consistent async API and clear error reporting.
- Engine adapters and tools that convert protocol messages into frame data (Unreal LiveLink, Maya/MotionBuilder).
- Cross‑platform builds with reproducible packaging and minimal optional dependencies.
- Clear extension points for new message types, connectors, and engine adapters.
- WebRTC as a first‑class transport for NAT traversal and secure, low‑latency delivery.

Non‑goals (out of scope):
- Offline authoring or long‑term storage.
- Specialized lossy compression pipelines beyond lightweight per‑frame packing.
- Proprietary formats as the protocol source of truth.

## Core architecture (mental model)

- Data model: FlatBuffers schema (`src/o3ds.fbs`) produces `o3ds_generated.h`. Messages wrap a `SubjectList` with transforms and curves.
- Serialization layer: `src/o3ds/model.h/.cpp` packs/unpacks between in‑memory structures and FlatBuffers.
- Connector layer: URLs select a transport (see `src/o3ds/base_connector.h/.cpp`), exposing a uniform interface (`start(url)`, `read()`, `write()`, state, `getError()`).
- Consumers/adapters: Engine/DCC plugins parse messages and feed per‑frame data (e.g., Unreal LiveLink).
- Build system: CMake configures optional components (e.g., WebRTC via `O3DS_ENABLE_WEBRTC`) and platform builds.

Connector URL patterns:
- `tcp://host:port`
- `udp://host:port`
- `webrtc://host:port/room`
- `nng+ipc://path`
- `nng+tcp://host:port`

## WebRTC transport (integrated)

- Enable via `-DO3DS_ENABLE_WEBRTC=ON` (requires libdatachannel + mbedtls).
- Signaling: use the example signaling server under `examples/` and connect with `webrtc://host:port/room`.
- Configure ICE/STUN/TURN as needed; avoid hard‑coding credentials.
- Transport is transparent to adapters; they consume decoded messages.

## O3DBroadcast design (Unreal sender)

Module shape (proposed, minimal surface):
- Module: `O3DBroadcast` (under `plugins/unreal/Open3DStream/Source/O3DBroadcast/`).
- Subsystem: `UO3DBroadcastSubsystem` to manage connection(s), registered SkeletalMeshComponents, and per‑frame capture.
- Component: `UO3DBroadcastComponent` attachable to Actors or directly register `USkeletalMeshComponent*`.
- Settings: `UO3DBroadcastSettings` for a default connector URL, subject naming policy, update rate, and transport options (e.g., WebRTC ICE).
- Editor UI: Details customization to pick meshes and set a URL; optional toolbar toggle to start/stop streaming.

Capture points (final pose + curves):
- Preferred hooks:
  - Use Unreal’s “bone transforms finalized” signal on skinned components if available in your UE version (e.g., USkinnedMeshComponent/USkeletalMeshComponent callbacks) to get final evaluated transforms.
  - Alternatively, schedule capture after animation evaluation on the game thread and read:
    - Final pose: `USkeletalMeshComponent`’s evaluated transforms (component space or parent‑relative; choose one and document).
    - Curves: evaluated animation/morph curves from the component/anim instance’s curve container.
- Time stamping and sync:
  - Include `FQualifiedFrameTime` (Timecode + SubFrame) when available.
  - Also include a high‑resolution monotonic timestamp (e.g., cycles in seconds) for jitter analysis.
- Multiple meshes:
  - Each registered mesh is a separate Subject with a stable identifier:
    - Default: `{WorldName}/{ActorName}/{ComponentName}`.
    - Allow overrides per component.
  - Send per‑frame Subject messages in one `SubjectList` envelope, batched by frame time.
- Bone/curve mapping:
  - Send the bone name array once per Subject when first seen (or when skeleton changes), then per‑frame transforms aligned by index.
  - Curves: Send names once per Subject; per‑frame send only values aligned by index.

Backpressure and rate control:
- Do not queue unbounded frames. If the connector is busy, drop the oldest pending frame (latest‑wins).
- Optional target FPS (e.g., 60) with frame skipping under load.
- Coalesce multiple mesh subjects into one network write per frame for efficiency.

Error handling and resilience:
- No exceptions; use `bool` results and `getError()` for diagnostics.
- Auto‑reconnect on transient network failure with exponential backoff.
- Expose connection state to the editor UI and log concise state transitions.

Security:
- WebRTC uses DTLS/SRTP. Keep deps current.
- Do not hard‑code TURN credentials. Use project settings or environment variables.

## Wire format and schema usage

- Schema file: `src/o3ds.fbs` (FlatBuffers). Regenerate headers on change:
  ```bash
  flatc --cpp src/o3ds.fbs
  ```
- For each Subject (skeletal mesh):
  - Names: bone names vector; curve names vector.
  - Per‑frame: transforms array aligned to bone names; curve values aligned to curve names.
  - Include timestamps/timecode fields where available in the message.
- If you add fields for timecode or subject metadata, maintain backward compatibility:
  - Use optional fields with sensible defaults.
  - Bump `O3DS_VERSION_TAG` and update all consumers.

## Build and setup

Common commands:
- Regenerate schema header after `.fbs` changes:
  ```bash
  flatc --cpp src/o3ds.fbs
  ```
- Configure and build (native):
  ```bash
  mkdir -p build && cd build
  cmake ..          # add -DO3DS_ENABLE_WEBRTC=ON to include WebRTC
  make -j
  ```
- Clone with submodules:
  ```bash
  git clone --recurse-submodules <repo>
  # or, if already cloned:
  git submodule update --init --recursive
  ```

Notes:
- Schema changes are breaking; rebuild core + affected plugins.
- When enabling WebRTC, ensure third‑party versions match build type (debug/release) across the toolchain.

## Conventions and constraints

- Versioning: Single source in `CMakeLists.txt` via `set(O3DS_VERSION_TAG "...")`.
- Error handling: No exceptions in connector code paths; return `bool`, inspect `getError()`.
- Ownership: `TransformList` owns `Transform*`; do not manually free.
- Curves: Per‑Subject arrays; `mCurveNames` and `mCurveValues` counts must match.
- Threading: Connectors may be async; follow lifecycle (`start(url)` → steady state → `stop()`/destruct). Avoid engine‑thread blocking.
- Logging: Keep hot‑path logs minimal; provide state‑change and error logs.

## Extending the system

Add/modify O3DBroadcast components (Unreal):
1) Implement capture and subject packaging for a SkeletalMeshComponent.
2) Batch subjects into a `SubjectList` with timestamps.
3) Send via the selected connector URL.
4) Provide editor settings and a simple start/stop control.
5) Add end‑to‑end tests with two UE instances (sender/receiver).

Add a new connector (transport):
1) Inherit `AsyncConnector` or `BlockingNngConnector`.
2) Implement `start(url)`, `write()`, `read()`, state transitions, and `getError()`.
3) Register scheme handling in `src/o3ds/base_connector.cpp`.
4) Wire sources in `src/CMakeLists.txt`, add flags/options, update CI.
5) Add tests and URL usage docs.

Add new message fields/types:
1) Edit `src/o3ds.fbs`; prefer optional fields for compatibility.
2) Regenerate headers, update `src/o3ds/model.cpp` and all consumers.
3) Bump `O3DS_VERSION_TAG` and document.

## File plan (proposed additions)

- `plugins/unreal/Open3DStream/Source/O3DBroadcast/O3DBroadcast.Build.cs`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Public/O3DBroadcastSubsystem.h`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Private/O3DBroadcastSubsystem.cpp`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Public/O3DBroadcastComponent.h`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Private/O3DBroadcastComponent.cpp`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Public/O3DBroadcastSettings.h`
- `plugins/unreal/Open3DStream/Source/O3DBroadcast/Private/O3DBroadcastSettings.cpp`
- optional editor module for UI:
  - `plugins/unreal/Open3DStream/Source/O3DBroadcastEditor/*`

These should reuse the existing core library (connectors, model) located under `src/o3ds/*`.

## Quick test flows

- Two‑UE smoke test (O3DBroadcast → LiveLink Source over WebRTC):
  1) Start signaling: `node examples/signaling-server.js`
  2) Sender UE:
     - Add O3DBroadcast component or register meshes in the subsystem.
     - Set URL: `webrtc://localhost:8080/room`
     - Play in Editor; confirm “Connected” and streaming.
  3) Receiver UE (with Open3DStream LiveLink Source):
     - Create a new LiveLink source with the same URL.
     - Verify subjects appear; animate in Preview/PIE and confirm motion matches.
- TCP fallback:
  - Use `tcp://localhost:5555` on both sides; verify frames arrive.

## Performance and reliability targets

- Latency: aim for sub‑frame at 60 fps on localhost; keep jitter stable over networks.
- Throughput: batch multiple Subjects into a single `SubjectList` per frame.
- Backpressure: latest‑wins; no unbounded queues.
- Configurable reliability for WebRTC data channels (ordered/unordered, reliable/semi‑reliable) as appropriate.

## Security and networking

- Use DTLS/SRTP with current deps for WebRTC.
- No plaintext TURN credentials in source; use settings/env.
- Validate URL inputs; reject invalid schemes/parameters.

## Files to read first

- Protocol: `src/o3ds.fbs`, `o3ds_generated.h`, `src/o3ds/model.h/.cpp`
- Connectors: `src/o3ds/base_connector.h/.cpp`, implementations in `src/o3ds/`
- Unreal LiveLink source (receiver reference): `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`
- Packaging: `package.py`, `Build/` scripts

## AI agent playbook (O3DBroadcast tasks)

- Implement per‑mesh capture:
  - Choose transform space (document choice).
  - Capture after final pose evaluation.
  - Extract curve names once and values per frame.
- Subject lifecycle:
  - Stable subject naming; handle add/remove and skeleton changes.
  - Send “descriptor” (names) when needed, then stream frames.
- Connector usage:
  - Single connection, batched writes per frame.
  - Reconnect/backoff logic and concise state logging.
- Editor UX:
  - Settings for URL, FPS cap, and subject naming policy.
  - Simple “Start/Stop Broadcast” toggle, and a per‑mesh include/exclude list.

Always:
- Add small, focused tests (e.g., validate bone count/order consistency and curve alignment).
- Update this document if conventions or flows change.
- Keep diffs minimal and well‑commented around hot paths.

## PR checklist

- Schema changes (if any): regenerated headers, version bumped, consumers updated.
- O3DBroadcast:
  - Captures final pose and curves for multiple meshes.
  - Stable subject naming and lifecycle events.
  - Batched `SubjectList` per frame; backpressure handled.
  - Connector URL configurable; WebRTC tested with signaling.
  - Editor/UI minimal but functional; docs updated with test steps.
- Build/CI: flags and deps documented; CI green across targets.
- Performance: no significant regressions; logs are concise.

If you want, I can generate stubs for the O3DBroadcast Unreal module (headers, build.cs, and a minimal subsystem/component) and a quick guide map for testing.
