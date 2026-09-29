# 0001: Platform scope for the first Fab release

- **Status:** Proposed (pending maintainer sign-off)
- **Date:** 2026-09-29
- **Plan decision:** D1 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0002](0002-webrtc-and-moq-in-first-fab-release.md) (D2), [ADR 0003](0003-core-library-delivery-to-plugin.md) (D3); feeds WP-F2, WP-F8, WP-F9, WP-D2

**Recommendation in one line:** ship v1 as Win64 only, declared explicitly in the `.uplugin` for every module and for the plugin, with the Build.cs `throw`s replaced by descriptor-level exclusion and stub builds. Mac/Linux is **not planned for v1.1** (maintainer, 2026-09-29). The tiered expansion of the five binary-free modules (Option C) stays documented as a possible later release that ADR 0003 makes cheap.

## Context

**Nothing in the descriptor restricts platforms today.** None of the eight module entries in `Plugin/Open3DBroadcast.uplugin:21-62` has `PlatformAllowList`, `PlatformDenyList`, `TargetAllowList` or `TargetDenyList`, and the plugin has no `SupportedTargetPlatforms` key (`:1-20`). The Fab requirement, as seen in search snippets of the "Fab Technical Requirements" page, is that each module has a `PlatformAllowList` or `PlatformDenyList` key (`docs/review/2026-09-plugin-review/fab-ci-docs.md`, "Sources used for Fab requirements").

**Five Build.cs files throw on any platform other than Win64:**

| Module | Throw | Why it is Win64-only |
|---|---|---|
| Open3DSender | `Open3DSender.Build.cs:24-32` | links `open3dstreamstatic.lib` and `flatbuffers.lib` (`:35-48`) |
| Open3DReceiver | `Open3DReceiver.Build.cs:24-32` | same libs (`:35-48`) |
| Open3DTransportNNG | `Open3DTransportNNG.Build.cs:19-27` | prebuilt `nng.lib` 1.3.0 (`:40-45`) |
| Open3DTransportWebRTC | `Open3DTransportWebRTC.Build.cs:19-27` | `livekit_ffi.dll` (`:36-60`) |
| Open3DTransportMoQ | `Open3DTransportMoQ.Build.cs:33-36` | `moq_ffi.dll` (`:44-97`); its Linux and Mac branches (`:25-32`, `:99-114`) point at binaries that do not exist |

The other three modules do not throw, but they are not portable either:
- `Open3DShared.Build.cs:24-53` silently links nothing on non-Win64 and sets `O3D_WITH_OPUS=0` (`:53`), which leads to the frame mislabelling in SHR-1.
- `Open3DTransportLoopback.Build.cs:25-30` and `Open3DTransportSockets.Build.cs:28-43` depend on Open3DSender and Open3DReceiver, so they inherit those throws.

**Platform decisions are made inconsistently.** `O3DBuildFlags` auto-disables MoQ off Win64 (`Open3DShared.Build.cs:107-111`) but not NNG or WebRTC (`:103-105`). It also caches its result process-wide in `Cached` (`:87-97`), so in one UBT process that builds two platforms, the second target inherits the first target's MoQ decision (SHR-22). When MoQ is disabled, its Build.cs returns before adding any include paths (`Open3DTransportMoQ.Build.cs:14-17`), yet its sources still include `moq_ffi.h` (TRF-27). Only the MoQ module has a stub for the disabled case (`Plugin/Source/Open3DTransportMoQ/Private/Open3DTransportMoQModule.cpp:4`, `:710-727`). The WebRTC module has `#error "Unsupported platform for LiveKit FFI"` at `Open3DTransportWebRTCModule.cpp:750-753`.

**Target types.** Every Build.cs carries `[SupportedTargetTypes(TargetType.Game, TargetType.Editor)]` (for example `Open3DShared.Build.cs:5`, `Open3DSender.Build.cs:5`). Server, Client and Program are excluded without any matching descriptor entry or documentation (FAB-8, SHR-21).

