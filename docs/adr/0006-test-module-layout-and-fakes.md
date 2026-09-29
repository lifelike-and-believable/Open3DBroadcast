# 0006: Test module layout and fakes

- **Status:** Proposed (pending maintainer sign-off)
- **Date:** 2026-09-29
- **Plan decision:** D10 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0001](0001-platform-scope-first-fab-release.md) (module keys), [ADR 0002](0002-webrtc-and-moq-in-first-fab-release.md) (add-on plugin, exclusion manifest), [ADR 0003](0003-core-library-delivery-to-plugin.md) (`Open3DStreamCore`), [ADR 0004](0004-credentials-and-secret-transport-options.md), [ADR 0005](0005-wire-resync-and-loss-contract.md); feeds WP-T1, WP-T2, WP-F8, WP-F11 and every WP's Acceptance

**Recommendation in one line:** add an `Open3DBroadcastTests` module (Type `Editor`, Win64) to the main plugin and an `Open3DBroadcastWebRTCTests` module to the add-on. Move every automation test out of the Runtime modules into them. Test transports through a registry-driven conformance suite, a recording fake transport, per-instance FFI function tables for LiveKit and MoQ, and socket-free parsers. Name every test `Open3DBroadcast.<Area>.…`. Register internet tests only when an env var is set. Run all other UE tests in PR CI; wire logic stays in the core and runs under CTest on every PR.

## Context

**Where tests live today.** Fourteen test files sit in `Private/Tests/` of the Runtime modules they test (Shared 2, Sender 1, Receiver 1, Loopback 1, Sockets 1, NNG 1, WebRTC 2, MoQ 5). There are about 85 tests, all `EditorContext | EngineFilter` except seven MoQ relay tests on `ProductFilter` (`Plugin/Source/Open3DTransportMoQ/Private/Tests/MoQCloudflareRelayTests.cpp`).
- **Layering (SHR-4).** `GenericTransportTests.cpp:5` includes `O3DSenderInterface.h` from Open3DShared, which Sender depends on. This works only through `PublicIncludePathModuleNames.Add("Open3DSender")` behind a sender-enabled flag (`Plugin/Source/Open3DShared/Open3DShared.Build.cs:62-66`), and Shared links the core "for tests" (`:16`, `:24`; SHR-20).
- **Placeholders (SHR-5).** `Open3DBroadcast.Generic.Concurrency.MultipleSends` and two siblings assert only `TestTrue(..., true)` (`GenericTransportTests.cpp:122-140`, `:187-213`, `:315-334`).
- **A broken test (RCV-2)** asserts a subject-name fallback that `FinalizeAudioMeta` no longer has (`Plugin/Source/Open3DReceiver/Private/Tests/O3DRemoteAudioComponentTests.cpp:133-173`). A MoQ test can never pass (TRF-34, `MoQTrackNamespaceTests.cpp:262-300`).
- **Internet dependence (UX-4, TRF-34).** `Cloudflare.Basic` is in EngineFilter and connects to a hard-coded public relay unless `O3D_MOQ_RELAY_URL` is set (`MoQCloudflareRelayTests.cpp:30`, `:40-45`).
- **Inconsistent names and guards.** Prefixes include `Open3DBroadcast.O3DSender.*`, `Open3DBroadcast.Open3DSender.*`, `Open3DBroadcast.O3DShared.*` and `Open3DBroadcast.O3DReceiver.*`. Guards mix `WITH_AUTOMATION_TESTS` (Sockets) and `WITH_DEV_AUTOMATION_TESTS` (grep of all test files).
- **What tests can reach.** The registries create instances by name, and registering an existing name overrides it (`Plugin/Source/Open3DSender/Public/O3DSenderRegistry.h:9-18`, `Open3DReceiver/Public/O3DReceiverRegistry.h:9-18`). MoQ calls its FFI directly by symbol (23 distinct `moq_*` functions, linked through an import library and `PublicDelayLoadDLLs`, `Open3DTransportMoQ.Build.cs:65`, `:87`); `FMoQFfiSupport` only loads and validates it (`Private/Shared/MoQFfiSupport.cpp:198-200`). A test-only friend already reaches into `FMoQSessionWrapper` callbacks (`Private/Shared/MoQSessionWrapper.h:81-104`). WebRTC calls about 16 `lk_*` functions directly (delay-loaded, `Open3DTransportWebRTC.Build.cs:57`). The TCP receiver's framing parser reads from the socket inside the same function (`Plugin/Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp:300-400`). UDP reassembly is the core `UdpMapper` (`SocketsUdpReceiver.cpp:32`), already covered by CTest (`test/udp_fragment_tests.cpp`).
- **Core tests.** One executable, one CTest entry (`test/CMakeLists.txt:8-29`), with a self-registering `O3DS_TEST` macro (`test/test_framework.h`). It runs on every PR with ASan/UBSan on Linux (`.github/workflows/core-tests.yml:13-89`).
- **UE CI.** PR CI builds the plugin on a self-hosted Windows runner, skips drafts, and runs no tests (`.github/workflows/open3dbroadcast-plugin-ci.yml:53-54`, CI-2). The test workflow compares a boolean input to `'true'`, so tests never run (`open3dbroadcast-plugin-test.yml:138`). The nightly runs `Open3DBroadcast.*` against binaries it did not build (`open3dbroadcast-plugin-nightly.yml:107-115`, CI-3). `Run-AutomationTests.ps1` passes if the editor exits 0, even without a report (`Build/Scripts/Run-AutomationTests.ps1:67-78`).
- **Gauntlet.** `Tests/Gauntlet/Open3DStreamTests.json:8-10` runs `Open3DStream.*`, which matches no test (every test starts with `Open3DBroadcast.`). No workflow calls `Run-Gauntlet.ps1` (grep of `.github/workflows`), although `Tests/Gauntlet/README.md:56-67` says the nightly does.

