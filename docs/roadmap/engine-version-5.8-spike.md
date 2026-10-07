# UE 5.8 compile spike (WP-V1)

Date: 2026-10-07. Scope: UE 5.7 and 5.8 (maintainer decision 2026-10-07; 5.6 is not supported).
Plan: [engine-version-5.8-plan.md](engine-version-5.8-plan.md); decision: [ADR 0014](../adr/0014-supported-engine-versions.md).

## What ran

On the development machine, from `develop` at 0bfb2ae6 (after #436 to #439) in a separate worktree,
with UE 5.8.2 (`C:\Program Files\Epic Games\UE_5.8`), the same steps and flags as
`open3dbroadcast-ue-build-test.yml` runs on 5.7. The source descriptors still say
`"EngineVersion": "5.7.0"`; nothing was changed for 5.8.

| Step | Command | Result |
|---|---|---|
| 1 | `Build-Plugin.ps1 -FailOnWarnings` (unity, PCH) | Builds; **fails** on 2 warnings |
| 2 | `Run-AutomationTests.ps1 -MinTestsKey plugin` | 494 tests ran (floor 494), 493 passed, **1 failed** |
| 3 | `Build-WebRTCAddOn.ps1 -StrictIncludes -FailOnWarnings` | Builds; **fails** on warnings; no package written |
| 4 | `Run-AutomationTests.ps1 -MinTestsKey plugin-with-webrtc` | 494 tests ran (floor 549): the add-on was missing because step 3 wrote no package; 1 failed (as step 2) |
| 5 | `Build-Plugin.ps1 -StrictIncludes -FailOnWarnings` | Builds; **fails** on the same warnings as step 1 |

Toolchain picked by UnrealBuildTool on 5.8: Visual Studio 2022, MSVC 14.44.35207, Windows SDK
10.0.22621.0, the same as on 5.7. The 5.7 Target.cs settings (`BuildSettingsVersion.V6`,
`EngineIncludeOrderVersion.Unreal5_7`) produced no warning or error on 5.8.

## Findings

1. **`FCoreDelegates::OnPostEngineInit` is deprecated in 5.8** (C4996), used at
   `Open3DSender/Private/Open3DSenderModule.cpp:31` and `:41`. These are the 2 warnings of steps
   1 and 5. Fix: the replacement the 5.8 deprecation message names, behind the version guard
   (`O3DEngineCompat.h`) only if the replacement does not exist in 5.7.
2. **`PLATFORM_64BITS` is deprecated in 5.8** ("UE only supports 64-bit"; C4996): 47 uses in 8
   files of the WebRTC add-on. The source uses are `WebRTCReceiver.cpp:345` and
   `WebRTCSender.cpp:260` (`#if !PLATFORM_WINDOWS || !PLATFORM_64BITS`); the rest are in the add-on's
   tests (`#if PLATFORM_WINDOWS && PLATFORM_64BITS`). Fix: test `PLATFORM_WINDOWS` alone; Win64 is
   the only platform (ADR 0001). This works on 5.7 too, so no guard is needed.
3. **`Open3DBroadcast.Sender.Capture.RecordsEveryPayload` fails on 5.8**: "Header reads" is false
   and no records are read. The capture writes to the full path
   (`O3DSenderCapture.cpp:54`, `ConvertRelativePathToFull`), but the test reads it back with
   `std::ifstream` on the relative `FPaths::ProjectSavedDir()` path
   (`O3DSenderPipelineTests.cpp:679, 705`), which resolves against the process's working
   directory. That happens to work on 5.7. A test bug: the test should convert the path to full,
   as the capture does.
4. **Nothing else failed.** The other 493 tests of the main plugin passed on 5.8 unchanged. The
   add-on's tests did not run (finding 2 stopped its build), so they are unverified on 5.8.

## What this means for the plan

- WP-V3 (code compatibility) is small: findings 1 to 3, roughly three files plus the add-on's tests.
- The 5.7 Target.cs settings work on 5.8 as they are. Whether to move 5.8 to its own settings
  (V7, `Unreal5_8`), as the plan proposes, is a WP-V2 choice, not a blocker.
- The EngineTime and LiveLink behaviour checks (WP-V6) and a live take still need the maintainer.

## Not verified

- The add-on's 55 tests on 5.8.
- A packaged game, the flag-combination builds and the Fab zip on 5.8.
- Any behaviour at the desk (LiveLink, audio, a live take).
