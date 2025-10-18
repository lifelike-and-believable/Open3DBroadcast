# Open3DStream Plugin

This plugin provides both receiver and broadcaster capabilities for real-time animation streaming using the Open3DStream protocol.

## Modules

### Open3DStream (Receiver Module)
- **Purpose**: Receives animation data from external sources via LiveLink
- **Status**: Mature, production-ready
- **Key Components**:
  - `FOpen3DStreamSource` - LiveLink source implementation
  - Protocol support: TCP, UDP, WebRTC, NNG
  - Real-time animation curve and transform streaming

### Open3DBroadcast (Sender Module)
- **Purpose**: Broadcasts animation data FROM Unreal TO remote clients
- **Status**: Framework implemented, core functionality pending
- **Planned Components**:
  - `UO3DBroadcastSubsystem` - Broadcast management
  - `UO3DBroadcastComponent` - Per-actor broadcast control
  - `UO3DBroadcastSettings` - Configuration and URL management
- **Build Configuration**: Can be optionally disabled via `O3DS_WITH_BROADCAST=0`

## Installation

1. Copy this plugin folder to your project's `Plugins/` directory
2. Enable the plugin in your project settings
3. Both modules will be available automatically

## Build Configuration

The broadcast module can be conditionally compiled:

### To disable the broadcast module:
Add to your Target.cs file:
```csharp
GlobalDefinitions.Add("O3DS_WITH_BROADCAST=0");
```

Or build with UBT flag:
```bash
-D O3DS_WITH_BROADCAST=0
```

### Default behavior:
Both modules are built and available by default.

## Dependencies

- Open3DBroadcast module depends on Open3DStream module for core networking
- Open3DStream module has no dependencies on Open3DBroadcast
- Both modules share the same core C++ library for protocol implementation

## Usage

### Receiving (Open3DStream module)
1. Open **Window → Virtual Production → Live Link**
2. Add source: **+ Source → Open3DStream Source**
3. Configure connection URL and protocol

### Broadcasting (Open3DBroadcast module)
*Implementation pending - framework is in place*

## Support

- Website: https://open3dstream.com/
- Repository: https://github.com/lifelike-and-believable/Open3DStream
- Issues: [GitHub Issues](https://github.com/lifelike-and-believable/Open3DStream/issues)