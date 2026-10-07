# Open3DBroadcast WebRTC (add-on plugin)

The WebRTC (LiveKit) transport for Open3DBroadcast, as a separate, free plugin. It installs next to Open3DBroadcast (from Fab or from a GitHub release) and adds "WebRTC" to the sender and receiver transport pickers. See [USER_GUIDE.md](USER_GUIDE.md) for installation and use.

## Requirements and status

- **Open3DBroadcast:** required, and it must be the release this add-on was built for (see "Transport API version" below).
- **Unreal Engine:** 5.7 and 5.8, the same engine as the Open3DBroadcast package it is built against. **Platform:** Win64 only, for editor and game targets; Server and Program targets are excluded (ADR 0001).
- **Status:** Beta.
- **Download:** **[DOWNLOAD LINK PLACEHOLDER: support-site URL for Open3DBroadcastWebRTC, to be added before release]**
- **Support:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)

## Why a separate plugin

`livekit_ffi.dll` statically links ffmpeg and OpenH264 (see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)), so WebRTC is not in the Fab package of Open3DBroadcast. ADR 0002 (`docs/adr/0002-webrtc-and-moq-in-first-fab-release.md`) chose to ship it as this add-on instead of a module added into the Fab-installed plugin: UE loads only the modules a plugin's `.uplugin` lists, and editing a Fab install's descriptor would be undone by every Fab update. WebRTC can return to the main plugin once `livekit_ffi` is rebuilt without the codecs and counsel has signed off (ADR 0002, Decision 1).

## Contents

- **Open3DTransportWebRTC** (Runtime, Win64): the transport. It keeps its module name from when it lived in Open3DBroadcast. It has no editor code: Open3DBroadcast's editor module draws its settings panel from the option schema it registers (ADR 0010).
- `Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/`: the LiveKit client library (DLL, import library, header, provenance and notices).
- Automation tests under `Source/Open3DTransportWebRTC/Private/Tests/`, named `Open3DBroadcast.Transport.WebRTC.*` so the usual `Open3DBroadcast` filter runs them.

## How it plugs into Open3DBroadcast

