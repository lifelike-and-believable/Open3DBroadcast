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

## The o3ds core in the plugin

#### `sync_o3ds_core.py`
Generates the plugin's copy of the o3ds core (docs/adr/0003-core-library-delivery-to-plugin.md). Python 3.8+, standard library only; needs the `thirdparty/flatbuffers` and `thirdparty/crccpp` submodules.

```bash
git submodule update --init thirdparty/flatbuffers thirdparty/crccpp
python3 Build/Scripts/sync_o3ds_core.py           # rewrite the copy
python3 Build/Scripts/sync_o3ds_core.py --check   # compare only (CI)
```

- **Input:** `Build/o3ds-core-manifest.txt` lists the core headers that plugin code includes. The script adds the `.cpp` next to each header and follows `#include` lines to the full closure.
- **Output:** `ProjectSandbox/Plugins/Open3DBroadcast/Source/ThirdParty/Open3DStreamCore/`: the closure (byte-for-byte copies of `src/o3ds` files), `src/o3ds_generated.h`, the FlatBuffers runtime headers and CRC++'s `CRC.h` from the submodule pins, their licences under `LICENSES/`, and `SYNC_STAMP.txt` (O3DS_VERSION_TAG, FlatBuffers version, submodule pins, manifest and content hashes). It also writes one `Source/Open3DStreamCore/Private/Core/O3DSCore_<file>.cpp` per mirrored `.cpp`; each includes the mirrored file between `O3DSCoreSourceBegin.h` and `O3DSCoreSourceEnd.h`, which switch compiler warnings off for the core. The copy sits outside the module folder because UBT compiles every `.cpp` in a module folder.
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
Runs `RunUAT BuildPlugin` on the contents of the Fab source zip (see "Fab source package" below), with `-FailOnWarnings`, then checks that every module in the zip's `.uplugin` produced an editor DLL and that no excluded module or `livekit` file reached the output.

```powershell
.\Build\Scripts\Build-FabZip.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -Zip "Artifacts\Fab" `
  -WorkDir "Artifacts\FabBuild" `
  -RequireStandalone
```

Nothing is added to the extracted zip before BuildPlugin: since WP-F1 the zip builds on its own, exactly as Fab's farm receives it. `-RequireStandalone` (passed by CI and the nightly) first checks that the zip holds `Source/Open3DStreamCore` and its core copy in `Source/ThirdParty/Open3DStreamCore`, and no prebuilt `open3dstreamstatic`/`flatbuffers.lib`, and fails with that reason instead of a compiler error.

#### `Build-FlagCombinations.ps1`
Runs `Build-Plugin.ps1` once per transport build-flag combination (WP-F2, TRB-24, TRF-27): each of `O3D_WITH_TRANSPORT_SOCKETS`, `_NNG`, `_WEBRTC` and `_MOQ` set to `0` on its own (`no-sockets`, `no-nng`, `no-webrtc`, `no-moq`), then all four at once (`loopback-only`). The flags are described in the plugin README, "Build flags". Every combination runs even after a failure; the script exits `1` if any failed and prints a summary. The nightly workflow runs it with `-StrictIncludes -FailOnWarnings`; it is not part of PR CI because each combination takes as long as the PR build.

```powershell
.\Build\Scripts\Build-FlagCombinations.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "$PWD\ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutRoot "$PWD\Artifacts\FlagBuilds" `
  -Only no-moq `
  -StrictIncludes -FailOnWarnings
```

`-Only` takes one or more combination names; without it all five run. Packages and logs go to `<OutRoot>\<name>` and `<OutRoot>\<name>-BuildPlugin.log`.

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

Run `Sync-O3DSCore.ps1` first, as for any build of the plugin. `-ProjectFile` selects another `.uproject` that enables the plugin.

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

Tests that need the internet register only when `O3DB_NETWORK_TESTS=1` is set in the environment (ADR 0006). CI sets it to `0` on pull requests.

