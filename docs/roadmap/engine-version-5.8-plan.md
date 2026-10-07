# Plan: Unreal Engine 5.8 support

Status: decided, 2026-10-07. Both plugins support UE 5.7 and 5.8, Win64 only
([ADR 0014](../adr/0014-supported-engine-versions.md)); 5.6 was considered and dropped. The 5.8
compile spike (WP-V1) is done: [engine-version-5.8-spike.md](engine-version-5.8-spike.md).

The facts were read from `develop` and from the engines installed on the development machine; the
spike then built and tested on 5.8.2. Statements marked "unverified" need a build, a test, or a
person at the desk.

## 1. Facts

### 1.1 Engines and toolchains

- **Development machine:** UE 5.7.4 (changelist 51494982) and UE 5.8.2 (56702186, a released build).
  MSVC 14.44.35207 under VS 2022; UnrealBuildTool picks it for both engines (spike).
- **CI runner** (a different machine): UE 5.7 only, as far as its logs show. VS 2026 with MSVC 14.51.
  **UE 5.8 must be installed there before the CI matrix lands** (WP-V8a).
- **Toolchains accepted:** UE 5.7 bans MSVC 14.44.0 to 14.44.35210; UE 5.8 prefers 14.50.35717 or
  later, or 14.44.35207 or later. Both engines accept the runner's 14.51.

### 1.2 Engine differences that touch the code

**Found by the spike (fix in WP-V3):**
1. `FCoreDelegates::OnPostEngineInit` is deprecated in 5.8 (`Open3DSenderModule.cpp:31`, `:41`).
2. `PLATFORM_64BITS` is deprecated in 5.8 (47 uses in 8 files of the WebRTC add-on).
3. A test bug the 5.8 run exposed: `Sender.Capture.RecordsEveryPayload` reads its capture through a
   relative path with `std::ifstream`.

**Build settings:** the Target.cs files set V6 and `Unreal5_7`; 5.8 accepts both without a warning
(spike). 5.8's own default is V7, which turns return-type, dangling-pointer and unreachable-code
warnings into errors and enables `/fp:precise` for editor targets. BuildPlugin uses the engine's own
targets, so the spike already built both plugins with 5.8's defaults; V7 raised nothing beyond the
deprecations.

**Descriptors:** `"EngineVersion": "5.7.0"` in a source `.uplugin` makes 5.8 skip that plugin in
unattended runs (the incompatible-plugin prompt defaults to No). BuildPlugin stamps the main plugin's
package with the building engine's version; the add-on's test host copies the source descriptor, so
the add-on would be skipped silently. The test floors catch the lower count.

**Checked compatible by header comparison:** the LiveLink client calls and buffer settings, the audio
listener, capture and procedural sound wave APIs, `TWeakObjectPtr`, the `FApp` time functions (5.8
forwards them to `FAppTime`; expect deprecations in 5.9), HTTP and JSON, and the editor's
PropertyEditor and Slate APIs. 5.8 adds `ULiveLinkSourceFactory::CreateSource(const FLiveLinkSourcePreset&)`,
which our string overload hides.

**Unverified:**
- 5.8's LiveLink EngineTime read time now comes from `Time.CurrentTime` (was `FApp::GetCurrentTime()`)
  and the default frame buffer grew from 10 to 64. The receiver mirrors that arithmetic (ADR 0013,
  verified on 5.7 only). The timing tests passed on 5.8 in the spike; a live take has not been done.
- `FSkeletalMeshLODRenderData` has a new base class in 5.8 (used by the test helper
  `O3DTestSkeletalMesh.cpp`); it compiled in the spike.

### 1.3 Descriptors and Fab

- `ProjectSandbox.uproject` has `"EngineAssociation": "5.7"`; the generated host projects in the
  build scripts use an empty association.
- From the 2026-09 review, unverified against live Fab pages: each engine version on a listing is a
  separate source-zip submission whose `EngineVersion` must match its engine.
- `fab-package.py` does not read or check `EngineVersion` today.

### 1.4 CI cost

