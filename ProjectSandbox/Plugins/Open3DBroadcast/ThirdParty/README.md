# Open3DBroadcast Plugin Third-Party Dependencies

This directory contains third-party libraries and headers that are used across multiple modules in the Open3DBroadcast plugin.

## Structure

Each third-party library is organized as follows:

```
ThirdParty/
├── <library-name>/
│   ├── lib/
│   │   └── Win64/
│   │       └── *.lib
│   └── include/
│       └── *.h
```

## Libraries

### Opus (Audio Codec)
- **Version**: 1.5.2
- **License**: BSD 3-Clause
- **Used by**: Open3DShared, Open3DTransportWebRTC
- **Description**: High-quality audio codec for WebRTC audio streaming

### Flatbuffers (Serialization)
- **License**: Apache 2.0
- **Used by**: Open3DSender, Open3DReceiver
- **Description**: Memory-efficient serialization library

### Open3DStream (Core Library)
- **Used by**: Open3DSender, Open3DReceiver
- **Description**: Core Open3DStream protocol implementation

## Module-Specific Dependencies

Some modules have their own ThirdParty dependencies:

- **Open3DTransportNNG**: NNG (nanomsg-next-generation) messaging library
  - Located in: `Source/Open3DTransportNNG/ThirdParty/nng/`

## Platform Support

Currently, only Win64 binaries are included. Additional platforms can be added by:

1. Creating platform-specific subdirectories (e.g., `lib/Linux/`, `lib/Mac/`)
2. Updating the corresponding module's `.Build.cs` file to support the new platform
