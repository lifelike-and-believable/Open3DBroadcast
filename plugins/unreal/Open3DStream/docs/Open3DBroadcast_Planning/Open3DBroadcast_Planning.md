# Open3DBroadcast Plan and Issue Generation Guide

Purpose
- Define a clear, incremental plan to implement Open3DBroadcast: capture final pose + curves from multiple Skeletal Meshes in Unreal and stream using the Open3DStream (O3DS) protocol over supported transports to remote Unreal app clients that use the Open3DStream LiveLink source.
- Provide ready-to-adapt issue seeds with acceptance criteria, dependencies, and references.
- Give coding agents the guardrails, conventions, and links they need to implement tasks correctly and efficiently.

Scope
- Unreal Engine 5.4+ plugin: new broadcast component/module inside the existing plugin at plugins/unreal/Open3DStream.
- Protocol: Reuse existing O3DS FlatBuffers schema and serializers in src/.
- Transports (first-class): TCP, UDP, NNG. WebRTC is included as optional/beta, aligned with repository status.
- Targets: Single mesh prototype → multi-mesh → robust transport handling → UX → tests/docs.

Key references in this repository
- Protocol and examples
  - [README.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/README.md)
  - FlatBuffers schema: src/o3ds.fbs (source of truth)
  - Generated header: src/o3ds_generated.h
  - C++ example in README shows Subject/SubjectList usage
- Curve semantics
  - [CURVE_SUPPORT.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/CURVE_SUPPORT.md)
  - Tests: [test_curve_comprehensive.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curve_comprehensive.cpp), [test_curves.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curves.cpp)
- Transports in core library
  - Build sources: [src/CMakeLists.txt](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/CMakeLists.txt)
  - TCP/UDP: src/o3ds/tcp.cpp, src/o3ds/udp.cpp, src/o3ds/udp_fragment.cpp, socket utils
  - NNG connectors: [src/o3ds/nng_connector.h](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/nng_connector.h), src/o3ds/publisher.cpp, src/o3ds/subscriber.cpp, [src/o3ds/pair.cpp](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/pair.cpp), request/reply and pipeline
  - Sphinx connector overview: [sphinx/connectors.rst](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/sphinx/connectors.rst)
- WebRTC (optional/beta path)
  - [WEBRTC_IMPLEMENTATION_SUMMARY.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_SUMMARY.md)
  - [WEBRTC_IMPLEMENTATION_COMPLETE.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_COMPLETE.md)
  - [WEBRTC_SUPPORT.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_SUPPORT.md)
  - [LIBDATACHANNEL_INTEGRATION.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/LIBDATACHANNEL_INTEGRATION.md)
  - [ISSUE_8_IMPLEMENTATION.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_8_IMPLEMENTATION.md)
- Unreal test harness and CI
  - [ProjectSandbox/README.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ProjectSandbox/README.md)
  - [Build/README.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/Build/README.md)
  - CI workflows overview: [.github/workflows/README.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/.github/workflows/README.md)

Current implementation status (UE 5.6)
- Modules/classes in this repo implementing broadcast capture and serialization:
  - `Open3DBroadcast` module with `UO3DSBroadcastComponent` (capture) and `FO3DSBroadcastSerializer` (O3DS mapping/bytes).
  - Dev automation tests in `Plugins/Open3DStream/Source/Open3DBroadcast/Private/Tests` validating protocol round-trip.
- Capture pipeline (component):
  - Binds to `USkinnedMeshComponent::OnBoneTransformsFinalized` to sample after evaluation; tick disabled.
  - Caches skeleton (`FReferenceSkeleton`) into a `FO3DSSkeletonDescriptor` with FNV-1a `Hash` and sets `bDescriptorDirty` on change.
  - Computes parent-relative transforms from component-space, with fallback parent resolution by name if LOD/required-bones reduce the active set (prevents OOB when `CompSpace.Num() != RefSkeleton.GetNum()`).
  - Subject naming uses `BuildSubjectName` + `SanitizeSubjectName`; allowed chars `[-._A-Za-z0-9/]`, whitespace replaced by `_`.
  - Optional rate limiting via `CaptureRateHz`.