## Decision drivers

1. Tests never ship in Runtime modules, and Fab users don't see developer tests in their editor.
2. Tests exercise behaviour through seams, with no internet by default and no wall-clock sleeps.
3. Core-first: whatever can run under CTest on Linux does (resilient-streaming §0.1).
4. The WebRTC add-on (WP-F11) tests itself with the same harness, through public API.
5. The PR signal is trustworthy: a failing test turns the PR red.

## Options considered

### Module type
- **`DeveloperTool`:** loads in any target where developer tools are built, which includes non-Shipping game targets (external sources in References, **needs-verification** for 5.7, Q1). Tests could end up in a Development packaged game. Rejected.
- **`UncookedOnly`:** loads only in uncooked editor and program targets. It suits tests, but an UncookedOnly module depending on Editor-type modules (the D9 editor module, for UI stripping tests from ADR 0004) is **needs-verification** (Q1).
- **`Editor` (chosen):** loads only in editor targets, can depend on the D9 editor module and `LiveLink` editor types, and matches how every existing test runs (`EditorContext`).

### Where the tests live
- **One test module per plugin (chosen).** A single dependency root, one place for fakes and the conformance harness.
- One test module per runtime module: eight or more modules for about 85 tests.
- Keep tests in place and fix only the includes: fails driver 1.

### Fake FFI seam
- **F1. Global function-pointer table swapped by tests:** simple, but global state that parallel tests and live instances would share.
- **F2. Per-instance function table injected at construction (chosen).** A `struct FMoQFfiApi` or `FLkFfiApi` with one pointer per used symbol. The production factory passes the table built from the linked symbols; a testing factory passes a fake. There is no global mutation.
- **F3. C++ interface with virtual methods:** equivalent to F2 with more boilerplate around C callbacks and `user_data`.

### Socket seam
- **S1. Fake `FSocket`:** broad, and couples tests to UE socket internals.
- **S2. Extract parsers as socket-free units, and use real 127.0.0.1 sockets for integration (chosen).**

## Decision

1. **Modules.**
   - `Plugin/Source/Open3DBroadcastTests/`, in `Open3DBroadcast.uplugin` with `"Type": "Editor"` and `"PlatformAllowList": ["Win64"]`. Per ADR 0001 §1 an Editor module needs no target list.
   - Private dependencies: `Core`, `CoreUObject`, `Engine`, `Open3DStreamCore` (ADR 0003 §5), `Open3DShared`, `Open3DSender`, `Open3DReceiver`, the transport modules, `LiveLinkInterface`, and the D9 editor module once it exists.
   - It exports its harness (`OPEN3DBROADCASTTESTS_API`: the conformance registry, fake transport and fixtures) so the add-on can use it.
   - The add-on gets `Open3DBroadcastWebRTCTests` (Type `Editor`, Win64), depending on `Open3DBroadcastTests` and `Open3DTransportWebRTC`.
   - **Fab:** `Open3DBroadcastTests` is listed in the ADR 0002 exclusion manifest (`Build/Fab/exclude-modules.txt`), so the Fab package neither contains nor compiles it. GitHub builds keep it.
   - Every test file is wrapped in `#if WITH_DEV_AUTOMATION_TESTS`.
