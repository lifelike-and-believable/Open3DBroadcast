# Open3DBroadcast

Unreal Engine 5.7 plugins that stream skeletal animation, curves, audio and control data between
engines and tools (Open3DStream protocol): the `Open3DBroadcast` plugin, the
`Open3DBroadcastWebRTC` add-on, and the engine-agnostic core library in `src/o3ds`. This file is
the one instruction file for coding agents (Claude Code reads it through `CLAUDE.md`, Copilot
reads it directly). Rules for Claude Code are in `.claude/rules/` (rules-for-robots core and
`unreal-plugin` packs, plus project rules prefixed `O3D-`).

## Where things are

- Plugins: `ProjectSandbox/Plugins/Open3DBroadcast/`, `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/`. Host project: `ProjectSandbox/ProjectSandbox.uproject`.
- Core library: `src/o3ds/`, schema `src/o3ds.fbs` (data) and `src/o3ds_control.fbs` (control channel), tests in `test/`.
- Decisions: `docs/adr/`. Plan: `docs/roadmap/plugin-hardening-and-fab-readiness.md`. Current state and the next steps: `docs/roadmap/handoff/HANDOFF.md` (read §0a first). Wire format: `docs/wire-format.md`. Build and CI: `Build/README.md`.

## Commands

- UE build (UE 5.7 only): `"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development "-Project=<repo>\ProjectSandbox\ProjectSandbox.uproject" -WaitMutex`
- UE automation tests: `Build/Scripts/Run-AutomationTests.ps1 -UEPath "C:\Program Files\Epic Games\UE_5.7" -ProjectFile <repo>\ProjectSandbox\ProjectSandbox.uproject -TestFilter Open3DBroadcast` (decide pass or fail from its report, not the editor's exit code).
- Before a local UE build in a worktree that was built before, delete `ProjectSandbox/Intermediate/Build/Win64/x64/ProjectSandboxEditor/Development/Makefile.bin` when files were added, removed, merged in or changed by a branch switch: UBT can otherwise reuse a stale makefile that skips new files or still includes deleted ones, and after a failed build the tests run the old binaries. Check that the build succeeded and that the test count is what you expect. CI enforces a floor (`Build/automation-test-floors.json`); raise it in the PR that adds tests.
- Checks run by CI, all cheap: `python3 Build/Scripts/check-copyright-headers.py`, `python3 Build/Scripts/check-runtime-editor-deps.py`, `python3 Build/Scripts/check-third-party-binaries.py`, `python3 Build/Scripts/fab-package.py --out-dir <scratch>`, `bash Build/Scripts/check-no-video-codecs.sh`, `python3 Build/Scripts/sync_o3ds_core.py --check` (needs the submodules).
- Core library: build and test as `.github/workflows/core-tests.yml` does (CMake with `-DO3DS_BUILD_TESTS=ON`, then `ctest`).
- Verify before reporting a change as done: the UE build and the automation tests for plugin changes, the core tests for `src/` changes, and the checks above. Include their output. A plugin change that only CI can build says so.

## Project decisions

- Unreal Engine 5.7 only, Win64 only (ADR 0001). The main plugin is published on Fab; the WebRTC add-on is distributed from the Open3DBroadcast website to registered plugin users, not through Fab, so Fab's rules (no dependency on user-made plugins) do not apply to it (maintainer, 2026-10-05). Target.cs files use `BuildSettingsVersion.V6` and `EngineIncludeOrderVersion.Unreal5_7` (rule O3D-001).
- Copyright: new plugin source files under `Source/` (outside `ThirdParty/`, not generated) start with `// Copyright <year> Lifelike & Believable. All Rights Reserved.` (the current year; Fab TR 4.3.6.1.b wants publisher and year) and a blank line. Files that came from Open3DStream carry `// Portions Copyright (c) Open3DStream Contributors` as the second line instead of the blank line; don't add it to new files (rule O3D-002).
- Schema: `src/o3ds.fbs` and `src/o3ds_control.fbs` are authoritative. Never reorder or delete fields. After a change, regenerate with `flatc --cpp -o src src/o3ds.fbs` and `flatc --cpp -o src src/o3ds_control.fbs` using flatc from the `thirdparty/flatbuffers` pin, then run `python3 Build/Scripts/sync_o3ds_core.py`.
- Core mirror (ADR 0003): the plugin compiles a generated copy of the core in `Source/ThirdParty/Open3DStreamCore/`. After changing `src/o3ds`, the generated headers or the flatbuffers/crccpp pins, run `sync_o3ds_core.py` and commit the result in the same PR; never edit the copy. A new core header the plugin includes goes in `Build/o3ds-core-manifest.txt`. The copy builds without exceptions or RTTI; an out-of-line class or function the plugin uses needs `O3DS_API`.
- Versioning (ADR 0009): bump `O3DS_VERSION_TAG` per the table in `docs/wire-format.md` section 8. Any wire change gets a "Schema/Protocol" entry in `CHANGELOG.md` (the only changelog) with the protocol version, the `min_reader_version` of affected frames, compatibility in both directions, and migration steps.
- No compatibility with formats from before protocol 2: nobody runs older receivers. Don't add shims for them.
- Quantization is never switched on implicitly. Defaults change only after real takes and a live test by the maintainer.
- Threads: nothing blocks the game thread; network I/O and encoding run asynchronously. Capture happens on the game thread after animation evaluation.
- No hard-coded credentials, ports or absolute paths in source control. Hot paths stay quiet in logs: state changes and errors only.
- Tests (ADR 0006): the main plugin's UE tests live in the editor-only `Source/Open3DBroadcastTests/Private/<Area>/` (the add-on's in `Source/Open3DTransportWebRTC/Private/Tests/`), named `Open3DBroadcast.<Area>.<Unit>.<Case>`, wrapped in `#if WITH_DEV_AUTOMATION_TESTS`, with white-box access through the owning module's `Public/Testing/*.h`. A new transport registers a conformance profile. Internet tests register only with `O3DB_NETWORK_TESTS=1` (rule O3D-003).
- Docs describe the intended behaviour. When code and docs disagree, fix the code unless the docs are demonstrably wrong; then fix the docs and say why in the commit.
- Rename or move files with `git mv`; don't delete and recreate them.