- Curves (component):
  - Builds a stable curve set: morph targets from mesh + named animation curves from skeleton metadata; deterministic lexicographic order.
  - Options: clamp morphs to [0,1], drop NaN/Inf, include/exclude wildcards (`*`/`?`), epsilon and delta filtering (`CurveEpsilon`, `CurveDeltaThreshold`) with per-index last-sent state.
  - Filtering logs optional; include/exclude patterns applied when (re)building the cache; changing patterns requires `RefreshCurveCache`.
- Serialization (serializer):
  - Maps descriptor and per-frame `FTransform` data into `O3DS::Subject`/`SubjectList` using `o3ds/model.h`.
  - Currently sends full-frame payloads (skeleton + per-frame TRS + full curves) and emits `OnSerializedFrame(Subject, Buffer, Timestamp)`.
  - Calls `CalcMatrices()` on `O3DS::Subject` before serialize to ensure matrices are propagated.
  - Timestamp in `Serialize` is currently `0.0`; wall-clock/timecode selection pending.
  - Delta updates (`Subject.SerializeUpdate` / `SubjectList.SerializeUpdate`) are not used yet; per-subject cache exists but not leveraged for updates.
- Tests:
  - `O3DSRoundTripTests.cpp` covers basic subject round-trip, curves, and update semantics using the core model API. These validate conformance and give examples for delta-threshold behavior.
- Build/runtime toggles:
  - Build define: `O3DS_WITH_BROADCAST` can disable the module as a stub in `Open3DBroadcast.Build.cs`.
  - CVars: `o3ds.Broadcast.DebugPose`, `o3ds.Broadcast.DebugCurves`, `o3ds.Broadcast.OnScreen` for verbose logging and on-screen notifications.

Known gaps and open items
- Transport wiring: no sender transports are hooked up yet. `FO3DSBroadcastSerializer` exposes `OnSerializedFrame`, but no TCP/UDP/NNG/WebRTC transmitter is connected in the broadcast path.
- Delta frames: serializer always sends full frames; does not use `SubjectList::SetDeltaThreshold` nor `SerializeUpdate` to emit sparse updates.
- Timestamp/timecode: `Serialize` uses `0.0`; LiveLink/receiver-side time alignment needs a consistent source (UE timecode, `FPlatformTime::Seconds`, `FApp::GetCurrentTime`, etc.).
- Multi-subject: component captures a single target; no subsystem yet to multiplex multiple subjects/components.
- Editor UX: no editor UI for selecting transports/endpoints, subject naming presets, or runtime status.
- Resilience: no reconnection/backoff logic, and no descriptor resend on reconnect (only on skeleton change within a session).
- Threading/perf: capture/serialize/send happens on the game thread; no async pipeline, no quantization/batching.

Transport pairing matrix (Broadcast role vs LiveLink Source option)
Use complementary roles so the Broadcast sender connects correctly to the Unreal LiveLink source.

| LiveLink Source option (receiver) | Broadcast role (this work) | Notes |
|---|---|---|
| TCP Client | TCP Server | Broadcast listens; client connects |
| UDP Server | UDP Client | Broadcast sends to IP:port (no handshake) |
| NNG Subscribe (to Publish) | NNG Publish | Broadcast publishes; LiveLink subscribes |
| NNG Client (to Server) | NNG Server | Broadcast listens as server |
| NNG Server (to Client) | NNG Client | Broadcast dials the server |
| WebRTC Client/Server | Complementary WebRTC role | Optional/Beta until hardened |

Architecture and rules of engagement
- Schema: Do not redefine protocol messages. Use src/o3ds.fbs via model/serializer APIs in src/.
- Curves: Follow naming, ranges, and filtering in CURVE_SUPPORT.md. Favor parity with existing tests.
- Timing: Include timecode/frame number aligned with LiveLink expectations; choose a consistent UE time source.
- Transform space/coords: Decide and document whether to use component/global space; adhere to Unreal/receiver coordinate system and units.
- Performance: Avoid game thread stalls; consider capture/encode/send pipeline and optional quantization and batching.

Milestones, tasks, and acceptance criteria
Each task is designed to be a standalone issue with clear deliverables.