2. **Move everything.** All 14 files leave `Source/*/Private/Tests/` for `Open3DBroadcastTests/Private/<Area>/`, and the WebRTC files go to the add-on test module.
   - Remove `PublicIncludePathModuleNames.Add("Open3DSender")` and the "for tests" core linkage from Shared (SHR-4, SHR-20).
   - White-box access goes through a narrow per-module `Public/Testing/<Module>Testing.h`, guarded by `WITH_DEV_AUTOMATION_TESTS` and exported. `FMoQSessionWrapperTestHelper` moves there. UE 5.x `Internal/` include folders would be tidier if they work across modules in one plugin (**needs-verification**, Q2).
   - Logic with no UE dependency moves to `src/o3ds` instead and is tested by CTest.
3. **Fakes.**
   - **Fake transport.** `FO3DFakeSender` records `SendSerialized` payloads, supports scripted backpressure and counts calls. `FO3DFakeReceiver` lets a test inject bytes into the consumer on a chosen thread. Tests register them under a unique name (`O3DTestFake_<Guid>`) for the duration of one test and unregister after, so they never override or appear alongside real transports.
   - **Fake FFI (F2).**
     - `Open3DTransportMoQ/Private/Shared/MoQFfiApi.h` defines `FMoQFfiApi` with the 23 used symbols, and every call site goes through the instance's table.
     - `Public/Testing/MoQTesting.h` exposes `CreateSenderForTest(const FMoQFfiApi&)` and `CreateReceiverForTest(...)`.
     - WebRTC does the same with `FLkFfiApi` in the add-on.
     - The fake records callbacks and `user_data`, so tests can fire connection-state, data and audio callbacks deterministically from any thread. This is what the WP-S5 lifetime tests need.
   - **Sockets (S2).**
     - Extract `FO3DTcpFrameAssembler` (append bytes, pop complete frames, resync on bad magic) out of `FO3DSocketsTcpReceiver::ReadFramed`. It is unit-tested with coalesced, split and garbage input (TRB-1, TRB-47).
     - The unified-message and audio parsers in Shared get direct unit tests (SHR-6).
     - Integration tests use real sockets on `127.0.0.1` with ephemeral ports (port 0), so they need no internet and no fixed ports.
   - **LiveLink.** Receiver tests use a recording `ILiveLinkClient` fake if UE 5.7 lets a test implement the interface (**needs-verification**, Q3); otherwise the real client in an editor test.
4. **Conformance suite.** `IMPLEMENT_COMPLEX_AUTOMATION_TEST` whose `GetTests` enumerates `GetRegisteredSenders()` and `GetRegisteredReceivers()` and emits one test per transport and case: `Open3DBroadcast.Conformance.<Transport>.<Case>`.
   - Each transport supplies an offline **profile** through `O3DTests::RegisterConformanceProfile(Transport, Profile)`. A profile builds a local config (loopback sockets or a fake FFI table) and says which cases apply (audio, `SendSerialized`, `GetDeliveryGuarantee` from ADR 0005).
   - Profiles for Loopback, TCP, UDP, NNG and MoQ live in `Open3DBroadcastTests`; the WebRTC profile lives in the add-on test module.
   - A registered transport with no profile produces one failing test, `Open3DBroadcast.Conformance.<Transport>.HasProfile`, so a new transport cannot silently escape.
   - Cases:
     - lifecycle idempotency (Initialize, Start, Stop, Stop; Start after Stop);
     - `SendSerialized` before Start returns false without crashing;
     - backpressure increments `Stats.DroppedFrames` and never blocks;
     - concurrent `SendSerialized` from four threads;
     - `GetStats` counters are monotonic;
     - round trip of recorded frames (from `src/o3ds/capture`) through sender, receiver and consumer is byte-exact where the transport is reliable;
     - destroying the sender while callbacks are in flight is safe (fake FFI only).