**What a user sees today.** In a project with the plugin enabled that builds a Linux, Mac or Android target, UBT runs these Build.cs files and throws a `BuildException`. That fails the whole target build, not only the plugin (FAB-3, SND-11, RCV-30, TRB-41, TRF-27).

**Which modules are portable in principle.** The source of Shared, Sender, Receiver, Loopback and Sockets has no platform-specific code: a grep for `PLATFORM_WINDOWS`, `_WIN32`, `#error` and `Windows/` in those five modules returns nothing. Their only third-party binaries are the o3ds core and FlatBuffers libs (removed by ADR 0003) and Opus in Shared. The o3ds core already builds and tests on Linux (`.github/workflows/core-tests.yml:26-80`). Its one platform branch is `src/o3ds/getTime.cpp:27-45` (`windows.h` versus `clock_gettime`). NNG, WebRTC and MoQ depend on prebuilt binaries that exist only for Win64 (`git ls-files`: `nng.lib`, `livekit_ffi.dll`, `moq_ffi.dll` under `bin|lib/Win64`).

## Decision drivers

1. Enabling the plugin must never break a user's build for a platform the plugin does not support (FAB-3).
2. It must meet the Fab descriptor requirement for every module (the checklist in `fab-ci-docs.md`).
3. The v1 submission should not wait for non-Win64 binaries that nobody builds today.
4. Every platform on the listing must be built in CI. An untested platform is not listed.
5. Mac and Linux should be cheap to add later.

## Options considered

### Option A: Win64 only, declared explicitly (plan D1 option a)
Add `"PlatformAllowList": ["Win64"]` to all eight modules and `"SupportedTargetPlatforms": ["Win64"]` to the plugin. List only Win64 on Fab.
- **Pros:** smallest change (S). Matches what CI builds (`Build/Scripts/Build-Plugin.ps1:5` defaults `-TargetPlatforms` to Win64). Meets the Fab key requirement. Non-Win64 targets skip the plugin instead of failing (**needs-verification**, see Q1).
- **Cons:** Mac and Linux editor users cannot use the plugin at all. Linux dedicated servers and render nodes are excluded.
- **Cost / effort:** S (descriptor), plus M for the Build.cs clean-up that WP-F2 needs anyway.
- **Risk:** low. The main unknown is the exact UBT behaviour for a disallowed platform (Q1, Q2).

### Option B: Win64, Mac and Linux for every module (plan D1 option b)
Build the core, NNG, Opus, `moq_ffi` and `livekit_ffi` for each platform.
- **Pros:** widest reach, which matters for virtual production and Linux render farms.
- **Cons:** needs Rust FFI builds for three platforms. The `livekit_ffi` codec removal would have to be repeated per platform. `moq_ffi` pulls in extra licence terms on Linux and macOS (s2n-bignum, `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/THIRD_PARTY_NOTICES.md`, the "What is actually linked" table). It also needs Mac and Linux UE CI runners, which the project does not have. Whether Fab's compile farm builds Linux at all is unclear: a search snippet of the "Plugin Compilation Environment" page says Linux cross-compilation "is not yet supported on the Marketplace" (**needs-verification**, Q4).
- **Cost / effort:** L (several weeks, mostly FFI and CI).
- **Risk:** high. It blocks v1 on work that has not started.

### Option C: Tiered. Portable modules on Win64, Mac and Linux; binary-backed transports on Win64 only
Shared, Sender, Receiver, Loopback and Sockets get `["Win64", "Mac", "Linux"]`. NNG, WebRTC and MoQ keep `["Win64"]`.
- **Pros:** Mac and Linux users get the core feature (LiveLink receive, skeletal send over UDP/TCP) at low marginal cost. There are no new third-party binaries.
- **Cons:** it requires ADR 0003 first, since the core must be compiled from source. It also requires a decision on Opus off Win64 (engine libOpus or none; FAB-10, SHR-1), confirmation that `AudioCaptureCore`, `AudioMixer` and `LiveLink` exist on those platforms (**needs-verification**, Q5), and at least a Linux UBT build in CI.
- **Cost / effort:** M after ADR 0003 lands.
- **Risk:** medium. It is safe only if CI builds each listed platform.