For a step-by-step M1 validation walkthrough, see: [M1_TEST_GUIDE.md](./M1_TEST_GUIDE.md)

M0 — Protocol and role alignment
- Tasks
  - Confirm message types and versioning from src/o3ds.fbs and model APIs for skeleton descriptions, transforms, curves.
  - Adopt CURVE_SUPPORT.md naming/ranging; define any filtering rules.
  - Decide transform space, coordinate system, units; define timecode source and mapping to LiveLink timing.
  - Document broadcast/receiver role pairing per transport (table above).
- Acceptance
  - Architecture notes added under docs/ (this file can be the anchor, plus a short mapping doc if needed).
  - LiveLink pairing is unambiguous for all supported transports.
- **Issues Created**:
  - [ISSUE_M0_1_PROTOCOL_ALIGNMENT.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_1_PROTOCOL_ALIGNMENT.md) - Protocol message types and versioning confirmation
  - [ISSUE_M0_2_CURVE_SEMANTICS.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_2_CURVE_SEMANTICS.md) - Curve semantics and filtering rules
  - [ISSUE_M0_3_TRANSFORM_SPACE.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_3_TRANSFORM_SPACE.md) - Transform space, coordinate system, and timing decisions
  - [ISSUE_M0_4_TRANSPORT_ROLES.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_4_TRANSPORT_ROLES.md) - Transport role pairing documentation

M1 — Single-mesh capture (Unreal)
- Status: Implemented in `UO3DSBroadcastComponent` (capture) with skeleton caching, subject naming, pose/curve extraction, filtering options, and rate limiting.
- Acceptance
  - In PIE, debug/log output shows bone transforms and curves matching Anim Previewer/AnimBP for a sandbox test mesh.

M2 — O3DS serialization integration
- Status: Implemented basic full-frame serialization in `FO3DSBroadcastSerializer` with `OnSerializedFrame` event; round-trip tests present. Delta updates and timestamping deferred.
- Acceptance
  - Unit tests pass; payloads conform to schema; no divergence from src-generated structures.

