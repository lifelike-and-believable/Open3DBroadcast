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

- **Open3DShared**: Shared utilities and base classes used by all modules
- **Open3DSender**: Captures and streams skeletal animation data
- **Open3DReceiver**: Receives and applies animation data to characters

### Transport Modules

- **Open3DTransportLoopback**: In-process loopback for testing
- **Open3DTransportSockets**: TCP/UDP socket transport
- **Open3DTransportNNG**: NNG (nanomsg-next-generation) messaging
- **Open3DTransportWebRTC**: WebRTC-based streaming with audio support
- **Open3DTransportMoQ** (Experimental): Media over QUIC (draft-07) through a MoQ relay

### Test Module

- **Open3DBroadcastTests** (editor only, Win64, left out of the Fab package): the `Open3DBroadcast.*` automation tests, fake transports and the transport conformance suite (ADR 0006). Internet tests (`Open3DBroadcast.Network.*`) register only when `O3DB_NETWORK_TESTS=1`.

## Third-Party Dependencies

All third-party libraries are pre-compiled and included in the plugin:

### Plugin-Level Dependencies (ThirdParty/)

- **open3dstream** (v1.0): Core Open3DStream protocol implementation
- **flatbuffers** (v24.3.25): Efficient serialization library
- **opus** (v1.5.2): High-quality audio codec for WebRTC

### Module-Level Dependencies

- **nng** (in Open3DTransportNNG): Messaging library for pub/sub patterns

### Platform Support

Currently includes pre-compiled libraries for:
- **Win64**: Full support for all modules

Additional platforms can be added by compiling libraries for the target platform and placing them in the appropriate `lib/<Platform>/` directories.

## Building

The plugin builds directly with Unreal Engine's build system (UAT). No pre-build steps are required.

### Local Development

1. Open `ProjectSandbox/ProjectSandbox.uproject` in Unreal Editor
2. The plugin will be automatically compiled when you open the project
3. Enable the plugin in Edit → Plugins if not already enabled

### Packaging for Distribution

Use the Unreal Automation Tool (UAT) to package the plugin:

```powershell
& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin `
  -Plugin="ProjectSandbox\Plugins\Open3DBroadcast\Open3DBroadcast.uplugin" `
  -Package="Output\Open3DBroadcast" `
  -TargetPlatforms=Win64 `
  -Configuration=Shipping
```

Or use the provided build script:

```powershell
.\Build\Scripts\Build-Plugin.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.6" `
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

To update third-party libraries:

1. Build the new version for your target platform(s)
2. Replace the libraries in `ThirdParty/<library>/lib/<Platform>/`
3. Update headers in `ThirdParty/<library>/include/` if needed
4. Update version information in `ThirdParty/README.md`
5. Test the plugin builds and runs correctly
6. Commit the updated libraries

**Note**: All third-party libraries should be committed to the repository to maintain the self-contained nature of the plugin.

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
