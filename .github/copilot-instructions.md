
# Copilot Coding Agent — Operating Instructions (Open3DStream / Unreal UE 5.6)

This file defines strict, testable rules so coding agents deliver high‑quality, deterministic work across the Open3DStream ecosystem (Unreal sender/receiver, DCC bridges, and utilities). Treat this as the single source of truth when planning work, writing code, and composing PRs.

---

## 0) Ground Rules (Read Me First)

- **Source of truth:** This document. Any ambiguity must be resolved by updating this doc first, then implementing.
- **UE API accuracy:** Verify all Unreal API signatures against **UE 5.6** documentation before use. Do not “best guess.” 
  - Reference Sources in order of preference:
    - Unreal Engine 5.6 source code on GitHub: @lifelike-and-believable/UnrealEngine - Always use Github MCP server to access
    - "Unreal Engine C++ API Reference" + class name(s) (web search). 
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API (direct URL)
- **Never Assume:** Always base decisions on actual APIs, build logs, etc. Always use Github MCP Server to interact with Github resources (logs, issues, PRs, repository access, etc)
- **Determinism first:** Prefer predictable behavior over opportunistic performance wins unless performance is a stated acceptance criterion.
- **Small steps:** Break work into small, testable tasks. Each PR should do one thing well, with passing checks and updated docs.
- **Protocol schema:** `src/o3ds.fbs` is the authoritative source for data structures. After changes, regenerate `src/o3ds_generated.h` with `flatc --cpp src/o3ds.fbs`.

---

## 0.1) Architecture Overview (Cross-Cutting Concerns)

### Core Library (`src/o3ds/`)
- **Protocol:** FlatBuffers schema (`src/o3ds.fbs`) defines serialization format for skeletal animation + curves.
- **Data model:** `O3DS::Subject` (skeletal hierarchy + curves) ↔ `O3DS::SubjectList` (multiple subjects + timestamp).
- **Serialization:** `Subject::Serialize()` creates full descriptors; `Subject::SerializeUpdate()` generates delta updates with threshold-based compression.
- **Connectors:** Inherit from `Connector` (blocking) or `AsyncConnector` (non-blocking). Implementations: TCP (`tcp.h`), UDP, NNG (pub/sub/pair), WebRTC (`webrtc_connector.h`).
- **Namespace:** All core types live in `namespace O3DS`.

### Unreal Plugin (`plugins/unreal/Open3DStream/`)
- **Two modules:**
  - `Open3DStream` (receiver): LiveLink source consuming animation streams. Mature, production-ready.
  - `Open3DBroadcast` (sender): Streams UE animation to external clients. Framework implemented, core functionality in progress. Conditionally compiled via `O3DS_WITH_BROADCAST` flag.
- **Receiver pattern:** `FOpen3DStreamSource` polls connectors, parses FlatBuffers, populates LiveLink subjects with skeletal pose + curves.
- **Sender pattern (broadcast):** `UO3DBroadcastComponent` captures post-anim-eval skeletal data via `UAnimInstance::GetCurveValue()`, encodes to FlatBuffers, sends via async connectors on worker threads.
- **Build system:** Uses PowerShell scripts in `Build/Scripts/` to build/test plugins with UAT (Unreal Automation Tool).

### DCC Plugins (`plugins/maya/`, `plugins/mobu/`)
- Maya and MotionBuilder plugins follow platform-specific APIs but share the `O3DS::Subject` data model for interop.
- Build with CMake; link against core library from `src/`.

### Development Workflows
- **Local dev:** `Build/Scripts/link_plugin_into_sandbox.sh` symlinks plugin → `ProjectSandbox/Plugins/Open3DStream` for rapid iteration.
- **Packaging:** `package.py` creates release ZIPs with `UE_X.X/Plugins/Open3DStream/` structure for easy project installation.
- **Testing:** `Build/Scripts/Run-AutomationTests.ps1` and `Run-Gauntlet.ps1` for UE automation tests.
- **C++ tests:** `test_curves.cpp` and `test_curve_comprehensive.cpp` validate FlatBuffers serialization round-trips.

---

## 1) Agent Playbook (How to Work)

1. **Plan**  
   - Read the related issue(s) and this document. Produce a short plan describing the change, risks, and tests.
2. **Implement**  
   - Keep changes minimal and well‑scoped. Respect prohibited items (see §14).
3. **Verify locally**  
   - Run all required unit/integration tests (see §10, §12), confirm the **Definition of Done** (see §13).
4. **Open PR**  
   - Use the PR template and checklists. Link the issue, include benchmarks/logs as required.
5. **Respond to review**  
   - Make focused changes; update docs/tests when requested.

### Agent Idioms (Shorthand)
- `pr-issue #123` → Read #123, summarize intent, and open PR titled `o3ds: <short imperative>` linking #123.
- `smoke-webrtc` → Run the two‑UE WebRTC smoke test; paste latency/jitter/drop metrics into PR.
- `bench-tcp` → Run TCP throughput/backpressure bench; summarize queue behavior and any frame drops.

---

## 2) General Programming Rules

- **Threading:** No blocking waits on the **game thread**. Network I/O and encoding run async.
- **Capture timing:** Capture occurs **after animation evaluation** on the game thread; no sampling from worker threads.
- **Error handling:** Prefer early returns and clear error types over silent fallthrough. Bubble actionable context.
- **Logging:** Quiet hot paths. Only state transitions and errors are logged by default (see §9).
- **Configs:** No hard‑coded credentials, ports, or absolute paths. Use project settings or env vars (see §8).
- **Docs & examples:** Update code comments and the README/CHANGELOG when behavior or schema changes.

**Prohibited**
- Calling Unreal APIs without verifying exact **UE 5.6** signatures in the official docs.
- Blocking on the game thread.
- Hard‑coding credentials/paths/ports in source control.
- Reordering or deleting existing fields in a serialized schema (FlatBuffers/JSON/etc.).

---

## 3) Versioning & Changelog

- Update `O3DS_VERSION_TAG` on any protocol or user‑visible behavior change.
- **CHANGELOG.md**: Add a “Schema/Protocol” subsection with: brief description, compatibility notes, and migration steps.
- Tag releases with `o3ds-vX.Y.Z` and attach a short release note referencing CHANGELOG sections touched in the PR.

---

## 4) Contributor Notes

- Prefer small, cohesive PRs over omnibus changes.
- Document tradeoffs explicitly in PR descriptions.
- Where choices exist (e.g., codec, transport), record rationale in comments to save future archeologists from guesswork.

---

*End of instructions. Keep this file authoritative; update it before you code.*
