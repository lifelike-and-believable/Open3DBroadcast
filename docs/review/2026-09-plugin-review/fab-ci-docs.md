# Open3DBroadcast: Fab readiness, build/CI/packaging and documentation review

- Repo: `/home/user/Open3DBroadcast` @ `7aea235`. Plugin root: `ProjectSandbox/Plugins/Open3DBroadcast/` (paths below are relative to the repo root unless they start with `Plugin/`, which means the plugin root).
- Review date: 2026-09-29. This was a read-only review; no repository files were changed. I could not compile anything here (Linux container, no UE install), so every build-behaviour claim comes from reading the code. Items marked **verify** need a CI log or a local BuildPlugin run to confirm.

## Summary

The plugin is **not ready for Fab submission.** It is well structured for internal Win64 use, and the licensing inventory (`THIRD_PARTY_LICENSES.md`) is unusually honest and thorough. But five problems block submission outright:

1. **An unresolved copyleft/patent problem in `livekit_ffi.dll`.** It statically links ffmpeg (LGPL) and OpenH264. The repo documents this itself, and its own check script fails by design.
2. **The packaged plugin is likely to fail to compile, and CI hides the failure.** Plugin-root `ThirdParty/` is outside the standard BuildPlugin include set and there is no `Config/FilterPlugin.ini`. `open3dstreamstatic.lib` and its headers are gitignored build outputs. `Build-Plugin.ps1` falls back to a non-BuildPlugin path whenever UAT fails, and still exits 0.
3. **No module declares `PlatformAllowList`.** Five of the eight Build.cs files throw a `BuildException` on any platform other than Win64. Enabling the plugin in a project that also targets Mac, Linux or Android breaks that whole build.
4. **Debug symbols (`.pdb`) ship.** Two of them (about 22 MB) sit inside `Source/`, and the MoQ one is also staged into every packaged game.
5. **Missing copyright headers.** 104 of 147 source files have none.

Beyond the blockers:

- **CI:** tests only run nightly. The feature-branch test job's condition compares a boolean to a string, so it never runs. The Gauntlet config filters on a test prefix that doesn't exist. CI covers only UE 5.7 and has no warnings-as-errors, strict-includes or non-unity build.
- **Docs:** the root README is largely stale. It describes the removed Open3DStream plugin, libdatachannel, a QUIC/MsQuic transport that doesn't exist, and 11 dead links. The plugin README claims "Marketplace Ready / no pre-build steps", which is false. USER_GUIDE omits MoQ and documents Blueprint events that don't exist.
- **Usability:** there is no sample content inside the plugin (all demo maps live in the sandbox project), no `UDeveloperSettings`, a very thin Blueprint API, and no supported-platform or engine-version statement for end users.

### Counts by severity

| Severity | Count |
|---|---|
| critical | 6 |
| high | 9 |
| medium | 15 |
| low | 18 |
| **total** | **48** |

### Fab submission checklist

