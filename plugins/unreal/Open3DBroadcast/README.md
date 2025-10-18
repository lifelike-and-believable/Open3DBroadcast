# Open3DBroadcast

## Description

Open3DBroadcast is a companion plugin to Open3DStream that enables broadcasting of skeletal mesh pose and curve data from Unreal Engine to remote clients. This plugin captures final pose and curve values from multiple skeletal meshes and streams them using the Open3DStream protocol over supported network transports.

## Dependencies

This plugin requires the **Open3DStream** plugin to be installed and enabled in your Unreal Engine project. Open3DBroadcast depends on Open3DStream for:
- Network protocol implementation (TCP/UDP/WebRTC/NNG)
- FlatBuffers serialization
- Core streaming functionality

## Features (Planned)

- Broadcast skeletal mesh animation data in real-time
- Support for multiple skeletal meshes simultaneously
- Morph target (curve) data streaming
- Transform data streaming
- Support for all Open3DStream network transports:
  - TCP
  - UDP
  - WebRTC
  - NNG (IPC/TCP)

## Installation

1. Ensure Open3DStream plugin is installed in your Unreal Engine project
2. Copy the Open3DBroadcast plugin folder to your project's `Plugins` directory
3. Enable both plugins in your project's `.uproject` file or through the Plugins browser in the editor
4. Restart Unreal Editor

## Build Instructions

This plugin will be built automatically when building your Unreal Engine project. The build system will:
1. First build the Open3DStream plugin (if not already built)
2. Build Open3DBroadcast with Open3DStream as a dependency

### Manual Build

If building manually from the Unreal Build Tool:

```bash
# Windows
Engine\Build\BatchFiles\RunUAT.bat BuildPlugin -Plugin="Path\To\Open3DBroadcast.uplugin" -Package="Path\To\Output"

# Mac/Linux
Engine/Build/BatchFiles/RunUAT.sh BuildPlugin -Plugin="Path/To/Open3DBroadcast.uplugin" -Package="Path/To/Output"
```

## Usage

*Usage instructions will be added as the plugin is developed.*

## Development Status

This plugin is currently in **initial framework setup** phase. The basic plugin structure has been established, and future development will add:
- Animation capture components
- Streaming configuration
- Network transport selection
- Performance optimization

## License

This plugin follows the same license as the Open3DStream project. See the main project LICENSE file for details.

## Support

For support, issues, or questions:
- Visit: https://open3dstream.com/
- Project Repository: https://github.com/lifelike-and-believable/Open3DStream

## Credits

Created by Alastair Macleod
https://peeldev.com/