### Option D: `PlatformDenyList` naming the unsupported platforms
- **Pros:** the same size of change as A.
- **Cons:** it is open-ended. Any platform not on the list (a console, or a platform added in a later engine) is implicitly allowed and hits the throw. It inverts driver 1.
- **Cost / effort:** S. **Risk:** medium. Rejected.

## Decision

**Adopt Option A for the first Fab release.** Option C is kept as a documented, unscheduled future option; the maintainer decided on 2026-09-29 that v1.1 will not add Mac or Linux. Option B is handled per transport in later ADRs.

**1. Descriptor (WP-F2).** Add the following to every entry in `Plugin/Open3DBroadcast.uplugin`:

```json
{
  "Name": "Open3DSender",
  "Type": "Runtime",
  "LoadingPhase": "Default",
  "PlatformAllowList": [ "Win64" ],
  "TargetDenyList": [ "Server", "Program" ]
}
```

Add at plugin level:

```json
"SupportedTargetPlatforms": [ "Win64" ],
```

Use the same shape for the new modules introduced by ADR 0003 (`Open3DStreamCore`), D9 (the Editor module, which keeps `PlatformAllowList` but uses `Type: Editor` and needs no target list) and D10 (the test module). The key names `PlatformAllowList`, `TargetDenyList` and `SupportedTargetPlatforms` are the UE 5 names as far as can be seen from search results. **needs-verification** against UE 5.7 `ModuleDescriptor.cs` and `PluginDescriptor.cs` (Q2).

**2. Target types (FAB-8).** v1 supports **Editor, Game and Client**. Server and Program are excluded explicitly.
- In each Build.cs, the attribute becomes `[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]`, so the descriptor and the rules agree.
- Why Client is included: a Client target is a Game target without server code, and nothing in the plugin depends on the server.
- Why Server is excluded: a dedicated server has no audio capture or rendering. The sender's pose sampling on a server and LiveLink on a server are untested.
- **Receiver-on-Server (Q6, answered 2026-09-29):** the maintainer does not need it now and expects that to stay the case, but does not want it ruled out. So v1 excludes Server, and the following constraints keep enabling it later a small change:
  - Receiver runtime code must not take an unconditional dependency on client-only or editor-only engine modules. Editor code moves to the D9 Editor module, and audio playback (`UO3DRemoteAudioComponent`) stays optional at runtime.
  - Enabling it later means removing `Server` from the Receiver's (and its transports') `TargetDenyList`, adding `TargetType.Server` to their `SupportedTargetTypes`, and adding a headless Server build and a test to CI. It gets its own WP when requested.
  - Whether `LiveLink` and `LiveLinkInterface` are built for Server targets is **needs-verification** at that point.

**3. Build.cs `throw`s become clean exclusion (WP-F2).** The descriptor is the primary mechanism. Build.cs only has to stay correct if a user edits the allow list.
- **Open3DSender, Open3DReceiver:** once ADR 0003 lands, these modules link no binaries and the platform branch (`:24-48`) is deleted outright. If WP-F2 lands before WP-F1, leave the branch in place; the allow list makes it unreachable.
- **Open3DShared:** no throw today. Emit a build-time warning when Opus is unavailable (SHR-21). Remove the libs that are there "for tests" (SHR-20, D10).
- **Open3DTransportNNG, WebRTC, MoQ:** replace each `throw` with the stub path that MoQ already has:
  - set `O3D_WITH_TRANSPORT_<X>=0` for this module;
  - still add `Core`, `CoreUObject`, `Engine` and `Projects`;
  - wrap every translation unit in `#if O3D_WITH_TRANSPORT_<X>` (TRF-27, TRB-24);
  - compile a stub `IModuleInterface` that logs once that the transport is unavailable on this platform.
- **Delete the dead Linux/Mac branches** in `Open3DTransportMoQ.Build.cs` (`:25-32`, `:50-57`, `:99-114`) (BUILD-2) and the WebRTC `#error` (`Open3DTransportWebRTCModule.cpp:753`).
- **`O3DBuildFlags`** computes the platform-dependent overrides per target and caches only the environment values (SHR-22). It moves out of `Open3DShared.Build.cs` (WP-F2).

