# Open3DBroadcast Plugin

A modular, self-contained Unreal Engine plugin for Open3DStream broadcasting and receiving, designed for easy distribution via Unreal Marketplace and Fab.

## Requirements and status

- **Unreal Engine:** 5.7. Other engine versions are not supported.
- **Platform:** Win64 (Windows 64-bit) only, for editor and game targets. Server and Program targets are not supported.
- **Status:** Beta. The MoQ transport (`Open3DTransportMoQ`) is Experimental: it implements draft-ietf-moq-transport-07, MoQ relays must speak draft-07, and its options and behaviour can change between releases.
- **Support:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)

## Features

- **Modular Architecture**: Separate sender, receiver, and transport modules
- **Multiple Transport Options**: Loopback, Sockets, NNG, WebRTC
- **Self-Contained**: All third-party dependencies included
- **Marketplace Ready**: No external dependencies or pre-build steps required

## Plugin Modules

### Core Modules

- **Open3DStreamCore**: The Open3DStream core library (serialization, sequencing, reordering, prediction), compiled from source. See "Third-Party Dependencies" below.
- **Open3DShared**: Shared utilities and base classes used by all modules
- **Open3DSender**: Captures and streams skeletal animation data
- **Open3DReceiver**: Receives and applies animation data to characters

### Transport Modules

- **Open3DTransportLoopback**: In-process loopback for testing
- **Open3DTransportSockets**: TCP/UDP socket transport
- **Open3DTransportNNG**: NNG (nanomsg-next-generation) messaging
- **Open3DTransportWebRTC**: WebRTC-based streaming with audio support
- **Open3DTransportMoQ** (Experimental): Media over QUIC (draft-07) through a MoQ relay

### Editor Module

- **Open3DBroadcastEditor** (editor only, Win64, included in the Fab package): the Sender component's Details panel, the Receiver's LiveLink "Add Source" panel, and one transport settings panel built from each transport's declared options (ADR 0010). The runtime modules above contain no editor or Slate code, which CI checks (`Build/Scripts/check-runtime-editor-deps.py`).

### Test Module

- **Open3DBroadcastTests** (editor only, Win64, left out of the Fab package): the `Open3DBroadcast.*` automation tests, fake transports and the transport conformance suite (ADR 0006). Internet tests (`Open3DBroadcast.Network.*`) register only when `O3DB_NETWORK_TESTS=1`.

## Third-Party Dependencies

Everything the plugin compiles or links is under `Source/`, so `RunUAT BuildPlugin` and the Fab source package need nothing else. `Config/FilterPlugin.ini` only adds the documentation and licence files at the plugin root to the package.

### Shared (`Source/ThirdParty/`)

- **Open3DStreamCore** (source): the part of the Open3DStream core (`src/o3ds` in the repository) that the plugin uses, the generated `o3ds_generated.h`, the FlatBuffers 2.0.6 runtime headers and CRC++'s `CRC.h`. The `Open3DStreamCore` module compiles it. It is a generated copy: see "Open3DStreamCore: the core compiled from source" below.
- **opus** (prebuilt Win64 library): audio codec used by `Open3DShared`. Version and provenance are in `THIRD_PARTY_LICENSES.md`.

### Module-Level Dependencies

- **nng** (prebuilt Win64 library, in `Source/Open3DTransportNNG/ThirdParty/`): messaging library for the NNG transport
- **moq-ffi** (DLL, in `Source/Open3DTransportMoQ/ThirdParty/`) and **livekit_ffi** (DLL, in `Source/Open3DTransportWebRTC/ThirdParty/`)

### Platform Support

The prebuilt libraries (Opus, NNG, the MoQ and LiveKit DLLs) exist for **Win64** only, and every module is limited to Win64 (see "Platforms and target types" below). The core itself is plain C++17 source with no platform code.

## Building

The plugin builds directly with Unreal Engine's build system (UAT). No pre-build steps, CMake or scripts are required: a clean clone builds with `RunUAT BuildPlugin` alone.

### Open3DStreamCore: the core compiled from source

`Open3DStreamCore` compiles the Open3DStream core from `Source/ThirdParty/Open3DStreamCore/` (ADR 0003, `docs/adr/0003-core-library-delivery-to-plugin.md`). That folder, and the `Source/Open3DStreamCore/Private/Core/O3DSCore_*.cpp` files that compile it, are generated from `src/` in the repository by `Build/Scripts/sync_o3ds_core.py`. Do not edit them by hand:

1. Change the core in `src/o3ds/` (or `src/o3ds.fbs`, then regenerate `src/o3ds_generated.h` with `flatc --cpp -o src src/o3ds.fbs`).
2. Run `python3 Build/Scripts/sync_o3ds_core.py` (needs `git submodule update --init thirdparty/flatbuffers thirdparty/crccpp`).
3. Commit both. CI (`core-tests.yml`) runs the script with `--check` and fails when the copy differs.

A new core header that plugin code includes must be listed in `Build/o3ds-core-manifest.txt`. The module is built without exceptions and RTTI, and the core's own compiler warnings are switched off in the plugin build (the core CI checks them). Core classes and functions defined in a `.cpp` and used by other modules carry `O3DS_API` (`src/o3ds/o3ds_export.h`), which the module defines as its export macro.

### Local Development

1. Open `ProjectSandbox/ProjectSandbox.uproject` in Unreal Editor
2. The plugin will be automatically compiled when you open the project
3. Enable the plugin in Edit → Plugins if not already enabled

