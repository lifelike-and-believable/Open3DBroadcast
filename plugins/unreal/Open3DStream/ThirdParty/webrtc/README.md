# WebRTC Libraries for Open3DStream

## Overview

This directory contains the pre-built libdatachannel static libraries and headers for WebRTC support in the Open3DStream Unreal plugin.

**Important:** The library artifacts are built via CI and should **NOT** be built manually unless developing libdatachannel itself.

## CI Build Process

The libraries in this directory are built automatically by GitHub Actions using the workflow defined in `.github/workflows/build-libdatachannel.yml`.

### Build Configuration

- **TLS Backend**: MbedTLS 3.6.5 (no OpenSSL or GnuTLS)
- **Static Libraries Only**: No shared/dynamic libraries
- **No Media Support**: Built with `NO_MEDIA=ON` (avoids libsrtp)
- **No WebSocket Support**: Built with `NO_WEBSOCKET=ON`
- **Platforms**: Windows, Linux, macOS

### Why MbedTLS?

We use MbedTLS instead of OpenSSL for several key reasons:

1. **Licensing**: Apache 2.0 license (more permissive than OpenSSL)
2. **Minimal Dependencies**: No system crypto library dependencies
3. **CI Reproducibility**: Builds consistently from source on all platforms
4. **Cross-Platform**: Works uniformly on Windows, Linux, and macOS

### Why No Media/WebSocket?

- Open3DStream uses WebRTC **data channels only** for 3D animation streaming
- Media support requires libsrtp and additional dependencies
- WebSocket support requires Nettle and additional dependencies
- Minimizing dependencies ensures reliable CI builds

## File Structure

```
plugins/unreal/Open3DStream/lib/webrtc/
├── datachannel.lib           # Windows static library (from CI)
├── libdatachannel.a          # Linux/macOS static library (from CI)
├── include/                  # libdatachannel headers (from CI)
│   └── rtc/
│       ├── rtc.hpp
│       ├── peerconnection.hpp
│       ├── datachannel.hpp
│       └── ...
├── BUILD_INFO.txt            # Build metadata (from CI)
├── README.md                 # This file
└── datachannel.lib.placeholder  # Git placeholder
```

## How the Plugin Uses These Libraries

The Unreal plugin's `Open3DStream.Build.cs` automatically:

1. Detects the platform (Windows, Linux, or macOS)
2. Links the appropriate static library
3. Adds the `include/rtc/` headers to the include path
4. Defines `RTC_STATIC=1` for static library mode

**No manual configuration required** - just ensure the artifacts are present in this directory.

## Local Development

If you need to build libdatachannel locally (e.g., for testing or development):

### Prerequisites

- CMake 3.13+
- C++17 compiler
- Python 3.x with `jsonschema` and `jinja2`
- Git (for submodules)

### Build Instructions

See **[LIBDATACHANNEL_INTEGRATION.md](../../../../LIBDATACHANNEL_INTEGRATION.md)** for complete build instructions.

**Quick Summary:**

1. Build MbedTLS from `thirdparty/mbedtls`
2. Build libdatachannel from `thirdparty/libdatachannel` with flags:
   - `-DBUILD_SHARED_LIBS=OFF`
   - `-DNO_MEDIA=ON`
   - `-DNO_WEBSOCKET=ON`
   - `-DUSE_MBEDTLS=ON`
3. Copy artifacts to this directory

## CI Artifacts

The CI workflow produces artifacts for each platform:

- **libdatachannel-windows**: Contains `datachannel.lib` and headers
- **libdatachannel-linux**: Contains `libdatachannel.a` and headers
- **libdatachannel-macos**: Contains `libdatachannel.a` and headers

These artifacts can be downloaded from GitHub Actions runs and extracted to this directory.

## Troubleshooting

### "datachannel.lib not found" or "libdatachannel.a not found"

**Solution**: Download the CI artifacts or build locally (see Local Development above)

### "Undefined reference to RTC symbols"

**Solution**: Ensure `RTC_STATIC=1` is defined in `Build.cs` - this should be automatic

### "OpenSSL required" error during build

**Solution**: Verify that `USE_MBEDTLS=ON` is set and MbedTLS is built first

### Plugin compiles but WebRTC features don't work

**Solution**: Check that the library file actually exists in this directory

## Version Information

- **libdatachannel**: 0.23.2
- **MbedTLS**: 3.6.5 (with DTLS-SRTP)
- **Build Type**: Static, Release
- **CMake Version**: 3.13+

## Important Notes

### DO NOT Use

- ❌ OpenSSL - Not supported, licensing issues
- ❌ GnuTLS - Not supported, platform inconsistencies
- ❌ Shared/Dynamic libraries - Only static linking supported
- ❌ Media support - Not needed for Open3DStream
- ❌ WebSocket support - Not needed for Open3DStream

### Why Static Linking Only?

1. **Portability**: No runtime DLL dependencies
2. **Versioning**: Exact library version bundled with plugin
3. **Deployment**: Simpler installation for end users
4. **CI**: Easier to test and validate

## Documentation

- **[LIBDATACHANNEL_INTEGRATION.md](../../../../LIBDATACHANNEL_INTEGRATION.md)** - Complete integration guide
- **[WEBRTC_QUICKSTART.md](../../../../WEBRTC_QUICKSTART.md)** - WebRTC usage guide
- **[BUILD_WEBRTC_LIBS.md](../../../../BUILD_WEBRTC_LIBS.md)** - Legacy build documentation

## Links

- **GitHub Workflow**: [.github/workflows/build-libdatachannel.yml](../../../../.github/workflows/build-libdatachannel.yml)
- **Issue #13**: [Integrate libdatachannel with MbedTLS](https://github.com/lifelike-and-believable/Open3DStream/issues/13)
- **libdatachannel**: https://github.com/paullouisageneau/libdatachannel
- **MbedTLS**: https://github.com/Mbed-TLS/mbedtls