**4. Listing and docs.** The Fab listing, `Plugin/README.md` (whose "Platform Support" section is at `:41-46`) and USER_GUIDE state "Unreal Engine 5.7, Windows 64-bit only" (DOC-6, WP-F9, WP-D2).

## What happens to a non-Win64 target

| Situation | Today | After this ADR (Option A) |
|---|---|---|
| Linux/Mac/Android **game** target, plugin enabled | `BuildException` from the Sender, Receiver, NNG or WebRTC Build.cs; the whole target fails | The plugin's modules are skipped for that platform and the target builds (**needs-verification**, Q1). Levels or Blueprints that reference plugin classes load with missing-class warnings on that platform. |
| Same, and the user's own game module lists `Open3DSender` in its Build.cs | Fails | Still fails: the user's module references a module that is not built. The user must make the dependency conditional on Win64. This goes in the USER_GUIDE. |
| Mac or Linux **editor** | Fails | Plugin modules are not loaded. The editor starts, and the plugin is shown as unavailable on this platform (**needs-verification**, Q1). |
| Win64 **Server** target | Undefined (FAB-8) | Modules skipped by `TargetDenyList`. |
| Win64 **Client** target | Excluded by the Build.cs attribute | Supported. |

## Consequences

- **Easier:**
  - WP-F2 can land immediately and independently of D2 and D3.
  - WP-F8's `fab-package` job only has to build one platform.
  - The Fab checklist rows "PlatformAllowList on each module" and "Builds or cleanly excludes on every listed platform" can pass.
- **Harder:**
  - Mac and Linux users are turned away in v1.
  - Anyone who references plugin modules from their own code has to write a platform condition.
