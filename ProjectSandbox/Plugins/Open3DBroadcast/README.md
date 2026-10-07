# Open3DBroadcast Plugin

An Unreal Engine plugin that streams skeletal animation, curves, audio and control data between Unreal Engine instances and other tools, and feeds received animation into LiveLink. It implements the Open3DStream protocol.

## Requirements and status

- **Unreal Engine:** 5.7. Other engine versions are not supported.
- **Platform:** Win64 (Windows 64-bit) only, for editor and game targets. Server and Program targets are not supported.
- **Status:** Beta. The MoQ transport (`Open3DTransportMoQ`) is Experimental: it implements draft-ietf-moq-transport-07, MoQ relays must speak draft-07, and its options and behaviour can change between releases.
- **WebRTC (LiveKit):** not part of this plugin. It is the free add-on plugin **Open3DBroadcastWebRTC** (`ProjectSandbox/Plugins/Open3DBroadcastWebRTC/` in the repository), installed next to this one. Download: **[DOWNLOAD LINK PLACEHOLDER: support-site URL for Open3DBroadcastWebRTC, to be added before release]**.
- **Support:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)

## Documentation

- [USER_GUIDE.md](USER_GUIDE.md): setup, the quick start, sending, receiving, audio, control, transports and troubleshooting.
- [Transport_Module_Comparison.md](Transport_Module_Comparison.md): how the transports differ (delivery, backpressure, audio, configuration).
- The transport READMEs (`Source/Open3DTransport*/README.md` in the [repository](https://github.com/lifelike-and-believable/Open3DBroadcast); not in the Fab package): options and details for each transport.
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and [LICENSE](LICENSE).

## Installation

### From Fab

1. In the Epic Games Launcher, open **Fab Library**, find Open3DBroadcast and choose **Install to Engine** for Unreal Engine 5.7.
2. Open your project, enable **Open3DBroadcast** in **Edit → Plugins**, and restart the editor.

### From a GitHub release or a package you built

1. Copy the `Open3DBroadcast` folder into your project's `Plugins/` folder. A release zip has it at `UE_5.7/Plugins/Open3DBroadcast`.
2. Open the project, enable **Open3DBroadcast** in **Edit → Plugins**, and restart the editor.

### Updating and removing

- To update, close the editor and replace the plugin: through the launcher for an engine install, or by replacing the `Plugins/Open3DBroadcast` folder. If you use the WebRTC add-on, update it to the same version; it only works with the matching Open3DBroadcast release.
- To remove it, disable it in **Edit → Plugins**, close the editor, and delete the folder (or uninstall it from the engine in the launcher). Remove Open3DBroadcast components and LiveLink sources from your assets first, or they load as missing.

## Network ports

Open these in the firewall of the machine that listens. A receiver and sender on one machine need nothing.

| Transport | Default port | Who listens | Notes |
|---|---|---|---|
| Loopback | none | | In-process only, sender and receiver in the same editor or game. |
| TCP | 17700 | The sender | It serves one receiver at a time. |
| UDP | 17800 | The receiver | The sender sends to it. Optional broadcast; no multicast. |
| NNG | 6000 (pub/sub), 7000 (pair), 8000 (push/pull) | By mode: the pub and pair sender, the pull receiver | TCP only. |
| MoQ | none locally | The relay | Outbound QUIC (UDP) to the relay's port. |
| WebRTC (add-on) | none locally | The LiveKit server | Outbound WSS, usually port 443, plus the TURN fallback the server offers. |

## Known limitations

- Win64 and Unreal Engine 5.7 only; no Server targets (see "Platforms and target types" below).
- No sample map or assets ship with the plugin. The USER_GUIDE quick start builds a working setup in a few steps.
- MoQ is Experimental, and the WebRTC transport is a separate add-on.
- Residual coding needs a transport that delivers every frame in order (Loopback, TCP, NNG pair or push/pull, WebRTC by default). The sender warns when the selected transport does not.
- The sender captures audio only when its **Enable Audio** is on (off by default). In the default **Mix (Main Submix or Custom)** capture mode it sends the game's mixed audio; the microphone is used only with **Audio Capture Mode** set to **Input (Microphone)**. The captured audio is sent to every receiver of the stream.

## Features

- A sender component that captures a skeletal mesh's pose, curves and, optionally, microphone audio, and streams them.
- A LiveLink source that receives streams and drives animation through LiveLink.
- Transports: Loopback, TCP, UDP, NNG and MoQ (Experimental) in this plugin; WebRTC with the free Open3DBroadcastWebRTC add-on.
- A control channel for sending values and events alongside the animation.
- Blueprint access to the sender, the receiver and the control channel.
- Everything it compiles or links is in the plugin: no external dependencies or pre-build steps.

## Plugin Modules

### Core Modules

- **Open3DStreamCore**: The Open3DStream core library (serialization, sequencing, reordering, prediction), compiled from source. See "Third-Party Dependencies" below.
- **Open3DShared**: the transport interfaces and registry, the send queue and receive demux transports share, the audio codec, the control bus, credentials and metrics ([its README](Source/Open3DShared/README.md) lists them with their threading rules)
- **Open3DSender**: Captures and streams skeletal animation data
- **Open3DReceiver**: Receives and applies animation data to characters

### Transport Modules

- **Open3DTransportLoopback**: In-process loopback for testing
- **Open3DTransportSockets**: TCP/UDP socket transport
- **Open3DTransportNNG**: NNG (nanomsg-next-generation) messaging
- **Open3DTransportMoQ** (Experimental): Media over QUIC (draft-07) through a MoQ relay

WebRTC (`Open3DTransportWebRTC`) is the separate Open3DBroadcastWebRTC add-on plugin (ADR 0002, WP-F11). It registers through the same public transport registries as the modules above and checks `O3D_TRANSPORT_API_VERSION` (`Open3DShared/Public/Transport/O3DTransportApiVersion.h`) before it does. Raise that number in the same change as any edit to the exported transport interface; the header says which types it covers.

### Editor Module

- **Open3DBroadcastEditor** (editor only, Win64, included in the Fab package): the Sender component's Details panel, the Receiver's LiveLink "Add Source" panel, and one transport settings panel built from each transport's declared options (ADR 0010). The runtime modules above contain no editor or Slate code, which CI checks (`Build/Scripts/check-runtime-editor-deps.py`).

### Test Module

- **Open3DBroadcastTests** (editor only, Win64, left out of the Fab package): the `Open3DBroadcast.*` automation tests, fake transports and the transport conformance suite (ADR 0006). Internet tests (`Open3DBroadcast.Network.*`) register only when `O3DB_NETWORK_TESTS=1`.

## For developers

The rest of this file is about building the plugin from the [Open3DBroadcast repository](https://github.com/lifelike-and-believable/Open3DBroadcast). Paths such as `Build/` and `docs/` are relative to the repository root.

### Third-Party Dependencies

Everything the plugin compiles or links is under `Source/`, so `RunUAT BuildPlugin` and the Fab source package need nothing else. `Config/FilterPlugin.ini` only adds the documentation and licence files at the plugin root to the package.

- **Open3DStreamCore** (source, `Source/ThirdParty/`): the part of the Open3DStream core (`src/o3ds` in the repository) that the plugin uses, the generated `o3ds_generated.h` and the FlatBuffers 2.0.6 runtime headers. The `Open3DStreamCore` module compiles it. It is a generated copy: see "Open3DStreamCore: the core compiled from source" below.
- **opus** (prebuilt Win64 library, `Source/ThirdParty/`): audio codec used by `Open3DShared`. Version and provenance are in `THIRD_PARTY_LICENSES.md`.
- **nng** (prebuilt Win64 library, in `Source/Open3DTransportNNG/ThirdParty/`): messaging library for the NNG transport.
- **moq-ffi** (DLL, in `Source/Open3DTransportMoQ/ThirdParty/`). `livekit_ffi` belongs to the WebRTC add-on and is not in this plugin.

The prebuilt libraries (Opus, NNG, the MoQ DLL) exist for **Win64** only, and every module is limited to Win64 (see "Platforms and target types" below). The core itself is plain C++17 source with no platform code.

### Building

The plugin builds directly with Unreal Engine's build system (UAT). No pre-build steps, CMake or scripts are required: a clean clone builds with `RunUAT BuildPlugin` alone.

#### Open3DStreamCore: the core compiled from source

`Open3DStreamCore` compiles the Open3DStream core from `Source/ThirdParty/Open3DStreamCore/` (ADR 0003, `docs/adr/0003-core-library-delivery-to-plugin.md`). That folder, and the `Source/Open3DStreamCore/Private/Core/O3DSCore_*.cpp` files that compile it, are generated from `src/` in the repository by `Build/Scripts/sync_o3ds_core.py`. Do not edit them by hand:

1. Change the core in `src/o3ds/` (or `src/o3ds.fbs`, then regenerate `src/o3ds_generated.h` with `flatc --cpp -o src src/o3ds.fbs`).
2. Run `python3 Build/Scripts/sync_o3ds_core.py` (needs `git submodule update --init thirdparty/flatbuffers thirdparty/crccpp`).
3. Commit both. CI (`core-tests.yml`) runs the script with `--check` and fails when the copy differs.

A new core header that plugin code includes must be listed in `Build/o3ds-core-manifest.txt`. The module is built without exceptions and RTTI, and the core's own compiler warnings are switched off in the plugin build (the core CI checks them). Core classes and functions defined in a `.cpp` and used by other modules carry `O3DS_API` (`src/o3ds/o3ds_export.h`), which the module defines as its export macro.

#### Local Development

1. Open `ProjectSandbox/ProjectSandbox.uproject` in Unreal Editor
2. The plugin will be automatically compiled when you open the project
3. Enable the plugin in Edit → Plugins if not already enabled

#### Packaging for Distribution

Use the Unreal Automation Tool (UAT) to package the plugin:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin `
  -Plugin="ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -Package="Output\Open3DBroadcast" `
  -TargetPlatforms=Win64 `
  -Configuration=Shipping
```

Or use the provided build script:

```powershell
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.7" `
  -PluginUPluginPath "ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -OutDir "Output\Open3DBroadcast" `
  -TargetPlatforms @("Win64") `
  -Configuration "Shipping"
```

### CI/CD Workflows

The plugin has automated GitHub Actions workflows for:

- **CI** (`open3dbroadcast-plugin-ci.yml`): Builds the plugin, runs the automation tests and checks the Fab source package on PRs and commits
- **Tests** (`open3dbroadcast-plugin-test.yml`): Manual build and test of any branch
- **Nightly** (`open3dbroadcast-plugin-nightly.yml`): Daily comprehensive builds
- **Release** (`open3dbroadcast-plugin-release.yml`): Builds, tests and publishes a release from an `open3dbroadcast-vX.Y.Z` tag

See `Build/README.md` in the repository ("CI/CD Integration" and "Releases") for details.

### Build Configuration

#### Build flags

Developer builds can switch transports off with environment variables, set before running UBT or `RunUAT BuildPlugin`. Values are `0`/`1` or `true`/`false`.

| Variable | Description | Default |
|----------|-------------|---------|
| `O3D_WITH_TRANSPORT_SOCKETS` | TCP/UDP sockets transport | `1` |
| `O3D_WITH_TRANSPORT_NNG` | NNG transport | `1` on Win64, always `0` elsewhere |
| `O3D_WITH_TRANSPORT_MOQ` | MoQ transport | `1` on Win64, always `0` elsewhere |

A transport that is off is still a module of the plugin, but it compiles to a stub that registers nothing and logs `Open3D <name> transport is not available in this build` at startup. The flags are read by `O3DBuildFlags` in `Source/Open3DBroadcastBuildFlags/Open3DBroadcastBuildFlags.Build.cs`, again for every target, so one UBT run can build targets with different results. The nightly workflow builds each combination (`Build/Scripts/Build-FlagCombinations.ps1`).

Open3DSender and Open3DReceiver are always built. `O3D_BUILD_SENDER=0` and `O3D_BUILD_RECEIVER=0` are rejected with a build error: they never produced a working build. `O3D_ENABLE_LEGACY` is no longer read. `O3D_WITH_TRANSPORT_WEBRTC` belongs to the Open3DBroadcastWebRTC add-on (see its README).

#### Platforms and target types

Every module entry in `Open3DBroadcast.uplugin` has `"PlatformAllowList": [ "Win64" ]`, and the plugin has `"SupportedTargetPlatforms": [ "Win64" ]` (ADR 0001, `docs/adr/0001-platform-scope-first-fab-release.md`). The runtime modules also have `"TargetDenyList": [ "Server", "Program" ]`, matching `[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]` in their `Build.cs`:

- **Editor, Game and Client** targets are supported. A Client target is a Game target without server code, and nothing in the plugin needs the server.
- **Server** is not supported in v1: a dedicated server has no audio capture, and sending or receiving (LiveLink) on a server is untested. ADR 0001 (decision 2) lists what enabling the receiver on Server later takes.
- **Program** targets are excluded.

If your own module depends on an Open3DBroadcast module, add that dependency only for Win64 (for example `if (Target.Platform == UnrealTargetPlatform.Win64)` in your `Build.cs`); otherwise your module will reference a module that is not built for the other platforms.

### Updating Third-Party Libraries

The core, FlatBuffers and CRC++ are updated through `src/` and the submodule pins, then `Build/Scripts/sync_o3ds_core.py` (see "Open3DStreamCore" above). To update a prebuilt library:

1. Build the new version for your target platform(s)
2. Replace the library in `Source/ThirdParty/<library>/lib/<Platform>/` (Opus) or `Source/<Module>/ThirdParty/<library>/` (NNG, moq-ffi; livekit_ffi is in the WebRTC add-on)
3. Update the headers next to it if needed
4. Update version information in `Source/ThirdParty/README.md` or the library's own README, and in `THIRD_PARTY_LICENSES.md`
5. Test the plugin builds and runs correctly
6. Commit the updated libraries

**Note**: Every third-party file the build needs is committed under `Source/`, so the plugin stays self-contained. Keep new ones there too: files elsewhere in the plugin folder are packaged only if `Config/FilterPlugin.ini` lists them.

## License

The plugin's own licence is in [LICENSE](LICENSE) (MIT). Third-party components and their licences are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
