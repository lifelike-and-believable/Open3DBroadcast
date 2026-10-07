# Build Scripts and Tools

This directory contains build scripts, test runners, utilities for developing and testing Unreal Engine plugins, and CMake configurations for building prebuilt libraries.

## Directory Structure

```
Build/
└── Scripts/          # PowerShell and bash scripts for building and testing
```

## Plugins Overview

### Open3DBroadcast Plugin
Located at `ProjectSandbox/Plugins/Open3DBroadcast/`. Everything it compiles or links is committed under its `Source/` folder, so a clean clone builds with `RunUAT BuildPlugin` alone: no CMake, no PowerShell pre-build step and no prebuilt core library (WP-F1, ADR 0003). The o3ds core is compiled by the plugin's `Open3DStreamCore` module from a generated copy of `src/o3ds` (see `sync_o3ds_core.py` below). Opus and NNG stay prebuilt Win64 libraries under `Source/`. `Config/FilterPlugin.ini` adds only the root documentation and licences to the package (FAB-2).

### Open3DBroadcastWebRTC add-on
Located at `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/` (WP-F11, ADR 0002). It holds the WebRTC transport (`Open3DTransportWebRTC`) and `livekit_ffi`, which are not in Open3DBroadcast at all, and depends on Open3DBroadcast through its `.uplugin`. `RunUAT BuildPlugin` cannot build it alone (its host project would lack Open3DBroadcast); `Build-WebRTCAddOn.ps1` builds it against an Open3DBroadcast package. ProjectSandbox enables both plugins. The copyright and runtime-editor-deps checks cover both plugins; `fab-package.py` fails if any WebRTC module or livekit file appears in Open3DBroadcast.

## The o3ds core in the plugin

#### `sync_o3ds_core.py`
Generates the plugin's copy of the o3ds core (docs/adr/0003-core-library-delivery-to-plugin.md). Python 3.8+, standard library only; needs the `thirdparty/flatbuffers` and `thirdparty/crccpp` submodules.

```bash
git submodule update --init thirdparty/flatbuffers thirdparty/crccpp
python3 Build/Scripts/sync_o3ds_core.py           # rewrite the copy
python3 Build/Scripts/sync_o3ds_core.py --check   # compare only (CI)
```

- **Input:** `Build/o3ds-core-manifest.txt` lists the core headers that plugin code includes. The script adds the `.cpp` next to each header and follows `#include` lines to the full closure.
- **Output:** `ProjectSandbox/Plugins/Open3DBroadcast/Source/ThirdParty/Open3DStreamCore/`: the closure (byte-for-byte copies of `src/o3ds` files), `src/o3ds_generated.h`, the FlatBuffers runtime headers and (only while a core file includes it; none has since WP-A2e) CRC++'s `CRC.h` from the submodule pins, their licences under `LICENSES/`, and `SYNC_STAMP.txt` (O3DS_VERSION_TAG, FlatBuffers version, submodule pins, manifest and content hashes). It also writes one `Source/Open3DStreamCore/Private/Core/O3DSCore_<file>.cpp` per mirrored `.cpp`; each includes the mirrored file between `O3DSCoreSourceBegin.h` and `O3DSCoreSourceEnd.h`, which switch compiler warnings off for the core. The copy sits outside the module folder because UBT compiles every `.cpp` in a module folder.
- **Checks (both modes, exit `1`):** a quoted include in a core file must resolve to `src/o3ds`, `src/o3ds_generated.h`, FlatBuffers or `CRC.h`; an angle-bracket include must be a standard header (no `<windows.h>`); no `throw`/`try`/`catch`/`dynamic_cast`/`typeid` in core files (the module is built without exceptions and RTTI); every `o3ds/`, `flatbuffers/` or `o3ds_generated.h` include in plugin sources is in the copy; the submodule checkouts match their pins. `--check` also fails on any file that differs (line endings ignored), is missing or is extra. Exit `2` means bad input (missing submodule or manifest).
- **CI:** `core-tests.yml` runs `--check` on every PR ("Plugin core mirror in sync"), and its Linux test job fails when the committed `src/o3ds_generated.h` differs from what the pinned `flatc` generates from `src/o3ds.fbs`.
- **Workflow:** change `src/`, run the script, commit both. Never edit the copy or the `O3DSCore_*.cpp` files by hand. A class or function with an out-of-line definition that the plugin uses needs `O3DS_API` (`src/o3ds/o3ds_export.h`); the module defines it as `OPEN3DSTREAMCORE_API`.

`Sync-O3DSCore.ps1`, which built the core with CMake and copied a static library into the plugin before every UE build, has been removed.

## Scripts

### Setup and Configuration

#### `Setup-UE.ps1`
Verifies Unreal Engine installation.

**Usage:**
```powershell
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine installation (default: `C:\Program Files\Epic Games\UE_5.7`)
- `-EngineVersion` - Optional `X.Y` (for example `5.8`); fails unless the engine's `Engine\Build\Build.version` says that version (ADR 0014)

---

### Building

#### `Build-Plugin.ps1`
Builds an Unreal plugin using Unreal Automation Tool (UAT).

**Usage:**
```powershell
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "Artifacts\Open3DBroadcast" `
  -TargetPlatforms @("Win64") `
  -Configuration "Shipping"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine installation (required)