- **Constrains:**
  - Every new module (ADR 0003, D9, D10) must carry the same keys. The review agent checks this (a grep in WP-F8's package-content step).
  - The stub pattern for transports is folded into WP-A1's shared transport base, so the fourth copy is not written by hand.
- **Enables:**
  - A future Option C (not scheduled; not in v1.1), which **depends on ADR 0003**. With the core compiled from source, the five portable modules have no Win64-only binaries except Opus.
  - What a Mac/Linux expansion needs:
    1. ADR 0003 implemented.
    2. Opus solved off Win64: the engine's libOpus (FAB-10, **needs-verification**) or `O3D_WITH_OPUS=0` with the SHR-1 mislabelling fixed.
    3. Engine-module availability confirmed on those platforms (Q5).
    4. A Linux UBT build job in WP-F8, and Mac if it is listed.
    5. The descriptor lists widened per module.
    6. The listing updated.
  - A later per-transport expansion needs Linux/Mac builds of `nng` (or a source build, see ADR 0003), `moq_ffi` and `livekit_ffi`, with macOS signing and notarization of `.dylib`s (**needs-verification**).

## Implementation outline

1. **WP-F2a (descriptor):** `Plugin/Open3DBroadcast.uplugin`. Add `PlatformAllowList`, `TargetDenyList` and plugin-level `SupportedTargetPlatforms`.
2. **WP-F2b (target types):** all eight `Plugin/Source/*/*.Build.cs`. Change the `SupportedTargetTypes` attribute to Editor, Game, Client.
3. **WP-F2c (flags):** move `O3DBuildFlags` out of `Open3DShared.Build.cs:72-189` into its own rules file. Make the platform-dependent values per target. Auto-disable NNG and WebRTC off Win64 as MoQ already is.
4. **WP-F2d (transport stubs):** in `Open3DTransportNNG.Build.cs`, `Open3DTransportWebRTC.Build.cs` and `Open3DTransportMoQ.Build.cs`, replace the throws with the stub path. Add `#if O3D_WITH_TRANSPORT_*` to every TU under `Private/`, following `Open3DTransportMoQModule.cpp:710-727`. Delete the dead branches.
5. **WP-F1 (after ADR 0003):** delete the platform branches in `Open3DSender.Build.cs:24-48` and `Open3DReceiver.Build.cs:24-48`.
6. **WP-F9 / WP-D2:** update `Plugin/README.md:41-46`, USER_GUIDE and the listing text.
7. **Not scheduled:** Option C (widen five modules, add a Linux CI build). No WP is created until the maintainer asks for Mac or Linux support.

## Verification / acceptance

- `grep -c PlatformAllowList Plugin/Open3DBroadcast.uplugin` equals the number of module entries. A CI step enforces this (WP-F8).
- `RunUAT BuildPlugin -TargetPlatforms=Win64` passes from the Fab source zip (WP-F1, WP-F8).
- A **Linux game target** of the sandbox project with the plugin enabled configures and builds without error, with the plugin's modules excluded. This can be a cross-compile on the Windows runner if the toolchain is installed; otherwise it is done once by hand and recorded in the PR (WP-F2 acceptance).
- A Win64 **Client** target builds. A Win64 **Server** target builds with the plugin's modules skipped.
- With `O3D_WITH_TRANSPORT_NNG=0`, `..._WEBRTC=0` and `..._MOQ=0` each set in turn, the Win64 editor builds and the stub modules load (TRB-24, TRF-27).

## Open questions for the maintainer

1. **needs-verification:** in UE 5.7, when an enabled plugin's modules all exclude the target platform, does UBT skip them silently, warn, or error? Does the plugin-level `SupportedTargetPlatforms` change that (for example by disabling the plugin for that platform)? What does the editor show on Mac or Linux?
2. **needs-verification:** are the exact 5.7 key names `PlatformAllowList`, `PlatformDenyList`, `TargetAllowList`, `TargetDenyList` and `SupportedTargetPlatforms`, and is `HasExplicitPlatforms` needed alongside them?
3. **needs-verification:** does UBT still construct the `ModuleRules` (and so run the Build.cs) for a module excluded by `PlatformAllowList` or `TargetDenyList`? If it does, the stub path in step 4 is required and not only defensive.
4. **needs-verification (WP-F0):** which platforms does Fab's compile farm build a code plugin for, and must every platform in `SupportedTargetPlatforms` compile there?
5. **needs-verification:** are `AudioCaptureCore`, `AudioMixer`, `LiveLink`, `LiveLinkAnimationCore` and `AnimGraphRuntime` available on Mac and Linux targets in 5.7 (needed for Option C)?
6. ~~Do you want Receiver-on-Server (headless ingest) in a later release?~~ **Answered 2026-09-29:** not needed now, likely to stay that way, but must not be ruled out. See Decision §2 for the constraints that keep it open.
7. ~~Is Option C wanted for v1.1?~~ **Answered 2026-09-29:** no Mac/Linux support in v1.1. Q5 only becomes relevant if that changes.

## References

- Findings: FAB-3, FAB-8, SND-11, RCV-30, TRB-41, TRF-27, SHR-21; also SHR-1, SHR-20, SHR-22, TRB-24, BUILD-2, DOC-6.
- Files: `Plugin/Open3DBroadcast.uplugin`; `Plugin/Source/*/*.Build.cs`; `Plugin/Source/Open3DTransportMoQ/Private/Open3DTransportMoQModule.cpp`; `Plugin/Source/Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp`; `src/o3ds/getTime.cpp`; `Build/Scripts/Build-Plugin.ps1`; `.github/workflows/core-tests.yml`.
- External, retrieved 2026-09-29:
  - WebSearch snippets of "(Fab) Technical Requirements" and "(Fab) Plugin Compilation Environment" (support.fab.com). They state the module key requirement, and that Linux cross-compilation "is not yet supported on the Marketplace".
  - A WebSearch snippet of the `FModuleDescriptor` UE 5.7 API page.
  - Direct fetches of `dev.epicgames.com`, `forums.unrealengine.com` and `support.fab.com` were **blocked** by the sandbox egress proxy.
  - The UE source mirror (`lifelike-and-believable/UnrealEngine`) is not attached to this session, so `ModuleDescriptor.cs` could not be read.