### Packaging for Distribution

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

## Installation (End Users)

1. Download the plugin package
2. Extract to your project's `Plugins/` directory
3. Open your project in Unreal Editor
4. Enable Open3DBroadcast in Edit → Plugins
5. Restart the editor

## CI/CD Workflows

The plugin has automated GitHub Actions workflows for:

- **CI** (`open3dbroadcast-plugin-ci.yml`): Builds the plugin, runs the automation tests and checks the Fab source package on PRs and commits
- **Tests** (`open3dbroadcast-plugin-test.yml`): Manual build and test of any branch
- **Nightly** (`open3dbroadcast-plugin-nightly.yml`): Daily comprehensive builds
- **Release** (`open3dbroadcast-plugin-release.yml`): Creates releases for version tags

See [Build/README.md](../../../Build/README.md#cicd-integration) for details.

## Build Configuration

### Build flags

Developer builds can switch transports off with environment variables, set before running UBT or `RunUAT BuildPlugin`. Values are `0`/`1` or `true`/`false`.

| Variable | Description | Default |
|----------|-------------|---------|
| `O3D_WITH_TRANSPORT_SOCKETS` | TCP/UDP sockets transport | `1` |
| `O3D_WITH_TRANSPORT_NNG` | NNG transport | `1` on Win64, always `0` elsewhere |
| `O3D_WITH_TRANSPORT_WEBRTC` | WebRTC (LiveKit) transport | `1` on Win64, always `0` elsewhere |
| `O3D_WITH_TRANSPORT_MOQ` | MoQ transport | `1` on Win64, always `0` elsewhere |

A transport that is off is still a module of the plugin, but it compiles to a stub that registers nothing and logs `Open3D <name> transport is not available in this build` at startup. The flags are read by `O3DBuildFlags` in `Source/Open3DBroadcastBuildFlags/Open3DBroadcastBuildFlags.Build.cs`, again for every target, so one UBT run can build targets with different results. The nightly workflow builds each combination (`Build/Scripts/Build-FlagCombinations.ps1`).

Open3DSender and Open3DReceiver are always built. `O3D_BUILD_SENDER=0` and `O3D_BUILD_RECEIVER=0` are rejected with a build error: they never produced a working build. `O3D_WEBRTC_BACKEND_LIVEKIT`, `O3D_WEBRTC_BACKEND_LIBDC` and `O3D_ENABLE_LEGACY` are no longer read.

### Platforms and target types

Every module entry in `Open3DBroadcast.uplugin` has `"PlatformAllowList": [ "Win64" ]`, and the plugin has `"SupportedTargetPlatforms": [ "Win64" ]` (ADR 0001, `docs/adr/0001-platform-scope-first-fab-release.md`). The runtime modules also have `"TargetDenyList": [ "Server", "Program" ]`, matching `[SupportedTargetTypes(TargetType.Editor, TargetType.Game, TargetType.Client)]` in their `Build.cs`:

- **Editor, Game and Client** targets are supported. A Client target is a Game target without server code, and nothing in the plugin needs the server.
- **Server** is not supported in v1: a dedicated server has no audio capture, and sending or receiving (LiveLink) on a server is untested. ADR 0001 (decision 2) lists what enabling the receiver on Server later takes.
- **Program** targets are excluded.

If your own module depends on an Open3DBroadcast module, add that dependency only for Win64 (for example `if (Target.Platform == UnrealTargetPlatform.Win64)` in your `Build.cs`); otherwise your module will reference a module that is not built for the other platforms.

## Updating Third-Party Libraries

The core, FlatBuffers and CRC++ are updated through `src/` and the submodule pins, then `Build/Scripts/sync_o3ds_core.py` (see "Open3DStreamCore" above). To update a prebuilt library:

1. Build the new version for your target platform(s)
2. Replace the library in `Source/ThirdParty/<library>/lib/<Platform>/` (Opus) or `Source/<Module>/ThirdParty/<library>/` (NNG, moq-ffi, livekit_ffi)
3. Update the headers next to it if needed
4. Update version information in `Source/ThirdParty/README.md` or the library's own README, and in `THIRD_PARTY_LICENSES.md`
5. Test the plugin builds and runs correctly
6. Commit the updated libraries

**Note**: Every third-party file the build needs is committed under `Source/`, so the plugin stays self-contained. Keep new ones there too: files elsewhere in the plugin folder are packaged only if `Config/FilterPlugin.ini` lists them.

## MoQ Draft-07 Hotfix & Fallback

- **Hotfix dependency**: `moq_ffi` now patches the vendored `moq-rs` submodule (see `moq_ffi/Cargo.toml`). Remove the `[patch]` stanza to fall back to upstream Cloudflare bits if needed.
- **Runtime fallback**: Set the advanced transport option `delivery_mode=datagram` (or `moq.delivery=datagram`) to bypass stream groups entirely. This avoids the code path that triggered the Draft-07 reader panic and mirrors the `moq-pub`/`moq-sub` single-object behavior if the new stream fix is insufficient.

## License

See the main repository LICENSE file for details.

## Contributing

This plugin is part of the Open3DStream project. See the main repository README for contribution guidelines.

## Support

For issues specific to this plugin:
- Check the [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)
- Review the [workflows documentation](../../.github/workflows/README.md)

For Unreal Engine integration questions:
- See the [Build Scripts documentation](../../Build/README.md)
- Check the plugin's inline documentation
