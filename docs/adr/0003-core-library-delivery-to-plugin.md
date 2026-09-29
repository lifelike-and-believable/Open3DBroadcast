# 0003: How the o3ds core library reaches the plugin

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Date:** 2026-09-29
- **Plan decision:** D3 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0001](0001-platform-scope-first-fab-release.md) (D1; this ADR is the prerequisite for its unscheduled Mac/Linux option); issue #203; [`resilient-streaming-and-motion-prediction.md`](../roadmap/resilient-streaming-and-motion-prediction.md) §0.2; feeds WP-F1, WP-F2, WP-F3, WP-T1, WP-A7

**Recommendation in one line:** compile the subset of `src/o3ds` that the plugin uses as a new UE module, `Open3DStreamCore`. Its sources are a **committed, script-generated mirror** of `src/o3ds`. `src/o3ds` stays the single source of truth, and a CI check on every PR fails if the mirror drifts. FlatBuffers becomes header-only, from the same pin as `flatc`. NNG and Opus stay prebuilt for v1.

## Context

**Today's pipeline.**
- `Build/Scripts/Sync-O3DSCore.ps1` builds NNG, CML, CRCpp and FlatBuffers (`:77-98`) and then the whole core (`:100-112`). It copies:
  - `open3dstreamstatic.lib` to `Plugin/ThirdParty/open3dstream/lib/Win64` (`:114-128`);
  - **all** of `src/o3ds`, sources included, to `include/o3ds` (`:134-141`);
  - the flatc output (`:145-151`).
