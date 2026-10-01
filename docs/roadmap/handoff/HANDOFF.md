# Open3DBroadcast: handoff to the next session

Written 2026-09-30 by the cloud session that drove M1, M2 and the start of M3 of the plugin hardening roadmap. Updated 2026-10-01 with the control-channel work (ADR 0011, CTL-1..5) that landed afterwards; ADR 0011 accepted and WP-CTL added to the roadmap the same day. Read this first, then the files it points to.

## 1. Where things stand

The plan is `docs/roadmap/plugin-hardening-and-fab-readiness.md`. Design decisions are in `docs/adr/0001`–`0011`, all **Accepted**; their open questions were accepted with the recommended defaults. Work is organised as work packages (WPs), one PR each (or a PR series for large ones), squash-merged into `develop`.

| Milestone | Status |
|---|---|
| M0 Decisions (ADRs 0001–0010) | Done (#261–#263) |
| M1 Safety and correctness: WP-S1..S11, WP-T1, WP-T2 | Done (#264–#279) |
| M2 Fab-buildable package: WP-F1..F4, F6..F9, F11 | Done (#274–#286). **F0 and F5 wait on the maintainer** (see §5) |
| M3 Architecture: WP-A1..A7 | **In progress: WP-A1 PR 1 merged (#289); PR 2 is next** (see §2) |
| M4 Usability and docs: WP-U1..U6, WP-D1..D4, WP-Q1 | Not started |
| M5 Fab submission: WP-F10 | Not started; needs F0, F5 and the listing details in §5 |
| WP-CTL control channel (D11, ADR 0011) | CTL-1..5 done (#290–#295); **CTL-6 (WebRTC add-on) and CTL-7 (docs) remain** (see §2b) |

Recent merges on `develop`: F7 editor split (#285, dc686e0), F11 WebRTC add-on (#286, 5c9af51), WP-A1 PR 1 transport registry (#289, d365c34), NNG unity-build fix and README rewrite (#287), ADR 0011 (#290), CTL-1..5 (#291–#295, last de02b8d).

## 2. The work in flight: WP-A1 (transport core consolidation)

Design: `docs/adr/0007-transport-abstraction-and-registry.md`, section "Implementation outline". WP-A1 is a series of PRs; each must keep every transport working and the conformance suite green.

1. **Interfaces and one registry** (SHR-12, SND-23, RCV-27, RCV-28, SHR-24). **Done: #289, merged as d365c34.**
   - `IOpen3DSender`, `IOpen3DReceiver`, their audio sinks, `ISerializedFrameConsumer` and `FO3DTransportConfig` now live in `Open3DShared/Public/Transport/`, exported by Open3DShared.
   - `FO3DTransportRegistry` (`Transport/O3DTransportRegistry.h`): each transport registers one immutable `FO3DTransportDescriptor` and holds a move-only `FO3DTransportRegistration`. `Find` returns `TSharedPtr<const FO3DTransportDescriptor>`; `GetNames(Role)` feeds the pickers from the same entries `CreateSender`/`CreateReceiver` use; `OnTransportsChanged` fires on every change. Duplicate names and API-version mismatches are refused.
   - Old Sender/Receiver headers are deprecated forwarding shims (comments only, no `UE_DEPRECATED`), removed next minor release with an API-version bump. Register/unregister are documented as game-thread only but not yet `check`ed; step 2 can add that.
   - Tests: `Open3DBroadcast.Shared.TransportRegistry.*` (7 cases).
   - **Start here next: step 2.**
2. **Lifetime** (SHR-13, TRF-14): live-instance lists, `OnTransportUnregistering`, drain before unload, `FO3DFfiLibrary`; WebRTC and MoQ modules adopt it.
3. **Results, state, capabilities** (SHR-14): `FO3DTransportResult`, `EO3DSendResult`, `FO3DSendPayload`, connection state, `FO3DTransportCapabilities`, `SendSerialized` pure virtual. The interface version already exists (`Open3DShared/Public/Transport/O3DTransportApiVersion.h`); CTL-2 raised it from 1 to **2**. The result type should cover `SendControl` too, and `SupportsControl()` belongs in the capability query.
4. **Shared building blocks, one transport per PR**, in order Loopback, TCP, UDP, NNG, MoQ, then WebRTC (in the add-on). Each PR deletes that transport's own queue, demux, sink and option-parsing copies and drops its Build.cs dependency on Open3DSender/Open3DReceiver. Control landed before this step, so each migration must also carry that transport's control path: the shared send queue needs the "control" item type that is never dropped for mocap (ADR 0007 item 7), and the shared demux should absorb `TryGetControlPayload` / `O3DTransport::DeliverControlEnvelope` (CTL-2/3).
5. **Typed config and consumer API** (SHR-36, TRB-27, SHR-16, TRF-38): removes the LiveKit string fields from `FO3DTransportConfig`, deletes `Send(SubjectList)`.
6. Next minor release: delete the shims and bump `O3D_TRANSPORT_API_VERSION` (to 3, or later if other steps bump it first).

WP-A1 acceptance (roadmap): conformance suite green after each migration, net transport LOC goes down, no transport keeps its own queue/demux/sink. ADR 0007 "Verification / acceptance" lists the extra test cases.

After WP-A1, the M3 order in the roadmap is WP-A2 (async sender, ADR 0008) → WP-A3 (god classes); WP-A4 (protocol, ADR 0009), WP-A5, WP-A6, WP-A7 can go in parallel where files don't overlap.

## 2b. Control channel (WP-CTL, ADR 0011)

Done outside the cloud session, after the first version of this doc. Design: `docs/adr/0011-control-channel.md` ("Implementation outline" and "Verification / acceptance"). A one-way sender-to-receivers stream of **values** (keyed, last-writer-wins, periodic snapshots) and **events** (fire-once, de-duplicated, TTL, redundant copies), as envelope kind `Control = 2` with its own FlatBuffers root (`src/o3ds_control.fbs`, `"O3DC"`).

| Step | Status |
|---|---|
| CTL-1 core: schema, codec, publisher/receiver state machines, CTest + fuzz | Done (#291) |
| CTL-2 envelope kind, `SendControl`/`SetControlSink` on the interfaces, `FO3DControlBus`; API version 1 → 2 | Done (#292) |
| CTL-3 TCP, UDP, NNG, Loopback | Done (#293) |
| CTL-4 sender component API, receiver `FControlSink` with mocap alignment, `UO3DControlSettings`, `UO3DRemoteControlComponent` | Done (#294) |
| CTL-5 MoQ (`control/<session>` track) | Done (#295). The live-relay test case is not written (needs a relay) |
| **CTL-6 WebRTC add-on**: `__o3d.ctl` send path and receive classification, tests, manual test step | **Not started** |
| **CTL-7 docs**: USER_GUIDE Control section, transport comparison row, CHANGELOG protocol entry | **Not started** |

Open items:
- ADR 0011 was accepted on 2026-10-01 (open questions with their defaults), and WP-CTL is in the roadmap as decision D11 with its own work package section.
- Until CTL-6 lands, the WebRTC transport reports no control support; `SupportsControl()` gates the conformance cases, so CI stays green.
- The packaged Shipping test ADR 0011 asks of CTL-4 (control enabled in a Shipping build through the project setting and the runtime call) is not in CI yet; the nightly Shipping build (`Build-ShippingGame.ps1`) is the natural place for it.
- New-peer snapshots wait for ADR 0005 (vi); until then recovery is bounded by the snapshot interval.
- CTL and WP-A1 overlap: see the notes on steps 3 and 4 in §2.

## 3. Repository map (what changed during M1/M2)

- Main plugin: `ProjectSandbox/Plugins/Open3DBroadcast`. Modules: `Open3DShared`, `Open3DSender`, `Open3DReceiver`, the transports (`Open3DTransportSockets`, NNG, MoQ, Loopback), `Open3DStreamCore`, `Open3DBroadcastEditor` (editor-only UI, ADR 0010), `Open3DBroadcastTests` (editor-only, ADR 0006).
- WebRTC add-on: `ProjectSandbox/Plugins/Open3DBroadcastWebRTC` (WP-F11, ADR 0002). Depends on Open3DBroadcast; checks the transport API version at startup. Publishing it waits on counsel question L1; the release workflow attaches it only when the repo variable `O3D_PUBLISH_WEBRTC_ADDON` is `true`.
- The o3ds core (`src/o3ds`) is compiled inside the plugin from a generated copy in `Source/ThirdParty/Open3DStreamCore` (ADR 0003). After touching `src/o3ds`, `src/o3ds_generated.h` or the flatbuffers/crccpp pins: run `python3 Build/Scripts/sync_o3ds_core.py` and commit the result. Never edit the copy by hand.
- Control channel: core in `src/o3ds/control.{h,cpp}` and `src/o3ds_control.fbs` (the second generated header is in the core manifest, so `sync_o3ds_core.py` mirrors it); UE types in `Open3DShared/Public/O3DControl*.h`; receiver-side `O3DControlSettings.h` and `O3DRemoteControlComponent.h` in `Open3DReceiver/Public`.
- Rider IDE settings are committed under `.idea/`.
- Rules for agents: `AGENTS.md` → `.github/copilot-instructions.md` (authoritative). `Build/README.md` documents every build script.

## 4. How to work in this repo

### Conventions
- Branch from `develop`; one WP (or one WP-A1 step) per PR; squash-merge. Before merging a later PR, merge `develop` into it and let CI re-run.
- Commit/PR titles start with the WP id and list finding ids, e.g. `WP-F7: editor module split (ADR 0010; FAB-7, SND-34)`.
- New source files start with `// Copyright Lifelike & Believable. All Rights Reserved.` and a blank line. Files that already carry `// Copyright (c) Open3DStream Contributors` keep it (maintainer decision).
- Every test name starts with `Open3DBroadcast.` so the CI filter finds it. Network tests register only with `O3DB_NETWORK_TESTS=1`.
- Update `CHANGELOG.md` and the relevant docs in the same PR.

### Local checks (run before every push)
```
python3 Build/Scripts/fab-package.py --out-dir <scratch>     # Fab tree/package checks
python3 Build/Scripts/check-copyright-headers.py
python3 Build/Scripts/check-runtime-editor-deps.py
bash    Build/Scripts/check-no-video-codecs.sh                # main plugin must pass; --addon fails by design
python3 Build/Scripts/sync_o3ds_core.py --check               # needs submodules initialised
```
With UE 5.7 installed locally you can also run the real build and tests: `Build/Scripts/Build-Plugin.ps1`, `Build-WebRTCAddOn.ps1`, `Run-AutomationTests.ps1 -TestFilter Open3DBroadcast` (usage in `Build/README.md`). The cloud session had no UE, so every C++ change was first compiled by CI; a local build before pushing will save cycles.

### CI
- `open3dbroadcast-plugin-ci.yml` (PRs): GitHub-hosted jobs (path filter, Fab source zip, copyright headers, runtime-editor deps) plus **one self-hosted Windows UE job**:
  1. BuildPlugin;
  2. automation tests with the main plugin only;
  3. build the WebRTC add-on against that package;
  4. automation tests with both plugins;
  5. a strict build (non-unity, no PCH, warnings as errors);
  6. BuildPlugin on the Fab zip.
- **Draft PRs skip the UE job**; mark the PR ready to get it.
- `core-tests.yml`: Linux ASan/UBSan, MSVC, libFuzzer, warning ratchet (baseline 8), core mirror sync check.
- Nightly (`open3dbroadcast-plugin-nightly.yml`): flag-combination builds, Linux exclusion check, Shipping game build. **These nightly additions (WP-F2, WP-F7, WP-F11) had not yet reported a run at handoff; check the first results.**
- When a test fails, `Run-AutomationTests.ps1` prints the last 150 lines of `Automation.log` into the job log.
- Known runner infrastructure failures (re-run once, only if the job died before any test ran): Windows Application Control blocking a UBT rules DLL (`0x800711C7`), and an occasional editor exit at startup with no report. A second failure is real.

### UE 5.7 compile pitfalls hit in this repo
1. Friend declarations must be unconditional (not inside `#if WITH_...`).
2. `*_API` classes with non-copyable members need `= delete` copy operations.
3. No `MakeShared` with a forward-declared type.
4. `NewObject<T>(GetTransientPackage())` needs `UObject/Package.h`.
5. Include `Widgets/Input/SComboBox.h`; never forward-declare `SComboBox<...>`.
6. Include what you use: the strict build has no PCH and no unity.
7. Anything tests or another module/plugin uses must be `*_API`-exported in a Public header.
8. `TStrongObjectPtr<T>` members need the complete type in the header.
9. `TSlateDelegates<int32>::FOnValueCommitted` does not exist in 5.7; use the SLATE_EVENT shorthand `.OnValueCommitted(this, &Method, Payload)`.
10. Windows PowerShell 5.1 ignores `-Include` together with `-LiteralPath`; filter with `Where-Object`.
11. Helpers in anonymous namespaces need names unique within their module: unity builds merge a module's .cpp files, and CI's strict build (non-unity) will not catch a collision. A local `ProjectSandboxEditor` build does (see #287, `GetPipeContextRegistry` in NNG).

## 5. Waiting on the maintainer

- **WP-F0:** check the live Fab technical requirements page against ADR 0001/0002 assumptions.
- **WP-F5:** counsel questions L1–L5 (ADR 0002). L1 gates publishing the WebRTC add-on before the codec-free `livekit_ffi` rebuild.
- **Listing details:** `CreatedByURL` (still `https://open3dstream.com/` in both `.uplugin` files); real `SupportURL` and `DocsURL` (stand-ins today); `MarketplaceURL` once the Fab listing exists; the WebRTC add-on download link (a marked placeholder in the user guides). Decided 2026-10-01: `CreatedBy` is "Lifelike & Believable and Open3DStream Contributors" in both descriptors.
- **Naming:** decided 2026-10-01: editor categories (`Category = "Open3DBroadcast|..."`, the Sender details customization) and `ClassGroup = (Open3DBroadcast)` use Open3DBroadcast. Still open: the LiveLink source name (factory display name "Open3DStream Receiver" and tooltip in `O3DReceiverSourceFactory.cpp`, source type "Open3D Stream" in `O3DReceiverSource.cpp`), and whether anything saved in assets, presets or config (property names, config section names, ini keys) should ever be renamed. None of those contain "Open3DStream" today; renaming them would need redirects.
- **ADR 0002 Q8(b):** confirm on a real Fab install that the installed plugin ships the import libraries (`UnrealEditor-Open3DShared.lib` etc.) a source build of the add-on needs.
- **Red-gate branches:** five throwaway branches proving each CI gate fails when it should (unused local with `-FailOnWarnings`, failing test, include only the PCH provided, dev notes in `Private/`, WebRTC in the Fab zip). They were never pushed. The patches are in `docs/roadmap/handoff/redgate-patches/`. To use one: branch from `develop`, `git am` the patch, open a **draft** PR, mark it ready, confirm the named check goes red, then close the PR and delete the branch. They predate F1/F7/F11, so a patch may need a small path fix.

## 6. Decisions already made (don't reopen)

- Rights holder: Lifelike & Believable. Open3DStream Contributors notices stay on files that had them.
- FriendlyName "Open3DBroadcast"; the add-on is "Open3DBroadcast WebRTC". Whole plugin is Beta for v1; MoQ ships as Experimental.
- WebRTC ships only as the separate free add-on, not in the Fab package.
- v1 platform scope and the rest: see ADRs 0001–0011.