- `-PluginUPluginPath` - Path to `.uplugin` file (required)
- `-OutDir` - Output directory for packaged plugin (required)
- `-TargetPlatforms` - Array of platforms to build (default: `@("Win64")`)
- `-Configuration` - Build configuration: Development, Shipping, etc. (default: `Development`)
- `-AllowFallback` - Local troubleshooting only. If `RunUAT BuildPlugin` fails, build ProjectSandbox with UBT and package from that instead. A fallback success does not mean the plugin package builds, so CI never passes this switch.
- `-StrictIncludes` - Passes BuildPlugin's `-StrictIncludes`: no precompiled headers and no unity build, so every source file has to include what it uses.
- `-FailOnWarnings` - Fails the script (exit code `1`) when the compiler reports a warning in a file under `Plugins/<Plugin>/`, even though BuildPlugin succeeded. Warnings located in engine headers are not counted. In GitHub Actions each warning becomes an error annotation on the source line.

**Exit code:** `0` only when `RunUAT BuildPlugin` succeeds (and, with `-FailOnWarnings`, no plugin warning was reported). Otherwise the script exits with UAT's exit code, or `1` for warnings, unless `-AllowFallback` is set.

**Output:**
- Packaged plugin in `OutDir`
- UAT's output in `<OutDir>-BuildPlugin.log`, next to the package
- Ready to install in other Unreal projects

#### `Build-FabZip.ps1`
Runs `RunUAT BuildPlugin` on the contents of the Fab source zip (see "Fab source package" below), with `-FailOnWarnings`, then checks that every module in the zip's `.uplugin` produced an editor DLL and that no excluded module and nothing of the WebRTC add-on (`livekit` or `Open3DTransportWebRTC` files) reached the output.

```powershell
.\Build\Scripts\Build-FabZip.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -Zip "Artifacts\Fab" `
  -WorkDir "Artifacts\FabBuild" `
  -RequireStandalone
```

Nothing is added to the extracted zip before BuildPlugin: since WP-F1 the zip builds on its own, exactly as Fab's farm receives it. `-RequireStandalone` (passed by CI and the nightly) first checks that the zip holds `Source/Open3DStreamCore` and its core copy in `Source/ThirdParty/Open3DStreamCore`, and no prebuilt `open3dstreamstatic`/`flatbuffers.lib`, and fails with that reason instead of a compiler error.

#### `Build-FlagCombinations.ps1`
Runs `Build-Plugin.ps1` once per transport build-flag combination (WP-F2, TRB-24, TRF-27): each of `O3D_WITH_TRANSPORT_SOCKETS`, `_NNG` and `_MOQ` set to `0` on its own (`no-sockets`, `no-nng`, `no-moq`), then all three at once (`loopback-only`). `O3D_WITH_TRANSPORT_WEBRTC` belongs to the WebRTC add-on and is not part of this matrix. The flags are described in the plugin README, "Build flags". Every combination runs even after a failure; the script exits `1` if any failed and prints a summary. The nightly workflow runs it with `-StrictIncludes -FailOnWarnings`; it is not part of PR CI because each combination takes as long as the PR build.

```powershell
.\Build\Scripts\Build-FlagCombinations.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutRoot "$PWD\Artifacts\FlagBuilds" `
  -Only no-moq `
  -StrictIncludes -FailOnWarnings
```

`-Only` takes one or more combination names; without it all four run. Packages and logs go to `<OutRoot>\<name>` and `<OutRoot>\<name>-BuildPlugin.log`.

#### `Test-LinuxExclusion.ps1`
Checks the second WP-F2 acceptance item: a game target that also targets Linux builds, with the plugin's modules left out. It builds the ProjectSandbox game target (which enables the plugin) for Linux through `Engine\Build\BatchFiles\Build.bat`, then fails if UBT wrote an intermediate folder for any `Open3D*` module under the plugin. See [Platforms](#platforms) for what it needs.

```powershell
.\Build\Scripts\Test-LinuxExclusion.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7" -Require
```

Without `LINUX_MULTIARCH_ROOT` it prints a notice and exits `0`, unless `-Require` is given.