5. **Naming and filters.**
   - Prefix `Open3DBroadcast.<Area>.<Unit>.<Case>`. Area is one of `Core` (UE-compiled core round trips, ADR 0003), `Shared`, `Sender`, `Receiver`, `Transport.<Name>`, `Conformance.<Name>` or `Network.<Name>`.
   - Flags: pure-logic tests use the application-wide context and `EngineFilter`; world and editor tests use `EditorContext`. The UE 5.7 spelling of these flags (plain enum or `EAutomationTestFlags_ApplicationContextMask`) is **needs-verification** (Q4).
   - The filter everywhere is the plain prefix `Open3DBroadcast`, not `Open3DBroadcast.*` (CI-3).
   - Fix the Gauntlet filter by **retiring** `Tests/Gauntlet/` and `Run-Gauntlet.ps1`. Nothing calls them, `Run-AutomationTests.ps1` covers the same ground,. The maintainer confirmed retirement (Q5). The README and `copilot-instructions.md:34`, `:152` are updated.
6. **Network-dependent tests.** Tests that need the internet or an external server live under `Open3DBroadcast.Network.*`, are complex tests, and **register no instances unless `O3DB_NETWORK_TESTS=1`**. They are therefore absent from every default run and from the Session Frontend. Endpoints come only from env vars (`O3D_MOQ_RELAY_URL`, `O3DB_LIVEKIT_URL`); the hard-coded public relay default is removed (`MoQCloudflareRelayTests.cpp:30`). A missing endpoint with the flag set is a test failure, not a pass.
7. **Core and UE split (core-first).**
   - **CTest (Linux, every PR; MSVC job per WP-T1):** parsers and fuzz targets, UDP reassembly, `ReorderGate`, `StreamWriter` and the ADR 0005 receiver state machine, residual and quantization loss and mid-join tests, capture and replay.
   - **UE tests:** glue only.
     - The sender serializer drives `StreamWriter` correctly; its bytes are parsed by the real core.
     - The receiver pushes the right LiveLink static and frame data.
     - Registry, customization and secret routing (ADR 0004 save tests).
     - Transport conformance.
     - Component lifecycle.

     A UE test never re-tests core arithmetic.
   - Core tests register one CTest entry per `O3DS_TEST` (CORE-19), so failures are reported individually.
8. **CI.**
   - **PR, non-draft, self-hosted Windows (WP-F8):**
     - build `ProjectSandboxEditor Win64 Development`;
     - run `Run-AutomationTests.ps1 -TestFilter Open3DBroadcast`, which fails on `failed > 0` or a missing `index.json`;
     - build and test the add-on with its test module against the same checkout;
     - target under 15 minutes of test time.
   - **PR, every commit including drafts, Linux:** `core-tests.yml` as today, plus WP-T1's fuzz and MSVC jobs.
   - **Nightly:** everything above, plus `O3DB_NETWORK_TESTS=1` with endpoints from repository secrets, a Development and a Shipping **game** build of `ProjectSandbox` to confirm no test module is loaded (Q1), and 10 minutes of core fuzzing.
9. **Test hygiene rules** (review checklist, plan §7): no `FPlatformProcess::Sleep` in tests (poll with a timeout on a fake clock or latent commands); no fixed ports; no internet outside `Network.*`; no `TestTrue(..., true)`; each test names the finding or WP it covers in a comment.

## Consequences

- **Easier:** every M1 WP has a place for real tests and a fake to drive them; the WP-S5 lifetime races become reproducible with scripted FFI callbacks; the add-on proves compatibility with the same suite (WP-F11 acceptance).
- **Harder:** transports route FFI calls through a table (a mechanical change to about 40 call sites), and some private helpers get a `Testing` header. The test module depends on every transport, so it builds last.
- **Constrains:** new transports must ship a conformance profile (the `HasProfile` test enforces it). WP-A1's shared base classes keep the FFI-table injection point.
- **RCV-2:** decided (Q6): `FinalizeAudioMeta` falls back to the stream label. The test is rewritten to assert that when it moves, and `LastObservedSubjectName` is deleted.

## Implementation outline

1. **WP-T2a:** create `Open3DBroadcastTests` (Build.cs, module cpp, `.uplugin` entry with the ADR 0001 keys), move the 12 non-WebRTC test files, delete the placeholders (SHR-5), fix names and guards, remove Shared's test-only include path and libs. Add `Open3DBroadcastTests` to `Build/Fab/exclude-modules.txt` (WP-F8).
2. **WP-T2b:** fake sender and receiver, fixtures, conformance harness and profiles for Loopback, TCP, UDP and NNG.
3. **WP-T2c:** `FMoQFfiApi`, per-instance injection, `MoQTesting.h`, MoQ profile; fix or delete `BackpressureByteLimit`; move the relay tests to `Network.MoQ.*` behind the env var (UX-4, TRF-34).
4. **WP-T2d:** `FO3DTcpFrameAssembler` extraction and tests (with WP-S6); Shared parser tests (SHR-6).
5. **WP-F11 / WP-T2e:** `FLkFfiApi`, `Open3DBroadcastWebRTCTests`, WebRTC profile, and the moved WebRTC tests.
6. **WP-F8:** PR test step, nightly editor build before tests, runner report parsing, retire Gauntlet.
7. **WP-T1:** per-test CTest registration and the core items in Decision §7.