**Exit code:** `0` when the editor exited cleanly and every test in the report passed (skipped tests are listed but allowed). `1` when the editor exited non-zero, `index.json` is missing or unreadable, no test ran, or any test is `Fail`, `NotRun` or `InProcess` (still running when the editor exited, usually a crash). `2` for bad arguments.

**Output (in `<ResultsDir>\<filter>\`):**
- `index.json` - The automation report
- `Automation.log` - The editor log
- Failed tests and their first error are printed as GitHub error annotations and added to the job summary

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

- `Build/Fab/exclude-modules.txt` lists modules left out of it: `Open3DTransportWebRTC` (ADR 0002; WebRTC ships as a separate add-on) and `Open3DBroadcastTests` (ADR 0006; skipped with a notice until that module exists).
- `Build/Fab/exclude-files.txt` lists files left out, as globs: the module-level `Source/*/*.md` READMEs and USER_GUIDEs, which stay in the repository for people reading the code. It has no `.pdb`, `.py` or developer-note rule on purpose: those fail the tree check below instead (see [Debug symbols](#debug-symbols); developer notes and scripts live in `docs/dev/<Module>/`).
- `Build/Scripts/fab-package.py` (Python 3.8+, standard library) takes the files git tracks under the plugin folder, applies both lists, removes the excluded modules' entries from the staged `.uplugin` without reformatting it, and writes a zip with one top-level `Open3DBroadcast/` folder and fixed timestamps (the same commit gives the same bytes). It then reopens the zip and fails if it finds an excluded module (folder or `.uplugin` entry), a `livekit` file while WebRTC is excluded, `.pdb`/`.py` files, `Binaries/` or `Intermediate/`, a Markdown file other than the root `README.md`, `USER_GUIDE.md`, `THIRD_PARTY_LICENSES.md`, `Transport_Module_Comparison.md` or anything under a `ThirdParty/` folder, a listed module without its `Build.cs`, a module without a `PlatformAllowList` (or one naming a platform the plugin's `SupportedTargetPlatforms` does not list; ADR 0001), a missing `Resources/Icon128.png`, `Config/FilterPlugin.ini` or `Source/ThirdParty/Open3DStreamCore/SYNC_STAMP.txt`, a prebuilt `open3dstreamstatic` or `flatbuffers.lib` (WP-F1), or a top-level file or folder outside `Binaries`, `Config`, `Content`, `Resources`, `Shaders` and `Source` that `Config/FilterPlugin.ini` does not list, since BuildPlugin would leave it out (FAB-2). Before any exclusion it also checks the tracked tree, including excluded modules, and fails if git tracks a `.pdb` (FAB-4) or a `.py`/`.pyc` anywhere under the plugin, or a Markdown file under `Source/` other than a `README.md` or `USER_GUIDE.md` outside `ThirdParty/` (HYG-1).

```bash
python3 Build/Scripts/fab-package.py --out-dir Artifacts/Fab
Build/Scripts/check-no-video-codecs.sh $(cat Artifacts/Fab/binaries.txt)
```

Then `Build-FabZip.ps1` (above) runs BuildPlugin on the zip's contents.

### Debug symbols

Debug symbols (`.pdb`) for the plugin's prebuilt third-party DLLs (`moq_ffi.dll`, `livekit_ffi.dll`) are not kept in the plugin tree and are not staged into packaged games (FAB-4, WP-F3).

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

Every `.h`, `.cpp` and `.cs` file under the plugin's `Source/` starts with a copyright line (FAB-5, FAB-9, WP-F4). New files, and files that had no notice, use this one, followed by a blank line:

```cpp
// Copyright Lifelike & Believable. All Rights Reserved.
```

No year, as in Epic's own headers. In headers, `#pragma once` comes after the blank line.

Files that already carried `// Copyright (c) Open3DStream Contributors` keep that line unchanged; the maintainer decided not to replace it. Don't add it to new files.

- **Not covered:** anything under a `ThirdParty/` directory (vendored nng, moq-ffi, livekit_ffi headers keep their own notices) and generated files (`*.generated.h`, `*_generated.h`, `*.gen.cpp`, or a file whose first ten lines say `@generated`, `automatically generated`, `auto-generated` or `DO NOT EDIT`).
- **Third-party code outside `ThirdParty/`:** keep its original notice and add the file to `Build/Fab/copyright-allowlist.txt`, one line per file: the path relative to the plugin root, then the reason (licence and origin). The list is empty today.
- **Check:** `Build/Scripts/check-copyright-headers.py` (Python 3.8+, standard library) reads the files git tracks under `Source/`, ignores a leading UTF-8 BOM and accepts LF or CRLF. It exits `0` when every checked file starts with one of the two lines, `1` when a file does not (it lists each file and its first line) or an allowlist entry names a file git does not track, and `2` for bad input. `-v` also lists the skipped ThirdParty files.

```bash
python3 Build/Scripts/check-copyright-headers.py
```

## Runtime modules without editor code

Editor UI lives in Editor-type modules: `Open3DBroadcastEditor` (Details customization, LiveLink creation panel, transport settings panels; ships in the Fab package) and `Open3DBroadcastTests` (ADR 0010, WP-F7). The runtime modules (every `.uplugin` entry whose `Type` is `Runtime` or another runtime type) must not use editor or Slate code, so that a packaged game builds and the Server option of ADR 0001 stays open.

- **Rule:** a runtime module's `Build.cs` names none of `UnrealEd`, `PropertyEditor`, `Slate`, `SlateCore`, `EditorStyle`, `ToolMenus`, `AppFramework` and the other editor modules listed in the script, anywhere in the file (a `Target.bBuildEditor` block included). Its sources include no editor-only header (`Editor.h`, `ScopedTransaction.h`, `PropertyEditorModule.h`, `IDetailCustomization.h`, `Widgets/...`, `Framework/Application/...`, and the others listed in the script), `WITH_EDITOR`-guarded or not. `InputCore` is allowed; it is a runtime module.
- **Transport settings:** a transport describes its options as data (`FO3DTransportOptionSchema`, `Open3DShared/Public/O3DTransportOptionSchema.h`) in its sender and receiver customizations. `Open3DBroadcastEditor` builds the panel from it. A transport module never builds widgets.
- **Check:** `Build/Scripts/check-runtime-editor-deps.py` (Python 3.8+, standard library) reads the `.uplugin`, then each runtime module's `Build.cs` string literals (comments ignored) and `#include` lines (ThirdParty skipped). It exits `0` with no violation, `1` with one or more (each printed as `path:line`), and `2` for bad input. `--self-test` runs it against a generated clean plugin and a generated bad one. A line that must stay can carry `o3d-allow-editor-dependency: <reason>`; nothing uses that today.

```bash
python3 Build/Scripts/check-runtime-editor-deps.py --self-test
python3 Build/Scripts/check-runtime-editor-deps.py
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
| `open3dbroadcast-plugin-ci.yml` | PRs to develop/main, pushes to develop/main, manual | Path filter, Fab source zip, copyright header check, runtime-modules-without-editor-code check, and on the UE runner: BuildPlugin (fails on plugin warnings), UE automation tests against that package, strict build, BuildPlugin on the Fab zip |
| `open3dbroadcast-fab-package.yml` | Called by CI and nightly, or manual | `fab-package.py`, then `check-no-video-codecs.sh` on every packaged binary (required), then uploads `Open3DBroadcast-Fab-Source-<sha>` |
| `open3dbroadcast-plugin-nightly.yml` | 03:00 UTC daily, manual | Same checks as CI with a Shipping `-Configuration`, plus network tests when the `O3D_MOQ_RELAY_URL` secret is set, the transport flag-combination builds (`Build-FlagCombinations.ps1`; the manual run can skip them with `run_flag_builds`), the Linux exclusion check (`Test-LinuxExclusion.ps1`, skipped while the runner has no Linux toolchain), and the Win64 Shipping game package (`Build-ShippingGame.ps1`; the manual run can skip it with `run_shipping_game`) |
| `open3dbroadcast-plugin-test.yml` | Manual only | Build any branch and optionally run the tests (with or without network tests) |
| `open3dbroadcast-plugin-release.yml` | `open3dbroadcast-v*.*.*` tags, manual | Shipping build, GitHub release of the Win64 binaries (UE 5.7 only) |
| `core-tests.yml` | Every PR and push | o3ds core under CTest with ASan/UBSan, fuzzing, warning ratchet, MSVC build, `o3ds_generated.h` against `flatc`, and `sync_o3ds_core.py --check` on the plugin's core copy (GitHub-hosted) |
| `o3ds-webrtc-windows-native.yaml` | Manual only | libwebrtc from source (up to 6 hours); nothing consumes its output |

No UE job has a pre-build step: the plugin compiles the o3ds core from source (WP-F1). The PR CI job also checks out without submodules, so it builds exactly what a clean clone has.

### Which PR jobs run, and when (CI-9)

- **Every PR commit, drafts included:** the path filter, the Fab source zip job, the copyright header check, the runtime-modules-without-editor-code check and `core-tests.yml`. They run on GitHub-hosted runners and take a few minutes.
- **Non-draft PRs, pushes to develop/main and manual runs:** the "UE build and tests" job on the single self-hosted `[self-hosted, ue5, windows]` runner. Drafts skip it so unfinished work does not hold the runner. To get the UE result for a draft, mark it ready for review, or run the workflow by hand on the branch (Actions > Open3DBroadcast Plugin CI > Run workflow).
- **Path filter:** all plugin CI jobs are skipped when a PR touches nothing the plugin build depends on. The filter covers `Build/**`, the plugin, `ProjectSandbox/` project files and the workflow files. The plugin build reads nothing else: a `src/o3ds` change reaches it only with the re-synced core copy inside the plugin, which `core-tests.yml` checks.

### What turns the UE job red

1. BuildPlugin fails, or the compiler reports a warning in a plugin source file (`-FailOnWarnings`).
2. Any automation test fails, no test runs, or no report is written (`Run-AutomationTests.ps1`). The step has a 20-minute timeout; ADR 0006 sets a 15-minute test budget per PR.
3. The strict build (`-StrictIncludes`, no PCH, no unity) fails or warns.
4. BuildPlugin on the Fab zip fails or warns, or an editor DLL is missing from its output.

The Fab zip job is red when a package check or the codec gate fails. The "Copyright headers" job is red when `check-copyright-headers.py` fails, and the "Runtime modules free of editor code" job when `check-runtime-editor-deps.py` or its self-test fails; they are separate jobs, so they do not stop the Fab zip from being built.

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

The `Build.cs` files no longer throw for other platforms, except Open3DSender and Open3DReceiver, which link Win64-only prebuilt core libraries until WP-F1; the allow list keeps them out of other targets. NNG, WebRTC and MoQ build as stubs on any platform without their prebuilt binaries.

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
- **Unreal Engine**: 5.7 (the plugin's `EngineVersion`)
- **Python**: 3.8+ for `fab-package.py`, `check-copyright-headers.py` and `check-runtime-editor-deps.py`
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

- [Unreal Automation Tool (UAT)](https://docs.unrealengine.com/5.4/en-US/unreal-automation-tool-in-unreal-engine/)
- [BuildPlugin Command](https://docs.unrealengine.com/5.4/en-US/using-the-buildplugin-command-in-unreal-engine/)
- [Automation Testing](https://docs.unrealengine.com/5.4/en-US/automation-system-overview-in-unreal-engine/)
