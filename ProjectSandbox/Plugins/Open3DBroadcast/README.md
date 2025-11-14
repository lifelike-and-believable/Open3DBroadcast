# Open3DBroadcast Plugin

A modular, self-contained Unreal Engine plugin for Open3DStream broadcasting and receiving, designed for easy distribution via Unreal Marketplace and Fab.

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

- **CI** (`open3dbroadcast-plugin-ci.yml`): Validates PRs and commits
- **Tests** (`open3dbroadcast-plugin-test.yml`): Runs on feature branches
- **Nightly** (`open3dbroadcast-plugin-nightly.yml`): Daily comprehensive builds
- **Release** (`open3dbroadcast-plugin-release.yml`): Creates releases for version tags

See [.github/workflows/README.md](../../.github/workflows/README.md) for details.

## Build Configuration

The plugin supports build-time configuration via environment variables:

| Variable | Description | Default |
|----------|-------------|---------|
| `O3D_BUILD_SENDER` | Build sender module | `true` |
| `O3D_BUILD_RECEIVER` | Build receiver module | `true` |
| `O3D_WITH_TRANSPORT_SOCKETS` | Enable sockets transport | `true` |
| `O3D_WITH_TRANSPORT_NNG` | Enable NNG transport | `true` |
| `O3D_WITH_TRANSPORT_WEBRTC` | Enable WebRTC transport | `true` |
| `O3D_WEBRTC_BACKEND_LIVEKIT` | Enable LiveKit WebRTC backend | `true` |
| `O3D_WEBRTC_BACKEND_LIBDC` | Enable libdatachannel backend | `true` |

Set these before building to customize which modules are included.

## Updating Third-Party Libraries

To update third-party libraries:

1. Build the new version for your target platform(s)
2. Replace the libraries in `ThirdParty/<library>/lib/<Platform>/`
3. Update headers in `ThirdParty/<library>/include/` if needed
4. Update version information in `ThirdParty/README.md`
5. Test the plugin builds and runs correctly
6. Commit the updated libraries

**Note**: All third-party libraries should be committed to the repository to maintain the self-contained nature of the plugin.

## License

See the main repository LICENSE file for details.

## Contributing

This plugin is part of the Open3DStream project. See the main repository README for contribution guidelines.

## Support

For issues specific to this plugin:
- Check the [GitHub Issues](https://github.com/lifelike-and-believable/Open3DStream/issues)
- Review the [workflows documentation](../../.github/workflows/README.md)

For Unreal Engine integration questions:
- See the [Build Scripts documentation](../../Build/README.md)
- Check the plugin's inline documentation