M3 — Transport integration (TCP, UDP, NNG)
- Tasks
  - Implement a transport interface in the plugin and wire to core connectors:
    - TCP: src/o3ds/tcp.cpp
    - UDP (+fragmentation if needed): src/o3ds/udp.cpp, src/o3ds/udp_fragment.cpp
    - NNG publish/subscribe, pair, request/reply as needed: src/o3ds/*
  - Expose a protocol selector in the Broadcast UI mirroring the LiveLink Source options.
  - Provide loopback validation via:
    - Local Unreal LiveLink source in a second instance, or
    - Minimal receiver harness (e.g., TCP) for smoke tests.
- Acceptance
  - Skeleton + frames stream successfully over TCP, UDP, and NNG at target FPS over short tests without drops.

M4 — Multi-mesh multiplexing and subject identity
- Tasks
  - Broadcast multiple `USkeletalMeshComponent`s as distinct O3DS subjects; stable naming scheme with collision handling.
  - Send per-subject skeleton description once and on change.
- Acceptance
  - Two+ subjects stream concurrently; receiver lists and consumes each stream correctly; skeleton change triggers resend.

M5 — Timing, rate control, and performance
- Tasks
  - Embed timecode/frame numbers using a chosen UE time source consistent with LiveLink behavior.
  - Add configurable update rate, batching, and optional quantization for transforms and curves.
  - Split capture/encode/send across threads to prevent game thread stalls.
- Acceptance
  - Profiling in ProjectSandbox shows acceptable CPU/GPU/memory overhead for target meshes/bones and FPS; monotonic timestamps.

M6 — Editor UX and configuration
- Tasks
  - Actor Component or Subsystem:
    - Mesh discovery/selection,
    - Transport selection and endpoint/URL config,
    - Rate and quantization controls,
    - Subject naming controls,
    - Start/Stop and live status indicators.
  - Config assets and/or Project Settings for default profiles.
- Acceptance
  - Non-programmers can configure and start a broadcast; settings persist; runtime status is visible.

M7 — Resilience and observability
- Tasks
  - Reconnection/backoff (TCP/NNG), skeleton description resend on reconnect or change.
  - Logging categories and counters: frames sent, drops, bytes/sec, estimated latency; optional on-screen debug overlay.
- Acceptance
  - Simulated network issues recover without editor restart; logs/metrics enable targeted troubleshooting.

M8 — Tests, examples, and docs
- Tasks
  - Add automation smoke tests leveraging ProjectSandbox and Build/Scripts.
  - Example map with two skeletal meshes and representative curves.
  - Documentation: setup, pairing per transport, timing and units, troubleshooting. Extend Sphinx/Doxygen where appropriate.
- Acceptance
  - Clean machine walkthrough succeeds end-to-end on TCP, UDP, and NNG.

M9 — WebRTC and optional additions
- Tasks
  - Expose WebRTC as optional/beta in Broadcast UI; reuse libdatachannel integration (see WebRTC docs above).
  - Optional: mDNS/ZeroConf discovery if aligned with ecosystem.
  - Optional: Blueprint nodes for start/stop and runtime reconfiguration.
- Acceptance
  - WebRTC validated where enabled; parity maintained across transports; BP control available if requested.

Next-phase recommendations and considerations
- Implement delta frames in the serializer:
  - Maintain per-subject last-sent TRS/curve state and call `SubjectList::SetDeltaThreshold` and `Subject::SerializeUpdate`/`SubjectList::SerializeUpdate`.
  - Only send when `count > 0`; avoid emitting protocol overhead for below-threshold updates.
- Add timestamps/timecode:
  - Choose `FPlatformTime::Seconds()` (monotonic) or UE Timecode if available; populate the SubjectList timestamp consistently.
- Wire transports:
  - Add a broadcaster-side adapter that subscribes to `OnSerializedFrame` and pushes bytes over selected transport (TCP Server/Client, UDP sender, NNG Publisher/Client/Server, optional WebRTC).
  - Mirror LiveLink Source URLs and roles.
- Multi-subject support:
  - A lightweight subsystem to manage multiple components/subjects and multiplex into one stream (one `SubjectList` per tick or per send).
- Performance and threading:
  - Move serialization and I/O to a worker thread; keep capture on the game thread; add a lock-free queue or ring buffer.
  - Optional quantization for scales/rotations to reduce bandwidth; batching frames at low FPS.
- Curve-set stability:
  - If include/exclude patterns change at runtime, rebuild curve cache and emit a new descriptor so receivers can realign indices.
- Skeleton/LOD differences:
  - Continue clamping against `CompSpace.Num()`; keep descriptor based on `FReferenceSkeleton` to remain stable across LOD changes.
- Validation:
  - Compare receiver pose vs. UE component pose (coordinate system parity); document any axis flips needed by consumers.

Issue generation guide (for maintainers and coding agents)
- General structure for issues
  - Title: “\[Milestone] Short, action-oriented description”
  - Description:
    - Context and purpose (1–2 sentences)
    - Tasks (bullet list)
    - Acceptance criteria (bullet list; concrete, testable)
    - References (links to files/docs above)
    - Dependencies (list issues by number or title)
    - Out-of-scope notes and risks
- Labels
  - “milestone:M#”, “area:unreal”, “area:transport”, “protocol”, “perf”, “docs”, “tests”, “good-first-task” (as applicable)
- Dependencies
  - Use cross-links and convert tasks to sub-issues when granular work benefits parallelization.
- Suggested seed issues
  - **M0 (✅ CREATED):**
    - [ISSUE_M0_1_PROTOCOL_ALIGNMENT.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_1_PROTOCOL_ALIGNMENT.md) - Protocol & Message Types
    - [ISSUE_M0_2_CURVE_SEMANTICS.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_2_CURVE_SEMANTICS.md) - Curve Semantics & Filtering
    - [ISSUE_M0_3_TRANSFORM_SPACE.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_3_TRANSFORM_SPACE.md) - Transform Space & Timing
    - [ISSUE_M0_4_TRANSPORT_ROLES.md](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_M0_4_TRANSPORT_ROLES.md) - Transport Role Mapping
  - **M1 (TODO):**
    - Single-Mesh Pose Capture Module
    - Single-Mesh Curve Capture (Morph + Named Curves)
  - **M2 (TODO):** Serializer Integration + Round-trip Tests
  - **M3 (TODO):**
    - Transport Interface + TCP Server
    - UDP Client Sender + Fragmentation Handling
    - NNG Publisher + Client/Server Modes
  - **M4 (TODO):** Multi-Subject Multiplex + Naming Scheme
  - **M5 (TODO):**
    - Timecode + Frame Number Integration
    - Rate Control + Quantization + Threading
  - **M6 (TODO):** Editor UI + Config Assets/Settings
  - **M7 (TODO):**
    - Reconnection + Skeleton Resend
    - Logging + Metrics + On-screen Debug
  - **M8 (TODO):**
    - Example Map + Automation Tests
    - Docs Refresh + Troubleshooting
  - **M9 (TODO - Optional):**
    - WebRTC Option in Broadcast UI (Optional/Beta)
    - Blueprint API (Optional)

Acceptance evidence patterns
- Log parity: Printed transforms/curves match Anim Previewer/AnimBP at capture time.
- Receiver parity: LiveLink client shows subjects with expected transforms/curves and stable FPS.
- Round-trip tests: Serialization/deserialization equality for skeleton and frame payloads.
- Profiling snapshots: CPU time, memory, and GC pressure before/after; FPS stability evidence.
- Resilience: Reconnect scenarios with clear logs; skeleton resends observed.

Guidance for coding agents
- Branching and PRs
  - One issue → one focused branch → one PR.
  - Use descriptive branch names: feature/o3dsbcast-M3-tcp-server.
  - Keep changes scoped; prefer follow-up issues for tangential improvements.
- C++/UE conventions
  - Follow existing plugin code style and module layout under plugins/unreal/Open3DStream.
  - Avoid changing src/o3ds.fbs unless explicitly tasked; if changed, regenerate src/o3ds_generated.h and update impacted code/tests.
  - Use UE 5.4 APIs consistent with current plugin configuration and CI.
- Transports
  - Reuse src/ connectors; do not re-implement TCP/UDP/NNG from scratch in the plugin. Add a plugin-side adapter/facade if needed.
  - Map Broadcast roles to LiveLink source options according to the pairing table.
- Performance and threading
  - Separate capture, encode, and send to avoid game thread stalls.
  - Add configuration hooks for rate control and quantization; tune defaults based on profiling in ProjectSandbox.
- Testing and CI
  - Use ProjectSandbox and Build/Scripts to validate builds and automation tests.
  - Add unit tests for serialization; add minimal automation tests for end-to-end smoke.
- Documentation
  - Update this plan and link new docs from README sections where applicable.
  - Keep Sphinx/Doxygen references synchronized when new public headers/classes are introduced.

Risks and decisions to track in issues
- Transform space and coordinate system agreement with LiveLink receiver.
- Curve naming and filtering; morph target normalization conventions.
- Bandwidth and latency vs. reliability choices per transport.
- Skeleton stability across retargeting/LOD/mesh swaps; when to resend descriptions.
- WebRTC support maturity in Broadcast path (optional until wider validation).

Appendix: Quick transport role recipes
- TCP
  - Broadcast runs TCP Server; LiveLink Source uses TCP Client.
- UDP
  - Broadcast runs UDP Client; configure destination IP/port for the LiveLink UDP Server.
- NNG
  - Publish/Subscribe: Broadcast = Publisher; LiveLink = Subscriber.
  - Client/Server: Choose complementary roles; Broadcast can be either Server (LiveLink Client) or Client (LiveLink Server).
- WebRTC (optional/beta)
  - Use repo’s libdatachannel setup; provide signaling endpoint config in Broadcast UI; pair complementary roles with LiveLink.

Change log for this plan
- v0.1: Initial plan aligned to repository transports (TCP/UDP/NNG), schema reuse, and Unreal plugin integration; WebRTC marked optional/beta.
- v0.2: Added M0 issue documentation - created four detailed issue files (ISSUE_M0_1 through ISSUE_M0_4) covering protocol alignment, curve semantics, transform space, and transport role pairing.
- v0.3: Documented current component/serializer implementation (UE 5.6), identified gaps (delta frames, timestamping, transport wiring, multi-subject, UI, resilience, threading), and added next-phase recommendations.