#### `Build-ShippingGame.ps1`
Checks the WP-F7 acceptance item "a packaged Shipping game builds" (ADR 0010). It builds `ProjectSandboxEditor` (Development, needed to cook), then runs `RunUAT BuildCookRun` for the ProjectSandbox game with the plugin enabled: Win64, Shipping, build, cook, stage, pak and archive. It fails if BuildCookRun fails (UBT refuses to build `UnrealEd` and other editor-only engine modules into a game target, so a runtime module that still depends on one fails here), if no executable was archived, or if UBT compiled `Open3DBroadcastEditor` or `Open3DBroadcastTests` for the Shipping game target. See [Shipping game build](#shipping-game-build).

```powershell
.\Build\Scripts\Build-ShippingGame.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ArchiveDir "$PWD\Artifacts\ShippingGame"
```

`-ProjectFile` selects another `.uproject` that enables the plugin. ProjectSandbox also enables the WebRTC add-on, so the Shipping game includes it.

#### `Build-WebRTCAddOn.ps1`
Builds the Open3DBroadcastWebRTC add-on against a built Open3DBroadcast package (WP-F11). It creates a throwaway host project with the package in `Plugins/Open3DBroadcast` and the add-on source in `Plugins/Open3DBroadcastWebRTC`, enables both and LiveLink, runs `Build.bat UnrealEditor Win64 <Configuration> -Project=<host>`, and stages the add-on folder (no `Intermediate/`, no `.pdb`) in `-OutDir`. The package has no intermediate files, so UBT may recompile Open3DBroadcast inside the host copy; the add-on compiles against the package's public headers either way.

```powershell
.\Build\Scripts\Build-WebRTCAddOn.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -HostPluginPackageDir "$PWD\Artifacts\Open3DBroadcast" `
  -HostProjectDir "$PWD\Artifacts\AddOnHost" `
  -OutDir "$PWD\Artifacts\AddOnPackage\Open3DBroadcastWebRTC" `
  -StrictIncludes -FailOnWarnings
```

- `-StrictIncludes` adds `-NoPCH -NoSharedPCH -DisableUnity`; `-FailOnWarnings` fails on a compiler warning in an add-on source file.
- UBT's output goes to `<HostProjectDir>-UBT.log`. Exit `0` built and staged, `1` build failure or warning, `2` bad arguments.
- Afterwards `Run-AutomationTests.ps1 -ProjectFile <HostProjectDir>\O3DWebRTCHost.uproject` runs the tests with both plugins enabled.

---

### Testing

#### `Run-AutomationTests.ps1`
Runs Unreal's automation tests for the plugin and decides pass or fail from the automation report, not from the editor's exit code alone.

**Usage:**
```powershell
# Against a BuildPlugin package (what CI does): the script creates a throwaway host
# project that contains only this package, so nothing is compiled and the tests load
# exactly these binaries.
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginPackageDir "Artifacts\Win64" `
  -TestFilter "Open3DBroadcast" `
  -ResultsDir "Artifacts\Tests"

# Against a project whose editor binaries are already built
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast"
```

**Parameters:**
- `-UEPath` - Path to Unreal Engine (required)
- `-ProjectFile` or `-PluginPackageDir` - exactly one of them (required)
- `-HostProjectDir` - Where the throwaway host project is created with `-PluginPackageDir` (default: a temp folder; deleted and recreated each run)
- `-TestFilter` - Name prefix passed to `Automation RunTests` (default: `Open3DBroadcast`). Use a plain prefix: a trailing `.*` is not a wildcard there, so the script strips it with a warning.
- `-ResultsDir` - Output directory for test results (default: `"Artifacts\Tests"`)
- Before the editor starts, the script fails (exit 2) when a project plugin's `EngineVersion` names another engine than `-UEPath`'s: the editor would skip that plugin, and its tests, without a word (ADR 0014).
- `-MinTestsKey` - A key of `Build/automation-test-floors.json` (`plugin`, `plugin-with-webrtc`); the run fails when fewer tests ran than that floor. CI passes it; local runs normally don't. Raise the floor in the PR that adds tests.

Tests that need the internet register only when `O3DB_NETWORK_TESTS=1` is set in the environment (ADR 0006). CI sets it to `0` on pull requests.

**Exit code:** `0` when the editor exited cleanly and every test in the report passed (skipped tests are listed but allowed). `1` when the editor exited non-zero, `index.json` is missing or unreadable, no test ran, or any test is `Fail`, `NotRun` or `InProcess` (still running when the editor exited, usually a crash). `2` for bad arguments.

**Output (in `<ResultsDir>\<filter>\`):**
- `index.json` - The automation report
- `Automation.log` - The editor log
- Failed tests and their first error are printed as GitHub error annotations and added to the job summary

#### `Run-SenderBenchmark.py`
Records the ADR 0008 sender pipeline measurement (Decision item 11) with Unreal Insights and summarizes it. Python 3.8+, standard library only; Windows; needs the project's editor binaries built.

```powershell
python Build/Scripts/Run-SenderBenchmark.py --ue "C:/Program Files/Epic Games/UE_5.7" `
  --project ProjectSandbox/ProjectSandbox.uproject --out U:/o3dbench
```

- Runs `Open3DBroadcast.Bench.SenderPipeline` in the editor with `O3DB_BENCH=1` (the benchmark registers no instances otherwise, so CI and default runs never see it) and `-trace=cpu,frame,region`: one, then ten, 250-bone, 250-curve senders built in code, on Loopback and on UDP, with `o3d.Sender.AsyncPipeline` 0 and 1, 600 frames each at 60 Hz of wall time, each case in an Insights region `O3D.Bench.<case>`.
- Then runs `UnrealInsights.exe -NoUI` with a response file that exports the `O3D.Sender.Sample`, `O3D.Sender.Pipeline.Serialize` and `O3D.Sender.Pipeline.Send` events per region, and prints a table (also `summary.md`): game-thread time per sender, worker time per frame (median, p99), capture-to-send latency (p50, p99, largest; sampled once per frame per sender), dropped frames.
- `--skip-run` re-exports and re-summarizes an existing `bench.utrace`. Keep `--out` short (long paths break the editor).

---

## Common Workflows

### Initial Setup

```powershell
# 1. Verify UE installation
.\Build\Scripts\Setup-UE.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7"

# 2. The plugin lives in-tree at ProjectSandbox/Plugins/Open3DBroadcast - nothing to link
```

### Local Development

```powershell
# Build plugin for local testing
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "$PWD\Artifacts\Local"
```

### Running Tests

```powershell
# Run all plugin automation tests
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DBroadcast"
```

### Full Build and Test

```powershell
# Complete workflow
.\Build\Scripts\Setup-UE.ps1
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "$PWD\Artifacts\Win64" `
  -FailOnWarnings
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginPackageDir "$PWD\Artifacts\Win64" `
  -TestFilter "Open3DBroadcast"
```

## Fab source package

The Fab listing gets a source-only zip, different from the GitHub build (ADR 0002).

- `Build/Fab/exclude-modules.txt` lists modules left out of it: `Open3DBroadcastTests` (ADR 0006). WebRTC is not listed: since WP-F11 it is not in the plugin at all but in the Open3DBroadcastWebRTC add-on (ADR 0002).
- `Build/Fab/exclude-files.txt` lists files left out, as globs: the module-level `Source/*/*.md` READMEs and USER_GUIDEs, which stay in the repository for people reading the code. It has no `.pdb`, `.py` or developer-note rule on purpose: those fail the tree check below instead (see [Debug symbols](#debug-symbols); developer notes and scripts live in `docs/dev/<Module>/`).
- `Build/Scripts/fab-package.py` (Python 3.8+, standard library) takes the files git tracks under the plugin folder, applies both lists, removes the excluded modules' entries from the staged `.uplugin` without reformatting it, stamps `"EngineVersion": "<X.Y>.0"` from the required `--engine-version` into it (the source descriptor has none; each Fab zip is for one engine, ADR 0014), and writes a zip with one top-level `Open3DBroadcast/` folder and fixed timestamps (the same commit gives the same bytes). It then reopens the zip and fails if it finds an excluded module (folder or `.uplugin` entry), anything of the WebRTC add-on (an `Open3DTransportWebRTC` folder or `.uplugin` entry, or a file with `livekit` in its path), `.pdb`/`.py` files, `Binaries/` or `Intermediate/`, a Markdown file other than the root `README.md`, `USER_GUIDE.md`, `THIRD_PARTY_LICENSES.md`, `Transport_Module_Comparison.md` or anything under a `ThirdParty/` folder, a listed module without its `Build.cs`, a module without a `PlatformAllowList` (or one naming a platform the plugin's `SupportedTargetPlatforms` does not list; ADR 0001), a missing `Resources/Icon128.png`, `Config/FilterPlugin.ini` or `Source/ThirdParty/Open3DStreamCore/SYNC_STAMP.txt`, a prebuilt `open3dstreamstatic` or `flatbuffers.lib` (WP-F1), or a top-level file or folder outside `Binaries`, `Config`, `Content`, `Resources`, `Shaders` and `Source` that `Config/FilterPlugin.ini` does not list, since BuildPlugin would leave it out (FAB-2). Before any exclusion it also checks the tracked tree, including excluded modules, and fails if git tracks a `.pdb` (FAB-4) or a `.py`/`.pyc` anywhere under the plugin, a Markdown file under `Source/` other than a `README.md` or `USER_GUIDE.md` outside `ThirdParty/` (HYG-1), or anything of the WebRTC add-on (WP-F11); the source `.uplugin` must not list `Open3DTransportWebRTC` either. No exclusion rule makes WebRTC files acceptable.

```bash
python3 Build/Scripts/fab-package.py --out-dir Artifacts/Fab --engine-version 5.7
Build/Scripts/check-no-video-codecs.sh $(cat Artifacts/Fab/binaries.txt)
Build/Scripts/check-no-video-codecs.sh            # the whole Open3DBroadcast tree; passes
Build/Scripts/check-no-video-codecs.sh --addon    # the WebRTC add-on; fails by design until the codec-free rebuild
```

Then `Build-FabZip.ps1` (above) runs BuildPlugin on the zip's contents.

### Debug symbols

Debug symbols (`.pdb`) for the prebuilt third-party DLLs (`moq_ffi.dll`; `livekit_ffi.dll` in the WebRTC add-on) are not kept in the plugin trees and are not staged into packaged games (FAB-4, WP-F3).

- **Enforcement.** `ProjectSandbox/.gitignore` ignores `*.pdb` under the plugin, and `fab-package.py` fails the Fab zip job if one is tracked anyway (it checks the tracked tree before any exclusion, so neither an exclude rule nor an excluded module hides it). No `Build.cs` lists a `.pdb` in `RuntimeDependencies`.
- **Where the symbols go (intended process).** When a third-party DLL is refreshed, the person doing the refresh keeps the matching `.pdb` out of the commit and attaches it to the next plugin GitHub release (`open3dbroadcast-v*`) as an extra asset, zipped per library and version, for example `moq_ffi-<upstream-commit>-Win64-symbols.zip`. The SHA256 of the `.pdb` stays in the artifact table of the library's `ThirdParty/<lib>/README.md`, so a downloaded file can be matched to the DLL. The release workflow does not attach these assets automatically yet; until it does, the maintainer uploads them by hand. There is no symbol server.
- **Symbols that were removed.** The two `.pdb` files that used to be committed are still in git history. They were last present at commit `6b49517c5bbc8f32a23bb6e79166429af7a26e17`:

  ```bash
  git show 6b49517c5bbc8f32a23bb6e79166429af7a26e17:ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.pdb > moq_ffi.pdb
  git show 6b49517c5bbc8f32a23bb6e79166429af7a26e17:ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.pdb > livekit_ffi.pdb
  ```

  They match the DLLs currently in the tree (hashes in the two ThirdParty READMEs). They should be attached to the next release as described above.
- **Using them.** Put the `.pdb` next to the DLL (in the plugin's `Source/<Module>/ThirdParty/<lib>/bin/Win64/...` folder, or next to the staged copy in a packaged game) or add its folder to the debugger's symbol path.

## Copyright headers

Every `.h`, `.cpp` and `.cs` file under `Source/` of both plugins (Open3DBroadcast and the Open3DBroadcastWebRTC add-on) starts with a copyright line (FAB-5, FAB-9, WP-F4) naming the publisher and the year, as Fab TR 4.3.6.1.b asks. New files use this one, with the current year, followed by a blank line:

```cpp
// Copyright 2026 Lifelike & Believable. All Rights Reserved.
```

In headers, `#pragma once` comes after the blank line.

Files that came from Open3DStream keep its notice as the second line (maintainer, 2026-10-05). Don't add it to new files:

```cpp
// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
```

- **Not covered:** anything under a `ThirdParty/` directory (vendored nng, moq-ffi, livekit_ffi headers keep their own notices) and generated files (`*.generated.h`, `*_generated.h`, `*.gen.cpp`, or a file whose first ten lines say `@generated`, `automatically generated`, `auto-generated` or `DO NOT EDIT`).
- **Third-party code outside `ThirdParty/`:** keep its original notice and add the file to `Build/Fab/copyright-allowlist.txt`, one line per file: the path relative to the plugin root, then the reason (licence and origin). The list is empty today.
- **Check:** `Build/Scripts/check-copyright-headers.py` (Python 3.8+, standard library) reads the files git tracks under each plugin's `Source/` (both plugins by default; `--plugin-dir` is repeatable), ignores a leading UTF-8 BOM and accepts LF or CRLF. It exits `0` when every checked file starts with the line (any four-digit year) followed by a blank line or the Open3DStream line, `1` when a file does not (it lists each file and its first line) or an allowlist entry names a file git does not track, and `2` for bad input. `-v` also lists the skipped ThirdParty files.

```bash
python3 Build/Scripts/check-copyright-headers.py
```

## Runtime modules without editor code

Editor UI lives in Editor-type modules: `Open3DBroadcastEditor` (Details customization, LiveLink creation panel, transport settings panels; ships in the Fab package) and `Open3DBroadcastTests` (ADR 0010, WP-F7). The runtime modules (every `.uplugin` entry whose `Type` is `Runtime` or another runtime type) must not use editor or Slate code, so that a packaged game builds and the Server option of ADR 0001 stays open.

- **Rule:** a runtime module's `Build.cs` names none of `UnrealEd`, `PropertyEditor`, `Slate`, `SlateCore`, `EditorStyle`, `ToolMenus`, `AppFramework` and the other editor modules listed in the script, anywhere in the file (a `Target.bBuildEditor` block included). Its sources include no editor-only header (`Editor.h`, `ScopedTransaction.h`, `PropertyEditorModule.h`, `IDetailCustomization.h`, `Widgets/...`, `Framework/Application/...`, and the others listed in the script), `WITH_EDITOR`-guarded or not. `InputCore` is allowed; it is a runtime module.
- **Transport settings:** a transport describes its options as data (`FO3DTransportOptionSchema`, `Open3DShared/Public/O3DTransportOptionSchema.h`) in its sender and receiver customizations. `Open3DBroadcastEditor` builds the panel from it. A transport module never builds widgets.
- **Check:** `Build/Scripts/check-runtime-editor-deps.py` (Python 3.8+, standard library) checks both plugins by default (`--plugin-dir` is repeatable). It reads each `.uplugin`, then each runtime module's `Build.cs` string literals (comments ignored) and `#include` lines (ThirdParty skipped). It exits `0` with no violation, `1` with one or more (each printed as `path:line`), and `2` for bad input. `--self-test` runs it against a generated clean plugin and a generated bad one. A line that must stay can carry `o3d-allow-editor-dependency: <reason>`; nothing uses that today.

```bash
python3 Build/Scripts/check-runtime-editor-deps.py --self-test
python3 Build/Scripts/check-runtime-editor-deps.py
```

## Markdown links

The docs link to each other and into the plugins' folders, and files move. The check keeps those links from rotting (WP-D4).

- **Check:** `Build/Scripts/check-markdown-links.py` (Python 3.8+, standard library) reads every Markdown file git tracks, except under `ThirdParty/` and `thirdparty/`. A relative link, or a GitHub link into this repository's tree (`https://github.com/lifelike-and-believable/Open3DBroadcast/blob/<branch>/<path>`), fails when the path is not in the checkout. A `#fragment` into a Markdown file fails when no heading (slugged as GitHub does) or explicit anchor matches. Other web links are not fetched. Links in code are ignored. It exits `0` with no broken link, `1` with one or more (each printed as `file:line: target -> reason`), and `2` outside a git checkout. `--self-test` runs it against generated fixtures.
- **CI:** the plugin CI runs it as "Markdown links resolve" on every PR and push, outside the path filter, so a docs-only change is checked too.

```bash
python3 Build/Scripts/check-markdown-links.py --self-test
python3 Build/Scripts/check-markdown-links.py
```

## Prebuilt third-party binaries

The plugins track a few prebuilt libraries (`moq_ffi.dll`, `nng.lib`, `opus.lib`, and in the WebRTC add-on `livekit_ffi.dll`) under their `ThirdParty/<library>/` folders. Each folder has a `README.md` with an inventory table: the binary's path relative to the README in backticks, and its SHA256 in backticks. The README also records the upstream source, version or commit and build flags where they are known; a value nobody recorded says "Not recorded" instead of guessing.

- **Check:** `Build/Scripts/check-third-party-binaries.py` (Python 3.8+, standard library) lists the tracked `.dll`, `.lib`, `.so`, `.dylib` and `.a` files under `ProjectSandbox/Plugins/*/Source/**/ThirdParty/` with `git ls-files` and fails when one has no README, no row, or a row whose SHA256 differs from the file. It also fails on a row that names a binary missing from the tree (rows that say "not in the tree", the `.pdb` release assets, are skipped). The plugin CI runs it as "Third-party binaries match their READMEs"; Markdown-only changes do not start that job, so run it after editing a README by hand.
- **Replacing a binary:** update the README row (hash, and the provenance rows) in the same commit.

```bash
python3 Build/Scripts/check-third-party-binaries.py --self-test
python3 Build/Scripts/check-third-party-binaries.py
```

## Shipping game build

`Build-ShippingGame.ps1` (see [Scripts](#build-shippinggameps1)) packages ProjectSandbox as a Win64 Shipping game. It takes as long as a full cook, so it runs in the nightly workflow and by hand, not on pull requests: PR CI has one shared self-hosted UE runner. The manual command, on a machine with UE 5.7 and the o3ds core built:

```powershell
.\Build\Scripts\Sync-O3DSCore.ps1
.\Build\Scripts\Build-ShippingGame.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7"
```

The same without the script:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development "-Project=$PWD\ProjectSandbox\ProjectSandbox.uproject" -WaitMutex
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun "-project=$PWD\ProjectSandbox\ProjectSandbox.uproject" -noP4 -unattended -utf8output -platform=Win64 -clientconfig=Shipping -build -nocompileeditor -cook -stage -pak -archive "-archivedirectory=$PWD\Artifacts\ShippingGame"
```

## CI/CD Integration

| Workflow | Runs on | What it does |
|---|---|---|
| `open3dbroadcast-plugin-ci.yml` | PRs to develop/main, pushes to develop/main, manual | Path filter, Fab source zip, copyright header check, runtime-modules-without-editor-code check, the release script's self-test, and on the UE runner (`open3dbroadcast-ue-build-test.yml`): BuildPlugin (fails on plugin warnings), UE automation tests against that package, the WebRTC add-on built against that package (strict, warnings as errors; uploaded as `Open3DBroadcastWebRTC-Win64-<sha>`) and the tests again with both plugins, strict build, BuildPlugin on the Fab zip |
| `open3dbroadcast-ue-build-test.yml` | Called by CI and the release | The UE runner job: BuildPlugin, the tests, the WebRTC add-on and the tests with both plugins, strict build, BuildPlugin on the Fab zip. Uploads the tested package as `Open3DBroadcast-Win64-<sha>`. With a `version` input it first replaces both `.uplugin` files with the release gate's stamped ones. `ue-version` (default `5.7`) picks `C:\Program Files\Epic Games\UE_<ue-version>` and checks it with `Setup-UE.ps1 -EngineVersion`; `profile: reduced` leaves out the strict build and the Fab zip build; `artifact-suffix` keeps two engines' artifacts apart (ADR 0014) |
| `open3dbroadcast-fab-package.yml` | Called by CI, nightly and the release, or manual | `fab-package.py --engine-version <engine-version>` (default `5.7`), then `check-no-video-codecs.sh` on every packaged binary and on the plugin tree (required), then uploads `Open3DBroadcast-Fab-Source-UE<engine>-<sha12>.zip` as the artifact `Open3DBroadcast-Fab-Source-<sha><artifact-suffix>`. The nightly and the release call it once per engine (ADR 0014) |
| `open3dbroadcast-plugin-nightly.yml` | 03:00 UTC daily, manual | Runs once per engine, UE 5.7 then UE 5.8 (ADR 0014; artifacts of 5.8 end in `-UE5.8`). Same checks as CI with a Shipping `-Configuration` (including the WebRTC add-on build and both-plugin test run), plus network tests when the `O3D_MOQ_RELAY_URL` secret is set, the transport flag-combination builds (`Build-FlagCombinations.ps1`; the manual run can skip them with `run_flag_builds`), the Linux exclusion check (`Test-LinuxExclusion.ps1 -Require`; the step shows as skipped while the runner has no Linux toolchain), and the Win64 Shipping game package (`Build-ShippingGame.ps1`; the manual run can skip it with `run_shipping_game`). The flag-combination builds, the Linux check and the Shipping game run on UE 5.7 only. A red scheduled run opens or comments on an issue labelled `nightly-failure`, and the next green scheduled run closes it (WP-R2) |
| `open3dbroadcast-plugin-test.yml` | Manual only | Build any branch on the chosen engine (`ue-version`, 5.7 or 5.8) and optionally run the tests (with or without network tests) |
| `open3dbroadcast-plugin-release.yml` | `open3dbroadcast-v*.*.*` tags; manual for a dry run | The release gate, then the same Fab zip and UE jobs as PR CI on the version-stamped tree, then the GitHub release of the tested package. See "Releases" below |
| `core-tests.yml` | Every PR (core jobs skipped for Markdown/`docs/`-only changes) and pushes | o3ds core under CTest with ASan/UBSan, fuzzing, warning ratchet, MSVC build, `o3ds_generated.h` against `flatc`, and `sync_o3ds_core.py --check` on the plugin's core copy (GitHub-hosted) |
| `o3ds-webrtc-windows-native.yaml` | Manual only | libwebrtc from source (up to 6 hours); nothing consumes its output |

No UE job has a pre-build step: the plugin compiles the o3ds core from source (WP-F1). The PR CI job also checks out without submodules, so it builds exactly what a clean clone has.

### Which PR jobs run, and when (CI-9)

- **Every PR commit, drafts included:** the path filter, the Fab source zip job, the copyright header check, the runtime-modules-without-editor-code check and `core-tests.yml`. They run on GitHub-hosted runners and take a few minutes.
- **Non-draft PRs, pushes to develop/main and manual runs:** the "UE build and tests" job on the single self-hosted `[self-hosted, ue5, windows]` runner. Drafts skip it so unfinished work does not hold the runner. To get the UE result for a draft, mark it ready for review, or run the workflow by hand on the branch (Actions > Open3DBroadcast Plugin CI > Run workflow).
- **Success comments:** every workflow that runs on pull requests (`open3dbroadcast-plugin-ci.yml`, `core-tests.yml`, `repeater-image-test.yml`, and `repeater-image.yml` when enabled) ends with a "Report success on the PR" job. When none of its jobs failed or was cancelled, it posts one comment, "✅ **<workflow> completed successfully!** (head <sha>)", listing the jobs that were skipped (a skipped job counts as passed: for example the UE job on a draft). Whoever watches a PR, a person or an agent's PR monitor, learns of a green run without polling. The plugin CI's UE job still posts its own "build started" and "build failed" comments. Merge only on the checks of the head commit; a comment can be about an older run.
- **Core path filter:** `core-tests.yml` starts on every PR; its "Detect core-relevant changes" job skips the core jobs when a PR changes only Markdown or `docs/`. Pushes to develop/main keep a workflow-level `paths-ignore` for the same files.
- **Required checks on `develop`:** a repository ruleset (2026-10-06) requires two checks, "Plugin CI result" and "Core tests result" (WP-R2). It also blocks deletion and force pushes, allows only squash merges, needs no approval, and has no bypass list. Each is the last job of its workflow (`open3dbroadcast-plugin-ci.yml`, `core-tests.yml`). It always runs and fails when any job of its workflow failed or was cancelled, while a job skipped by its `if:` counts as passed, so the Fab zip, the third-party binaries check, the transport-metrics check and the release script's self-test all gate the merge without being listed one by one. Jobs inside a workflow can be renamed or split without touching branch protection, but the two `result` jobs keep their names: a required check that never reports blocks every PR. Not the Repeater image test: it is path-filtered and does not report on every PR. Leave "require branches to be up to date" off, or every merge re-queues the UE job on the single runner.
- **Path filter:** all plugin CI jobs are skipped when a PR touches nothing the plugin build depends on. The filter covers `Build/**`, both plugins (`Open3DBroadcast` and `Open3DBroadcastWebRTC`), `ProjectSandbox/` project files and the workflow files. The plugin build reads nothing else: a `src/o3ds` change reaches it only with the re-synced core copy inside the plugin, which `core-tests.yml` checks.

### What turns the UE job red

PR CI runs the job twice on the one runner (ADR 0014, WP-V4): the full job on UE 5.7, and a reduced one on UE 5.8 (items 1 to 3 below; its artifacts end in `-UE5.8`). Either one red makes "Plugin CI result" red. The nightly and the release run every engine in full (WP-V5); the manual test workflow runs the engine you pick.

1. BuildPlugin fails, or the compiler reports a warning in a plugin source file (`-FailOnWarnings`).
2. Any automation test fails, no test runs, or no report is written (`Run-AutomationTests.ps1`). The step has a 20-minute timeout; ADR 0006 sets a 15-minute test budget per PR.
3. The WebRTC add-on does not build against the package, warns (strict, `-FailOnWarnings`), or a test fails in the run with both plugins enabled.
4. The strict build (`-StrictIncludes`, no PCH, no unity) fails or warns.
5. BuildPlugin on the Fab zip fails or warns, or an editor DLL is missing from its output.

The Fab zip job is red when a package check or the codec gate fails. The "Copyright headers" job is red when `check-copyright-headers.py` fails, and the "Runtime modules free of editor code" job when `check-runtime-editor-deps.py` or its self-test fails; they are separate jobs, so they do not stop the Fab zip from being built. "Markdown links resolve" is red when `check-markdown-links.py` or its self-test fails.

### Releases

A release is made by pushing a tag `open3dbroadcast-vX.Y.Z` (WP-R2; mid-project review BC-4, BC-5). `open3dbroadcast-plugin-release.yml` then runs:

1. **Release gate** (GitHub-hosted). `Build/Scripts/release-version.py check` reads X.Y.Z from the tag and requires exactly one `## [X.Y.Z]` heading in `CHANGELOG.md` (` - date` may follow). The section must name the versions the release carries: the wire protocol (`protocol 2`), the transport API (`transport API 5`) and the core library (`core 1.1.0`). `release-version.py stamp` then writes `VersionName` X.Y.Z and `Version` X×10000 + Y×100 + Z into both `.uplugin` files (minor and patch 0..99, so the integer grows with the version). The edit is textual, and only those two values change.
2. **Fab source zip and UE build and tests**, the same workflows as PR CI, on the stamped files, once per supported engine (UE 5.7 and UE 5.8, ADR 0014), each in full: warnings as errors, the automation tests alone and with the WebRTC add-on (each at its floor in `automation-test-floors.json`), the strict build, and BuildPlugin on that engine's Fab zip.
3. **Publish**, on a tag push only, when 1 and 2 passed. `release-version.py archives` checks that there is exactly one tested package per engine, that each carries X.Y.Z and that each add-on was built for the same engine, then writes one zip per engine for each plugin: `Open3DBroadcast-Plugin-X.Y.Z-UE<engine>-Win64.zip`, laid out as `UE_<engine>/Plugins/Open3DBroadcast` and holding the package the tests ran against, and `Open3DBroadcastWebRTC-Plugin-X.Y.Z-UE<engine>-Win64.zip`. The GitHub release is named after the tag; its notes are the CHANGELOG section and install steps naming each engine's zip. The WebRTC add-on zips are attached only when the repository variable `O3D_PUBLISH_WEBRTC_ADDON` is `true` (counsel question L1, ADR 0002). The Fab zips (one per engine), the plugin zips and the add-on zips are kept as the run artifact `Open3DBroadcast-Release-X.Y.Z` for 90 days; the Fab zips are uploaded to Fab by hand.

The committed `.uplugin` files keep their placeholder version; only the released files carry X.Y.Z. To release: add the `## [X.Y.Z] - date` section to `CHANGELOG.md` in a PR, merge it, then tag that merge commit on `develop` and push the tag.

**Dry run:** Actions > Open3DBroadcast Plugin Release > Run workflow, on any branch, with a version. It runs the gate (a missing CHANGELOG section is a warning) and the build and tests on every engine, and never publishes, tags or creates a release. It holds the UE runner for two full CI runs.

See `.github/workflows/` for workflow definitions.

### Launch Unreal Editor on a self-hosted runner

We provide a convenience workflow to open the Unreal Editor on a self-hosted machine—useful for local validation runs.

- Workflow: `dev-open-ue-editor` (`.github/workflows/open-ue-editor.yml`)
- Trigger: Manual (workflow_dispatch)
- Inputs:
  - `ue_path` (required): Absolute UE root (e.g., `C:\\Program Files\\Epic Games\\UE_5.7` or `/opt/Unreal/UE_5.7`)
  - `project` (optional): Path to `.uproject` (default: `ProjectSandbox/ProjectSandbox.uproject`)
  - `map` (optional): Map to load (e.g., `/Game/Maps/Example`)
  - `extra_args` (optional): Editor CLI args (default: `-log`)
  - `detach` (optional): Start detached (default: `true`)

Requirements:
- A self-hosted runner on the target machine with Unreal installed and a user session capable of launching GUI apps.
- Windows and Linux are supported; macOS can be added similarly.

Usage:
1. In GitHub → Actions → `dev-open-ue-editor`, click “Run workflow”.
2. Fill in `ue_path` and optionally override `project`, `map`, `extra_args`, or `detach`.
3. Select the appropriate self-hosted runner and run.


## Platforms

The plugin is Win64 only (ADR 0001). `Open3DBroadcast.uplugin` declares it: `"SupportedTargetPlatforms": [ "Win64" ]` for the plugin, and `"PlatformAllowList": [ "Win64" ]` on every module, so a target for another platform leaves the modules out instead of running their `Build.cs` and failing. The runtime modules also have `"TargetDenyList": [ "Server", "Program" ]` (see the plugin README, "Platforms and target types"). `fab-package.py` fails when a module entry has no `PlatformAllowList`.

The `Build.cs` files no longer throw for other platforms, except Open3DSender and Open3DReceiver, which link Win64-only prebuilt core libraries until WP-F1; the allow list keeps them out of other targets. NNG, MoQ and the WebRTC add-on's module build as stubs on any platform without their prebuilt binaries.

**Linux check.** The acceptance item "a game target that also targets Linux configures without error, with the plugin excluded on Linux" needs UBT to build a Linux target, which on Windows needs Epic's Linux cross-compile toolchain (`LINUX_MULTIARCH_ROOT`) and an engine with the Linux target platform installed. The nightly runs the check when `LINUX_MULTIARCH_ROOT` is set on the runner and otherwise skips it with a notice; whether the runner has the toolchain is not known. To run it by hand on a machine that has both:

```powershell
.\Build\Scripts\Test-LinuxExclusion.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7" -Require
```

Record the result in the PR. The check has not been run yet.

### Script platforms

| Script | Windows | Linux/Mac |
|--------|---------|-----------|
| Setup-UE | ✅ | ❌ |
| Build-Plugin | ✅ | ❌ |
| Run-AutomationTests | ✅ | ❌ |

**Note:** Linux/Mac support can be added by creating bash equivalents of the PowerShell scripts.

## Requirements

- **Windows**: PowerShell 5.1+ (or PowerShell Core 7+)
- **Unreal Engine**: 5.7 or 5.8 (ADR 0014). The source `.uplugin` files carry no `EngineVersion`: BuildPlugin stamps a package with the engine that built it, and `fab-package.py --engine-version` stamps the Fab zip. Use one worktree per engine, since `Binaries/` and `Intermediate/` are per-engine output.
- **Python**: 3.8+ for `fab-package.py`, `check-copyright-headers.py`, `check-runtime-editor-deps.py`, `check-third-party-binaries.py` and `check-markdown-links.py`
- **Visual Studio**: 2022 (for building)
- **Git**: For repository operations

## Troubleshooting

### "RunUAT not found"
- Verify UE installation path
- Ensure UE includes Engine/Build directory
- Run Setup-UE.ps1 first

### "Plugin file not found"
- Check plugin path is correct
- Ensure you're running from repository root
- Verify `ProjectSandbox/Plugins/Open3DBroadcast/Open3DBroadcast.uplugin` exists

### Test failures
- Check test logs in ResultsDir
- Ensure plugin is properly linked
- Verify project opens in UE Editor without errors

## Contributing

When adding new scripts:
1. Follow existing naming conventions
2. Add comprehensive parameter help
3. Include error handling
4. Update this README
5. Test on clean environment

## Resources

- [Unreal Automation Tool (UAT)](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-automation-tool-for-unreal-engine); `RunUAT BuildPlugin -help` lists the BuildPlugin options
- [Automation System Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-system-overview-in-unreal-engine)
- [Automation Test Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine)
