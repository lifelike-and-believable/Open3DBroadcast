# libdatachannel Integration Guide

## Overview

Open3DStream uses libdatachannel for WebRTC support, providing real-time data channel communication. This document describes how libdatachannel is integrated into the project with a focus on CI compatibility and reproducible builds.

## Build Configuration

### Requirements

libdatachannel is built as a **static library only** with the following configuration:

- **TLS Backend**: MbedTLS v3.6.5 (stable) with DTLS-SRTP enabled
  - **Important**: DTLS-SRTP must be explicitly enabled via `python scripts/config.py set MBEDTLS_SSL_DTLS_SRTP`
  - MbedTLS 4.0+ (development) is not compatible with libdatachannel 0.23.2
- **No Media Support**: Built with `NO_MEDIA=ON` to avoid libsrtp and related dependencies
- **No WebSocket Support**: Built with `NO_WEBSOCKET=ON` to minimize dependencies
- **Static Linking**: Only static libraries are produced and consumed

### Why MbedTLS?

We use MbedTLS instead of OpenSSL or GnuTLS for several reasons:

1. **Licensing**: MbedTLS uses Apache 2.0 license, which is more permissive and compatible with commercial projects
2. **Minimal Dependencies**: Smaller footprint and fewer system dependencies
3. **CI Reproducibility**: Easier to build from source consistently across all platforms
4. **Cross-Platform**: Works consistently on Windows, Linux, and macOS

### Why No Media/WebSocket Support?

- **Dependency Minimization**: Media support requires libsrtp which has additional crypto dependencies
- **Use Case Focus**: Open3DStream primarily uses WebRTC data channels for streaming 3D data, not media streams
- **Build Simplicity**: Reduces build time and complexity in CI environments

## CI/CD Workflow

### Workflow Trigger Strategy

The libdatachannel build workflow is configured for **manual trigger only** (`workflow_dispatch`). This approach treats libdatachannel as a **vendored dependency**:

- ✅ **Pre-built libraries are committed** to `plugins/unreal/Open3DStream/ThirdParty/webrtc/`
- ✅ **No automatic rebuilds** on push or pull requests
- ✅ **Manual rebuild only** when updating libdatachannel or MbedTLS versions

**When to manually trigger the workflow:**
1. Updating to a new libdatachannel release
2. Updating to a new MbedTLS version
3. Changing build configuration flags (e.g., adding/removing features)
4. Debugging build issues across platforms

**How to manually trigger:**
1. Go to GitHub repository → **Actions** tab
2. Select **"Build libdatachannel"** workflow from the left sidebar
3. Click **"Run workflow"** button
4. Select the branch (usually `develop` or your feature branch)
5. Click **"Run workflow"** to start
6. Wait for builds to complete (~5-7 minutes)
7. Download artifacts for each platform
8. Extract and commit to `plugins/unreal/Open3DStream/ThirdParty/webrtc/`

### Build Pipeline

The GitHub Actions workflow `.github/workflows/build-libdatachannel.yml` builds libdatachannel for all platforms:

1. **Clone Repository**: Checks out Open3DStream with all submodules
2. **Build MbedTLS**: 
   - Configures with `GEN_FILES=ON`, `ENABLE_TESTING=OFF`, `ENABLE_PROGRAMS=OFF`
   - Builds and installs to a staging directory
3. **Build libdatachannel**:
   - Configures with required flags:
     ```
     -DBUILD_SHARED_LIBS=OFF
     -DNO_MEDIA=ON
     -DNO_WEBSOCKET=ON
     -DUSE_MBEDTLS=ON
     -DNO_EXAMPLES=ON
     -DNO_TESTS=ON
     ```
   - Points to MbedTLS installation
   - Builds and installs static library
4. **Package Artifacts**: 
   - Collects headers from `include/rtc/`
   - Collects static library (`datachannel.lib` on Windows, `libdatachannel.a` on Linux/macOS)
   - Uploads as CI artifacts

### Supported Platforms

- **Windows**: Visual Studio 2019/2022, produces `datachannel.lib`
- **Linux**: GCC/Clang, produces `libdatachannel.a`
- **macOS**: Clang, produces `libdatachannel.a`

## Unreal Plugin Integration

### File Layout

Pre-built libdatachannel artifacts are stored in:
```
plugins/unreal/Open3DStream/ThirdParty/webrtc/
├── include/
│   └── rtc/              # libdatachannel headers
├── datachannel.lib       # Windows static library
└── libdatachannel.a      # Linux/macOS static library
```

### Build.cs Configuration

The Unreal plugin's `Open3DStream.Build.cs` is configured to:

1. Add WebRTC include directory to the include path
2. Link the appropriate static library based on platform
3. Define `RTC_STATIC=1` to use static library mode

Example from `Build.cs`:
```csharp
string WebRTCDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "../../lib/webrtc/"));

PrivateIncludePaths.AddRange(new string[] 
{ 
    WebRTCDir + "include"
});

if (Target.Platform == UnrealTargetPlatform.Win64)
{
    PublicAdditionalLibraries.Add(WebRTCDir + "datachannel.lib");
}
else if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.Mac)
{
    PublicAdditionalLibraries.Add(WebRTCDir + "libdatachannel.a");
}

PublicDefinitions.Add("RTC_STATIC=1");
```

## Local Development

### Building libdatachannel Locally

If you need to build libdatachannel locally for development:

#### Prerequisites
- CMake 3.13 or later
- C++17 compatible compiler
- Python 3.x with `jsonschema` and `jinja2` packages

#### Windows

```powershell
# Build MbedTLS
cd thirdparty/mbedtls
cmake -S . -B build -DGEN_FILES=ON -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_INSTALL_PREFIX=../../install
cmake --build build --config Release
cmake --install build --config Release

# Build libdatachannel
cd ../libdatachannel
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DUSE_MBEDTLS=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DCMAKE_PREFIX_PATH=../../install -DCMAKE_INSTALL_PREFIX=../../install
cmake --build build --config Release
cmake --install build --config Release

# Copy artifacts to Unreal plugin
Copy-Item -Recurse ../../install/include/rtc ../../../plugins/unreal/Open3DStream/ThirdParty/webrtc/include/
Copy-Item ../../install/lib/datachannel.lib ../../../plugins/unreal/Open3DStream/ThirdParty/webrtc/
```

#### Linux/macOS

```bash
# Build MbedTLS
cd thirdparty/mbedtls
cmake -S . -B build -DGEN_FILES=ON -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../../install
cmake --build build
cmake --install build

# Build libdatachannel
cd ../libdatachannel
cmake -S . -B build -DBUILD_SHARED_LIBS=OFF -DNO_MEDIA=ON -DNO_WEBSOCKET=ON -DUSE_MBEDTLS=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=../../install -DCMAKE_INSTALL_PREFIX=../../install
cmake --build build
cmake --install build

# Copy artifacts to Unreal plugin
cp -r ../../install/include/rtc ../../../plugins/unreal/Open3DStream/ThirdParty/webrtc/include/
cp ../../install/lib/libdatachannel.a ../../../plugins/unreal/Open3DStream/ThirdParty/webrtc/
```

## Important Notes

### Explicitly NOT Supported

- **OpenSSL**: Not used due to licensing concerns and system dependency issues
- **GnuTLS**: Not used to maintain consistency across platforms
- **Dynamic/Shared Libraries**: Only static libraries are supported for the Unreal plugin
- **Media Support**: Not needed for Open3DStream's use case
- **WebSocket Support**: Not needed for Open3DStream's use case

### Version Requirements

- **MbedTLS**: 4.0+ (development branch) 
  - **Note**: Stable 3.x releases do not have DTLS-SRTP APIs
  - The submodule in `thirdparty/mbedtls` is checked out to the `development` branch
- **libdatachannel**: 0.23.2 or compatible
- **CMake**: 3.13 or later
- **C++ Standard**: C++17

## Troubleshooting

### Build Failures

1. **MbedTLS not found**: Ensure MbedTLS is built and installed first, and `CMAKE_PREFIX_PATH` points to the installation directory
2. **Missing headers**: Check that the `include/rtc/` directory is properly copied to the plugin's lib directory
3. **Linker errors about OpenSSL**: Ensure `USE_MBEDTLS=ON` and `USE_GNUTLS=OFF` are set
4. **Media-related errors**: Ensure `NO_MEDIA=ON` is set

### Plugin Integration Issues

1. **Undefined symbols at link time**: Ensure `RTC_STATIC=1` is defined in Build.cs
2. **Header not found errors**: Verify the WebRTC include path is added to PrivateIncludePaths
3. **Platform-specific build failures**: Check that the correct library file is being linked for the target platform

## References

- [libdatachannel GitHub](https://github.com/paullouisageneau/libdatachannel)
- [MbedTLS GitHub](https://github.com/Mbed-TLS/mbedtls)
- [Open3DStream WebRTC Documentation](WEBRTC_QUICKSTART.md)

## License

- **libdatachannel**: MPL 2.0
- **MbedTLS**: Apache 2.0
- **Open3DStream**: See LICENSE file
