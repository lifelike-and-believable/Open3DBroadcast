# Plan: Unreal Engine 5.6 and 5.8 support

Status: draft, 2026-10-06. **Decision 1 is made (maintainer, 2026-10-07): support UE 5.7 and 5.8; 5.6 is dropped.** Decisions 2 to 7 are still open; until they are answered, read them for two engines (for example, the reduced CI job runs on 5.8 only). Everything about 5.6 below is kept for the record. The 5.8 compile spike (WP-V1) is done: [engine-version-5.8-spike.md](engine-version-5.8-spike.md). Today both plugins support UE 5.7 only (ADR 0001).

The facts below were read from develop at 29a4b861 and from the engines installed on the development machine. The C++ evidence comes from comparing engine headers; nothing was compiled. Statements marked "unverified" need a build, a test, or a person at the desk.

## 1. Facts

### 1.1 Engines and toolchains

**Installed on the development machine:**

| Engine | Version | Changelist | Notes |
|---|---|---|---|
| UE 5.6 | 5.6.1 | 44394996 | |
| UE 5.7 | 5.7.4 | 51494982 | |
| UE 5.8 | 5.8.2 | 56702186 | Released build (`IsPromotedBuild: 1`), not a preview. |

The development machine has MSVC 14.34, 14.38.33130 and 14.44.35207, all under VS 2022.

**The CI runner** is a different machine.
- Its logs show only UE 5.7. Whether 5.6 or 5.8 is installed there is unverified.
- Its toolchain is VS 2026 only, with MSVC 14.51.

**Toolchains each engine accepts:**
- **UE 5.6:** its UnrealBuildTool knows VS 2022 only. Minimum MSVC 14.38; 14.39.x and 14.40.x are banned (`Engine/Config/Windows/Windows_SDK.json`). **5.6 cannot build on the runner until a VS 2022 MSVC toolset is installed there.**
- **UE 5.7:** bans 14.44.0 to 14.44.35210.
- **UE 5.8:** prefers 14.50.35717 or later, or 14.44.35207 or later.

### 1.2 Engine API differences that touch the code

Every engine header the two plugins include exists in 5.6, 5.7 and 5.8.

**Verified incompatibilities:**
1. **5.6 can't compile the ProjectSandbox targets.** `ProjectSandbox.Target.cs` and `ProjectSandboxEditor.Target.cs` set `BuildSettingsVersion.V6` and `EngineIncludeOrderVersion.Unreal5_7`.
   - 5.6 has only V1 to V5 and include orders up to `Unreal5_6`.
   - 5.8 has V7 and `Unreal5_8`, and still accepts `Unreal5_7`.
   - Each engine's rules compiler defines `UE_5_<n>_OR_LATER`, so `#if UE_5_7_OR_LATER` works in a Target.cs.
   - This affects local development, the Shipping game build and the Linux exclusion check. It does not affect BuildPlugin or Fab, which use the engine's own targets.
2. **5.8 builds plugins with build settings V7 by default.** V7 turns three warnings into errors: return type, dangling pointer and unreachable code. It also turns on `/fp:precise` for editor targets.
   - Whether any plugin or core code trips these is unknown until a build. This is the most likely real compile risk on 5.8.
3. **`"EngineVersion": "5.7.0"` in the source `.uplugin` files makes 5.6 and 5.8 skip the plugin in unattended runs.**
   - The incompatible-plugin prompt defaults to No under `-unattended`.
   - BuildPlugin stamps the main plugin's package with the building engine's version, so that package is fine.
   - The WebRTC add-on's test host copies its source descriptor, so the add-on would be skipped silently. The test floor would catch the lower count.
4. **5.8 removed the deprecated pointer overloads of `RegisterSubmixBufferListener`.** The plugin uses the `TSharedRef` overloads, which exist in all three engines, so this needs no change.

**Checked and compatible across the three engines, by header comparison:**
- **LiveLink:**
  - the client calls the receiver uses;
  - `FrameId`, which is `int32` in all three;
  - the buffer settings.
  - 5.8 adds `ULiveLinkSourceFactory::CreateSource(const FLiveLinkSourcePreset&)`. Our override of the string overload hides it. Probably no warning, but unverified.
- **Audio:**
  - `ISubmixBufferListener`, `AudioCaptureCore.h` and `SoundWaveProcedural.h` are effectively unchanged;
  - the `USoundWave` fields the remote audio component sets exist in all three.