- The `.uplugin` declares a plugin dependency on `Open3DBroadcast`, so UE loads Open3DBroadcast first.
- The module uses only exported, public API of Open3DBroadcast. The runtime code uses `Open3DShared` (including the shared transport blocks: `FO3DSendQueue` for the receiver's hand-off to `Poll`, `FO3DUnifiedReceiveDemux`, `O3DTransportOptions`; WP-A1 PR 4f) and `Open3DStreamCore` (for `O3DS::SubjectList`). Since WP-A1 PR 5a it links neither `Open3DSender` nor `Open3DReceiver`, not even for its tests: the configure functions take an `FO3DTransportOptionsView`, and the test helpers it uses are in `Open3DShared`. Its `Build.cs` never reaches into Open3DBroadcast's folders, so it builds wherever Open3DBroadcast is installed.
- Sending stays on the caller's thread: LiveKit's data channel buffers each message and refuses it when it cannot take it, and the refusal is returned at once. Audio is PCM16 to a LiveKit track per subject; LiveKit encodes and decodes Opus itself.
- At startup it:
  1. checks the transport API version (below) and stops if it differs;
  2. loads `livekit_ffi.dll` from **this** plugin's folder, through Open3DBroadcast's shared `FO3DFfiLibrary` loader, and stops if that fails;
  3. registers the "WebRTC" sender and receiver factories and their option schema.
- At shutdown it unregisters "WebRTC", which drains it: sender components and LiveLink sources stop and release their WebRTC instances, and the transport registry stops any left and reports them. Then it frees `livekit_ffi.dll`, only if the registry counts no WebRTC instance that is still referenced (TRF-14; ADR 0007 item 5). If it registered nothing, it undoes nothing.

### Transport API version

Open3DBroadcast defines `O3D_TRANSPORT_API_VERSION` in `Open3DShared/Public/Transport/O3DTransportApiVersion.h` and exports `O3DTransport::GetHostApiVersion()`. The add-on compares the value it was compiled with against the loaded Open3DBroadcast in `StartupModule`. On a mismatch it logs one error (`WebRTC transport not registered: Open3DBroadcastWebRTC was built for Open3DBroadcast transport API version ...`) and registers nothing, instead of calling into classes whose layout may have changed. Build and ship one add-on per Open3DBroadcast release.

Version 5 (WP-A1 PR 5a) is the typed config: the configure functions take the options view, and `FO3DTransportConfig` has no LiveKit fields. The transport reads `webrtc.useAutoTokenFetch`, `webrtc.tokenEndpointUrl` and `webrtc.tokenRefreshLeadTimeSec` as options and the token from `Config.Secrets`. Option keys, defaults and saved settings are unchanged. WP-A1 PR 5b is part of the same version: `ISerializedFrameConsumer` has a view form and an owned form of `SubmitFrame` (the receiver hands its frames over in the owned form), and `IOpen3DSender::Send(SubjectList)` is gone, so the add-on's runtime code no longer includes the o3ds core. WP-A1 PR 5c is part of it too: `FO3DTransportConfig::Transport` is the registered name (`FName`) and `Role` an `EO3DTransportRole`, and the add-on declares `webrtc.token` and `webrtc.tokenEndpointAuth` as `Secret` entries of its option schema, each with its environment variable. The keys, environment variables and the way secrets are stored are unchanged. WP-A1 step 6 is part of version 5 as well (no release carried the deprecated APIs): the forwarding registries, transport customizations, forwarding headers and `SupportsAudio()`/`SupportsControl()` are gone, so a transport registers one `FO3DTransportDescriptor` with `FO3DTransportRegistry`, declares secrets only with `Secret` schema entries, and asks `GetCapabilities()`. The add-on's runtime code needed no change; its tests now ask `GetCapabilities()`. An add-on built for version 4 does not load into it.

## Building

The add-on cannot be built with `RunUAT BuildPlugin` on its own, because BuildPlugin's generated host project contains only the plugin being built, and the Open3DBroadcast dependency is then missing. Options:

- **In the repository:** open `ProjectSandbox/ProjectSandbox.uproject`, which enables both plugins; the editor build compiles both.
- **Against a built Open3DBroadcast package (what CI does):**

  ```powershell
  .\Build\Scripts\Build-WebRTCAddOn.ps1 `
    -UEPath "C:\Program Files\Epic Games\UE_5.7" `
    -HostPluginPackageDir "Output\Open3DBroadcast" `
    -HostProjectDir "Output\AddOnHost" `
    -OutDir "Output\Open3DBroadcastWebRTC"
  ```

  It makes a throwaway project with both plugins in `Plugins/`, builds its editor target, and stages the add-on folder in `-OutDir`. `Run-AutomationTests.ps1 -ProjectFile Output\AddOnHost\O3DWebRTCHost.uproject` then runs every `Open3DBroadcast` test with both plugins enabled.

### Build flags

| Variable | Description | Default |
|----------|-------------|---------|
| `O3D_WITH_TRANSPORT_WEBRTC` | WebRTC (LiveKit) transport | `1` on Win64, always `0` elsewhere |

With `0` the module compiles to a stub that registers nothing and logs `Open3D WebRTC transport is not available in this build`. The flag is read by `O3DWebRtcBuildFlags` in `Open3DTransportWebRTC.Build.cs`, not by Open3DBroadcast's `O3DBuildFlags`, whose rules are in another rules assembly when Open3DBroadcast is installed under the engine. `O3D_WEBRTC_BACKEND_LIVEKIT` and `O3D_WEBRTC_BACKEND_LIBDC` are no longer read.

## CI and releases

- PR CI builds this add-on against the Open3DBroadcast package of the same commit (strict: no PCH, no unity build, warnings as errors), uploads it as `Open3DBroadcastWebRTC-Win64-<sha>`, and runs the tests with both plugins enabled.
- The release workflow builds it for every Open3DBroadcast release with the same version number. It is attached to the GitHub release only when the repository variable `O3D_PUBLISH_WEBRTC_ADDON` is `true`; until counsel answers ADR 0002 question L1 it is kept as a workflow artifact.
- `Build/Scripts/check-no-video-codecs.sh --addon` scans this plugin's binaries. It fails today by design (`docs/webrtc-codec-removal-plan.md`).

## License

MIT, see [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

The icon in `Resources/` is a copy of Open3DBroadcast's.
