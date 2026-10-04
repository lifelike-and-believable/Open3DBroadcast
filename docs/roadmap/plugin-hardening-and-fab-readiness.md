# Plan: Plugin Hardening, Refactoring and Fab Readiness

**Status:** Ready for delegation · **Date:** 2026-09-29 · **Baseline:** `develop` @ `7aea235`
**Audience:** design agents, coding agents, code review agents, maintainers

This plan comes from an in-depth review of the Open3DBroadcast Unreal plugin (all eight modules), the `src/o3ds` core library it depends on, and the build, CI, documentation and packaging around them. It turns 274 findings into work packages that an agent can pick up, implement and review on their own.

The findings themselves, each with file:line evidence and a recommendation, are in [`docs/review/2026-09-plugin-review/`](../review/2026-09-plugin-review/README.md). **This plan cites findings by ID, for example `SND-1` or `CORE-2`. Read the finding before working on it.** Findings marked *needs-UE-verification* or *needs-FFI-verification* depend on behaviour that could not be confirmed from this repository. Confirm those against UE 5.7 source or the FFI source before acting on them.

---

## 0. How to use this document

### 0.1 Roles

| Role | What it does with a work package (WP) |
|---|---|
| **Design agent** | Owns every WP marked **[design]** and every decision in §3. It produces a short ADR in `docs/adr/NNNN-<slug>.md` covering the problem, the options, the decision, the consequences and a migration plan. It updates this plan's "Decision" line and gets maintainer sign-off before coding starts. It does not write production code. |
| **Coding agent** | Implements one WP (or one numbered sub-item of a large WP) per PR. It reads the cited findings first and follows `.github/copilot-instructions.md`. It adds the tests listed under **Acceptance**. In the PR description it says what was built, what was tested, and what was only compiled. It never claims a fix is verified if it only compiled. |
| **Code review agent** | Reviews each PR against the WP's **Acceptance** list and the checklist in §7. It checks the diff against the original findings and confirms every cited finding is actually closed. It rejects PRs that widen scope without saying so. |

### 0.2 Ground rules (carried over from the repo rules)

- Verify every Unreal API against **UE 5.7** (see `.github/copilot-instructions.md` §0). The review agents flagged where they could not.
- Never block the game thread. Networking, encoding and FFI connect/disconnect run off the game thread.
- `src/o3ds.fbs` is append-only. Regenerate `src/o3ds_generated.h` with `flatc --cpp` and never hand-edit it.
- No credentials, ports or absolute paths in source control. That includes docs.
- Follow the **core-first principle** from `docs/roadmap/resilient-streaming-and-motion-prediction.md` §0.1. Anything that can be tested with plain CMake on Linux goes in `src/o3ds`, where CI can actually run it.
- Keep PRs small. One WP, or one numbered sub-item, per PR. Never mix a refactor with a behaviour change in one PR.

### 0.3 Priority and size key

- **P0**: memory safety, remote crash, data corruption, or a hard Fab blocker. Do these first.
- **P1**: functional bug, thread-safety hazard, or significant performance or usability problem.
- **P2**: architecture and maintainability.
- **P3**: polish.
- **Size**: S is under a day, M is 1 to 3 days, L is more than 3 days (split it).

---

## 1. Executive summary

**Overall health.** The plugin is well structured at the module level and the newer core modules (reorder gate, clock offset, predictors, quantization) are careful and documented. The licensing inventory is thorough and honest. But the plugin is **not production-safe and not Fab-submittable today**:

1. **Remote memory-safety bugs in the core parser and UDP reassembly.** Two were reproduced under AddressSanitizer: a SEGV in `CalcMatrices` from a crafted packet (CORE-1) and a heap overflow in `UdpCombiner::addFragment` (CORE-2). There is also unbounded reassembly memory (CORE-3, TRB-15) and O(N²) hierarchy solving (CORE-8). Every UDP receiver is exposed, and it binds 0.0.0.0 by default.
2. **Wire-level data corruption on the sender.** The skeleton descriptor is never re-sent after Stop/Start (SND-1). Quantized resyncs desynchronise translation anchors (SND-2). Curve filtering misaligns curves (SND-3, SND-4). The TCP receiver discards every coalesced frame (TRB-1). Residual coding is not loss-tolerant (CORE-5, CORE-6).
3. **Lifetime races on the audio and FFI thread boundaries** in all five transports (TRF-1, TRB-30, TRB-35, RCV-1, SND-6). The Sockets transport already has a correct `OwnerGuard` pattern that the others should reuse.
4. **Broken features:** WebRTC token auto-fetch never connects (TRF-3). Opus is effectively never used, and frames are mislabelled when it falls back (SHR-1, SHR-2). NNG Pair defaults never connect (TRB-40).
5. **Credentials persisted in plain text** into level assets, `GameUserSettings.ini` and LiveLink connection strings (SND-10, RCV-3, TRF-21).
6. **Fab blockers:**
   - ffmpeg and OpenH264 inside `livekit_ffi.dll` (FAB-1).
   - No `FilterPlugin.ini`, and a core library that is a gitignored build output, so the packaged plugin cannot compile (FAB-2, FAB-6).
   - No `PlatformAllowList`, and the Build.cs files throw on non-Win64 (FAB-3).
   - `.pdb` files are shipped (FAB-4).
   - Copyright headers are missing (FAB-5).
   - CI hides BuildPlugin failures (CI-1) and never runs tests on PRs (CI-2).
7. **Architecture debt:**
   - Two copy-pasted transport registries (SHR-12).
   - Receive, demux, audio and queue plumbing copy-pasted across five transports (TRB-38, TRF-32).
   - God classes (SND-22, RCV-29).
   - All encoding runs synchronously on the game thread (SND-8).
   - Editor code lives in Runtime modules (FAB-7).
8. **Tests are thin:**
   - The core has 174 passing happy-path tests and no adversarial, loss or MSVC coverage.
   - The UE side has a handful of tests. Several are placeholders that always pass (SHR-5), one fails against current code (RCV-2), and one needs the public internet (TRF-34).
9. **Docs are stale or wrong in user-facing places:**
   - The root README describes removed code (DOC-3).
   - The plugin README claims "Marketplace Ready" (DOC-1).
   - The USER_GUIDE documents Blueprint events that don't exist (DOC-4).

### 1.1 Findings by area and severity

| Area | File | Critical | High | Medium | Low |
|---|---|---:|---:|---:|---:|
| Open3DSender | `sender.md` (SND) | 1 | 10 | 18 | 8 |
| Open3DReceiver | `receiver.md` (RCV) | 0 | 4 | 20 | 10 |
| Open3DShared | `shared.md` (SHR) | 0 | 5 | 19 | 14 |
| Sockets / Loopback / NNG | `transports-sockets-loopback-nng.md` (TRB) | 2 | 12 | 23 | 10 |
| WebRTC / MoQ | `transports-webrtc-moq.md` (TRF) | 1 | 10 | 19 | 10 |
| Core `src/o3ds` | `core-library.md` (CORE) | 2 | 5 | 14 | 9 |
| Fab / CI / docs / UX | `fab-ci-docs.md` (FAB, CI, BUILD, HYG, DOC, UX, LIC) | 6 | 9 | 15 | 18 |

### 1.2 What was verified, and how

- **Core:** built and tested with GCC 13, Debug, ASan and UBSan against the pinned submodules. 174 of 174 tests pass. CORE-1 and CORE-2 were **reproduced under ASan** with the harnesses in `docs/review/2026-09-plugin-review/poc/`: `./poc calc` gives a SEGV at `math.h:389`, and `./poc frag` gives a heap-buffer-overflow. The performance numbers in CORE-7 and CORE-8 come from `perf.cpp` and `chain.cpp`.
- **UE plugin:** read-only review. **Nothing was compiled against UE 5.7** because no engine is available in the review environment. The following were re-checked by hand against the source: SND-1, SND-4, SND-5, SND-11, RCV-2, RCV-4, TRB-1, SHR-1, SHR-5, TRF-2, TRF-3, TRF-23, FAB-2, FAB-4, FAB-6, CI-1 and CI-2.
- **Fab requirements:** the official Fab pages were blocked by the review sandbox's network proxy. The requirements come from search snippets of the official "Fab Technical Requirements" page, two community Fab CI tool repos, and the legacy Marketplace guidelines (sources in `fab-ci-docs.md`). **WP-F0 re-verifies them against the live page before any Fab work is signed off.**

---

## 2. Cross-cutting themes

These themes explain why many findings exist. Fixing the theme is cheaper than fixing each symptom, so the architecture WPs in §5 are built around them.