- **`TWeakObjectPtr`:** the deleted `operator bool` is the same in all three.
- **Timecode:** the `FApp` time functions exist in all three. 5.8 forwards them to `FAppTime`, with commented-out deprecations, so expect real deprecations in 5.9.
- **HTTP and JSON** (the WebRTC token fetcher) and the PropertyEditor/Slate APIs the editor module uses.
- No engine-version guard exists in the plugin today.

**Unverified until a build:**
- On 5.8, `FSkeletalMeshLODRenderData` has a new base class. This affects one test helper, `O3DTestSkeletalMesh.cpp`.
- A full "present in 5.7, absent in 5.6" check was not done. The core UObject and container headers changed a lot.
- **5.8 LiveLink behaviour:**
  - EngineTime read time now comes from `Time.CurrentTime`, where it used to come from `FApp::GetCurrentTime()`;
  - the default frame buffer grew from 10 to 64.

  The receiver mirrors that read-time arithmetic (ADR 0013, which was verified against 5.7 only).

### 1.3 Descriptors and Fab

- Both `.uplugin` files say `"EngineVersion": "5.7.0"` and "Unreal Engine 5.7". Rule O3D-005 pins the version.
- `ProjectSandbox.uproject` has `"EngineAssociation": "5.7"`.
- The generated host projects in the build scripts use an empty association, so they work with any engine.
- **From the 2026-09 review, unverified against live Fab pages:** each engine version on a Fab listing is a separate source-zip submission, and each zip's `EngineVersion` must match its engine. Whether Fab still accepts new 5.6 submissions is unknown.
- `fab-package.py` doesn't read or check `EngineVersion` today.

### 1.4 CI cost

- One self-hosted UE runner.
- `UE_ROOT` is hard-coded to UE 5.7 in `open3dbroadcast-ue-build-test.yml`, the nightly and the manual test workflow.
- **Measured run times:**
  - the PR UE job takes about 9 minutes: the build, tests, add-on, strict build and Fab zip;
  - the nightly takes about 12 to 15 minutes.
- **Matrix estimates, with the runs serialised on the single runner:**
  - every engine, full job, on every PR: about 26 minutes per PR;
  - the full 5.7 job plus a reduced job on 5.6 and 5.8: about 18 minutes.
- "Plugin CI result" keeps working with a matrix: a failed leg fails the rolled-up job.
- Artifact names would collide across engines, so they need an engine suffix.

### 1.5 Third-party code

- **`opus.lib` and `nng.lib`:** built with MSVC 19.38, `/MD`, no LTCG. They link with every toolset the three engines accept.
- **`moq_ffi.dll` and `livekit_ffi.dll`:** C-ABI DLLs.
- All four are engine-independent.
- **The core:** compiled from source by `Open3DStreamCore`, so it follows each engine's compiler. Its only engine-dependent risk is V7 on 5.8.

### 1.6 Tests

- No test depends on the engine version.
- The counts should match on every engine. A drop, such as the add-on skipped by the EngineVersion prompt, is what the floors catch.
- The LiveLink timing tests are the most likely to differ on 5.8: `O3DReceiverSceneTimeTests`, `O3DControlTimecodeAlignmentTests` and `O3DSceneTimeMapperTests`.

## 2. Decisions for the maintainer

Each decision gives the planning agent's recommendation in brackets. The decisions would go into a new ADR, 0014, which amends ADR 0001's engine scope; ADR 0001's platform decision, Win64 only, stays.

1. **Which versions.** [5.6, 5.7 and 5.8, with 5.7 as the main development engine.]
   - Alternative: 5.7 and 5.8 only. That avoids the VS 2022 toolset on the runner and the 5.6 compile risk.
   - 5.8 already marks 5.6's include order "unsupported in 5.9".