## Verification / acceptance

- `grep -r "Private/Tests" Plugin/Source` finds nothing; no Runtime module Build.cs mentions tests.
- The Fab zip (ADR 0002 job) contains no `Open3DBroadcastTests`, and a Shipping game build of `ProjectSandbox` loads no test module (nightly).
- The default `Open3DBroadcast` run passes with the network cable unplugged, and `Open3DBroadcast.Network.*` lists no tests without `O3DB_NETWORK_TESTS=1`.
- A deliberately broken assertion turns the PR job red; a missing `index.json` also fails it.
- Zero `TestTrue(..., true)` remain (grep in CI).
- The conformance suite lists every registered transport, including WebRTC when the add-on is enabled.

## Open questions for the maintainer

1. **needs-verification:** in UE 5.7, is an `Editor` module excluded from Game, Client and packaged builds in every configuration, and can an `UncookedOnly` module depend on an `Editor` module? Is `DeveloperTool` built into Development game targets by default (`bBuildDeveloperTools`)?
2. **needs-verification:** can a module's `Internal/` headers be included by another module in the same plugin in UE 5.7? If yes, the `Public/Testing/` headers move there.
3. **needs-verification:** can a test implement `ILiveLinkClient` in UE 5.7 (pure-virtual surface), or must receiver tests use the real client?
4. **needs-verification:** the UE 5.7 spelling of `EAutomationTestFlags` context and filter values.
5. ~~Keep or retire Gauntlet?~~ **Answered 2026-09-29:** retire it. `Tests/Gauntlet/` and `Build/Scripts/Run-Gauntlet.ps1` are deleted in WP-F8, and the docs that mention them are updated. Whether `RunGauntlet -Config=` reads the JSON no longer matters.
6. ~~**RCV-2:** last observed subject name or stream label?~~ **Answered 2026-09-29:** the stream label. The test is updated to assert the stream-label fallback, and the unused `LastObservedSubjectName` field is deleted.
7. Is a 15-minute test budget on the self-hosted runner acceptable for every non-draft PR push?

## References

- Findings: SHR-4, SHR-5, SHR-6, UX-4, TRF-34, TRF-35, TRB-47, SND-36, RCV-2, RCV-31, CORE-19; related CI-2, CI-3, SHR-20, TRB-1.
- Files: every `Plugin/Source/*/Private/Tests/*.cpp`; `Plugin/Source/Open3DTransportWebRTC/Tests/`; `Plugin/Source/Open3DShared/Open3DShared.Build.cs`; `Plugin/Source/Open3DTransportMoQ/Private/Shared/MoQFfiSupport.cpp`, `MoQSessionWrapper.h`; `Plugin/Source/Open3DTransportSockets/Private/Receiver/SocketsTcpReceiver.cpp`; `Plugin/Source/Open3DSender/Public/O3DSenderRegistry.h`; `test/CMakeLists.txt`, `test/test_framework.h`; `Tests/Gauntlet/`; `Build/Scripts/Run-AutomationTests.ps1`, `Run-Gauntlet.ps1`; `.github/workflows/open3dbroadcast-plugin-ci.yml`, `open3dbroadcast-plugin-test.yml`, `open3dbroadcast-plugin-nightly.yml`, `core-tests.yml`.
- External (retrieved 2026-09-29, search snippets only, not Epic's own module-type page): ibbles, "LearningUnrealEngine/ModuleType.md", https://github.com/ibbles/LearningUnrealEngine/blob/master/ModuleType.md ; itsBaffled, "Creating A Plugin Or Module", https://itsbaffled.github.io/posts/UE/Creating-A-Plugin-Or-Module ; Epic forums, "'Developer' module type has been deprecated in 4.24", https://forums.unrealengine.com/t/developer-module-type-has-been-deprecated-in-4-24/487730 .