| # | Theme | Representative findings | Addressed by |
|---|---|---|---|
| T1 | Untrusted bytes reach parsers without complete validation. Verification is done, but semantic checks (counts, cycles, sizes, versions, ranges) are missing. | CORE-1/2/3/8/9/15/26, TRB-8/9/15/17/18, SHR-7/8 | WP-S1, WP-S2, WP-A4 |
| T2 | Objects captured by audio or FFI threads outlive their owners. Each transport reinvented this and most got it wrong; Sockets' `OwnerGuard` is the exception. | TRF-1/9/10/12/15, TRB-10/11/12/30/33/35, RCV-1, SND-6/7, SHR-3/10 | WP-S5, WP-A1 |
| T3 | Stateful wire coding (descriptor cache, delta, residual, quantization) has no resync or loss contract. Sender and receiver state can diverge silently. | SND-1/2/3/4/13/15, RCV-4/5, CORE-5/6/10/11/12/29 | WP-S3, WP-S4, WP-A4 |
| T4 | Work on the game thread: serialization, transport sends, FFI connect and disconnect, audio device enumeration, and unbounded receive loops. | SND-8/18, TRF-6/7, TRB-18/20, RCV-19 | WP-A2, WP-A5 |
| T5 | The transport abstraction is split and duplicated. There are two registries, five copies of demux, audio-encode and queue code, and a stringly-typed config. | SHR-12/13/14/16/24/36, SND-23, RCV-27/28, TRB-38, TRF-32/38 | WP-A1 |
| T6 | Secrets are ordinary strings in serialized option maps. | SND-10, RCV-3, TRF-21/22, SHR-11, LIC-1 | WP-S9 |
| T7 | The build and packaging model assumes a developer machine: Win64-only binaries, gitignored core outputs, env-var feature flags, no FilterPlugin, and failures hidden by a fallback. | FAB-2/3/4/6, CI-1, TRB-24/41, TRF-27/28, SHR-4/20/21/22, BUILD-1..5 | WP-F1..F4, WP-F8 |
| T8 | Tests assert structure, not behaviour. There are no fakes for FFI or sockets and no adversarial inputs. | SHR-5/6, SND-36, RCV-31, TRB-47, TRF-34, CORE-19 | WP-T1, WP-T2 (and every WP's Acceptance) |
| T9 | Docs describe intent or history rather than the code, and dev notes ship inside the plugin. | DOC-1..10, HYG-1, TRB-44, TRF-36, SND-37, RCV-32, SHR-37 | WP-D1..D4 |
| T10 | End-user onboarding: no sample content, no project settings, a thin Blueprint API, and silent failure modes. | UX-1/2/3, SND-24..27, RCV-15..18 | WP-U1..U5 |

---

## 3. Decisions to make first ([design], milestone M0)

Several WPs depend on these decisions. Each gets an ADR. The design agent should start with **D1, D2 and D3**, because they gate Fab work.

**D1: Platform scope for the first Fab release.**
- **Options:**
  - (a) Win64 only. Add `PlatformAllowList: ["Win64"]` to every module and `SupportedTargetPlatforms` to the plugin, turn Build.cs `throw`s into clean exclusions, and list only Win64 on Fab.
  - (b) Add Mac and Linux by building the core, NNG and Opus from source per platform.
- **Recommendation:** (a) for v1, while the core stays portable (it already builds on Linux) so (b) remains a follow-up.
- **Findings:** FAB-3, SND-11, RCV-30, TRB-41, TRF-27, SHR-21, FAB-8.
- **ADR:** [docs/adr/0001-platform-scope-first-fab-release.md](../adr/0001-platform-scope-first-fab-release.md) (Accepted)
- **Decision:** Win64 only for v1 and v1.1; every module gets `PlatformAllowList: ["Win64"]` and `TargetDenyList: ["Server","Program"]`; Build.cs throws become stub builds. Mac/Linux (option b, tiered) is unscheduled. Receiver-on-Server is not planned but must stay possible.

**D2: WebRTC in the Fab SKU.**
- **Options:**
  - (a) Rebuild `livekit_ffi` without ffmpeg and OpenH264, following `docs/webrtc-codec-removal-plan.md`, and make `Build/Scripts/check-no-video-codecs.sh` a required CI gate.
  - (b) Ship the first Fab release without `Open3DTransportWebRTC` and add it back later.
- Either way, counsel signs off on the livekit and moq notices (FAB-11).
- **Recommendation:** (b) unblocks submission now; (a) proceeds in parallel.
- **Also decide:** whether MoQ ships in v1. It is built on draft IETF protocols (FAB-12), so it should at least be marked Experimental.
- **Findings:** FAB-1, FAB-11, FAB-12.
- **ADR:** [docs/adr/0002-webrtc-and-moq-in-first-fab-release.md](../adr/0002-webrtc-and-moq-in-first-fab-release.md) (Accepted)
- **Decision:** WebRTC is excluded from the Fab package and distributed as the free `Open3DBroadcastWebRTC` add-on plugin from the support site, linked from the Fab listing (WP-F11). The codec-free rebuild continues in parallel. MoQ ships in v1 as Experimental with Fab-ready licences. The whole plugin is marked Beta for v1. Counsel questions L1 to L5 are open.

**D3: How the core library reaches the plugin.** Today `Sync-O3DSCore.ps1` builds a `.lib` that is gitignored, and the headers only install for one configuration (FAB-6, CORE-20, CORE-21).
- **Options:**
  - (a) Compile `src/o3ds` sources directly as a UE module, for example `Open3DStreamCore`. The sources would be copied or synced into `Source/Open3DStreamCore/`, with flatbuffers header-only.
  - (b) Commit prebuilt libs and headers into the Fab zip.
- **Recommendation:** (a). It removes the prebuilt-lib provenance question and builds for every platform, and it makes D1(b) possible later.
- This decision also resolves roadmap §0.2 (core duplication, issue #203).
- **Consequence to plan for:** core code compiled under UE's warning levels and MSVC settings. See CORE-20 (68 `-Wall -Wextra` warnings today).
- **ADR:** [docs/adr/0003-core-library-delivery-to-plugin.md](../adr/0003-core-library-delivery-to-plugin.md) (Accepted)
- **Decision:** compile the used `src/o3ds` subset as an `Open3DStreamCore` UE module, mirrored from `src/o3ds` by a Python 3 sync script with a CI drift check; add an `O3DS_API` export macro to the core. NNG and Opus stay prebuilt for v1.

**D4: Transport abstraction home and shape** (feeds WP-A1).
- Move `IOpen3DSender`, `IOpen3DReceiver` and a **single** registry into `Open3DShared`, or into a new `Open3DTransportCore` module.
- Define the following:
  - **Threading contract:** which thread calls which method, and which callbacks may arrive on which thread.
  - **Lifetime contract:** factories registered per module, live instances tracked, and unload drained before `FreeDllHandle` (TRF-14).
  - **Error model:** a result struct with a code and message instead of `bool`.
  - **Interface version:** one integer.
  - **Typed secret and option handling:** see D6.
- **Findings:** SHR-12/13/14/16/24/36/38, SND-23, RCV-28, TRB-38, TRF-32.
- **ADR:** [docs/adr/0007-transport-abstraction-and-registry.md](../adr/0007-transport-abstraction-and-registry.md) (Accepted)
- **Decision:** interfaces and one thread-safe registry move into `Open3DShared`; each transport registers one immutable descriptor (factories, capabilities including delivery guarantee and new-peer signal, typed option schema with secret keys); lookups return shared pointers; live instances are tracked and drained before unload; result type plus per-frame enum; exported `GetHostApiVersion()` for the WebRTC add-on; queue, worker, demux, audio-sink guard (redesigned so no lock is held across encode or send), host:port parsing, backoff and DLL loader written once; forwarding shims for one release; one transport per PR.

**D5: Sender pipeline threading** (feeds WP-A2). The game thread samples into a pooled immutable frame. A per-sender worker (a `UE::Tasks` pipe or an `FRunnable` with an SPSC queue) owns the serializer and calls `SendSerialized`. The queue is bounded and drops the oldest frame when full. Decide the ownership of serializer state and how descriptor and keyframe requests reach the worker (see SND-1). **Findings:** SND-8, SND-9, SND-12, TRF-7.
- **ADR:** [docs/adr/0008-sender-pipeline-threading.md](../adr/0008-sender-pipeline-threading.md) (Accepted)
- **Decision:** the game thread samples into a pooled immutable frame (depth-2 queue, drop oldest); a per-sender `UE::Tasks` pipe owns curve filtering, serializer, `StreamWriter`, CRC and `SendSerialized` and never touches UObjects; capture ticks in `TG_PostUpdateWork` with the mesh as prerequisite; `FPlatformTime` is the shared clock; audio keeps its own path; StopCapture discards without waiting.

**D6: Credentials model** (feeds WP-S9).
- Secret option keys are declared by each transport customization.
- Secrets are stored transient, or resolved from env vars or a per-user store. They are never written to UPROPERTYs, `GameUserSettings.ini` or LiveLink connection strings.
- `bPersistToken` is honoured.
- Logs redact keys matching `*token*|*secret*|*key*`.
- The token server is documented as a reference only, with no default secret (LIC-1).
- **Findings:** SND-10, RCV-3, TRF-21, TRF-22, SHR-11.
- **ADR:** [docs/adr/0004-credentials-and-secret-transport-options.md](../adr/0004-credentials-and-secret-transport-options.md) (Accepted)
- **Decision:** transports declare secret option keys; secrets go to an exported `FO3DSecretStore` (session, then env var, then opt-in per-user editor settings, which is what `bPersistToken` means); assets and connection strings keep only a profile name; logs redact by key pattern; token endpoints need bearer auth and HTTPS except on localhost; no OS credential store in v1.x.

**D7: Wire-coding resync and loss contract** (feeds WP-S3, WP-A4).
- Decide the following:
  - (i) Whether descriptor delivery becomes pull-based: the serializer asks for the descriptor whenever its cache is empty or stale (SND-1).
  - (ii) A periodic full-descriptor keyframe for late joiners (SND-13, CORE-6).
  - (iii) Residual mode is restricted to reliable ordered transports until loss handling lands (CORE-5). The UI blocks it on UDP and unreliable WebRTC.
  - (iv) The UE sender stamps `tx_seq` and `frame_epoch` so the reorder gate engages (SND-15, CORE-29).
  - (v) Whether scale is sent in updates (CORE-11).
- Coordinate with the existing A, C and D workstreams in `resilient-streaming-and-motion-prediction.md`, which own the schema fields.
- **ADR:** [docs/adr/0005-wire-resync-and-loss-contract.md](../adr/0005-wire-resync-and-loss-contract.md) (Accepted)
- **Decision:** a core `StreamWriter` stamps `tx_seq`, `tx_wallclock_us` and `frame_epoch` on every frame; the descriptor travels with pose frames; full syncs on start, rename, descriptor change, new peer and every 1.0 s; both ends re-anchor quantization at each full sync; new append-only `SubjectUpdate.ref_seq`; residual only on transports reporting reliable ordered delivery; scale sent in updates; no receiver keyframe request in v1.

**D8: Protocol versioning.** Add a FlatBuffers `file_identifier` and a wire protocol version. Bump `O3DS_VERSION_TAG`. Old readers must reject residual and quantized payloads rather than misapply them. Fix the endianness of the unified and audio headers. Start using `CHANGELOG.md` (with the "Schema/Protocol" section required by the repo rules). **Findings:** CORE-16, SHR-7, SHR-30, DOC-8.
- **ADR:** [docs/adr/0009-protocol-versioning.md](../adr/0009-protocol-versioning.md) (Accepted)
- **Decision:** the frame header's first word becomes the minimum reader version (plain frames stay 1, residual and quantized frames use 2, so pre-D8 readers drop them); add `protocol_version` and `file_identifier "O3DS"` (checked on version-2 frames); all wire data little-endian including PCM; audio envelope v2 (`O3DU`, with sequence number); UDP fragment magic and version (`O3DF`); defined clock domains; length-prefixed name hash; protocol 2 and `O3DS_VERSION_TAG` 1.1.0; one root `CHANGELOG.md`; old/new compatibility test matrix.

**D9: Editor module split.** Create an `Open3DBroadcastEditor` module (Type `Editor`), or one Editor module per side. Move all `IDetailCustomization`, Slate panels and PropertyEditor dependencies there. Transport modules register their editor panels through a small registration interface in that module. **Findings:** FAB-7, SND-34, TRB-45, TRB-46.
- **ADR:** [docs/adr/0010-editor-module-split.md](../adr/0010-editor-module-split.md) (Accepted)
- **Decision:** one `Open3DBroadcastEditor` module (Type Editor, `PostEngineInit`, Win64) that ships on Fab; runtime transports register only data (the ADR 0007 option schema) and the editor module builds generic, undoable panels from it; per-transport editor modules only as an opt-in escape hatch; the LiveLink source factory stays in Receiver and asks the editor module for its panel; TRB-45/SND-35 fixes implemented once; `o3d.ProfileGuide` deleted.

**D10: Test module layout.** Create a dedicated test module (for example `Open3DBroadcastTests`, Type `DeveloperTool` or `UncookedOnly`) so tests don't ship in Runtime modules or invert layering. Also define a fake-FFI and fake-socket seam for the transports. **Findings:** SHR-4, UX-4, TRF-34, TRB-47.
- **ADR:** [docs/adr/0006-test-module-layout-and-fakes.md](../adr/0006-test-module-layout-and-fakes.md) (Accepted)
- **Decision:** an `Editor`-type `Open3DBroadcastTests` module (plus one for the WebRTC add-on), excluded from the Fab package; a transport conformance suite; fake transports, per-instance FFI function tables and socket-free parsers; `Open3DBroadcast.*` naming; network tests opt-in via `O3DB_NETWORK_TESTS=1`; Gauntlet retired; RCV-2 uses the stream-label fallback; PR CI runs the suite within 15 minutes.

**D11: Control channel for events and values** (feeds WP-CTL; added 2026-09-30, after the original plan). Let the sender trigger VFX, lighting and audio cues on remote clients and change environment and character parameters there. **Findings:** none (new feature).
- **ADR:** [docs/adr/0011-control-channel.md](../adr/0011-control-channel.md) (Accepted)
- **Decision:** a one-way sender-to-receivers control stream beside mocap and audio, as envelope kind `Control = 2` with its own FlatBuffers root (`src/o3ds_control.fbs`, `file_identifier "O3DC"`); keyed last-writer-wins **values** re-sent as periodic snapshots, and fire-once **events** with de-duplication, a TTL and redundant copies on unreliable transports; state machines in core with CTest and fuzzing; `FireControlEvent`/`SetControlValue` on the sender component, `FO3DControlBus` and `UO3DRemoteControlComponent` on receivers; cues held to align with mocap by default; receiving off by default, enabled by a project setting or a runtime call, allowlisted and rate-limited, never reflection or console commands; 1,100-byte envelope budget; `O3D_TRANSPORT_API_VERSION` 2.

---

## 4. Milestones and dependency graph

```
M0  Decisions D1–D11 (ADRs) ──┬──────────────────────────────────────────────┐
                              │                                              │
M1  Safety & correctness      │   M2  Fab-buildable package                  │
    WP-S1..S10  (P0/P1)       │       WP-F0..F9  (P0/P1)                      │
    WP-T1, WP-T2 (test infra, │       (needs D1, D2, D3, D9)                  │
    lands first)              │                                              │
            └─────────────┬───┴───────────────┬──────────────────────────────┘
                          ▼                   ▼
M3  Architecture: WP-A1 (transport core) → WP-A2 (async sender) → WP-A3 (god classes)
                  WP-A4 (protocol), WP-A5 (connection lifecycle), WP-A6 (globals), WP-A7 (core API/legacy)
                          ▼
    WP-CTL (control channel, D11): CTL-1..7 landed beside M3 (CTL-6 after WP-F11)
                          ▼
M4  Usability & docs: WP-U1..U6, WP-D1..D4, WP-Q1 (cleanup batch)
                          ▼
M5  Fab submission: WP-F10 (submission dry-run + listing); WP-F11 (WebRTC add-on) ships alongside
```

- M1 and M2 run **in parallel**. They touch mostly different files (source versus Build.cs, uplugin, CI and ThirdParty). Where they overlap (Build.cs guards), WP-F2 goes first.
- WP-T1 and WP-T2 (test infrastructure) should land **early in M1**, so every later WP can add real tests.
- WP-A1 should land **before** the per-transport fixes in WP-S5, S7 and S8 become large. For M1, make only the minimal local fix, then fold it into the shared base class in A1. Each M1 WP says which parts are "minimal fix now" and which are "fold into A1".
- **Definition of Fab-ready (exit of M2 plus the M1 P0 items):**
  - Every P0 WP is closed.
  - BuildPlugin passes from the source zip with warnings-as-errors.
  - Automation tests run in PR CI.
  - The package contains no pdb, py or dev docs.
  - WP-F0 has confirmed the live Fab requirements.

---

## 5. Work packages

Each WP lists: **Priority · Size · Owner**, **Findings**, **Goal**, **Approach**, **Acceptance**, and **Depends on**. The approach summarises the findings' recommendations. The finding file has the full detail and file:line locations.

### M1: Safety and correctness

#### WP-T1: Core adversarial and loss test harness  ·  P0 · M · coding
- **Findings:** CORE-19, CORE-20 (warnings), TRB-47 (parser parts)
- **Goal:** CI can prove the fixes in WP-S1 and WP-S2 and catch regressions.
- **Approach:**
  - Turn the harnesses in `docs/review/2026-09-plugin-review/poc/` into CTest regression tests.
  - Add a libFuzzer or deterministic random-bytes target for `SubjectList::Parse`, `ParseUpdate`, `UdpMapper`/`UdpCombiner`, `ReorderGate` and the residual decoder.
  - Add loss, reorder and mid-join tests for the residual and quantized paths, using `channel_model`.
  - Run ASan/UBSan in `core-tests.yml`, and add an MSVC Release job (the configuration the plugin ships).
  - Turn on `-Wall -Wextra` for `src/o3ds` (not the legacy connectors) and ratchet the warning count down.
- **Acceptance:**
  - The two ASan PoCs exist as tests and fail on today's `develop`.
  - Fuzz targets run for at least 60 s in CI without findings once WP-S1 has landed.
  - An MSVC job runs the core tests.
- **Depends on:** none.

#### WP-T2: UE test module and fakes  ·  P0 · M · design+coding
- **Findings:** SHR-4, SHR-5, SHR-6, SND-36, RCV-2, RCV-31, TRF-34, TRF-35, UX-4, TRB-47
- **Goal:** automation tests test behaviour, run in PR CI, and don't ship.
- **Approach:**
  - Create the test module per D10.
  - Move `GenericTransportTests.cpp` out of Shared (SHR-4).
  - Replace the `TestTrue(true)` placeholders with a parameterized transport conformance suite over the registered senders and receivers, covering lifecycle idempotency, the backpressure `DroppedFrames` contract, concurrent `SendSerialized`, and `GetStats` monotonicity (SHR-5).
  - Add a fake `IOpen3DSender`/`IOpen3DReceiver` and a fake-FFI seam for WebRTC and MoQ (TRF-34).
  - Fix or re-specify the FinalizeAudioMeta test (RCV-2). Decide the intended behaviour first.
  - Take `MoQCloudflareRelayTests` out of the default EngineFilter, or gate it behind an env var (UX-4).
  - Delete the MoQ test that cannot pass, or fix it (TRF-34).
- **Acceptance:**
  - Zero placeholder asserts remain.
  - The suite passes locally with the tests from WP-F8 enabled.
  - No test needs the internet by default.
- **Depends on:** D10.

#### WP-S1: Core parser hardening  ·  P0 · M · coding
- **Findings:** CORE-1, CORE-8, CORE-9, CORE-23, CORE-26, CORE-17 (the double-delete on assignment only)
- **Goal:** `SubjectList::Parse` and `ParseUpdate` are safe on any byte input.
- **Approach:**
  - Bounds-check matrix components in `ParseSubject` and `CalcMatrices` (CORE-1).
  - Solve the hierarchy in one pass, or enforce `parent < index` on the wire.
  - Detect cycles.
  - Enforce max nodes, curves and subjects per buffer.
  - Make world-matrix computation opt-in, since the UE receiver consumes local TRS (CORE-8).
  - Reject non-finite floats (CORE-9).
  - Fix the signed and unsigned comparisons in `ParseUpdate` (CORE-23).
  - Fix the overflow UB in `ClockOffsetEstimator` (CORE-26).
  - Delete or implement copy assignment for `SubjectList` (CORE-17).
- **Acceptance:**
  - The WP-T1 PoCs pass.
  - The fuzz target stays clean.
  - A 16k-node packet parses in under 10 ms, or is rejected by the size limit.
- **Depends on:** WP-T1 (tests can land in the same PR).

#### WP-S2: UDP fragmentation and reassembly hardening  ·  P0 · M · coding
- **Findings:** CORE-2, CORE-3, CORE-4, TRB-15, TRB-17, TRB-18, TRB-19, TRB-23, CORE-22
- **Goal:** a hostile or buggy peer cannot crash the receiver, exhaust its memory, or stall it.
- **Approach:**
  - Lock `fragSize`, `bufSz` and the frame count on the first fragment, and reject any later fragment that disagrees (CORE-2).
  - Bound in-flight messages (for example 8) and total bytes, and evict by age or LRU.
  - Key reassembly on (source address, message id), and use serial-number comparison for ids.
  - Add a configurable `udp.maxframe` (default about 1 to 4 MiB), and use rule-of-zero buffers (CORE-3, TRB-15).
  - Stop `UdpMapper` from emitting empty frames (CORE-4).
  - Add a magic and version to the fragment header (TRB-17). This is a wire change, so coordinate with D8.
  - Cap datagrams and bytes per `Poll()`, or move receive to a socket thread with a bounded SPSC queue (TRB-18).
  - Use a pooled receive buffer (TRB-19).
  - Size the receive buffer independently of the local `udp.maxdatagram` (TRB-23).
  - Use aligned, endian-explicit header reads (CORE-22).
- **Acceptance:**
  - CORE-2 PoC passes.
  - A test with 10k spoofed first fragments keeps memory under the configured cap.
  - Mixed fragment sizes are rejected.
  - `Poll()` returns in bounded time under a flood (unit test with a fake socket).
- **Depends on:** WP-T1. D8 for the header change. The header change can ship as a separate follow-up PR.

#### WP-S3: Sender wire correctness  ·  P0 · M · coding (after D7 i)
- **Findings:** SND-1, SND-2, SND-3, SND-4, SND-5, SND-13, SND-14, SND-19, SND-20
- **Goal:** what the sender puts on the wire is always decodable and correct.
- **Approach:**
  - **SND-1:** pull-based descriptor delivery. When the serializer's cache is empty or stale, it requests `DescriptorCache`. `StartCapture` and a rename always re-broadcast the descriptor. Reset `CachedSkeletalMesh` on stop. Drop frames with a rate-limited warning on a bone-count mismatch instead of padding with parent 0.
  - **SND-2:** preserve `Transform` objects, or carry quantization anchors across a resync when names and parents are unchanged.
  - **SND-3:** keep the per-subject curve list stable and leave change suppression to the core delta logic, or disable curve filtering in residual and quantized modes.
  - **SND-4:** send a curve's return to zero once.
  - **SND-5:** use an accumulator-based rate limiter with a tolerance.
  - **SND-13:** send a periodic full descriptor (per D7 ii).
  - **SND-14:** apply encoding setting changes atomically.
  - **SND-19:** roll back on a partial `StartCapture` failure.
  - **SND-20:** honour the filtering-disabled state, and pick up pattern edits.
- **Acceptance:** automation or core round-trip tests for each of these:
  - Stop, Start, then correct names and parents.
  - Rename.
  - Quantized full, delta, resync, delta, with translations within tolerance.
  - A curve going 0.8, 0, 0.
  - Curve add and remove with residual and quantized on.
  - A jittered 60 Hz tick sequence accepting about 60 frames per second.
- **Depends on:** D7(i, ii), WP-T2.

#### WP-S4: Receiver correctness  ·  P1 · M · coding
- **Findings:** RCV-4, RCV-5, RCV-7, RCV-10, RCV-14, RCV-34, TRB-37
- **Goal:** the receiver publishes exactly what was sent, with the right bone names, parents and subjects.
- **Approach:**
  - Include bone names in the skeleton fingerprint, or invalidate the cache on a full descriptor (RCV-4).
  - Use a per-packet scratch list, and push only the subjects present in the packet (RCV-5).
  - Keep the subject settings object instead of recreating it each time (RCV-7, needs-UE-verification).
  - Don't push duplicate render-ahead frames (RCV-10).
  - Keep parent indices aligned when skipping null transforms (RCV-14).
  - Don't wipe gate and concealment state on a legacy-path silence reset (RCV-34).
  - Strip the unified header before handing mocap to the consumer in the NNG receiver (TRB-37).
- **Acceptance:**
  - Tests with a fake transport feeding recorded frames (use `src/o3ds/capture` and `replay`): renamed bones republished, two senders on one channel don't interfere, and a null transform in the middle keeps parents correct.
- **Depends on:** WP-T2.

#### WP-S5: Audio and FFI lifetime safety  ·  P0 · M · design+coding
- **Findings:** TRF-1, TRB-30, TRB-35, RCV-1, SND-6, SND-7, TRB-10, TRB-11, TRB-12, TRF-10, TRF-12, TRF-40, SHR-10, SHR-15
- **Goal:** no audio, capture or FFI thread can touch a destroyed sender, receiver, sink, UObject or FFI handle.
- **Approach:**
  - **Minimal fix now:** extract the Sockets `OwnerGuard` into a reusable `TO3DSinkOwnerGuard<T>`, or a `TWeakPtr` to a ref-counted "publish state", in Open3DShared. Adopt it in Loopback, NNG, WebRTC and MoQ.
  - `Stop()` marks the sink invalid **before** destroying handles, and drains in-flight publishes with an `FRWLock` or a closing flag.
  - Snapshot immutable audio metadata into the receiver-side `FAudioSink` so it holds no owner back-reference. Never let an FFI thread hold the last strong reference (RCV-1).
  - Sender audio capture: use an immutable thread-safe parameter snapshot swapped on the game thread, and a per-producer scratch buffer (SND-6).
  - Unregister the listener from the submix it was registered on (SND-7).
  - Don't take the socket lock on the audio thread. Encode into a local scratch buffer and hand off through an MPSC queue (TRB-10).
  - Keep encoder reconfiguration off the audio thread's live object (TRB-11, TRF-10).
  - Use one audio decoder per stream (SHR-15).
  - Make the audio bus thread-safe, or document it as game-thread-only and enforce that (SHR-10).
  - **Fold into WP-A1:** the guard, sink base class and decoder-per-stream map become part of the shared transport core.
- **Acceptance:**
  - A stress test starts and stops each transport 1,000 times while a fake audio thread submits PCM.
  - The test is clean under ASan on Win64 (MSVC ASan) or under a TSan-instrumented Linux core test where applicable.
  - The code review agent confirms no `Owner&` or raw `this` remains in any object reachable from a non-game thread.
- **Depends on:** a short design note, which can be an addendum to D4.

#### WP-S6: TCP transport correctness  ·  P0 · M · coding
- **Findings:** TRB-1, TRB-2, TRB-3, TRB-4, TRB-5, TRB-6, TRB-8, TRB-9, TRB-13, TRB-14
- **Goal:** TCP delivers every frame, survives bursts and slow links, and reconnects sanely.
- **Approach:**
  - Write a proper stream parser that loops over buffered frames, compacts or uses a read offset, and only calls `Recv` when no complete frame is buffered (TRB-1).
  - Handle partial sends and `EWOULDBLOCK` with `Wait(WaitForWrite)` and a stall timeout. Drop whole frames only, never partial ones (TRB-2).
  - Use atomic byte accounting (TRB-3). This is folded into WP-A1's shared queue.
  - Use exponential backoff and a connect timeout (TRB-4, TRB-5).
  - Add a keepalive or heartbeat to stop idle flapping (TRB-6).
  - Resync by scanning for the magic in one pass (TRB-8).
  - Use a configurable max frame size (TRB-9).
  - Clean up correctly when Start fails (TRB-13).
  - Make the queue cap configurable and add an age limit (TRB-14).
- **Acceptance:**
  - A pure parser unit test covers three frames in one read, a header split across reads, and garbage then magic.
  - A localhost integration test covers a burst of 1,000 frames with none lost, a slow reader, and a reconnect after the sender restarts.
- **Depends on:** WP-T2.

#### WP-S7: WebRTC functional fixes  ·  P1 · M · coding
- **Findings:** TRF-2, TRF-3, TRF-4, TRF-5, TRF-15, TRF-16, TRF-19, TRF-23, TRF-24, TRF-25, TRF-31
- **Goal:** the WebRTC transport connects with auto-fetched tokens, refreshes them, and doesn't corrupt memory.
- **Approach:**
  - Keep the UTF-8 converter alive across the FFI call, and grep for other stored `TCHAR_TO_UTF8` results (TRF-2).
  - Separate "connect requested" from the token value. Don't write `Token` from the HTTP callback (TRF-3).
  - Fix the double free on serialize failure, or delete the `Send(SubjectList)` path in favour of `SendSerialized` (TRF-4, TRF-19).
  - Remove the heuristic backpressure estimator, or base it on `lk_get_data_stats` (TRF-5).
  - Synchronise receiver state across the FFI, HTTP and game threads (TRF-15).
  - Register one data callback only (TRF-16).
  - Apply refreshed tokens through `lk_refresh_token`, which is declared in `livekit_ffi.h:369` and currently never called (TRF-23).
  - Make the token fetcher lifetime independent of `GWorld` (TRF-24).
  - Fix identity and room collisions (TRF-25).
  - Handle UTF-8 labels consistently (TRF-31).
- **Acceptance:**
  - Fake-FFI tests cover auto-fetch leading to connect, refresh leading to `lk_refresh_token`, and serialize failure without a double free.
  - Manual test notes against a local LiveKit server are included in the PR.
- **Depends on:** WP-T2 (fake FFI). D2 does not block this; fixes are worthwhile even if WebRTC ships later.

#### WP-S8: MoQ functional fixes  ·  P1 · M · coding
- **Findings:** TRF-8, TRF-9, TRF-11, TRF-13, TRF-20, TRF-29, TRF-37, TRF-39
- **Approach:**
  - Clear announced namespaces on disconnect, or recreate the client (TRF-8).
  - Keep the publisher lifecycle on the worker thread, or snapshot it under a lock (TRF-9).
  - Clear the connect-in-flight flag on every exit path, and add a connect timeout (TRF-11).
  - Simplify the dispatcher, and stop it on module shutdown (TRF-13).
  - Add backoff to subscribe retries (TRF-20).
  - Make symbol validation consistent (TRF-29).
  - Pick the audio codec from the frame, not local config (TRF-37).
  - Use `moq_last_error` correctly, and remove the ineffective `catch(...)` (TRF-39).
- **Acceptance:**
  - Fake-FFI tests cover a disconnect, reconnect and re-announce cycle, and a connect that never completes before retrying.
- **Depends on:** WP-T2.

#### WP-S9: Credentials handling  ·  P0 · M · coding (after D6)
- **Findings:** SND-10, RCV-3, TRF-21, TRF-22, SHR-11, LIC-1
- **Goal:** no secret is written to an asset, an ini file, a connection string or a log.
- **Approach:**
  - Implement D6: secret keys are declared per customization and routed to a transient store.
  - Strip secrets before `ExportText` and `SaveConfig`.
  - Honour `bPersistToken`.
  - Redact secrets in `ToDebugString` and in logs.
  - Remove the default secret from the mock token server.
  - Document that the token endpoint must be authenticated, and that grants come from the server, not the client (TRF-22).
- **Acceptance:**
  - A test saves a component and a receiver source with a token set, then asserts that the token string appears in none of the saved asset bytes, the ini, or the connection string.
  - A test checks the `ToDebugString` redaction.
- **Depends on:** D6.

#### WP-S10: Audio codec and metrics correctness  ·  P1 · M · coding
- **Findings:** SHR-1, SHR-2, SHR-3, SHR-8, SHR-17, SHR-18, SHR-26, SHR-31, SHR-32, SND-16, SND-21, SND-28
- **Approach:**
  - Label a frame's codec only after encoding succeeds, and force PCM16 when Opus is compiled out (SHR-1).
  - Add a per-stream accumulator that emits exact Opus frame sizes, with no silent permanent fallback (SHR-2).
  - Give metrics stable addresses, have each transport own its metrics handle, and return snapshots instead of shared state (SHR-3, SHR-17, SHR-26).
  - Validate sample rate and channel count at the parse boundary (SHR-8).
  - Size Opus buffers correctly and check ctl return values (SHR-31).
  - Compensate for codec delay in the round-trip test (SHR-32).
  - Align the audio stream label with the pose subject name (SND-16).
  - Cut the allocations and copies on the audio encode and send path (SHR-18).
  - Use a stateful resampler with anti-aliasing (SND-21).
  - Stop capturing audio when audio is disabled (SND-28).
- **Acceptance:**
  - Unit tests cover 512- and 1024-frame buffers producing Opus packets, and "Opus unavailable" producing PCM16 labelled correctly.
  - A metrics stress test with concurrent registration and updates is clean.
- **Depends on:** WP-T2.

#### WP-S11: NNG transport fixes  ·  P1 · M · coding
- **Findings:** TRB-33, TRB-34, TRB-36, TRB-39, TRB-40, TRB-42, TRB-43
- **Approach:**
  - The worker thread owns the socket lifecycle (TRB-33).
  - Drop the oldest frame on EAGAIN, and never requeue at the tail (TRB-34).
  - Fix the `NNG_OPT_SENDBUF` type and units, and the `SENDTIMEO` usage (TRB-36, needs NNG-doc verification).
  - Honour the URI host (TRB-39).
  - Change the Pair defaults so one side dials, and fix "Pull (client dial)" (TRB-40).
  - Fix the receiver's limits, stats and callback lifetime (TRB-42).
  - Log errors at Warning (TRB-43).
- **Acceptance:** a localhost integration test covers each mode and role pair with default settings.
- **Depends on:** WP-T2.

### M2: Fab-buildable package

#### WP-F0: Re-verify the Fab requirements  ·  P0 · S · design
- **Findings:** summary section of `fab-ci-docs.md`
- **Approach:** read the live "Fab Technical Requirements" and "Fab Plugin Compilation Environment" pages. Record the checklist, with the retrieval date, in `docs/fab/requirements.md`. Correct any row of the checklist in `fab-ci-docs.md` that turns out to be wrong.
- **Acceptance:** a maintainer has reviewed `docs/fab/requirements.md`. Every WP-F* acceptance line refers to it.

#### WP-F1: Packaging layout  ·  P0 · M · coding (after D3)
- **Findings:** FAB-2, FAB-6, CORE-20, CORE-21, HYG-2
- **Approach:**
  - Implement D3. Either add an `Open3DStreamCore` module that compiles the core sources, or move the plugin-root `ThirdParty/` under `Source/ThirdParty/` with one `Type=External` module per library.
  - If anything stays outside `Source/`, `Content/` or `Resources/`, add `Config/FilterPlugin.ini`.
  - Retire `Sync-O3DSCore.ps1`, or reduce it to a dev convenience.
  - Set `CanContainContent` to match reality (HYG-2). This interacts with WP-U5.
- **Acceptance:** a clean clone, with no scripts run, passes `RunUAT BuildPlugin -Plugin=... -TargetPlatforms=Win64 -Rocket` on UE 5.7, verified in CI (WP-F8).

#### WP-F2: Platform declarations and Build.cs guards  ·  P0 · M · coding (after D1)
- **Findings:** FAB-3, FAB-8, SND-11, RCV-30, TRB-24, TRB-41, TRF-27, SHR-4 (layering part), SHR-20, SHR-21, SHR-22, BUILD-1, BUILD-2, BUILD-4, BUILD-5
- **Approach:**
  - Add `PlatformAllowList` to every module and `SupportedTargetPlatforms` to the plugin.
  - Replace the `throw`s with clean exclusion.
  - Make the feature flags actually exclude sources (`#if` guards or module-level exclusion). Move `O3DBuildFlags` out of Shared's Build.cs and stop caching it per process.
  - Fix the wrong ThirdParty paths (BUILD-1). Delete the dead platform branches (BUILD-2).
  - Make public dependencies private where possible (SHR-20).
  - Decide `SupportedTargetTypes` for Server and Client (FAB-8).
  - Set exceptions and RTTI only where needed (BUILD-5).
- **Acceptance:**
  - CI builds each flag combination.
  - A game target that also targets Linux configures without error, with the plugin excluded on Linux.

#### WP-F3: Binaries and symbols  ·  P0 · S · coding
- **Findings:** FAB-4, TRF-14, TRF-28, BUILD-3
- **Approach:**
  - Remove both `.pdb` files from the tree, and move them to a release asset or symbol store.
  - Remove the pdb `RuntimeDependencies` entry.
  - Unify DLL loading.
  - Don't register factories when the DLL failed to load. Drain instances before `FreeDllHandle`.
  - Wrap third-party includes in `THIRD_PARTY_INCLUDES_START`/`END`, and add them as system includes (BUILD-3).
- **Acceptance:** the CI package-content check fails on `*.pdb`. The module unloads cleanly in an editor session with a live transport.

#### WP-F4: Copyright headers and rights holder  ·  P0 · S · coding
- **Findings:** FAB-5, FAB-9
- **Approach:**
  - The maintainer decides the rights-holder string (FAB-9).
  - Add a uniform header to every `.h`, `.cpp` and `.cs` file under `Source/`.
  - Add a CI lint for it.
- **Acceptance:** the lint passes with zero exceptions under `Source/`.

#### WP-F5: Third-party licensing  ·  P0 · M · design (+counsel) then coding
- **Findings:** FAB-1, FAB-10, FAB-11, SHR-23, LIC-1
- **Approach:**
  - Implement D2.
  - Delete the orphaned `ThirdParty/Include` Opus headers (SHR-23, FAB-10). Establish Opus provenance and version, and check for a duplicate-symbol clash with the engine's libOpus. Consider using the engine's Opus module instead (needs-UE-verification).
  - Get the livekit_ffi license text added upstream. Generate notices with `cargo about`, and add mlkem-native (FAB-11).
- **Acceptance:**
  - `THIRD_PARTY_LICENSES.md` matches the shipped binaries exactly.
  - `check-no-video-codecs.sh` passes, or WebRTC is excluded from the Fab package.
  - Counsel sign-off is recorded.

#### WP-F6: Remove dev material from the shipped tree  ·  P1 · S · coding
- **Findings:** HYG-1, HYG-3, HYG-4, SHR-27, TRB-44 (the IP part), CI-7
- **Approach:**
  - Move planning, review and analysis `.md` files and `Tests/mock-token-server.py` from `Source/` to `docs/dev/`.
  - Remove the hard-coded public IP from the NNG README.
  - Remove the development-diary console command (SHR-27).
  - Fix the sandbox startup map (HYG-3).
  - Move the root-level plan files into `docs/` (HYG-4).
  - Delete the stale workflows and scripts (CI-7).
- **Acceptance:** the CI rule rejecting `*.py` and planning-doc names under `Plugin/Source` passes.

#### WP-F7: Editor module split  ·  P1 · M · coding (after D9)
- **Findings:** FAB-7, SND-34, TRB-45, TRB-46
- **Approach:**
  - Move all detail customizations and Slate panels into the Editor module(s).
  - Fix the Slate panels: use weak UObject pointers, don't mutate the asset on construction, debounce drag updates, and support undo (TRB-45, SND-35).
- **Acceptance:**
  - Runtime modules have no `UnrealEd`, `PropertyEditor` or `Slate` dependencies (grep check in CI).
  - A packaged Shipping game builds.

#### WP-F8: CI that tells the truth  ·  P0 · M · coding
- **Findings:** CI-1, CI-2, CI-3, CI-4, CI-5, CI-6, CI-8, CI-9
- **Approach:**
  - Remove the fallback in `Build-Plugin.ps1`, or put it behind `-AllowFallback`, which CI never passes (CI-1).
  - Fix the boolean `if`, and run the automation tests in PR CI (CI-2).
  - Make the test runner fail on failures, and make sure the nightly tests fresh binaries (CI-3).
  - Add a warnings-as-errors build, a strict-includes build and a non-unity build (CI-4).
  - Add a `fab-package` job that builds the source-only zip, runs BuildPlugin on the zip's contents, asserts the package contents, and uploads the zip. Fix the release notes' compatibility claim (CI-5).
  - Stop the 6-hour WebRTC build running on every push (CI-6).
  - Wire in `check-no-video-codecs.sh` (CI-8).
  - Revisit skipping draft PRs (CI-9).
- **Acceptance:** a deliberately broken commit turns each job red. A clean commit turns them green, and the Fab zip artifact is uploaded.

#### WP-F9: Plugin descriptor metadata  ·  P1 · S · coding
- **Findings:** FAB-12, FAB-13, FAB-14, UX-5, DOC-6
- **Approach:**
  - Set `IsBetaVersion` or mark MoQ as Experimental per D2.
  - Set a real SupportURL.
  - Point DocsURL at a public, versioned docs page.
  - Use consistent naming (UX-5).
  - State the supported engine versions and platforms (DOC-6).

#### WP-F10: Fab submission dry run  ·  P0 · S · review (M5)
- Run the WP-F0 checklist against the CI Fab zip.
- Install into a clean UE 5.7 project from the zip, enable the plugin, open the sample map (WP-U5), and run the quick start (WP-D2).
- File any gap as a new WP.

#### WP-F11: WebRTC add-on plugin (`Open3DBroadcastWebRTC`)  ·  P1 · M · coding (after WP-F3 and WP-F1)
- **Decision:** [ADR 0002](../adr/0002-webrtc-and-moq-in-first-fab-release.md), Decision §1 and Implementation outline step 7. Related findings (owned elsewhere): TRF-4, TRF-14, TRF-19, TRF-28, SHR-13, SHR-14, SHR-19, SHR-36, BUILD-1.
- **Goal:** WebRTC reaches users as a separate plugin that installs next to the Fab-installed Open3DBroadcast, without editing the Fab install. The main plugin no longer contains the WebRTC module.
- **Approach:**
  - Create `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/` with its own `.uplugin`: a plugin dependency on `Open3DBroadcast`, `PlatformAllowList: ["Win64"]` and `TargetDenyList: ["Server","Program"]` per ADR 0001, `IsBetaVersion: true`, and the same `EngineVersion`.
  - Move `Source/Open3DTransportWebRTC/` into it with `git mv`, keeping the module name and history. Remove the module's entry from `Open3DBroadcast.uplugin`, and move WebRTC's licence files and notices with it.
  - Fix the three blockers recorded in ADR 0002:
    1. The DLL lookup must find the add-on's own plugin, not `FindPlugin(TEXT("Open3DBroadcast"))` (`Open3DTransportWebRTCModule.cpp:735`). Unify DLL loading at the same time (TRF-28).
    2. Build.cs depends on the `Open3DStreamCore` module (ADR 0003) instead of plugin-root `ThirdParty` paths and the core library coming indirectly through Open3DSender (`Open3DTransportWebRTC.Build.cs:66-70`, BUILD-1). Alternatively, delete the legacy `Send(const O3DS::SubjectList&)` path (TRF-4, TRF-19) so no core dependency remains.
    3. Unload safety: don't register factories if the DLL failed to load; drain live instances before `FreeDllHandle` (TRF-14). Rely on WP-A1's instance tracking (SHR-13) when it lands.
  - Add a transport-interface version constant to the main plugin (the minimal form of SHR-14). The add-on checks it in `StartupModule`, and refuses to register with a clear log message on a mismatch.
  - Move the WebRTC-only build flags and console variables out of `Open3DShared` into the add-on (SHR-19). Leave the LiveKit fields in `FO3DTransportConfig` until D6 and WP-A1 replace them (SHR-36).
  - CI builds the add-on for each release against the matching Open3DBroadcast package and publishes it as a release asset for the support site. The Fab listing text links to the support-site download (with WP-F9).
  - The WebRTC docs move with the add-on. The main USER_GUIDE says WebRTC is available as a free add-on and links to the download (with WP-D2).
- **Acceptance:**
  - `Open3DBroadcast` alone builds and packages with no WebRTC module or `livekit_ffi` files, and passes `check-no-video-codecs.sh`.
  - In a clean UE 5.7 project, the Fab-style package plus the add-on in the project's `Plugins/` folder:
    - lists WebRTC in the sender and receiver transport pickers;
    - passes the WP-T2 conformance suite with both plugins enabled;
    - keeps the main plugin working after the add-on is disabled and the editor restarted.
  - An add-on built against a different interface version logs the mismatch and does not register or crash.
  - The needs-verification items Q7 and Q8 in ADR 0002 are answered in the PR. They cover installing next to a Fab-installed plugin, and whether the Fab package ships the headers and import libraries a source build needs.
- **Blocked on counsel:** publishing the add-on (and making the listing link live) before the codec-free `livekit_ffi` rebuild depends on L1 in ADR 0002. The code work does not wait for it.
- **Depends on:** WP-F3, WP-F1 (`Open3DStreamCore`), WP-F2 (descriptor keys). It benefits from WP-S7 landing first.

### M3: Architecture

#### WP-A1: Transport core consolidation  ·  P2 · L · design (D4) then coding (split into 5+ PRs)
- **Findings:** SHR-12, SHR-13, SHR-14, SHR-16, SHR-24, SHR-35, SHR-36, SND-23, RCV-27, RCV-28, TRB-3, TRB-38, TRF-32, TRF-38, TRB-26, TRB-27, SHR-9
- **Goal:** one abstraction and one registry, with the shared plumbing written once.
- **PR sequence:**
  1. Move the interfaces and a single thread-safe registry into the D4 home. Keep the old headers as forwarding shims for one release (SHR-12, SND-23, RCV-27, RCV-28, SHR-24).
  2. Add the lifetime contract: instance tracking and drain-on-unload (SHR-13).
  3. Add the result and error type, interface version and capability query (SHR-14).
  4. Add shared building blocks, and migrate one transport per PR:
     - `FO3DBoundedByteQueue` plus a worker, with atomic accounting (TRB-3).
     - `FO3DUnifiedReceiveDemux`.
     - `FO3DAudioSinkBase` with the WP-S5 owner guard and a per-stream decoder map.
     - Shared host:port parsing that resolves hostnames (TRB-26, SHR-9).
     - Shared audio (de)serializers (SHR-35).
     - **Progress:** PR 4a added the blocks under ADR 0007's names (`FO3DSendQueue` + `FO3DTransportWorker`, `FO3DUnifiedReceiveDemux`, `FO3DAudioPublishState` + `FO3DQueuedSenderAudioSink`, `O3DTransportOptions`) and migrated Loopback; PR 4b migrated TCP; PR 4c migrated UDP and removed `Open3DTransportSockets`' Open3DSender/Open3DReceiver dependency; PR 4d migrated NNG and removed `Open3DTransportNNG`'s; PR 4e migrated MoQ and removed its Open3DReceiver dependency (Open3DSender stays for `SubjectName` until step 5); PR 4f moved the WebRTC add-on's receiver hand-off, demux and options onto the blocks and kept its synchronous LiveKit send path and PCM16 audio sink (ADR 0007 addenda "WP-A1 PR 4a" to "PR 4f"). Step 4 is complete. Step 5 is split into 5a, 5b and 5c. PR 5a added `FO3DTransportOptionsView`, gave both configure functions the view signature, removed the LiveKit fields from `FO3DTransportConfig` (version 5) and MoQ's and the WebRTC add-on's Open3DSender/Open3DReceiver dependencies, and made switching transports keep each transport's options (SND-35) (addendum "WP-A1 PR 5a"). PR 5b gave `ISerializedFrameConsumer` its view and owned forms (the demux no longer copies mocap; Loopback, MoQ and WebRTC hand over their buffers) and deleted `IOpen3DSender::Send(SubjectList)`, in the same unreleased version 5 (addendum "WP-A1 PR 5b"). PR 5c made `FO3DTransportConfig::Transport` the registered `FName` and `Role` an `EO3DTransportRole` (TRB-27), let `Secret` schema entries declare their key and environment variable (the old lists merged in as deprecated inputs; ADR 0004 behaviour unchanged), and added `Float`, `bRestartOnChange` and `Validate` to the schema (start fails with `InvalidConfig` on a refused value; the panels show the error and restart a running sender on a restart field), still in version 5 (addenda "WP-A1 PR 5c" in ADR 0007 and ADR 0004). Step 5 is complete. Step 6 removed the forwarding shims, the deprecated transport customizations, `EditLegacyDescriptor`, the `SecretOptionKeys`/`SecretEnvVars` lists and `SupportsAudio()`/`SupportsControl()`, in this unreleased cycle by the maintainer's decision (no release carried them, no third-party code builds against them), so it also stays in version 5 (addendum "WP-A1 step 6"). **WP-A1 is complete**; next in M3 is WP-A2.
  5. Add a typed config in place of the stringly-typed LiveKit fields in `FO3DTransportConfig` (SHR-36). Normalise identity and role fields (TRB-27). Make the consumer API zero-copy, with a documented threading contract (SHR-16, TRF-38).
  6. Remove the forwarding shims (done, WP-A1 step 6; see the Progress note above).
- **Acceptance:**
  - The WP-T2 conformance suite passes for every transport after each migration.
  - Net lines of transport code go down.
  - No transport keeps its own copy of the queue, demux or sink code.

#### WP-A2: Asynchronous sender pipeline  ·  P1 · L · coding (after D5)
- **Findings:** SND-8, SND-9, SND-12, SND-17, SND-18, SND-29, CORE-7, CORE-18, TRB-20, TRF-19
- **Approach:**
  - Implement D5.
  - Order the capture tick after the mesh's animation evaluation (SND-12, needs-UE-verification of the tick-prerequisite API).
  - Pool frame allocations (SND-9, CORE-18).
  - Use a table-driven CRC, or make the CRC optional behind a flag (CORE-7).
  - Read local transforms from the bone-space pose (SND-29).
  - Use one clock domain for pose and audio (SND-17).
  - Do audio device enumeration asynchronously and cache it (SND-18).
  - Move UDP sends off the game and audio threads (TRB-20).
  - **Progress:** split into A2a to A2e per ADR 0008's implementation outline. A2a landed the settings snapshot (encoding and curve filtering settings in `FO3DSenderEncodingSettings`), the serializer without a component pointer (SND-22), curve filtering after sampling, the bounded `FO3DSPoseFramePool` (SND-9) and the sampling-time clock, still synchronous on the game thread (ADR 0008 addendum "WP-A2a"). A2b moved the capture tick to `TG_PostUpdateWork` and made the target mesh's tick a prerequisite of it, still synchronous (SND-12; ADR 0008 addendum "WP-A2b"); the root-bone acceptance test waits for a skeletal mesh test asset, and an ordering test in a ticking world stands in for it. A2c added the per-sender pose pipeline: sampling stays on the game thread, and curve filtering, serialization and `SendSerialized` run on a `UE::Tasks` worker behind a bounded drop-oldest queue (depth 2, `o3d.Sender.PipelineDepth`) with in-order control items, behind `o3d.Sender.AsyncPipeline` (default on) for one release (SND-8, TRB-20; ADR 0008 addendum "WP-A2c"); `tx_seq` on the wire waits for WP-A4a, and the Unreal Insights numbers are a manual step for the maintainer. A2a, A2b and A2c merged as #316, #317 and #318 (2026-10-03). A2d mapped the submix and microphone audio clocks onto the sender clock with a drift-tracking offset (SND-17), cached the capture device list (enumerated once per Input start, once in the editor, and on request) and opens the device once per start (SND-18; ADR 0008 addendum "WP-A2d"). A2e made the core's CRC-32 slicing-by-8 (same value, so no wire change) and reuses one FlatBuffers builder per thread with an in-place `finalize()`: Serialize + Parse of one 250-bone, 250-curve subject is 3.5x faster on MSVC Release (CORE-7, CORE-18 serialize half; ADR 0008 addendum "WP-A2e"). WP-A2 is complete except removing `o3d.Sender.AsyncPipeline` one release later; a follow-up made Parse reuse its subjects, transforms and curve names on a full sync with the same result as a fresh parse (CORE-18 Parse half; Parse about 160 to 85 µs; ADR 0008 addendum "Parse object reuse").
- **Acceptance:**
  - A profiling capture (Unreal Insights) shows the game-thread cost per sender under a defined budget, for example 0.1 ms for a 250-bone, 250-curve subject. Record the before and after numbers in the PR.
  - The core Serialize and Parse benchmark improves by at least 3x (CORE-7 measured about 4x possible).

#### WP-A3: Decompose the god classes  ·  P2 · L · coding
- **Findings:** SND-22, RCV-29, SHR-38, RCV-11, RCV-12, RCV-13
- **Approach:**
  - Split `UO3DSenderComponent` into pose capture, curve processor (already separate), audio capture binding, and a transport controller.
  - Split `FO3DReceiverSource` into transport session, frame decoder, reorder and clock stage, LiveLink publisher, and audio routing.
  - Remove core third-party headers from the receiver's public header.
  - Fix the hot-path allocations and FName construction during the split (RCV-11, RCV-12, RCV-13).
  - This is a pure refactor with no behaviour change. Tests from WP-S3 and WP-S4 must pass unchanged.
- **Acceptance:** each new class is under 400 lines and has its own unit test. The public headers include no `o3ds/` headers.
  - **Plan** (2026-10-03, one PR per step, each a refactor with output identical to before except step 5):
    1. Receiver `FO3DReceiverFrameDecoder`: parse result to LiveLink names, parents, transforms and curves, with the topology caches and the RCV-11/RCV-12 reuse.
    2. Receiver `FO3DLiveLinkPublisher`: static and frame pushes, initialized subjects, inactivity removal, the test push hooks.
    3. Receiver stream scheduler: the stream table, reorder gate, clock mapping, legacy ordering, concealment and their metric deltas.
    4. Receiver control routing and audio metadata in small classes; `FO3DReceiverSource` holds the pieces through forward-declared pointers, its header drops every `o3ds/` include, and `Open3DStreamCore` becomes a private dependency of Open3DReceiver.
    5. RCV-13 (one transform conversion keeping doubles, a counter for dropped poses): a behaviour change, with a CHANGELOG note.
    6. Sender pose sampler: skeleton and descriptor cache, subject naming, frame filling.
    7. Sender audio binding.
    8. Sender transport options and secrets, and the editor restart policy.
  - **Rules:** no `UPROPERTY` or `UFUNCTION` on `UO3DSenderComponent` moves or is renamed (saved assets and Blueprints bind to them); work stays on the thread it runs on today; existing tests are not edited (test accessor bodies may adapt); a new private class is reached by its unit test through an exported probe (`FO3DSenderPipelineProbe` pattern); "under 400 lines" counts the class's `.h` and `.cpp` together; headers under `Public/Testing/` are test-only and outside the "no `o3ds/` in public headers" rule, but no other public header includes them. SHR-38 (global singletons) needs a design decision and is not part of this series.
  - **Progress:** step 1, `FO3DReceiverFrameDecoder` (about 290 lines) with `Open3DBroadcast.Receiver.FrameDecoder.*`; step 2, `FO3DLiveLinkPublisher` (about 295 lines) with `Open3DBroadcast.Receiver.LiveLinkPublisher.*`; step 3, `FO3DReceiverStreamScheduler` (about 180 lines) and `FO3DReceiverConcealment` (about 315 lines) with `Open3DBroadcast.Receiver.StreamScheduler.*` and `.Concealment.*`. Step 4, `FO3DReceiverControlRouter` (about 195 lines) with `Open3DBroadcast.Receiver.ControlRouter.*`; `O3DReceiverSource.h` includes no `o3ds/` header and `Open3DStreamCore` is a private dependency of Open3DReceiver. Audio metadata stayed in the source (about 100 lines, Open3DShared types only; a deviation from the plan's step 4). `O3DReceiverSource.cpp` went from 1,807 to 1,035 lines. Step 5 (RCV-13), the decoder keeps the core's doubles and counts dropped poses in the new `InvalidPosesDropped` receiver metric, with `Open3DBroadcast.Receiver.FrameDecoder.KeepsDoublePrecision` and `.CountsDroppedPoses`. Step 6, `FO3DSenderPoseSampler` (about 360 lines: descriptor cache, subject naming, frame shell and bones; curve capture stays in the component) with `Open3DBroadcast.Sender.PoseSampler.*`; `O3DSenderComponent.cpp` went from 1,709 to 1,466 lines. Step 7, `FO3DSenderAudioBinding` (about 220 lines) with `Open3DBroadcast.Sender.AudioBinding.*`; `O3DSenderComponent.cpp` at 1,334 lines. Step 8, `FO3DSenderTransportSettings` (about 225 lines: options, secrets, migration, option switching, restart properties) with `Open3DBroadcast.Sender.TransportSettings.*`; `O3DSenderComponent.cpp` at 1,218 lines (1,709 before WP-A3). The plan's eight steps are done; SHR-38 remains for a design decision.

#### WP-A4: Protocol robustness and versioning  ·  P1 · L · design (D7, D8) then coding
- **Findings:** CORE-5, CORE-6, CORE-10, CORE-11, CORE-12, CORE-13, CORE-14, CORE-15, CORE-16, CORE-29, SND-15, SHR-7, SHR-30, SHR-33, RCV-8, RCV-9
- **Approach:**
  - Stamp `tx_seq` and `frame_epoch` from the UE sender (SND-15, CORE-29).
  - Make the residual decoder skip updates on a sequence gap until the next keyframe, and return "needs keyframe" (CORE-5, CORE-6).
  - Make gate decisions before `Parse()` mutates codec state.
  - Resend on loss, or add a periodic resync on the delta paths (CORE-10).
  - Send scale (CORE-11).
  - Fix the rotation quantization precision choice (CORE-12).
  - Normalise quaternions (CORE-13).
  - Document the determinism limits, or pin the floating-point mode (CORE-14).
  - Make the gate robust to a forged `tx_seq`, and verify the CRC in `PeekMeta` (CORE-15).
  - Implement D8 versioning (CORE-16, SHR-7, SHR-30).
  - Hash names with length prefixes (SHR-33).
  - Set timecode on LiveLink frames (RCV-8).
  - Verify how mapped time interacts with LiveLink's `WorldTime` (RCV-9, needs-UE-verification).
  - Every schema change here is append-only and must be coordinated with the A, C and D phases in `resilient-streaming-and-motion-prediction.md`.
- **Acceptance:**
  - The WP-T1 loss and mid-join tests show bounded error that recovers at the next keyframe.
  - An old-reader and new-writer compatibility test passes in both directions.
- **Progress** (2026-10-03): PR 1, D8 core (ADR 0009 item 1-3, 9, 11): frame word as `min_reader_version`, `file_identifier`, `protocol_version`, `CheckFrame` in every reader, core 1.1.0, golden frames and the baseline-reader CI step. PR 2, UDP fragment header v2 (item 5, TRB-17): fragments are recognised by their magic; other datagrams pass through whole, so the transports stay byte-opaque (a deviation from item 5's drop-and-count, recorded in the CHANGELOG). PR 3, envelope v2 (item 4): `O3DU`, little-endian, sequence number, codec in core, LE PCM; the TCP keepalive stays v1 and audio payload v3 is not done (MoQ's envelope-less audio track needs the payload's codec and timestamp), both recorded in the CHANGELOG. PR 4, the length-prefixed name hash in core (item 8, SHR-33). PR 5, `docs/wire-format.md`, one root changelog with its policy, and the ADR 0009 implementation notes (item 10). ADR 0009 is implemented except packaging the changelog in the Fab zip (Q4, WP-F8). A follow-up PR removed the compatibility with formats from before protocol 2 (no users run old receivers): envelope v1, the v1 keepalive, the pre-D8 frame check, the golden fixtures and the old-reader CI step. ADR 0005, PR A (SND-15, CORE-29): `O3DS::StreamWriter` in core, and the UE serializer stamps `tx_seq`, `tx_wallclock_us` and `frame_epoch` through one writer per subject (the receiver keys streams by subject names), with a process-wide epoch floor so a restart within one second still gets a newer epoch; receivers now take UE senders' frames through the reorder gate. ADR 0005, PR B (CORE-5, CORE-6): `SubjectUpdate.ref_seq` (protocol 2, no bump), and receivers parsing sequenced frames drop updates relative to a missed full Subject, residual updates after a gap, and residual updates without decoder history, holding the subject until its next full Subject (residual streams recover only there, a deviation recorded in ADR 0005); the sender sends a full Subject after a residual update it did not deliver; new metric `UpdatesAwaitingFullSync`. ADR 0005, PR C (item (iii)): residual coding falls back to quantized, with one Warning and a details-panel warning, on a transport whose capabilities are not `ReliableOrdered`. ADR 0005, PR D (item (vi)): `IOpen3DSender::SetPeerJoinedCallback`; TCP (accept) and NNG (pipe add) report new peers, and the sender's next frame is a full sync; WebRTC waits for FFI verification (Q4). ADR 0005 is implemented except the WebRTC peer join. CORE-15 is closed: the gate needs a confirming next frame before it follows a forward jump beyond `max_forward_jump` (#345; the CRC half was #335). Remaining in WP-A4: the other approach items (resend or resync on loss, scale, rotation quantization precision, quaternion normalisation, determinism notes, LiveLink timecode).

#### WP-A5: Connection lifecycle and reconnect  ·  P1 · M · coding
- **Findings:** TRF-6, TRF-7, TRF-26, TRB-4, TRB-5, TRB-6, TRB-7, TRF-17, TRF-18, RCV-19
- **Approach:**
  - Add a shared backoff helper, starting from the MoQ `ComputeReconnectDelaySeconds` idea.
  - Move FFI connect, disconnect, destroy, announce and subscribe onto transport workers (TRF-7).
  - Turn the WebRTC watchdog off by default, or give it backoff (TRF-6).
  - Add WebRTC sender reconnect (TRF-26).
  - Support multiple TCP clients, or reject extra clients explicitly (TRB-7).
  - Bound the receive queues and reduce copies (TRF-17, TRF-18, RCV-19).
- **Acceptance:**
  - Insights shows no FFI call on the game thread.
  - A kill-the-server test reconnects every transport within the backoff budget.

#### WP-A6: Reduce global state and clean up console variables  ·  P2 · M · coding
- **Findings:** SHR-19, SHR-22, SHR-25, SHR-34, SND-32, RCV-25, RCV-26, RCV-33
- **Approach:**
  - Delete the dead WebRTC CVars from Shared, and turn off the verbose CVar that is on by default.
  - Make console command and global instance lifetimes module-scoped.
  - Use per-instance log-once flags.
  - Fix or remove the misleading "round-trip latency" metric (RCV-33).
  - Delete the dead helpers.
- **Progress** (2026-10-03): SHR-19 (the WebRTC CVars in Shared) and SHR-22 (the Build.cs flag cache) were already fixed by earlier work. PR 1 (receiver logs): RCV-25, RCV-26, RCV-33. PR 2 (Shared): SHR-25 (reset, dead metrics removed, Display, CSV with transports), SHR-34 (dead URL helpers), SND-32 (DumpStats command lifetime). With both, WP-A6 is complete.

#### WP-A7: Core public API and legacy code  ·  P2 · M · design then coding
- **Findings:** CORE-17, CORE-24, CORE-25, CORE-27, CORE-28, CORE-30
- **Approach:**
  - Define the small stable surface the plugin uses (CORE-30): `model`, `udp_fragment`, `reorder_gate`, `sequencing`, `clock_offset` and `predict/*`.
  - Move the legacy connectors into a separate CMake target that is off by default (CORE-27).
  - Archive `apps/FbxStream`, `Test1`, `SubscribeTest`, `XSensTest`, `plugins/maya`, `plugins/mobu`, `python/` and `sphinx/` in an archive branch or tag, after confirming with the maintainer. Keep `DeterminismProbe`, `PredictorEval`, and `Repeater` if its image is still deployed (CORE-28).
  - Use owning types in the model layer (CORE-17).
  - Fix the `Context` copy constructor (CORE-24).
  - Remove dead code (CORE-25).

#### WP-CTL: Control channel for events and values  ·  P1 · L · design (D11) then coding (7 PRs)
- **Decision:** [ADR 0011](../adr/0011-control-channel.md), Implementation outline and Verification / acceptance. No owned findings (new feature).
- **Goal:** from the sender, fire cues (events) and set parameters (values) on every receiver of a stream, on every transport, with loss recovery, late-joiner convergence and cue timing aligned to the mocap they were fired against.
- **PR sequence and status:**
  1. **CTL-1 Core:** `src/o3ds_control.fbs`, `src/o3ds/control.{h,cpp}` (codec, `ControlPublisher`, `ControlReceiver`), core mirror, CTest and `fuzz_control`. **Done (#291).**
  2. **CTL-2 Shared and interfaces:** `EUnifiedKind::Control`, `WriteControlEnvelope`/`TryGetControlPayload`, `SupportsControl`/`SendControl`/`SetControlSink`, `FO3DControlBus`, fake-transport support; `O3D_TRANSPORT_API_VERSION` 1 → 2. **Done (#292).**
  3. **CTL-3 In-band transports:** TCP, UDP, NNG, Loopback, with conformance cases. **Done (#293).**
  4. **CTL-4 Gameplay surface:** sender component API, receiver `FControlSink` with the alignment hold queue, `UO3DControlSettings`, `UO3DRemoteControlComponent`. **Done (#294).** The packaged Shipping test that confirms control can be enabled in Shipping (ADR 0011 open question 11) is not in CI yet; add it to the nightly Shipping build.
  5. **CTL-5 MoQ:** `control/<session>` track. **Done (#295).** The live-relay test case is still to write (needs a relay).
  6. **CTL-6 WebRTC add-on:** `__o3d.ctl` send path and receive classification in `WebRTCSender.cpp`/`WebRTCReceiver.cpp`, tests, a manual test step. **Done.** Tests are `Open3DBroadcast.Transport.WebRTC.Control.*` in the add-on (fake LiveKit FFI); the WebRTC conformance profile itself stays deferred to WP-T2e, so those tests cover the control conformance cases for WebRTC. The live-server check is case 12 in `docs/testing/webrtc-manual-test.md` (it also answers ADR 0011 open question 4 on ordering).
  7. **CTL-7 Docs:** USER_GUIDE Control section, `Transport_Module_Comparison.md` row, CHANGELOG Schema/Protocol entry. **Done.** The wire layout goes into `docs/wire-format.md` when WP-D3 creates it.
- **Acceptance:** ADR 0011 "Verification / acceptance": the core state, timing, load and fuzz cases in `core-tests.yml`; the control conformance cases pass for every transport whose `GetCapabilities().bControl` is true; control never changes mocap frame counts or content.
- **Interaction with WP-A1:** control landed before WP-A1 steps 3–4. The shared send queue (ADR 0007 item 7, "control" items never dropped for mocap), the shared receive demux and the result type in WP-A1 must absorb each transport's control path as that transport migrates.
- **Depends on:** nothing unlanded for CTL-1..5. New-peer snapshots wait for ADR 0005 (vi); until then recovery is bounded by the snapshot interval. CTL-6 needs the add-on (WP-F11, done).

### M4: Usability and documentation

#### WP-U1: Project settings  ·  P1 · M · coding
- **Findings:** UX-2, RCV-18
- **Approach:**
  - Add `UOpen3DBroadcastSettings : UDeveloperSettings` with per-transport defaults, the token endpoint (with no secrets, per D6) and default ports.
  - Stop the receiver factory from silently overwriting global defaults.

#### WP-U2: Blueprint API  ·  P1 · M · coding
- **Findings:** SND-26, UX-3, DOC-4
- **Approach:**
  - Add `BlueprintAssignable` dynamic delegates for connection state, errors and stats, and setters for transport and options.
  - Add runtime Blueprint control of the receiver.
  - Make the USER_GUIDE events real, as the repo's docs-first rule requires (copilot-instructions §5).
- **Acceptance:** every Blueprint node in the USER_GUIDE exists and is used in the sample map.

#### WP-U3: Details panel and feedback  ·  P1 · M · coding
- **Findings:** SND-24, SND-25, SND-27, SND-30, SND-35, RCV-15, RCV-16, RCV-17, TRB-43, TRF-30, TRB-21 (the UI part)
- **Approach:**
  - Put the residual and quantization settings in the custom layout.
  - Warn when the default configuration sends nowhere.
  - Add clamps and tooltips.
  - Use a picker-friendly mesh selector.
  - Make concealment settings live-editable.
  - Show real source status (connected, receiving, error cause).
  - Validate in the factory panel.
  - Raise NNG error logs from Verbose to Warning, and reduce Warning-level noise in WebRTC.
  - Hide options that do nothing, or implement them (TRB-21, TRB-25).

#### WP-U4: Receiver and audio behaviour  ·  P1 · M · coding
- **Findings:** RCV-6, RCV-20, RCV-21, RCV-22, RCV-23, RCV-24, SND-20
- **Approach:**
  - Make the subject-removal timeout configurable, and don't discard user settings (RCV-6).
  - Add a jitter buffer with bounds and drift handling (RCV-20).
  - Route one SoundWave per stream, without magic labels (RCV-21).
  - Fix self-attach when the component is the root (RCV-22).
  - Give the internal `UAudioComponent` a UPROPERTY-held lifetime (RCV-23).
  - Delete the dead `OnAudioFrame` (RCV-24).

#### WP-U5: Sample content  ·  P1 · M · coding
- **Findings:** UX-1
- **Approach:**
  - Add `Content/Demo/` with a loopback demo map: a sender on the mannequin, a LiveLink receiver preset, and a LiveLink-driven animation Blueprint.
  - Add a second map for sockets between two PIE instances.
  - Reference only engine or template content. Keep the assets small.

#### WP-U6: UDP features and network defaults  ·  P2 · M · coding
- **Findings:** TRB-16, TRB-21, TRB-22, TRB-25, TRB-28, TRB-29, TRB-31, TRB-32
- **Approach:**
  - Fragment whenever a payload exceeds the MTU (TRB-16).
  - Consider multicast and source filtering (TRB-21).
  - Don't use `SO_REUSEADDR` silently (TRB-22).
  - Remove the dead audio-port config (TRB-25).
  - Keep default ports in one place, from project settings (TRB-28).
  - Bind to localhost by default, and document exposure (TRB-29).
  - Stop the Loopback receiver overwriting the sender's queue capacity, and fix its lifecycle and diagnostics (TRB-31, TRB-32).

#### WP-D1: Rewrite the root README  ·  P1 · S · docs
- **Findings:** DOC-3
- A short landing page: what the repo contains, links to the plugin guide, and the build steps via `Build/README.md`. Fix the 11 dead links. Move the history to the CHANGELOG.

#### WP-D2: Plugin README and USER_GUIDE  ·  P0 (for Fab) · M · docs
- **Findings:** DOC-1, DOC-2, DOC-4, DOC-5, DOC-6, DOC-10, TRF-36, TRB-44, SND-37, RCV-32
- **Approach:**
  - Remove the false "Marketplace Ready" claims.
  - Add a quick start, a setup section per transport (including MoQ), a Blueprint API reference, troubleshooting, an FAQ, and the supported platforms and engine versions.
  - Make the transport comparison consistent.
  - Fix the NNG README.
  - Fix the WebRTC docs (API-key auth, the thread-safety claims).
  - Fix the public-header doc comments.
- **Acceptance:** a reviewer follows the quick start on a clean machine without help.

#### WP-D3: Developer docs and agent instructions  ·  P2 · S · docs
- **Findings:** DOC-7, DOC-8, DOC-9, SHR-37
- **Approach:**
  - Fix `.claude/claude.md` and `.github/agents/*`: remove the `E:\` paths and the references to `DEVELOPMENT_GUIDELINES.md`, `test_curves.cpp` and `TRANSPORT_DESIGN_COMPARISON.md`.
  - Merge the two CHANGELOGs.
  - Replace or remove Sphinx and Doxygen (DOC-9).
  - Document the Shared APIs, the threading contracts and the wire format (SHR-37). This becomes the D4 and D8 ADRs' public summary.

#### WP-D4: Keep the docs honest  ·  P2 · S · coding
- A CI link checker for the Markdown docs.
- A PR template with the checklist from §7, since the repo has none today.

#### WP-Q1: Low-severity cleanup batch  ·  P3 · M · coding (bundle into 2–3 PRs)
- **Findings:** SND-31, SND-33, TRF-33, SHR-28, SHR-29, CORE-25 (anything not already in WP-A7)
- **Approach:**
  - Dead code and stale paths.
  - Hot-path logging.
  - Export macros on public log categories.
  - Header-level `static` functions and IWYU fixes.
- Pick up only the items not already closed by an earlier WP. The code review agent confirms this against the coverage table in §8.

---

## 6. Suggested first two weeks (parallel lanes)

| Lane | Agent(s) | Items |
|---|---|---|
| Decisions | design | D1, D2, D3 (days 1–3), then D6, D7, D10, then D4, D5, D8, D9 |
| Core safety | coding A | WP-T1 → WP-S1 → WP-S2 |
| Sender and receiver correctness | coding B | WP-T2 (after D10) → WP-S3 (after D7) → WP-S4 |
| Transport safety | coding C | WP-S6 (TCP) → WP-S5 (after its design note) |
| Build and Fab | coding D | WP-F0 (design) → WP-F8 (CI truth) → WP-F3 → WP-F4 → WP-F2 (after D1) → WP-F1 (after D3) → WP-F11 |
| Review | review | Reviews every PR against §7. Keeps the §8 coverage table current. |

---

## 7. Code review checklist (for the review agent)

Apply every item to every PR. Items marked ⚑ are blocking.

1. ⚑ **Finding closure.** For each finding ID in the PR, re-read the finding and confirm the evidence no longer holds. Quote the new code.
2. ⚑ **Threads.** Every callback that can run off the game thread (audio render, capture, FFI, HTTP, socket worker) holds no raw `this`, `Owner&` or UObject pointer, and pins shared state safely. `Stop()` and destructors quiesce in-flight callbacks before freeing anything.
3. ⚑ **Game thread.** No new blocking call (connect, disconnect, join, `Wait`, sleep, synchronous HTTP, file I/O, device enumeration) on the game thread.
4. ⚑ **Untrusted input.** Every length, count, index, enum and float from the wire is range-checked before use, and there is a test with malformed input.
5. ⚑ **UE API verification.** New UE API usage cites the UE 5.7 source location or doc page in the PR description.
6. ⚑ **Tests.** The WP's Acceptance tests exist, fail without the fix (the PR states this), and pass with it. No `TestTrue(true)` placeholders and no network-dependent tests in the default filter.
7. ⚑ **Secrets.** Nothing token-like is persisted or logged.
8. ⚑ **Wire compatibility.** The schema is append-only. `o3ds_generated.h` is regenerated, not hand-edited. The version and CHANGELOG are updated per D8.
9. **Build.** Win64 Development and Shipping, a non-unity build, and the flag-combination builds pass. There are no new warnings.
10. **Scope.** No drive-by refactors. Behaviour changes and refactors go in separate PRs.
11. **Docs.** User-visible behaviour changes update the USER_GUIDE and CHANGELOG in the same PR.
12. **Duplication.** New code does not re-create something WP-A1 has made shared.

---

## 8. Finding coverage

Every finding ID in `docs/review/2026-09-plugin-review/` is assigned to exactly one primary WP below. When a WP closes, the review agent marks its findings closed in the finding files by adding `- Status: closed in #<PR>` under the finding.

| WP | Findings |
|---|---|
| WP-T1 | CORE-19, CORE-20 |
| WP-T2 | SHR-4, SHR-5, SHR-6, SND-36, RCV-2, RCV-31, TRF-34, TRF-35, UX-4, TRB-47 |
| WP-S1 | CORE-1, CORE-8, CORE-9, CORE-23, CORE-26 |
| WP-S2 | CORE-2, CORE-3, CORE-4, CORE-22, TRB-15, TRB-17, TRB-18, TRB-19, TRB-23 |
| WP-S3 | SND-1, SND-2, SND-3, SND-4, SND-5, SND-13, SND-14, SND-19, SND-20 |
| WP-S4 | RCV-4, RCV-5, RCV-7, RCV-10, RCV-14, RCV-34, TRB-37 |
| WP-S5 | TRF-1, TRF-10, TRF-12, TRF-40, TRB-10, TRB-11, TRB-12, TRB-30, TRB-35, RCV-1, SND-6, SND-7, SHR-10, SHR-15 |
| WP-S6 | TRB-1, TRB-2, TRB-3, TRB-4, TRB-5, TRB-6, TRB-8, TRB-9, TRB-13, TRB-14 |
| WP-S7 | TRF-2, TRF-3, TRF-4, TRF-5, TRF-15, TRF-16, TRF-19, TRF-23, TRF-24, TRF-25, TRF-31 |
| WP-S8 | TRF-8, TRF-9, TRF-11, TRF-13, TRF-20, TRF-29, TRF-37, TRF-39 |
| WP-S9 | SND-10, RCV-3, TRF-21, TRF-22, SHR-11, LIC-1 |
| WP-S10 | SHR-1, SHR-2, SHR-3, SHR-8, SHR-17, SHR-18, SHR-26, SHR-31, SHR-32, SND-16, SND-21, SND-28 |
| WP-S11 | TRB-33, TRB-34, TRB-36, TRB-39, TRB-40, TRB-42, TRB-43 |
| WP-F0 | (Fab checklist) |
| WP-F1 | FAB-2, FAB-6, CORE-21, HYG-2 |
| WP-F2 | FAB-3, FAB-8, SND-11, RCV-30, TRB-24, TRB-41, TRF-27, SHR-20, SHR-21, SHR-22, BUILD-1, BUILD-2, BUILD-4, BUILD-5 |
| WP-F3 | FAB-4, TRF-14, TRF-28, BUILD-3 |
| WP-F4 | FAB-5, FAB-9 |
| WP-F5 | FAB-1, FAB-10, FAB-11, SHR-23 |
| WP-F6 | HYG-1, HYG-3, HYG-4, SHR-27, CI-7 |
| WP-F7 | FAB-7, SND-34, TRB-45, TRB-46, SND-35 |
| WP-F8 | CI-1, CI-2, CI-3, CI-4, CI-5, CI-6, CI-8, CI-9 |
| WP-F9 | FAB-12, FAB-13, FAB-14, UX-5 |
| WP-F11 | (no owned findings; implements ADR 0002, see its related findings) |
| WP-A1 | SHR-9, SHR-12, SHR-13, SHR-14, SHR-16, SHR-24, SHR-35, SHR-36, SND-23, RCV-27, RCV-28, TRB-26, TRB-27, TRB-38, TRF-32, TRF-38 |
| WP-A2 | SND-8, SND-9, SND-12, SND-17, SND-18, SND-29, CORE-7, CORE-18, TRB-20 |
| WP-A3 | SND-22, RCV-29, SHR-38, RCV-11, RCV-12, RCV-13 |
| WP-A4 | CORE-5, CORE-6, CORE-10, CORE-11, CORE-12, CORE-13, CORE-14, CORE-15, CORE-16, CORE-29, SND-15, SHR-7, SHR-30, SHR-33, RCV-8, RCV-9 |
| WP-A5 | TRF-6, TRF-7, TRF-17, TRF-18, TRF-26, TRB-7, RCV-19 |
| WP-A6 | SHR-19, SHR-25, SHR-34, SND-32, RCV-25, RCV-26, RCV-33 |
| WP-A7 | CORE-17, CORE-24, CORE-25, CORE-27, CORE-28, CORE-30 |
| WP-CTL | (no owned findings; implements ADR 0011) |
| WP-U1 | UX-2, RCV-18 |
| WP-U2 | SND-26, UX-3 |
| WP-U3 | SND-24, SND-25, SND-27, SND-30, RCV-15, RCV-16, RCV-17, TRF-30 |
| WP-U4 | RCV-6, RCV-20, RCV-21, RCV-22, RCV-23, RCV-24 |
| WP-U5 | UX-1 |
| WP-U6 | TRB-16, TRB-21, TRB-22, TRB-25, TRB-28, TRB-29, TRB-31, TRB-32 |
| WP-D1 | DOC-3 |
| WP-D2 | DOC-1, DOC-2, DOC-4, DOC-5, DOC-6, DOC-10, TRF-36, TRB-44, SND-37, RCV-32 |
| WP-D3 | DOC-7, DOC-8, DOC-9, SHR-37 |
| WP-Q1 | SND-31, SND-33, TRF-33, SHR-28, SHR-29 |

Some findings are cited in more than one WP's text, for example as a "fold into A1" follow-up or as context. The table above names the WP that **owns** closing each one.