- One self-hosted UE runner. The PR UE job takes about 9 minutes; the nightly 12 to 15.
- The full 5.7 job plus a reduced 5.8 job, serialised: about 15 minutes per PR.
- "Plugin CI result" keeps working with a matrix. Artifact names need an engine suffix.

### 1.5 Third-party code

`opus.lib`, `nng.lib` (MSVC 19.38, `/MD`), `moq_ffi.dll` and `livekit_ffi.dll` (C ABI) are
engine-independent. The core is compiled from source by each engine.

## 2. Decisions (ADR 0014, maintainer 2026-10-07)

1. UE 5.7 and 5.8; 5.7 is the main development engine.
2. Support the latest minor versions Fab accepts, two at a time.
3. One source tree with `O3DEngineCompat.h` for the differences.
4. The WebRTC add-on follows the same engines.
5. `EngineVersion` removed from the source descriptors and stamped per engine at packaging.
6. PR CI: the full job on 5.7 and a reduced job on 5.8 (build, tests, add-on) for non-draft PRs.
7. Fab: one listing, one source zip per engine, uploaded by a person.

## 3. Approach

**Code:** `Open3DShared/Public/O3DEngineCompat.h` on top of `Misc/EngineVersionComparison.h`, only
when a guard is needed; every guard names the engine change. Prefer code that compiles cleanly on
both engines.

**Target.cs:** `#if UE_5_8_OR_LATER` → V7 and `Unreal5_8`; `#else` → V6 and `Unreal5_7`. Rule
O3D-001 says this.

**Descriptors and scripts:**
- `release-version.py` stamps the engine version, with self-tests.
- `Setup-UE.ps1` takes `-EngineVersion` and checks it against `Build.version`.
- `Build-WebRTCAddOn.ps1` stamps the copied add-on.
- `Run-AutomationTests.ps1` fails when a project plugin's `EngineVersion` does not match the engine,
  and accepts per-engine floor keys.
- `fab-package.py --engine-version` stamps the zip and checks it; `Build-FabZip.ps1` checks it.

**Local development:** one git worktree per engine.

**CI:** the reusable UE job gains `ue-version` and `profile` (full or reduced) inputs and per-engine
artifact names; the plugin CI, nightly, manual test and Fab-package workflows run the matrix. The
flag-combination builds, the Shipping game and the Linux check stay on 5.7.

**Release:** a matrix build; one zip per engine for each plugin
(`Open3DBroadcast-Plugin-X.Y.Z-UE5.8-Win64.zip`, laid out as `UE_5.8/Plugins/...`); per-engine Fab zips.

## 4. Work packages, in order

1. **WP-V0, ADR 0014** (done).
2. **WP-V1, compile spike** (done): [engine-version-5.8-spike.md](engine-version-5.8-spike.md).
3. **WP-V3, code compatibility** (S): the three spike findings; then re-run the spike, including the
   add-on's tests.
4. **WP-V2, build system and descriptors** (M): Target.cs guards, rules O3D-001 and O3D-005,
   `EngineVersion` removal and stamping, the script parameters and checks.
5. **WP-V8a, runner preparation** (desk): install UE 5.8 (Win64 only) on the runner; check disk space.
6. **WP-V4, CI matrix** (M), after WP-V8a, V2 and V3.
7. **WP-V5, release** (M): the matrix release, per-engine zips and notes, a dry run on both engines.
8. **WP-V6, behaviour on 5.8** (M, partly desk): LiveLink EngineTime and Timecode against ADR 0013,
   one LiveLink client per process, and a live take by the maintainer.
9. **WP-V7, docs** (S): requirements in every README and guide, AGENTS.md, the rules, Build/README.
10. **WP-V8b, Fab** (desk): confirm the multi-version rules, upload one source zip per engine.

## 5. Risks

- **Runner:** the CI matrix cannot run until UE 5.8 is installed on the runner.
- **5.8 LiveLink:** the EngineTime semantics changed; covered only by tests until a live take.
- **Silent plugin skip:** an `EngineVersion` mismatch shows only as a lower test count until WP-V2's
  check lands.
- **Fab:** the multi-version rules are unverified.
- **CI queue time:** grows by the reduced 5.8 job on every non-draft PR.
- **5.9:** expect the `FApp` time functions to be deprecated.