## External sources of truth

- Unreal APIs: the installed UE 5.7 engine source (`C:\Program Files\Epic Games\UE_5.7\Engine\Source`), or `lifelike-and-believable/UnrealEngine` on GitHub. Compiler output beats memory.
- O3DS data model: `src/o3ds/model.h` (`Subject`, `SubjectList`, `Transform`, `TransformList`). Read it before using a member.
- LiveKit FFI: the header the add-on ships (`ProjectSandbox/Plugins/Open3DBroadcastWebRTC/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h`) and the FFI source, `lifelike-and-believable/livekit-ffi`. Check what the FFI does at runtime (which callback fires in which mode) in its source, not only that a function exists. New FFI APIs are requested from its developers through the maintainer: add them to `docs/livekit_ffi_feature_request.md`.
- A fake of an external library in tests models the real library's behaviour; cite the source it follows.

## Git and pull requests

- Branch from `develop`; one work package (WP) per PR. Titles start with the WP id and list finding ids, for example `WP-F7: editor module split (ADR 0010; FAB-7, SND-34)`. Update `CHANGELOG.md` and the affected docs in the same PR.
- Never rebase or force-push a pushed branch. To update one, merge `develop` into it.
- Merge pull requests into `develop` by squash (`gh pr merge <n> --squash`), only when every check on the head commit has passed or been skipped.
- Release tags are `open3dbroadcast-vX.Y.Z` (the plugin release workflow runs on them).
- Line endings: `core.autocrlf` is true and many files are CRLF. Edit them with tools that keep the file's line endings (not `sed -i` in Git Bash), and check `git diff --numstat` for whole-file rewrites before committing. A file whose committed blob is CRLF is staged with `git -c core.autocrlf=false add`.
- If git reports "dubious ownership", pass `safe.directory` for that one command (`GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=*`); never change the global git config.
- Never run two Unreal builds at once on one machine; wait for any running build (including other projects') to finish.
- `HANDOFF.md` status changes go in one docs PR at the end of a batch. Don't edit the long "Start here" line in its §0.
- Anything that needs a person at the desk (live servers, hardware, takes) is reported as not verified and listed in HANDOFF's "Next" section.