2. **Support policy.** [Support the three latest minor versions that Fab accepts, and drop the oldest when a new one ships.]
3. **One source tree, or branches.** [One tree, with a small compatibility header for the version differences.]
4. **WebRTC add-on.** [Follows the same engines. It is built against the main plugin's package, so each engine needs its own build anyway.]
5. **`EngineVersion` in the source descriptors.** [Remove it from both source `.uplugin` files, and stamp it per engine when packaging.] The alternative keeps 5.7.0 in source and re-stamps it everywhere else. Either way, rule O3D-005 changes.
6. **PR CI.** [Option B: the full job on 5.7 plus a reduced job (build, tests, add-on) on 5.6 and 5.8 for every non-draft PR, all behind "Plugin CI result".] If the queue gets too long, Option C: 5.7 on PRs, with 5.6 and 5.8 run nightly, at release and on demand.
7. **Fab.** [One listing with one source zip per engine. A person confirms Fab's current multi-version rules and uploads each zip.]

## 3. Approach

**Code:**
- Add `Open3DShared/Public/O3DEngineCompat.h` on top of `Misc/EngineVersionComparison.h`, which exists in all three engines.
- Every version guard lives in it, or is named by it, with a comment naming the engine change.
- Prefer code that compiles cleanly everywhere over guards; in particular, fix V7's error-level warnings unconditionally.

**Target.cs:**
- `#if UE_5_8_OR_LATER` → V7 and `Unreal5_8`;
- `#elif UE_5_7_OR_LATER` → V6 and `Unreal5_7`;
- `#else` → V5 and `Unreal5_6`.

These match the settings BuildPlugin uses on each engine. Rule O3D-001 is rewritten to say this.

**Build.cs:** no change. Nothing in any of them is version-specific.

**Descriptors and scripts:**
- `release-version.py` gains engine-version stamping, with self-tests.
- `Setup-UE.ps1` takes `-EngineVersion` and checks it against `Build.version`.
- `Build-WebRTCAddOn.ps1` stamps the copied add-on.
- `Run-AutomationTests.ps1` fails when a project plugin's `EngineVersion` doesn't match the engine, and accepts per-engine floor keys.
- `fab-package.py --engine-version` stamps the zip and checks it.
- `Build-FabZip.ps1` checks the version.

**Local development:** one git worktree per engine, because the in-tree `Binaries/` and `Intermediate/` folders are per-engine build output.

**CI:**
- The reusable UE job gains `ue-version` and `profile` (full or reduced) inputs, and per-engine artifact names.
- The plugin CI, nightly, manual test and Fab-package workflows run a matrix.
- The flag-combination builds, the Shipping game and the Linux check stay on 5.7.

**Release:**
- A matrix build.
- One zip per engine for each plugin: `Open3DBroadcast-Plugin-X.Y.Z-UE5.6-Win64.zip` and so on, laid out as `UE_5.6/Plugins/...`.
- Per-engine Fab zips, kept as 90-day artifacts.

## 4. Work packages, in order

1. **WP-V0, decisions and ADR 0014** (S, docs). Record the section-2 answers, amend ADR 0001's status, and add the roadmap entry.
2. **WP-V8a, runner preparation** (desk). Install UE 5.8, and 5.6 if chosen, with the Win64 target only. Add the VS 2022 Build Tools with MSVC 14.44.35211 or later for 5.6, and check the free disk space. Confirm how UnrealBuildTool picks a compiler when VS 2022 and 2026 are both installed.
3. **WP-V1, compile spike** (M, by hand on the development machine, which has all three engines). Run BuildPlugin with warnings as errors, a strict build, the add-on build and the automation tests on 5.6 and on 5.8. Write a findings note listing every error and warning, and the test counts against the floors.
4. **WP-V2, build system and descriptors** (M): the Target.cs guards, rules O3D-001 and O3D-005, engine-version stamping and checks, and the script parameters.
5. **WP-V3, code compatibility** (S or M, sized by WP-V1): the compatibility header, and fixes for whatever WP-V1 finds.
6. **WP-V4, CI matrix** (M). Depends on WP-V8a, WP-V2 and WP-V3.
7. **WP-V5, release** (M): the matrix release, the per-engine zips and notes, and a dry run on every engine.
8. **WP-V6, behaviour on 5.6 and 5.8** (M, partly desk):
   - re-check LiveLink's EngineTime and Timecode evaluation against ADR 0013 and the receiver;
   - check that a process still has one LiveLink client;
   - a live take on each engine by the maintainer.
9. **WP-V7, docs** (S): the requirements in every README and guide, AGENTS.md, the rules and Build/README.
10. **WP-V8b, Fab** (desk): confirm the multi-version rules on the live Fab pages, then upload one source zip per engine.

## 5. Risks

- **5.6 toolchain:** the runner can't build 5.6 without a VS 2022 toolset.
- **5.6 source compatibility:** unverified beyond header presence and spot checks.
- **5.8 V7:** may turn current warnings into errors; the size of the work is unknown until WP-V1.
- **5.8 LiveLink:** the EngineTime semantics changed.
- **Silent plugin skip:** an engine-version mismatch shows only as a lower test count until WP-V2's check lands.
- **Fab:** the multi-version rules are unverified, and 5.6 may already be outside Fab's window.
- **CI queue time:** grows about 2× with Option B on the single runner.
- **5.9:** the `FApp` time functions will likely be deprecated, and 5.6's include order dropped.
