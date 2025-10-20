
# Copilot Coding Agent — Operating Instructions (Open3DStream / Unreal UE 5.6)

This file defines strict, testable rules so coding agents deliver high‑quality, deterministic work across the Open3DStream ecosystem (Unreal sender/receiver, DCC bridges, and utilities). Treat this as the single source of truth when planning work, writing code, and composing PRs.

---

## 0) Ground Rules (Read Me First)

- **Source of truth:** This document. Any ambiguity must be resolved by updating this doc first, then implementing.
- **UE API accuracy:** Verify all Unreal API signatures against **UE 5.6** documentation before use. Do not “best guess.” 
  - Reference Sources in order of preference:
    - Unreal Engine 5.6 source code on GitHub: @lifelike-and-believable/UnrealEngine
    - "Unreal Engine C++ API Reference" + class name(s) (web search). 
    - https://dev.epicgames.com/documentation/en-us/unreal-engine/API (direct URL)
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

## 3) Transform Space & Timestamps (Non‑Negotiable)

- **Transform space:** **Parent‑relative (local)** only. Do **not** mix spaces in a single stream.
- **Timestamps (send both):**
  1) `FQualifiedFrameTime` when available (engine time/frame rate exactness).  
  2) A monotonic **double** seconds‑since‑start (high‑res clock) for cross‑process alignment.

Receivers must be able to select either source. Send both to maximize compatibility.

---

## 4) Subject Identity & Ordering

- **Subject ID format:** `{WorldName}/{ActorName}/{ComponentName}` by default.
- **Constraints:** ASCII only; `'/'` separated; no spaces. Replace whitespace with `_`. Strip any chars not in `[-._A-Za-z0-9/]`.
- **Deterministic ordering:** Before first descriptor send, **stable‑sort** bone and curve arrays **lexicographically (case‑sensitive)**. Indices must be stable across runs on identical assets.

---

## 5) Open3DBroadcast — Capture & Send

- **What to capture (minimum viable):**
  - Skeletal pose (local space), curve values (face/body), blendshapes if applicable.
  - Optional: camera transform/FOV and root motion when explicitly enabled.
- **What not to do in v1:**
  - No custom editor windows/toolbars. Keep UI minimal (see §11).
  - No per‑frame logging unless compiled with a Verbose flag.

---

## 6) Backpressure & Queuing Policy

- **Queue capacity:** `2` frames. Overridable via env `O3DS_BROADCAST_MAX_QUEUE` (integer ≥ 1).
- **Policy:** **Drop oldest** when the queue is full (latest‑wins).
- **Test hooks:** Integration tests must simulate 2× load and confirm: queue never > capacity; oldest drop policy observed.

---

## 7) Schema & Compatibility (FlatBuffers / JSON)

When modifying `.fbs` or any on‑wire schema:

- Only **add** optional fields with defaults; **never** reorder existing fields or remove them.
- Regenerate code and update **all** consumers in the repo.
- **Versioning:** Increment `O3DS_VERSION_TAG`.
- **CHANGELOG:** Add a “Schema/Protocol” section describing changes and compatibility.
- **CI impact:** Receivers (UE plugin, Maya, MotionBuilder, etc.) must build and pass smoke tests against the new schema.

---

## 8) Configuration, Secrets, and WebRTC

- **TURN/STUN configuration sources (exact order):**
  1) Project Settings → Open3DBroadcast (stored in `DefaultEngine.ini`)
  2) Environment variables: `O3DS_TURN_URL`, `O3DS_TURN_USER`, `O3DS_TURN_PASS`
- **Security:** No plaintext secrets in source or example code. Use env or secured config files.
- **Defaults:** Provide safe, non‑public defaults; do not auto‑enable TURN without credentials.

---

## 9) Logging Contract

- **Categories:**
  - `Open3DStream.Broadcast.State`  → `Info` for state transitions (Disconnected/Connecting/Connected/Error)
  - `Open3DStream.Broadcast.Error`  → `Warning`/`Error` for failures
- **Hot path:** No logs in the per‑frame send loop unless a build‑time Verbose flag is enabled.
- **Crash triage:** Include minimally sufficient context (subject id, transport kind, last state).

---

## 10) Build & CI Gates (Must Pass)

- **Unit tests:** Schema/packing/unpacking/ordering tests.
- **Integration tests:**
  - **TCP loopback** (sender→receiver on localhost) verifying backpressure and zero schema/subject mismatches across ≥ 300 frames.
  - **Two‑UE WebRTC smoke test** reporting latency/jitter/drops.
- **Lint/format:** `clang-format` and `.editorconfig` enforced.
- **Size budget:** UE plugin binary delta ≤ **+1.0 MB** per PR unless justified. Explain exceptions in PR body.

---

## 11) Editor UI Minimum Spec

- **Project Settings** page: remote URL(s), FPS cap, naming policy, transport enable flags.
- **Actor Component**: Start/Stop buttons; per‑mesh include checkbox.
- **Status Indicator**: `Disconnected / Connecting / Connected / Error` visible on the component.
- **No additional** custom windows/toolbars in v1.

---

## 12) Local Verification (Smoke Tests)

When local verification is possible, do the following before opening a PR:

- **Two‑UE WebRTC smoke test** shows motion parity within **±1 frame** (±16.6 ms @ 60 fps).
- **TCP fallback test** transmits **≥ 300** consecutive frames with **zero** schema/subject mismatches.
- **Backpressure stress** at **2× load** exhibits **oldest‑drop** policy; queue ≤ capacity (see §6).
- **Logs**: No hot‑path logs present in release builds; only state transitions + errors.

---

## 13) Definition of Done (PR Checklist)

Include the following in the PR description (check each item):

- [ ] Local tests passed (unit + integration), with pasted metrics from `smoke-webrtc` and `bench-tcp`.
- [ ] Deterministic ordering verified: bone/curve arrays stable‑sorted before descriptor send.
- [ ] Transform space is **local** only; both timestamp sources included.
- [ ] Subject IDs conform to naming constraints; no spaces; path‑safe.
- [ ] Backpressure queue set to capacity `2` (or env override stated) and oldest‑drop policy verified.
- [ ] No hot‑path logging; logging categories limited to State/Error.
- [ ] If schema changed: fields only added as optional; `O3DS_VERSION_TAG` bumped; CHANGELOG updated; receivers built.
- [ ] Size budget respected or exception justified.
- [ ] Docs updated where applicable (README/module docs).

---

## 14) Module Structure (Reference)

- **Broadcast Module (UE sender)**
  - Capture after anim evaluation (game thread).
  - Async encode and transport (TCP/WebRTC) on worker threads.
- **Receiver Modules**
  - Map indexed bones/curves by the deterministically sorted descriptors.
  - Expose selection of timestamp source (engine frame vs. monotonic).

(Keep this section synchronized with the code tree and update paths as modules evolve.)

---

## 15) Versioning & Changelog

- Update `O3DS_VERSION_TAG` on any protocol or user‑visible behavior change.
- **CHANGELOG.md**: Add a “Schema/Protocol” subsection with: brief description, compatibility notes, and migration steps.
- Tag releases with `o3ds-vX.Y.Z` and attach a short release note referencing CHANGELOG sections touched in the PR.

---

## 16) Contributor Notes

- Prefer small, cohesive PRs over omnibus changes.
- Document tradeoffs explicitly in PR descriptions.
- Where choices exist (e.g., codec, transport), record rationale in comments to save future archeologists from guesswork.

---

*End of instructions. Keep this file authoritative; update it before you code.*