- All of these are gitignored (`ProjectSandbox/.gitignore:29-36`, added by #245, commit `2fa7fe7`). The script is Win64 only and has to run before every plugin CI build (`.github/workflows/open3dbroadcast-plugin-ci.yml:120-127`, and the same step in the nightly and release workflows).

**Consequences of that pipeline.**
- A clean clone or a Fab source zip has no core headers or lib, so `Open3DSender.Build.cs:38-45` and `Open3DReceiver.Build.cs:38-45` throw (FAB-6, FAB-2).
- Header install is limited to `CONFIGURATIONS RelWithDebInfo` (`src/CMakeLists.txt:240-243`), which is why the script copies headers by hand (CORE-20, CORE-21).
- CML is built even though nothing under `src/o3ds` uses it: a grep for `cml` finds nothing. Yet `Plugin/THIRD_PARTY_LICENSES.md:84` lists CML as statically linked.
- `resilient-streaming-and-motion-prediction.md` §0.2 still describes a committed verbatim copy. Since #245 the duplicate is a build output instead, but #203's underlying problem remains: two trees, and nothing that checks them against each other.

**What the plugin actually uses.** Grepping `Plugin/Source` for `#include "o3ds/` finds seven core headers:
- `o3ds/model.h` (16 files, e.g. `Open3DSender/Private/O3DSenderSerializer.cpp:11`)
- `o3ds/udp_fragment.h` (`Open3DTransportSockets/Private/Receiver/SocketsUdpReceiver.cpp:15`)
- `o3ds/reorder_gate.h`, `o3ds/clock_offset.h`, `o3ds/predict/concealment.h` (`Open3DReceiver/Public/O3DReceiverSource.h:14-17`)
- `o3ds/sequencing.h`, `o3ds/predict/linear_predictor.h` (`Open3DReceiver/Private/O3DReceiverSource.cpp:25-27`)

Plus `o3ds_generated.h` directly. The transitive closure of those headers is:

| | Files |
|---|---|
| Headers (19, 3,962 lines incl. generated) | `model.h`, `math.h`, `context.h`, `transform_component.h`, `getTime.h`, `sequencing.h`, `reorder_gate.h`, `clock_offset.h`, `udp_fragment.h`, `predict/{concealment,hold_predictor,linear_predictor,quadratic_predictor,pose_predictor,quat_math,residual_codec,sample_ring}.h`, `quant/channel_quant.h`, `o3ds_generated.h` |
| Sources (15, 3,440 lines) | the `.cpp` for each of the above that has one |
| External | `flatbuffers/flatbuffers.h` (from `src/o3ds_generated.h:7`), CRCpp `CRC.h` (`src/o3ds/model.cpp:27`, used as `CRCPP::CRC` at `:872`, `:982`, so `CRCPP_USE_NAMESPACE` is required, as at `src/CMakeLists.txt:238`), `<windows.h>` (`src/o3ds/getTime.cpp:28`), and the standard library |

The following are **not** in the closure:
- the connectors (`nng_connector`, `tcp`, `udp`, `publisher`, ...);
- `capture`, `replay`, `channel_model`;
- `xsens_parser`, `websocket`, `webrtc_connector`;
- NNG.

The static lib that the plugin links today contains all of these (`src/CMakeLists.txt:152-192`).

**Properties of the closure** (grep-verified):
- no `throw`, `try` or `catch`;
- no `dynamic_cast` or `typeid`;
- FlatBuffers is used only through header-only runtime types: `FlatBufferBuilder`, `Offset`, `Vector` and `Verifier`. So `flatbuffers.lib` (`Open3DSender.Build.cs:36`) is not needed.
- The FlatBuffers runtime headers included by `flatbuffers.h` do not throw (only `util.h` does, and it is not included).
- No core header has an export macro (a grep for `__declspec`, `visibility` and `O3DS_API` in `src/o3ds/*.h` finds nothing).

**Generated header and versions.**
- `src/o3ds_generated.h` is committed (`git ls-files`), per the repo rule to regenerate it with `flatc --cpp`.
- CMake also regenerates it into `build/_generated` and puts that first on the include path (`src/CMakeLists.txt:95-100`, `:210-213`). So a stale committed copy would go unnoticed by the Linux build.
- The FlatBuffers pin is `thirdparty/flatbuffers` @ `615616c` (v2.0.6, CORE-20). The plugin separately vendors headers reporting 2.0.6 (`Plugin/ThirdParty/flatbuffers/include/flatbuffers/base.h:141-143`), with no check tying them together (CORE-21).

**Warnings.** `-Wall -Wextra` on the core gives 68 warnings today (CORE-20). UE compiles module sources with its own MSVC warning set, and Fab requires "no errors or consequential warnings" (`fab-ci-docs.md` checklist).

## Decision drivers

1. A clean clone and the Fab zip must build with only `RunUAT BuildPlugin`: no CMake, PowerShell or prebuilt core (WP-F1 acceptance).
2. There is one source of truth for the core, and drift is caught by a machine, not by review (#203).
3. The Linux CMake build and the 174 core tests keep working unchanged. The core stays engine-agnostic, so no UE macros go in `src/o3ds` (core-first principle, plan §0.2).
4. Any platform UE targets can build it (keeps ADR 0001's unscheduled Mac/Linux option open).
5. No new consequential warnings under UE or Fab's toolchain.
6. Small reviewable steps.

## Options considered

### Option A: compile the used core sources as a UE module (`Open3DStreamCore`)
- **Pros:**
  - Removes the prebuilt-lib provenance and toolset question (the `/GL` risk in FAB-6).
  - Builds on every platform with Fab's own toolchain.
  - Ships about 7,400 lines of source instead of a lib.
  - Drops `flatbuffers.lib` and the unused NNG and connector code from the core path.
- **Cons:**
  - The core must compile cleanly under UE's warnings and MSVC settings.
  - Symbols must be exported for modular (editor) builds.
  - The files have to physically exist under `Source/`.
- **Cost / effort:** M. **Risk:** medium. See the sub-decisions and risks below.

### Option B: commit prebuilt libs and headers into the plugin
- **Pros:** no source changes. The core is compiled with its own CMake flags, so there is no UE warnings exposure.
- **Cons:**
  - A binary per platform and per toolset, built without `/GL` and matching Fab's MSVC version (unknown; **needs-verification**, Q3).
  - Binary blobs in git.
  - The header/lib mismatch risk remains.
  - The CML, NNG and connector code stays linked into every consumer.
  - A Mac or Linux tier would need a build farm.
- **Cost / effort:** S now, recurring later. **Risk:** high for Fab acceptance (toolset) and for drift.

### Option C: keep the sources uncommitted, populated by a submodule or a sync step before the build (today's model, extended)
- **Pros:** no second copy in git.
- **Cons:** fails driver 1. A clean clone cannot build without running the step, which is exactly FAB-6. A submodule would require splitting `src/o3ds` into its own repository, which is a larger restructuring and still needs a materialization step for the Fab zip.
- **Cost / effort:** M. **Risk:** high. Rejected.

### Sub-decision for Option A: how the plugin copy stays in sync

| Variant | Pros | Cons |
|---|---|---|
| **A1. Committed mirror generated by a script from a manifest, with a `--check` mode in CI** | `src/o3ds` stays canonical and the CMake tree is untouched. The mirror is byte-identical by construction, and drift fails CI on every PR on GitHub-hosted runners. | Two copies in git (one generated). Each core PR also commits the regenerated mirror. |
| A2. Move the canonical files into `Plugin/Source/Open3DStreamCore/` and point CMake at them | One physical copy | `src/o3ds` splits into "plugin" and "non-plugin" halves that include each other. CMake and core CI reach into `ProjectSandbox/`. It works against the core-first layout. |
| A3. Symlinks from the plugin to `src/o3ds` | One copy | Git symlinks on Windows need `core.symlinks` and developer mode. How BuildPlugin and zip packaging treat links is **needs-verification** (Q4). Fragile on the self-hosted runner. |
| A4. Build.cs copies from `../../../../src` at UBT time | No committed copy | Fails in the Fab zip, where `src/` doesn't exist. Writing into `Source/` during a build is fragile. |

**A1 is chosen.** The mirror is generated, so review effort stays on `src/o3ds`. The check is what resolves #203.

## Decision

**Adopt Option A with sync variant A1.** Specifically:

1. **Module.** Add `Plugin/Source/Open3DStreamCore/` with `Type: Runtime` and the D1 keys (`PlatformAllowList`, `TargetDenyList`), listed first in the `.uplugin`.

   ```
   Open3DStreamCore/
     Open3DStreamCore.Build.cs
     Private/Open3DStreamCoreModule.cpp      // IMPLEMENT_MODULE; static_assert on FlatBuffers version
     Vendor/                                 // GENERATED - do not edit; see SYNC_STAMP.txt
       o3ds/*.h, o3ds/*.cpp, o3ds/predict/*, o3ds/quant/*   (the closure above)
       o3ds_generated.h                      // copy of src/o3ds_generated.h
       flatbuffers/*.h                       // runtime headers only, from thirdparty/flatbuffers@pin
       crccpp/CRC.h                          // from thirdparty/crccpp@pin
       LICENSES/                             // o3ds MIT, FlatBuffers Apache-2.0, CRCpp BSD-3
       SYNC_STAMP.txt                        // source commit, manifest hash, flatc/FlatBuffers version, O3DS_VERSION_TAG
   ```

2. **Build.cs settings:**
   - `PublicSystemIncludePaths.Add(Vendor)` and `PrivateIncludePaths.Add(Vendor/crccpp)`.
   - Never add `Vendor/o3ds` itself to an include path: `o3ds/math.h` would shadow `<math.h>`.
   - `PrivateDefinitions`: `CRCPP_USE_NAMESPACE`.
   - `PublicDefinitions`: `O3DS_API=OPEN3DSTREAMCORE_API`.
   - `bEnableExceptions = false` and `bUseRTTI = false` (the closure uses neither).
   - `bUseUnity = false`, to avoid collisions between the core's file-local names in unity TUs; it is only 15 TUs.
   - `PublicDependencyModuleNames = { "Core" }`. No platform branches, no `PublicAdditionalLibraries`.
   - UBT is assumed to compile every `.cpp` under the module folder, including `Vendor/` (**needs-verification**, Q1).

3. **Exports.** Add `src/o3ds/o3ds_export.h` with `#ifndef O3DS_API` / `#define O3DS_API` / `#endif`. Annotate the non-inline classes and free functions in the closure with it (for example `class O3DS_API SubjectList`).
   - In CMake the macro is empty, so the Linux build is unchanged.
   - In UE it expands to the module's DLL export/import macro, which modular editor builds need (**needs-verification** that UE 5.7 has no static-library module type that would make this unnecessary, and how UE treats C4251 for exported classes with STL members; Q2).
   - This lines up with CORE-30's proposed `o3ds_api.h` stable surface (WP-A7), which can later narrow what is exported.

4. **Core source changes made in `src/o3ds`, never in the mirror:**
   - `getTime.cpp`: replace the `windows.h`/`clock_gettime` branches (`:27-45`) with `std::chrono::steady_clock`. This removes the only platform header from the closure and avoids `windows.h` inside a UE TU.
   - Fix the warnings that UE's MSVC settings flag, driven by an MSVC `/W4` core CI job (WP-T1, CORE-20).
   - No UE macro, `THIRD_PARTY_INCLUDES_START` or `TEXT()` goes into `src/o3ds`.

5. **Include hygiene for consumers (BUILD-3).** Every UE file that includes a core header wraps it in `THIRD_PARTY_INCLUDES_START` / `THIRD_PARTY_INCLUDES_END`. The o3ds, FlatBuffers and generated headers arrive through `PublicSystemIncludePaths`. The dependency is public for `Open3DReceiver`, because `O3DReceiverSource.h:14-17` exposes core types; it is private for Sender, Loopback, Sockets, NNG, WebRTC, MoQ and the D10 test module.

6. **Sync tooling.**
   - Add `Build/Scripts/sync_o3ds_core.py` (Python 3, so it runs on both the Linux and Windows runners) and a committed manifest `Build/o3ds-core-manifest.txt` listing the seed headers.
   - The script:
     - computes the include closure and fails if a quoted include resolves outside the manifest-allowed set;
     - copies the closure, the committed `src/o3ds_generated.h`, the FlatBuffers runtime headers and `CRC.h` from the submodule pins;
     - writes `SYNC_STAMP.txt`;
     - deletes any stale file in `Vendor/`.
   - `--check` does the same into a temporary folder and diffs, normalising line endings (there is no `.gitattributes`). It exits non-zero on any difference, missing file or extra file.

7. **CI (`.github/workflows/core-tests.yml`, every PR):**
   - (a) `sync_o3ds_core.py --check`;
   - (b) build `flatc` from the pinned submodule, regenerate `o3ds_generated.h` from `src/o3ds.fbs`, and diff it against the committed `src/o3ds_generated.h`.

   Together these tie the schema, the generated header, the FlatBuffers runtime headers and the mirror to one pin. A `static_assert(FLATBUFFERS_VERSION_MAJOR == 2 && FLATBUFFERS_VERSION_MINOR == 0 && FLATBUFFERS_VERSION_REVISION == 6)` in `Open3DStreamCoreModule.cpp` catches a header mismatch at compile time. A FlatBuffers upgrade (CORE-20) then becomes one PR that bumps the submodule, regenerates, and re-syncs.

8. **NNG and Opus stay prebuilt for v1.**
   - **NNG** is not in the core closure. After this ADR it is used only by `Open3DTransportNNG`, which is Win64-only under ADR 0001. Building NNG (C, CMake-configured, platform-specific) under UBT is an L-sized job that only pays off with the Mac/Linux transport tier. Revisit then, together with the 1.3.0 upgrade question (TRB-41).
   - **Opus**: WP-F5 evaluates switching to the engine's libOpus (FAB-10, **needs-verification**, Q5), which would remove the unknown-provenance `opus.lib` and make `Open3DShared` portable. If that fails, rebuild from a tagged release. Either way it is independent of this ADR.

9. **Retire the old path:**
   - Delete `Plugin/ThirdParty/open3dstream/` (its licences move to `Vendor/LICENSES`), `Plugin/ThirdParty/flatbuffers/` (including `flatbuffers.lib` and the compiler-only headers, SHR-23), and `ProjectSandbox/.gitignore:29-36`.
   - Remove the `Sync-O3DSCore.ps1` steps from the plugin workflows, and delete the script or reduce it to a note pointing at the new one.
   - Remove the CML row from `THIRD_PARTY_LICENSES.md`.

## Consequences

- **Easier:**
  - WP-F1 acceptance (a clean clone builds) becomes reachable.
  - Plugin CI loses the CMake prelude.
  - Core changes reach the plugin in the same PR, with CI proving the copy.
  - ADR 0001's Mac/Linux option, if ever scheduled, needs no core binaries.
  - WP-F2 can delete the platform branches in the Sender and Receiver Build.cs files.
- **Harder:**
  - Core PRs that touch mirrored files must also commit the regenerated mirror. CI tells you, but it is an extra step for contributors (documented in `CONTRIBUTING` or `.github/copilot-instructions.md` in the same PR).
  - Core code is now held to UE's warning bar. WP-T1's MSVC job becomes a prerequisite for merging core changes.
  - The core headers carry `O3DS_API`.
- **Constrains:**
  - New core files the plugin needs must be added to the manifest.
  - `src/o3ds` must stay free of exceptions and RTTI in the mirrored closure. The script can grep for this and warn.
  - The plugin can no longer reach connector code (`tcp.h`, `nng_connector.h`), which is intended.
- **Resolves** issue #203 and roadmap §0.2. That section should be updated to point here in a separate docs PR (WP-D3).

## Implementation outline

1. **WP-T1 (prerequisite):** add an MSVC core build job with `/W4`, and ratchet the warnings in the closure files to zero (CORE-20). Files: `.github/workflows/core-tests.yml`, `src/o3ds/*`.
2. **WP-F1a (core prep, CMake-only PR):**
   - add `src/o3ds/o3ds_export.h` and annotate the closure;
   - move `getTime.cpp` to `std::chrono`;
   - fix `src/CMakeLists.txt:236` (`FATAL` should be `FATAL_ERROR`) and the duplicate `o3ds_version.h` (`:144-145`) while there.

   The Linux tests must stay 174 of 174.
3. **WP-F1b (tooling):** `Build/Scripts/sync_o3ds_core.py`, `Build/o3ds-core-manifest.txt`, and the two CI checks in `core-tests.yml`.
4. **WP-F1c (module):**
   - add `Plugin/Source/Open3DStreamCore/` (Build.cs, module cpp, generated `Vendor/`) and its `.uplugin` entry;
   - switch Shared, Sender, Receiver, Loopback, Sockets, NNG, WebRTC and MoQ from include paths and libs to a module dependency;
   - delete `PublicAdditionalLibraries` for core and FlatBuffers (`Open3DSender.Build.cs:34-48`, `Open3DReceiver.Build.cs:34-48`, `Open3DShared.Build.cs:16-38`);
   - delete the broken `pluginThirdPartyDir` blocks (`Open3DTransportWebRTC.Build.cs:30`, `:65-70`; `Open3DTransportMoQ.Build.cs:39`, `:116-121`; BUILD-1);
   - drop `ws2_32`, `iphlpapi` and the other system libs from Sender and Receiver (`Open3DSender.Build.cs:50-61`, `Open3DReceiver.Build.cs:78-89`) if the link succeeds without them (they served the static lib's connectors).
5. **WP-F3 (BUILD-3):** add `THIRD_PARTY_INCLUDES_START/END` around every core include in `Plugin/Source`.
6. **WP-F2 (BUILD-5):** set `bEnableExceptions` only where needed. That is MoQ's `catch (...)` (`Plugin/Source/Open3DTransportMoQ/Private/Shared/MoQSessionWrapper.cpp:116-131`) until TRF-39 removes it.
7. **WP-F1d (retire):** delete `Plugin/ThirdParty/{open3dstream,flatbuffers}` and `.gitignore:29-36`; update the plugin workflows and `THIRD_PARTY_LICENSES.md` §1 and §2; delete `Sync-O3DSCore.ps1`.

## Verification / acceptance

- From a fresh clone with **no scripts run**, `RunUAT BuildPlugin -TargetPlatforms=Win64` passes with warnings as errors, in both unity and non-unity builds (WP-F1, WP-F8, CI-4).
- The Win64 editor (modular) and a Win64 Shipping game (monolithic) both link. This proves the `O3DS_API` exports work.
- `python3 Build/Scripts/sync_o3ds_core.py --check` passes in `core-tests.yml`. A PR that edits `src/o3ds/model.cpp` without re-syncing fails it, as does a PR that edits `Vendor/` by hand.
- The flatc regeneration diff passes, and fails if `src/o3ds.fbs` is changed without regenerating.
- The Linux core tests remain 174 of 174 under ASan/UBSan, and `cmake` is configured with the same options as today.
- `git ls-files Plugin/ThirdParty` shows no `open3dstream` or `flatbuffers` directories. The package-content assertion finds no `.lib` for the core.
- The existing automation tests pass. The WP-T1 round-trip tests pass through the UE-compiled core, via the D10 test module.

## Open questions for the maintainer

1. **needs-verification:** does UBT 5.7 compile every `.cpp` under a module folder recursively (including a `Vendor/` subfolder with no `Private` or `Public` parent), and can the module opt specific files out if needed?
2. **needs-verification:** is there any way in UE 5.7 to make a plugin module link as a static library in modular builds (which would make `O3DS_API` unnecessary)? How does UE handle C4251 for exported classes with `std::vector` members?
3. **needs-verification (WP-F0):** which MSVC toolset does Fab's farm use? This matters only if Option B is chosen instead.
4. **needs-verification:** how do BuildPlugin and the Fab zip handle symlinks? This matters only if A3 is preferred.
5. **needs-verification:** can `Open3DShared` use the engine's `libOpus` third-party module in UE 5.7 on Win64, Mac and Linux, and does its version expose the API `O3DAudioFrameCodec` uses?
6. **needs-verification:** does the engine, or any commonly co-installed plugin, ship a different FlatBuffers version whose inline symbols could clash with ours in monolithic builds?
7. ~~Is Python 3 acceptable as a required dev tool for core contributors?~~ **Answered 2026-09-29:** yes, Python 3 is fine for the sync script.
8. ~~Do you agree to adding `O3DS_API` annotations to `src/o3ds` headers?~~ **Answered 2026-09-29:** yes.

## References

- Findings: FAB-2, FAB-6, CORE-20, CORE-21; also CORE-30, BUILD-1, BUILD-3, BUILD-5, SHR-20, SHR-23, TRB-41, FAB-10.
- Files:
  - `Build/Scripts/Sync-O3DSCore.ps1`
  - `src/CMakeLists.txt`, `CMakeLists.txt`
  - `src/o3ds/{model.h,model.cpp,getTime.cpp}`, `src/o3ds_generated.h`
  - `ProjectSandbox/.gitignore`
  - `Plugin/Source/*/*.Build.cs`
  - `Plugin/Source/Open3DReceiver/Public/O3DReceiverSource.h`
  - `Plugin/ThirdParty/`
  - `Plugin/THIRD_PARTY_LICENSES.md`
  - `.gitmodules`
  - `.github/workflows/{core-tests.yml,open3dbroadcast-plugin-ci.yml}`
- Other docs: `docs/roadmap/resilient-streaming-and-motion-prediction.md` §0.1 and §0.2; issue #203; PR #245.
- External: none fetched for this ADR (see ADR 0001, References, for what was tried and blocked).