| Requirement | Status | Evidence |
|---|---|---|
| Plugin compiles with `RunUAT BuildPlugin` for the target engine version, with no errors | **fail / verify** | `Build/Scripts/Build-Plugin.ps1:306-320` falls back and exits 0 when BuildPlugin fails. Root `ThirdParty/` is not packaged (no FilterPlugin.ini), and the Build.cs files throw when libs are missing (`Open3DSender.Build.cs:38-45`). See FAB-2 and CI-1. |
| No consequential warnings | unknown | No `-WarningsAsErrors` or strict build in CI (`Build-Plugin.ps1:288-297`). o3ds headers are added as non-system includes and never wrapped in `THIRD_PARTY_INCLUDES_START` (0 occurrences). |
| Source-only submission (no Binaries/Intermediate) | unknown | There is no Fab-zip job. The release workflow zips BuildPlugin *output*, which includes binaries (`open3dbroadcast-plugin-release.yml:152-193`). |
| `PlatformAllowList`/`PlatformDenyList` on each module | **fail** | `Open3DBroadcast.uplugin:21-62`: no module has either key. |
| Builds or cleanly excludes on every platform listed on Fab | **fail** | `BuildException` on non-Win64 in Sender/Receiver/NNG/WebRTC/MoQ Build.cs. |
| `.uplugin` metadata complete (FriendlyName, Description, Category, CreatedBy, CreatedByURL, DocsURL, SupportURL, EngineVersion) | partial | All present (`uplugin:3-13`). But SupportURL is a website, not a support contact, and `MarketplaceURL` is empty (`:12`). |
| `EngineVersion` matches the target version | pass (5.7 only) | `uplugin:3` is `"5.7.0"`. No other version is built in CI. |
| `Resources/Icon128.png` present, 128x128 | **pass** | `Plugin/Resources/Icon128.png` is PNG 128x128 RGBA. The SVG source is also present (harmless; could be excluded). |
| Copyright notice at the top of every source file | **fail** | 104 of 147 `.h/.cpp/.cs` files have no copyright line in their first 3 lines (for example every file under `Source/Open3DTransportMoQ/`). |
| Third-party software declared, with licenses shipped | partial | `THIRD_PARTY_LICENSES.md` is thorough. But livekit_ffi has no license text (`:12-16`), the Opus version and provenance are unknown (`:104-109`), and mlkem-native is unattributed (`:166-168`). |
| No copyleft or patent-encumbered code without clearance | **fail** | ffmpeg and OpenH264 inside `livekit_ffi.dll` (`THIRD_PARTY_LICENSES.md:24-54`). `check-no-video-codecs.sh` "currently fails by design" (`:8-12`). |
| No `.pdb` or debug artifacts shipped | **fail** | `moq_ffi.pdb` (7.0 MB) and `livekit_ffi.pdb` (15.4 MB) are under `Source/`. The MoQ pdb is added to RuntimeDependencies (`Open3DTransportMoQ.Build.cs:92-97`). |
| No dev/test/planning files in the shipped tree | **fail** | 9 planning or review `.md` files and `Tests/mock-token-server.py` under `Source/`. |
| No absolute paths | partial | None in Source code. `E:\OtherProjects\...` appears in `Source/Open3DTransportMoQ/CLOUDFLARE_RELAY_TESTING.md:78` (ships). `D:/P4_PD/...` in `package.py:87` (doesn't ship). |
| All content inside the plugin folder | pass | The plugin needs nothing outside its folder at runtime. Sample content is in the project, not the plugin. |
| Editor code confined to Editor modules | partial | Detail customizations live in Runtime modules, guarded by `WITH_EDITOR` (`Open3DSenderModule.cpp:4-8,23-29`). This compiles, but it is not the recommended structure. |
| Documentation URL reachable and accurate | unknown / partial | DocsURL is a GitHub blob (`uplugin:11`); I don't know whether the repo is public. The docs have accuracy problems (see DOC-*). |
| No engine modifications, no dependency on other marketplace plugins | pass | Only the engine-bundled LiveLink plugin (`uplugin:18-20`). |
| Example or demo content | fail (usability) | `CanContainContent: true`, but the plugin has no `Content/`. Demo maps are only in `ProjectSandbox/Content`. |

### Top blockers

1. FAB-1: ffmpeg and OpenH264 in `livekit_ffi.dll`, with no clearance.
2. FAB-2 / CI-1: root `ThirdParty/` and the docs are dropped by BuildPlugin (no FilterPlugin.ini), `open3dstreamstatic.lib` is not in git, and the build-script fallback hides the failure.
3. FAB-3: no `PlatformAllowList`, and the Build.cs files throw on non-Win64.
4. FAB-4: `.pdb` files ship, and one is staged into packaged games.
5. FAB-5: 104 of 147 source files have no copyright header.

### Sources used for Fab requirements

Live fetches of `support.fab.com`, `dev.epicgames.com` and `unrealengine.com` were **blocked by the sandbox egress proxy** (EGRESS_BLOCKED), so I relied on the following:

- Search-result snippets from the official "(Fab) Technical Requirements" page, https://support.fab.com/s/article/FAB-TECHNICAL-REQUIREMENTS (retrieved via WebSearch on 2026-09-29):
  - "Fab .uplugin descriptors must have a 'PlatformAllowList' or 'PlatformDenyList' key in each module".
  - "For folders in the overarching plugin folder meant for distribution besides the Content, Resources, or Source folders (like Docs folders), there must exist a Config folder in which there is a 'FilterPlugin.ini'".
  - "Code Plugins must contain at least one code module".
- "(Fab) Plugin Compilation Environment", https://support.fab.com/s/article/Fab-Plugin-Compilation-Environment. The title and existence are confirmed; the contents were not fetchable.
- https://github.com/MuddyTerrain/unreal-ci-cd-for-fab (fetched): "Code plugins must generate no errors or consequential warnings" and must be submitted as source, not binaries.
- https://github.com/metyatech/fab-plugin-release-tools (fetched): its checks include copyright statements in all source files, third-party declarations, rejected file types, a real UAT BuildPlugin run, and a unity-build check.
- Background knowledge of the legacy Unreal Marketplace guidelines, which Fab inherits for code plugins: Icon128.png, DocsURL and SupportURL, no Binaries/Intermediate, copyright headers, and editor code in Editor modules. Please re-verify against the live Fab page before submitting.

---

## Findings

### FAB-1: livekit_ffi.dll ships statically linked ffmpeg (LGPL-2.1) and OpenH264 (H.264 patents)
- Category: licensing
- Severity: critical
- Location: Plugin/THIRD_PARTY_LICENSES.md:24-54; Plugin/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.dll; Build/Scripts/check-no-video-codecs.sh:8-12; docs/webrtc-codec-removal-plan.md:3-9
- Evidence: The repo's own inventory says "livekit_ffi.dll statically links ffmpeg and OpenH264 … This cannot be closed by documentation." The codec-check script "CURRENTLY FAILS BY DESIGN". The removal plan says "plan only — no build has been attempted." Neither component is reachable from the plugin's API, but LGPL obligations attach to distribution.
- Recommendation: Do one of the following. (a) Rebuild livekit_ffi against libwebrtc with H.264 and ffmpeg disabled (per the plan), then wire `check-no-video-codecs.sh` into CI as a required gate. (b) Exclude `Open3DTransportWebRTC` from the Fab SKU until the rebuild is done. Get counsel sign-off in either case.
- Effort: L
- Owner: design

### FAB-2: Plugin-root ThirdParty/, docs and licenses are not packaged by BuildPlugin (no Config/FilterPlugin.ini), so the packaged plugin can't compile
- Category: fab-readiness
- Severity: critical
- Location: Plugin/ (no `Config/` directory); Plugin/ThirdParty/{flatbuffers,opus,open3dstream}; Plugin/Source/Open3DSender/Open3DSender.Build.cs:17-45; Plugin/Source/Open3DReceiver/Open3DReceiver.Build.cs:17-45; Plugin/Source/Open3DShared/Open3DShared.Build.cs:17-53
- Evidence: Sender and Receiver throw `BuildException` when `ThirdParty/flatbuffers/lib/Win64/flatbuffers.lib` or `open3dstreamstatic.lib` is missing. Those files, plus `README.md`, `USER_GUIDE.md`, `LICENSE`, `THIRD_PARTY_LICENSES.md` and `Transport_Module_Comparison.md`, sit outside the standard packaged folders (Binaries/Config/Content/Resources/Shaders/Source). Fab's requirement says other folders need `Config/FilterPlugin.ini`. BuildPlugin compiles the *packaged copy* in a HostProject, so the libs will be missing there. **Verify** in a CI log whether BuildPlugin reports "Missing required library" and then the "Attempting project-driven fallback build" warning.
- Recommendation: Either move `ThirdParty/` under `Source/ThirdParty/` (preferred; wrap each lib in its own `Type=External` module), or add `Config/FilterPlugin.ini` with `[FilterPlugin]` entries: `/ThirdParty/...`, `/README.md`, `/USER_GUIDE.md`, `/LICENSE`, `/THIRD_PARTY_LICENSES.md`. Then assert on the package contents in CI.
- Effort: S
- Owner: coding

### FAB-3: No PlatformAllowList on any module, and non-Win64 targets hard-fail the whole build
- Category: fab-readiness
- Severity: critical
- Location: Plugin/Open3DBroadcast.uplugin:21-62; Open3DSender.Build.cs:24-32; Open3DReceiver.Build.cs:24-32; Open3DTransportNNG.Build.cs:19-27; Open3DTransportWebRTC.Build.cs:19-27; Open3DTransportMoQ.Build.cs:33-36
- Evidence: None of the 8 module entries has `PlatformAllowList` or `PlatformDenyList`. Fab's snippet says each module must have one. With the plugin enabled, any Mac, Linux, Android, iOS or console target runs these Build.cs files and throws `BuildException("... does not define third-party binaries for platform ...")`. That fails the user's entire game build, not just the module. Open3DShared silently builds with `O3D_WITH_OPUS=0` on other platforms (`Open3DShared.Build.cs:41-53`), so behaviour is inconsistent across modules.
- Recommendation: Add `"PlatformAllowList": ["Win64"]` to every module, and `"SupportedTargetPlatforms": ["Win64"]` at plugin level. Replace each `throw` with a no-op or a clean `bBuildModule`-style exclusion. List only Win64 on Fab.
- Effort: S
- Owner: coding

### FAB-4: .pdb files ship in Source/, and moq_ffi.pdb is staged into every packaged game
- Category: fab-readiness
- Severity: critical
- Location: Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.pdb (7,057,408 B); Plugin/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/livekit_ffi.pdb (15,405,056 B); Open3DTransportMoQ.Build.cs:92-97
- Evidence: `RuntimeDependencies.Add(moqFfiPdbPath)` under the comment "Also copy PDB if available for debugging". Anything under `Source/` is packaged by BuildPlugin.
- Recommendation: Delete both pdbs from the plugin tree; keep them in a symbol store or a GitHub release asset. Remove Build.cs lines 92-97. Add a CI check that fails on `*.pdb` in the package.
- Effort: S
- Owner: coding

### FAB-5: 104 of 147 source files have no copyright header
- Category: fab-readiness
- Severity: critical
- Location: e.g. Plugin/Source/Open3DTransportMoQ/Open3DTransportMoQ.Build.cs:1; Plugin/Source/Open3DTransportMoQ/Private/**/*.cpp|h (all); Plugin/Source/Open3DTransportMoQ/Private/Tests/*.cpp
- Evidence: A scan of the first 3 lines of every `.h/.cpp/.cs` under `Plugin/Source` (excluding ThirdParty) found 43 files with `// Copyright (c) Open3DStream Contributors` and 104 with nothing.
- Recommendation: Add a uniform header naming the actual rights holder or seller (see FAB-9) to all files, and add a CI lint for it.
- Effort: S
- Owner: coding

### FAB-6: open3dstream core headers and lib are gitignored build outputs, and Fab cannot run Sync-O3DSCore.ps1
- Category: build
- Severity: critical
- Location: ProjectSandbox/.gitignore:29-36; Build/Scripts/Sync-O3DSCore.ps1:115-150; Plugin/ThirdParty/open3dstream/ (only LICENSE and THIRD_PARTY_LICENSES/ on disk)
- Evidence: `ThirdParty/open3dstream/include/o3ds/`, `o3ds_generated.h` and `lib/` are gitignored and produced by CMake. A clean checkout or submission zip has no o3ds headers, so the Sender/Receiver Build.cs throw. Fab's compile farm only runs BuildPlugin, not CMake or PowerShell. On top of that, prebuilt static `.lib`s built with a different MSVC toolset than Fab's can fail to link if they were built with `/GL`.
- Recommendation: Compile the o3ds core (MIT, in-repo `src/o3ds`) directly as a UE module (for example an `Open3DStreamCore` module with the sources copied into `Source/`), so no prebuilt `open3dstreamstatic.lib` is needed. Do the same for flatbuffers (header-only use is possible) and consider NNG. Failing that, commit the generated headers and lib into the Fab source zip, built without `/GL` using Fab's documented toolset.
- Effort: M
- Owner: design

### CI-1: Build-Plugin.ps1 hides BuildPlugin failure behind a fallback that exits 0
- Category: ci
- Severity: high
- Location: Build/Scripts/Build-Plugin.ps1:306-320, 60-259
- Evidence: If `RunUAT BuildPlugin` returns non-zero, the script prints a warning, builds `ProjectSandboxEditor`/`ProjectSandbox` via UBT, copies the raw plugin *source* folder into `HostProject/Plugins` and then `exit 0`. Every plugin workflow (ci, test, nightly, release) calls this script, so a broken BuildPlugin, which is exactly what Fab runs, shows as green. The release workflow would then ship a source copy as a "release".
- Recommendation: Remove the fallback, or put it behind an explicit `-AllowFallback` switch that CI never passes. Fail the job on BuildPlugin failure. Add a package-content assertion step (expected Binaries DLLs, ThirdParty libs, license files, and no pdb/md/py).
- Effort: S
- Owner: coding

### CI-2: The Test workflow never runs automation tests (boolean input compared to a string)
- Category: ci
- Severity: high
- Location: .github/workflows/open3dbroadcast-plugin-test.yml:10-15, 138, 148
- Evidence: `run_editor_tests` is `type: boolean`, and the step condition is `if: inputs.run_editor_tests == 'true'`. In GitHub Actions a boolean `true` compared with the string `'true'` evaluates false, and on `push` events `inputs` is empty. So the tests never execute. PR CI (`open3dbroadcast-plugin-ci.yml`) has no test step at all. Only the nightly runs tests (`nightly.yml:106-107`).
- Recommendation: Use `if: ${{ inputs.run_editor_tests }}` (or `github.event.inputs.run_editor_tests == 'true'`). Better, run the automation tests in PR CI after the build, on the self-hosted runner.
- Effort: S
- Owner: coding

### CI-3: The automation test runner doesn't reliably detect failures, and the nightly may test stale binaries
- Category: ci
- Severity: medium
- Location: Build/Scripts/Run-AutomationTests.ps1:57-78; .github/workflows/open3dbroadcast-plugin-nightly.yml:106-114
- Evidence: Pass/fail relies only on `UnrealEditor-Cmd` `$LASTEXITCODE`. A missing `index.json` only produces a warning (`:72`). The editor project `ProjectSandbox` needs compiled editor binaries, but the nightly only runs BuildPlugin, which builds into a temp HostProject, not the sandbox. So the tests either use stale workspace binaries or fail to load modules under `-unattended` (**verify**). The filter `Open3DBroadcast.*` is passed straight into `Automation RunTests`; whether `.*` is honoured as a wildcard should be verified. A plain prefix `Open3DBroadcast` is safer.
- Recommendation: Build `ProjectSandboxEditor Win64 Development` via UBT before the tests. Parse `index.json` and fail on `failed > 0` or a missing report. Use `-TestExit="Automation Test Queue Empty"`. Use the filter `Open3DBroadcast`.
- Effort: S
- Owner: coding

### CI-4: CI builds only UE 5.7 Win64, with no strict or warnings-as-errors variants
- Category: ci
- Severity: medium
- Location: .github/workflows/open3dbroadcast-plugin-ci.yml:20-21, 148-157; Build/Scripts/Build-Plugin.ps1:288-297
- Evidence: `UE_ROOT` is hard-coded to UE_5.7. The UAT arguments are `BuildPlugin -Rocket -VeryVerbose -VS2022`: no `-StrictIncludes`, no non-unity build, no warning gating. Fab requires "no errors or consequential warnings", and each engine version on the listing is a separate submission.
- Recommendation: Add a matrix over the engine versions the listing will support (at least 5.7; also 5.6 if you want back-compat, which needs `EngineVersion` per package). Add a job that runs BuildPlugin with `-StrictIncludes` and `bUseUnity=false`, and fail on `warning C` / `warning:` lines from plugin sources.
- Effort: M
- Owner: coding

### CI-5: There is no Fab source-zip packaging job; the release ships compiled output with a misleading compatibility claim
- Category: ci
- Severity: high
- Location: .github/workflows/open3dbroadcast-plugin-release.yml:152-219
- Evidence: The release zips the BuildPlugin package (binaries). The release notes claim "compatible with UE 5.6+" (`:211`) although the binaries are built for UE 5.7 only (`uplugin:3`, `:24`), and "Host project with plugin pre-mounted for quick validation" (`:218`), which doesn't exist in BuildPlugin output. The `UE_5.6` fallback in the version parsing (`:174`) is stale. `Version` is incremented in the runner copy only and never committed (`:145-149`), and `ConvertTo-Json` rewrites the descriptor formatting.
- Recommendation: Add a `fab-package` job that produces a source-only zip (the uplugin plus Source/Resources/Config/Content/ThirdParty/docs, no Binaries/Intermediate/pdb/md-dev/py), runs BuildPlugin against *that zip's* contents, and uploads it. Fix the release notes to say "UE 5.7, Win64 only". Commit version bumps via a PR, or derive them from the tag.
- Effort: M
- Owner: coding

### CI-6: A 6-hour Google WebRTC build runs on every develop push/PR, and its output is unused
- Category: ci
- Severity: medium
- Location: .github/workflows/o3ds-webrtc-windows-native.yaml:3-8, 18-21, 71-89; .github/workflows/build-webrtc.yml:3-7
- Evidence: Triggered on `pull_request`/`push` to develop, with `timeout-minutes: 360`, on `[self-hosted, windows]`, the same pool as the UE plugin CI. `build-webrtc.yml`'s own note says the libdatachannel/WebRTC output "nothing consumes today".
- Recommendation: Switch it to `workflow_dispatch` only, or delete it. That frees the self-hosted runner for plugin CI.
- Effort: S
- Owner: coding

### CI-7: Stale workflows and scripts reference removed code or files
- Category: ci
- Severity: low
- Location: Build/Scripts/Setup-UE.ps1:1 (default `UE_5.4`); Tests/Gauntlet/Open3DStreamTests.json:8-9 (`"Open3DStream.*"`, which matches no tests; all tests are `Open3DBroadcast.*`); scripts/test_package_layout.py:6-24 (tests `UE_5.4/5.5/Plugins/Open3DStream` layout); package.py:15-16, 87 (absolute `D:/P4_PD/o3ds/plugins/Open3DStream/...`, `winreg`); usr/UE_5.4/Plugins/Open3DStream/Open3DStream.uplugin and usr/UE_5.5/... (committed stubs); .github/workflows/create-webrtc-audio-issues.yml:4 (references the missing `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`); .github/workflows/doc.yml:32 (`inputs.github_token` is undefined for workflow_dispatch; `breathe` is not installed at `:17`, although `sphinx/conf.py:5` requires it); windows.yml:32 and linux.yml:23 use `actions/checkout@v2`.
- Evidence: As listed. None of this targets `Open3DBroadcast`; it's all the legacy Open3DStream plugin or packaging.
- Recommendation: Delete `package.py`, `scripts/test_package_layout.py`, `usr/`, `build*.bat`, `o3ds.nsi` (if the installer is dead), and `create-webrtc-audio-issues.yml`. Fix the Gauntlet filter and the `Setup-UE.ps1` default. Fix or remove `doc.yml`.
- Effort: S
- Owner: coding

### CI-8: What check-no-video-codecs.sh and render-icon.py do, and where they're wired
- Category: ci
- Severity: low
- Location: Build/Scripts/check-no-video-codecs.sh:1-12; Build/Scripts/render-icon.py:1-18
- Evidence: `check-no-video-codecs.sh` greps `strings` of every vendored `.dll/.so/.dylib` for ffmpeg/OpenH264 symbols. It is deliberately not wired into CI because it currently fails (FAB-1). `render-icon.py` regenerates `Icon128.png` from the SVG. `core-tests.yml` is the only CI job that runs on GitHub-hosted runners for every PR (o3ds core with ASan/UBSan, good).
- Recommendation: Once FAB-1 is fixed, add `check-no-video-codecs.sh` as a required step in plugin CI (it needs `binutils`; run it in the `changes` ubuntu job). Add a CI check that the PNG matches the SVG render.
- Effort: S
- Owner: coding

### FAB-7: Editor-only detail customizations live inside Runtime modules
- Category: fab-readiness
- Severity: medium
- Location: Plugin/Source/Open3DSender/Private/Open3DSenderModule.cpp:4-8, 23-45; Plugin/Source/Open3DSender/Private/O3DSenderComponentCustomization.h:5-18; Plugin/Source/Open3DReceiver/Private/O3DReceiverSourceFactory.cpp:15-34, 71; Open3DSender.Build.cs:79-89 (PropertyEditor, EditorStyle under `bBuildEditor`); Open3DReceiver.Build.cs:69-76; Transport Build.cs files add Slate/AppFramework for editor (e.g. Open3DTransportNNG.Build.cs:68-76); Plugin/Source/Open3DTransportSockets/Private/Shared/SocketsTransport*Widget.cpp
- Evidence: `IDetailCustomization` and `FPropertyEditorModule` are used in Runtime-type modules. Everything is correctly `#if WITH_EDITOR` guarded and the deps are gated on `Target.bBuildEditor`, so it compiles. But the Epic/Fab convention is editor code in `Type: Editor` modules. `EditorStyle` is deprecated in UE5 (use `AppStyle`).
- Recommendation: Create an `Open3DBroadcastEditor` module (`Type: Editor`, `LoadingPhase: PostEngineInit`) that registers the Sender/Receiver/transport customizations, and strip the `WITH_EDITOR` UI blocks from the runtime modules. Drop the `EditorStyle` dependency.
- Effort: M
- Owner: design

### FAB-8: [SupportedTargetTypes(Game, Editor)] excludes Server, Client and Program, with nothing matching in the uplugin
- Category: build
- Severity: medium
- Location: every Plugin/Source/*/*.Build.cs:4-5 (e.g. Open3DShared.Build.cs:5)
- Evidence: Dedicated-server or client-only projects that enable the plugin will hit UBT errors or odd skips (**verify** UBT 5.7 behaviour). The uplugin declares no `TargetDenyList` or `TargetAllowList`.
- Recommendation: Either support Server/Client (the receiver is useful headless), or declare `"TargetAllowList": ["Game","Editor"]` in each uplugin module entry so the exclusion is explicit and consistent.
- Effort: S
- Owner: coding

### FAB-9: Rights holder and author identity are inconsistent
- Category: licensing
- Severity: medium
- Location: Plugin/LICENSE:3 ("Copyright (c) 2020-2024 Alastair Macleod"); Plugin/Open3DBroadcast.uplugin:9-10, 13 (CreatedBy "Open3DStream Contributors", CreatedByURL and SupportURL `https://open3dstream.com/`); source headers "Open3DStream Contributors"; Plugin/README.md:136 ("See the main repository LICENSE")
- Evidence: A Fab seller must be able to grant the Fab licence. MIT permits resale, but the seller entity, the copyright headers and the LICENSE should agree, and the README contradicts the plugin's own `LICENSE`.
- Recommendation: Decide the seller entity. Update CreatedBy, CreatedByURL and the header text. Add a proper SupportURL (an issue tracker or support email page). Fill `MarketplaceURL` with the Fab listing URL after approval. Keep the original author's MIT notice for the o3ds core.
- Effort: S
- Owner: design

### FAB-10: Duplicate, divergent Opus headers; unknown Opus provenance; possible symbol clash with the engine's libOpus
- Category: licensing
- Severity: medium
- Location: Plugin/ThirdParty/Include/opus*.h vs Plugin/ThirdParty/opus/include/opus*.h; Plugin/ThirdParty/README.md:22-26 ("Version: 1.5.2"); Plugin/README.md:35 ("opus (v1.5.2)"); Plugin/THIRD_PARTY_LICENSES.md:104-118 ("libopus unknown"); Open3DShared.Build.cs:41-53
- Evidence: 5 of 6 headers differ. `ThirdParty/Include/opus_defines.h` has the QEXT/OSCE requests (`OPUS_SET_QEXT_REQUEST 4056`, lines 176-181) that the linked copy lacks. Nothing references `ThirdParty/Include` (grep of Build.cs finds nothing), so it is dead weight with a mismatched API. The docs claim three different versions. UE also ships libOpus, so linking a second static Opus into monolithic Shipping builds that also pull in the engine's libOpus (Voice/PixelStreaming) risks LNK2005 duplicate symbols (**verify**).
- Recommendation: Delete `ThirdParty/Include/`. Switch Open3DShared to the engine's `libOpus` third-party module (`AddEngineThirdPartyPrivateStaticDependencies(Target, "libOpus")`) and remove the vendored `opus.lib`. If you keep it, rebuild from a tagged release and record the version.
- Effort: S
- Owner: coding

### FAB-11: livekit_ffi has no MIT license text, and mlkem-native is unattributed
- Category: licensing
- Severity: high
- Location: Plugin/THIRD_PARTY_LICENSES.md:12-16, 130-143, 158-168; Plugin/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/ (no LICENSE file)
- Evidence: The repo documents both as open obligations to clear "before submission". The OpenSSL/SSLeay advertising clause from AWS-LC may require listing text.
- Recommendation: Add a LICENSE upstream in `lifelike-and-believable/livekit-ffi` and copy it in. Generate notices with `cargo about` for both FFI crates. Add the mlkem-native entry. Get counsel to decide where the OpenSSL acknowledgement goes (Fab listing and/or in-editor About).
- Effort: M
- Owner: design

### FAB-12: MoQ transport is built on draft IETF protocols but the plugin is not marked beta or experimental
- Category: fab-readiness
- Severity: low
- Location: Plugin/Open3DBroadcast.uplugin:15-16, 58-61; Plugin/README.md:129-132 ("Draft-07 Hotfix… reader panic"); Plugin/Source/Open3DTransportMoQ/Phase3_Readiness_Report.md
- Evidence: `IsBetaVersion: false`. The MoQ docs describe hotfixes and panics in a draft-07 dependency.
- Recommendation: Ship MoQ as a separate optional plugin or module marked Experimental, or flag it Beta in the listing and the UI.
- Effort: S
- Owner: design

### BUILD-1: Wrong path to plugin ThirdParty in the WebRTC and MoQ Build.cs, silently ignored
- Category: build
- Severity: low
- Location: Open3DTransportWebRTC.Build.cs:30, 66-70; Open3DTransportMoQ.Build.cs:39, 116-121
- Evidence: `Path.Combine(PluginDirectory, "..", "..", "ThirdParty")`. `PluginDirectory` is already the plugin root, so this resolves to `<Project>/ThirdParty` (or `Engine/ThirdParty` when installed as an engine plugin). It is guarded by `Directory.Exists`, so it is silently skipped. The include path only works through Open3DShared's public include propagation.
- Recommendation: Use `Path.Combine(PluginDirectory, "ThirdParty", "open3dstream", "include")`, or rely on the Open3DShared dependency and delete the block.
- Effort: S
- Owner: coding

### BUILD-2: Dead Linux/Mac branches in MoQ Build.cs contradict the Open3DShared gate
- Category: build
- Severity: low
- Location: Open3DTransportMoQ.Build.cs:25-32, 50-57, 99-114; Open3DShared.Build.cs:107-111
- Evidence: `O3DBuildFlags` force-disables MoQ on non-Win64, so the Linux/Mac branches (and the `libmoq_ffi.so`/`.dylib` paths, which don't exist) never run. The README implies they are supported.
- Recommendation: Remove them until binaries exist, or keep them behind the same PlatformAllowList.
- Effort: S
- Owner: coding

### BUILD-3: Third-party headers are not wrapped, and the o3ds include is not a system include
- Category: build
- Severity: medium
- Location: Open3DSender.Build.cs:17-18, Open3DReceiver.Build.cs:17-18, Open3DShared.Build.cs:17-18 (`PublicIncludePaths` for o3ds); includes in e.g. Plugin/Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp, Plugin/Source/Open3DTransportSockets/Private/Sender/SocketsUdpSender.cpp
- Evidence: `THIRD_PARTY_INCLUDES_START` appears 0 times in the plugin source. o3ds, flatbuffers-generated and nng headers compile under UE's warning set (C4668, C4456 shadowing, and so on), which risks "consequential warnings" on Fab's toolchain.
- Recommendation: Add the o3ds include as `PublicSystemIncludePaths`, and wrap every third-party include in `THIRD_PARTY_INCLUDES_START/END`.
- Effort: S
- Owner: coding

### BUILD-4: Build-time environment-variable feature flags are cached statically and default to a removed backend
- Category: build
- Severity: low
- Location: Open3DShared.Build.cs:72-129 (static `Cached`), 82-83 (`WebRtcBackendLibDc = true`); Plugin/README.md:100-114
- Evidence: `O3D_WEBRTC_BACKEND_LIBDC` defaults to 1, but libdatachannel is gone (grep for `LibDataChannel` in plugin source returns nothing). The flags are process-wide and cached across targets in one UBT invocation, and Fab users can't set them anyway.
- Recommendation: Remove the LIBDC flag. Document the flags as developer-only, or remove them for the Fab build.
- Effort: S
- Owner: coding

### BUILD-5: Every module sets bEnableExceptions; RTTI isn't set anywhere
- Category: build
- Severity: low
- Location: Open3DShared.Build.cs:156-159
- Evidence: `O3DBuildFlags.Apply` sets `bEnableExceptions = true` for all 8 modules, including Loopback and Sockets, which don't need it. `bUseRTTI` is not set anywhere, which is fine.
- Recommendation: Enable exceptions only in the modules that include flatbuffers or o3ds with try/catch.
- Effort: S
- Owner: coding

### HYG-1: Dev and planning docs, a Python test server and absolute paths inside the shipped Source/ tree
- Category: repo-hygiene
- Severity: high
- Location: Plugin/Source/Open3DTransportMoQ/{CLOUDFLARE_RELAY_TESTING.md, MOQ_FFI_REVIEW_AND_WORK_PLAN.md, O3D_WITH_TRANSPORT_MOQ.md, OPEN3DTRANSPORTMOQ_REVIEW.md, Phase3_Readiness_Report.md}; Plugin/Source/Open3DTransportWebRTC/{IMPLEMENTATION_SUMMARY.md, OPEN3DTRANSPORTWEBRTC_ANALYSIS.md, TOKEN_AUTO_FETCH_IMPLEMENTATION.md}; Plugin/Source/Open3DTransportWebRTC/Tests/{mock-token-server.py, README.md}; CLOUDFLARE_RELAY_TESTING.md:78 (`E:\OtherProjects\Open3DStream\...`)
- Evidence: About 150 KB of internal review and planning prose, a Flask JWT mock with a default secret `test-secret` (`mock-token-server.py:31`), and an absolute developer path. BuildPlugin packages all of `Source/`.
- Recommendation: Move these to `docs/dev/` outside the plugin. Keep only user-facing docs (the module READMEs, the WebRTC USER_GUIDE) and license files. Add a CI rule rejecting `*.py` and planning-document names under `Plugin/Source`.
- Effort: S
- Owner: docs

### HYG-2: CanContainContent is true, but the plugin has no Content/ and no Config/
- Category: repo-hygiene
- Severity: low
- Location: Plugin/Open3DBroadcast.uplugin:14
- Evidence: There is no `Plugin/Content` directory. All demo assets are in `ProjectSandbox/Content`.
- Recommendation: Either add sample content (UX-1) or set `false`. Add `Config/FilterPlugin.ini` (FAB-2).
- Effort: S
- Owner: coding

### HYG-3: The sandbox project points at a nonexistent startup map, and its .gitignore contradicts the committed Content
- Category: repo-hygiene
- Severity: low
- Location: ProjectSandbox/Config/DefaultEngine.ini:2 (`EditorStartupMap=/Game/O3DBroadcastMap.O3DBroadcastMap`, no such asset); ProjectSandbox/.gitignore:10 (`Content/` ignored, yet 260 files under `ProjectSandbox/Content` are tracked); ProjectSandbox/ProjectSandbox.uproject:21 (trailing comma, not strict JSON); ProjectSandbox/.ignore:6-9 (references `/Plugins/Open3DStream/...`)
- Evidence: As listed.
- Recommendation: Point the startup map at an existing demo map, fix the .gitignore, remove the trailing comma, and drop the stale ignore entries.
- Effort: S
- Owner: coding

### HYG-4: Root-level planning files and internal business docs
- Category: repo-hygiene
- Severity: low
- Location: MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md (66 KB); MOQ_PLAN_SUMMARY.txt; PRODUCTION_READINESS_JWT_TOKEN_AUTO_FETCH.md:5 ("Date: November 23, 2024", "85% done"); docs/product-management/go-to-market-strategy-2024.md, market-research-report-2025.md, and others; docs/roadmap/*
- Evidence: These don't ship, but they clutter the repo, and the DocsURL points readers into this repo. The go-to-market and market-research docs may be sensitive if the repo is public.
- Recommendation: Move them to `docs/archive/` or an internal wiki. Keep the root to README, LICENSE, CHANGELOG and CONTRIBUTING.
- Effort: S
- Owner: docs

### DOC-1: The plugin README claims "Marketplace Ready / no pre-build steps", which is false
- Category: docs
- Severity: high
- Location: Plugin/README.md:3, 9-10, 50, 127
- Evidence: "Marketplace Ready: No external dependencies or pre-build steps required" and "No pre-build steps are required". In fact `Sync-O3DSCore.ps1` (CMake + MSVC) must run first (Build/README.md:15). "All third-party libraries should be committed" contradicts the gitignored o3ds lib (ProjectSandbox/.gitignore:29-36).
- Recommendation: Rewrite the Building section: prerequisites, run `Sync-O3DSCore.ps1`, then BuildPlugin. Remove the marketing claims until FAB-1 to FAB-6 are closed.
- Effort: S
- Owner: docs

### DOC-2: The plugin README is inaccurate on modules, versions, engine and links
- Category: docs
- Severity: medium
- Location: Plugin/README.md:20-25 (no MoQ module); :33-35 (flatbuffers "v24.3.25" vs 2.0.6 in THIRD_PARTY_LICENSES.md:83,96; opus "v1.5.2" vs "unknown"); :39 (omits livekit_ffi and moq-ffi); :63, :74 (`UE_5.6` in commands; the plugin is 5.7.0); :98, :146 (`../../.github/workflows/README.md` doesn't exist); :149 (`../../Build/README.md` resolves to `ProjectSandbox/Build/README.md`, which doesn't exist); :112 (LIBDC backend); :145 (Issues link to the `Open3DStream` repo while DocsURL is `Open3DBroadcast`); :136 (License points elsewhere though `Plugin/LICENSE` exists)
- Evidence: As listed. I checked the links with a script.
- Recommendation: Update the module list, versions, engine version and links. Point Support at the right issue tracker.
- Effort: S
- Owner: docs

### DOC-3: The root README is largely stale (removed plugin, removed backend, nonexistent QUIC transport, 11 dead links)
- Category: docs
- Severity: high
- Location: README.md:22, 26 ("WebRTC: P2P (libdatachannel)"); :76-83 (install `UE_X.X/Plugins/Open3DStream`, search "Open3DStream"; the plugin is Open3DBroadcast, FriendlyName "Open3D Broadcast Suite"); :97 ("+ Source → Open3DStream Source"; the actual display name is "Open3DStream Receiver", O3DReceiverSourceFactory.cpp:530); :102 (`O3DSRemoteAudioComponent`; the class is `UO3DRemoteAudioComponent`, O3DRemoteAudioComponent.h); :114-135 (describes an `Open3DStream` receiver module that no longer exists); :137-141 (empty headings); :158-162 ("QUIC Transport Updates… MsQuic"; there is no QUIC module); :155-182, :226 (links to WEBRTC_QUICKSTART.md, LIVEKIT_README.md, CURVE_SUPPORT.md, IMPLEMENTATION_SUMMARY.md, WEBRTC_SUPPORT.md, WEBRTC_IMPLEMENTATION_SUMMARY.md, WEBRTC_UNREAL_IMPLEMENTATION.md, LIBDATACHANNEL_INTEGRATION.md, none of which exist); :208-215 (protocol table omits MoQ); :274-292 (`package.py` with UE_5.4/5.5); :46, :78, :108 (clone and release links to `Open3DStream` repo)
- Evidence: As listed.
- Recommendation: Rewrite it as a short landing page: what the repo contains (core lib, Unreal plugin, DCC plugins), links to the plugin USER_GUIDE, build instructions via `Build/README.md`, and supported engine versions and platforms. Delete the "Recent Updates" changelog prose and move anything worth keeping to CHANGELOG.md.
- Effort: M
- Owner: docs

### DOC-4: USER_GUIDE documents Blueprint events that don't exist, and the C++ sample doesn't compile
- Category: docs
- Severity: high
- Location: Plugin/USER_GUIDE.md:242-265; Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:104-106, 259-261
- Evidence: The guide says to bind `OnDescriptorReady`/`OnPoseFrameReady`/`OnSerializedFrame` "in Event Graph". They are native `DECLARE_MULTICAST_DELEGATE_TwoParams` members, not `BlueprintAssignable` (0 `BlueprintAssignable` properties in the plugin). The sample uses `AddDynamic` with a one-parameter handler, but the delegate is non-dynamic with parameters `(const FString&, const FO3DSPoseFrame&)`.
- Recommendation: Fix the sample (`AddUObject` with a two-parameter handler). Either add `BlueprintAssignable` dynamic delegates (see UX-3) or remove the Blueprint instructions.
- Effort: S
- Owner: docs
- Status: closed in #386

### DOC-5: USER_GUIDE omits the MoQ transport and contradicts the transport comparison on audio
- Category: docs
- Severity: medium
- Location: Plugin/USER_GUIDE.md:28, 140-148 ("The plugin provides 4 transport modules", Sockets/NNG audio "No (V1)"); grep for "moq" in USER_GUIDE.md finds 0 hits; Plugin/Transport_Module_Comparison.md:5, 14-20 ("5 transport modules … All modules support both motion capture data streaming and audio"); sockets audio exists (Plugin/Source/Open3DTransportSockets/Private/Sender/SocketsTcpSender.h:6-22, Private/Tests/SocketsAudioTests.cpp)
- Evidence: As listed.
- Recommendation: Add a MoQ section (relay setup, URL format, delivery modes, limitations) and correct the audio support matrix.
- Effort: M
- Owner: docs

### DOC-6: No supported engine-version or platform statement for end users, and install steps don't match Fab
- Category: docs
- Severity: medium
- Location: Plugin/USER_GUIDE.md:42-50 (manual copy only); Build/README.md:236 ("Unreal Engine: 5.4 or later"); ProjectSandbox/README.md:74, 108, 139 (UE_5.4); Tests/Gauntlet/README.md:25, 35 (UE_5.4)
- Evidence: USER_GUIDE never says "Win64 only, UE 5.7". Other docs say 5.4+ while the uplugin says 5.7.0.
- Recommendation: Add a "Requirements" box at the top of USER_GUIDE and README (UE 5.7, Win64 editor and packaged games, network ports/firewall notes). Add Fab install steps (Fab → Install to Engine → enable plugin). Fix the 5.4 references.
- Effort: S
- Owner: docs

### DOC-7: Agent and assistant instruction files carry stale references and absolute paths
- Category: docs
- Severity: low
- Location: .claude/claude.md:11, 54 (`e:\OtherProjects\Open3DStream\...`, `E:\OtherProjects\livekit-ffi-ue\livekit_ffi`); .claude/claude.md:199 (`DEVELOPMENT_GUIDELINES.md`, which doesn't exist); .claude/claude.md:117-136 (mandates Exa MCP tools and forbids the built-in WebSearch, which is environment-specific); .claude/settings.local.json:10-14 (absolute E:\ paths, committed local settings); .github/copilot-instructions.md:35 (`test_curves.cpp`, `test_curve_comprehensive.cpp`, neither exists; tests are `test/*_tests.cpp`); .github/copilot-instructions.md:33-36 plus the §1 "Use the PR template" (no PR template in `.github/`); .github/agents/planning-agent.md:30 (`TRANSPORT_DESIGN_COMPARISON.md`, doesn't exist), :69 (`test_curves.cpp`); AGENTS.md:1-12 (OK: points to copilot-instructions)
- Evidence: I checked each referenced file with `ls` and `find`.
- Recommendation: Replace absolute paths with repo-relative ones. Remove or rename the missing-file references. Stop committing `.claude/settings.local.json` (add it to .gitignore). Add `.github/pull_request_template.md` or drop the reference.
- Effort: S
- Owner: docs

### DOC-8: The two CHANGELOGs are duplicated and empty
- Category: docs
- Severity: low
- Location: CHANGELOG.md:1-3; docs/CHANGELOG.md:1-3
- Evidence: Both files read "## Unreleased / Nothing yet." while the uplugin says VersionName 1.0. A Fab listing needs version notes per update.
- Recommendation: Keep one CHANGELOG (ideally at `Plugin/CHANGELOG.md`, included via FilterPlugin), and fill in 1.0.0 with modules, transports and known limitations.
- Effort: S
- Owner: docs

### DOC-9: The Sphinx and Doxygen docs cover only the legacy core and are broken
- Category: docs
- Severity: low
- Location: sphinx/index.rst:15-22 (toctree `gettingstarted`, `o3ds/o3ds`, neither exists), sphinx/plugins.rst (0 bytes), sphinx/index.rst:25-27 and sphinx/downloads.rst:9 (links to `mocap-ca/Open3DStream`); Doxyfile:1-3 (`INPUT = src`, PROJECT_NAME "Open 3D Stream"); .github/workflows/doc.yml:17, 32
- Evidence: As listed. There is no generated API reference for the UE plugin at all.
- Recommendation: Either retire Sphinx, or add a Doxygen pass over `Plugin/Source/*/Public` and publish a Blueprint/C++ API reference page linked from DocsURL.
- Effort: M
- Owner: docs

### DOC-10: Key end-user documentation is missing
- Category: docs
- Severity: medium
- Location: Plugin/USER_GUIDE.md (overall); Plugin/Source/Open3DTransportWebRTC/USER_GUIDE.md
- Evidence: Present: a quick start (loopback), per-transport options, troubleshooting, and a WebRTC FAQ. Missing:
  - a Blueprint API reference (and a C++ one for the public headers);
  - a MoQ setup guide;
  - an explanation of how LiveKit tokens are obtained and stored (security note);
  - a firewall/ports table;
  - a supported platforms/engine matrix;
  - a description of the sample map (none ships);
  - "known limitations" (Win64 only, draft MoQ, WebRTC codec licensing);
  - uninstall/upgrade notes;
  - the privacy implications of microphone capture.
- Recommendation: Add these sections. For Fab, host a docs page (GitHub Pages or README anchors) at a stable, public DocsURL.
- Effort: M
- Owner: docs

### UX-1: No sample map or demo content ships with the plugin
- Category: usability
- Severity: high
- Location: Plugin/ (no Content/); ProjectSandbox/Content/{BroadcasterTest.umap, ReceiverTest.umap, TestMap.umap, WebRTCTestMap.umap, BP_BroadcastTest.uasset, BP_ReceiverTest.uasset, ABP_LiveLink.uasset}
- Evidence: Fab buyers get only code. The only demos sit in the project, use the Third Person template's Mannequin, and are named like test scratch assets (`NewWebRTCTestMapTwo.umap`, `NewSoundSubmix.uasset`).
- Recommendation: Add `Plugin/Content/Demo/` with one self-contained loopback demo map: a sender actor on the engine mannequin (reference `/Engine` or template content the user already has), a LiveLink receiver preset, and an ABP driven by LiveLink. Add a second map for sockets between two PIE instances. Keep the assets small.
- Effort: M
- Owner: design

### UX-2: No project-level settings; all configuration is per component or source through stringly-typed maps
- Category: usability
- Severity: medium
- Location: grep for `UDeveloperSettings` in Plugin/Source finds 0 hits; Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:153 (`bAutoCreateTransport = false`), 157 (`TransportName`, `HideInDetailPanel`), 161 (`TMap<FString,FString> TransportOptions`, not BlueprintReadWrite); Plugin/Source/Open3DReceiver/Public/O3DReceiverSourceSettings.h:33 (`UCLASS(Config = GameUserSettings)`)
- Evidence: Server URLs, relay URLs and token endpoints must be re-entered per component or per LiveLink source. Transport options (including tokens) are serialized into level and actor assets, and Blueprints can't set them. The receiver settings persist into GameUserSettings, a per-user runtime file, which is an odd home for editor LiveLink defaults.
- Recommendation: Add a `UOpen3DBroadcastSettings : UDeveloperSettings` (Project Settings → Plugins → Open3D Broadcast) with per-transport defaults and a token-endpoint URL. Add Blueprint setters (`SetTransport(Name, Options)`). Mark token fields `PasswordField` / `Transient`, or resolve them from settings or environment variables so they aren't saved into levels.
- Effort: M
- Owner: design
- Status: closed in #385

### UX-3: The Blueprint API is minimal; the receiver has no runtime Blueprint control
- Category: usability
- Severity: medium
- Location: Plugin/Source/Open3DSender/Public/O3DSenderComponent.h:124-133, 336-337 (5 UFUNCTIONs in total across the plugin); O3DSenderAudioCaptureComponent.h:102-103
- Evidence: There are no connection-state or error events, no stats getters, and no Blueprint way to create or remove an Open3DStream LiveLink source at runtime (it can only be done through the LiveLink UI or presets). The sender defaults `bAutoStartCapture = true` but `bAutoCreateTransport = false` (`:149, :153`), so dropping the component on an actor captures but sends nothing until the user discovers a hidden transport step.
- Recommendation: Add BlueprintAssignable `OnConnected`/`OnDisconnected`/`OnError`, `GetStats()`, and a `UO3DReceiverBlueprintLibrary::CreateLiveLinkSource(Transport, Options)`. Default `bAutoCreateTransport = true` with loopback, or warn in the details panel when no transport is configured.
- Effort: M
- Owner: design
- Status: closed in #387

### UX-4: The MoQ relay test depends on a public Cloudflare relay; the tests ship in runtime modules
- Category: usability
- Severity: low
- Location: Plugin/Source/Open3DTransportMoQ/Private/Tests/MoQCloudflareRelayTests.cpp:30, 40-43
- Evidence: `kDefaultRelayUrl = "https://relay.cloudflare.mediaoverquic.com"`. These tests show up in end users' Session Frontend and hit the internet. They are correctly guarded by `WITH_DEV_AUTOMATION_TESTS`.
- Recommendation: Mark the network tests `EAutomationTestFlags::StressFilter`, or skip them unless `O3D_MOQ_RELAY_URL` is set. Consider moving tests to a separate `Open3DBroadcastTests` module (`Type: DeveloperTool` or `UncookedOnly`).
- Effort: S
- Owner: coding

### UX-5: The plugin description and naming are confusing across surfaces
- Category: usability
- Severity: low
- Location: Plugin/Open3DBroadcast.uplugin:6-7 ("Open3D Broadcast Suite", "…for Open3DStream"); LiveLink source "Open3DStream Receiver" (O3DReceiverSourceFactory.cpp:530); component ClassGroup `Open3DStream` (O3DSenderComponent.h:112); README title "Open3DStream"
- Evidence: Users searching the Plugins browser, the Add Component menu and the LiveLink menu meet three different names.
- Recommendation: Pick one product name and apply it to the FriendlyName, ClassGroup, LiveLink display name and docs.
- Effort: S
- Owner: design

### LIC-1: The WebRTC token mock server ships with a default signing secret
- Category: licensing
- Severity: low
- Location: Plugin/Source/Open3DTransportWebRTC/Tests/mock-token-server.py:16, 31
- Evidence: `API_SECRET = os.environ.get('API_SECRET', 'test-secret')`. It's test-only, but it sits in the shipped tree and conflicts with AGENTS.md "No hard-coded credentials".
- Recommendation: Move it out of the plugin (HYG-1) and require the environment variable with no default.
- Effort: S
- Owner: coding

### CI-9: Plugin CI skips draft PRs and runs only on self-hosted runners; docs-only changes skip the build
- Category: ci
- Severity: low
- Location: .github/workflows/open3dbroadcast-plugin-ci.yml:41-54
- Evidence: The paths-filter excludes Plugin `*.md` changes only indirectly (`Plugin/**` includes them, which is fine). Build needs a self-hosted `[ue5, windows]` runner. Forks and outside contributors get no plugin signal, and `core-tests.yml` covers only the core library.
- Recommendation: Add a cheap GitHub-hosted job on every PR: validate the uplugin JSON and PlatformAllowList, the copyright-header lint, a check for forbidden files (pdb/py/planning md) under `Plugin/`, and a link check on the docs.
- Effort: S
- Owner: coding

### FAB-13: MarketplaceURL is empty and SupportURL is a generic website
- Category: fab-readiness
- Severity: low
- Location: Plugin/Open3DBroadcast.uplugin:12-13
- Evidence: `"MarketplaceURL": ""`, and `"SupportURL": "https://open3dstream.com/"`, the same as CreatedByURL.
- Recommendation: After the listing is created, set MarketplaceURL to the Fab product URL. Set SupportURL to a dedicated support page, issue tracker or support email.
- Effort: S
- Owner: design

### FAB-14: DocsURL points at a branch blob in a GitHub repo that may be private
- Category: fab-readiness
- Severity: medium
- Location: Plugin/Open3DBroadcast.uplugin:11
- Evidence: `https://github.com/lifelike-and-believable/Open3DBroadcast/blob/main/.../USER_GUIDE.md`. I couldn't verify its visibility. Other docs link to `Open3DStream` repos (README.md:46, 78, 108; sphinx links to `mocap-ca`).
- Recommendation: Host docs at a stable public URL (GitHub Pages or a versioned tag path) and use that for DocsURL.
- Effort: S
- Owner: docs
