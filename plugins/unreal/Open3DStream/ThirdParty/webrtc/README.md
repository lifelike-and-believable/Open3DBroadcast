# WebRTC Libraries for Open3DStream

## Overview

This directory contains the pre-built libdatachannel static libraries and headers for WebRTC support in the Open3DStream Unreal plugin.

**Important:** The library artifacts are downloaded from the [lifelike-and-believable/libdatachannel](https://github.com/lifelike-and-believable/libdatachannel) releases during the build process. They are **NOT** committed to this repository to avoid binary bloat.

## Automated Download Process

The libraries in this directory are downloaded automatically by the GitHub Actions workflow in `.github/actions/build-plugin-core/action.yml` from the latest release of the libdatachannel repository.

### Download Configuration

The workflow downloads the appropriate pre-built libraries based on the runner's platform and automatically extracts them to this directory during the build process.


### Library Features

- **TLS Backend**: MbedTLS (no OpenSSL or GnuTLS)
- **Static Libraries Only**: No shared/dynamic libraries
- **No Media Support**: Built with `NO_MEDIA=ON` (avoids libsrtp)
- **No WebSocket Support**: Built with `NO_WEBSOCKET=ON`
- **Platforms**: Windows, Linux, macOS


### Why MbedTLS?

The pre-built libraries use MbedTLS instead of OpenSSL for several key reasons:


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
plugins/unreal/Open3DStream/ThirdParty/webrtc/
├── datachannel.lib           # Windows static library (downloaded from releases)
├── libdatachannel.a          # Linux static library (downloaded from releases)
├── include/                  # libdatachannel headers (downloaded from releases)
│   └── rtc/
│       ├── rtc.hpp
│       ├── peerconnection.hpp
│       ├── datachannel.hpp
│       └── ...
├── README.md                 # This file
└── datachannel.lib.placeholder  # Git placeholder (libraries not committed)
```

## How the Plugin Uses These Libraries

The Unreal plugin's `Open3DStream.Build.cs` automatically:

1. Detects the platform (Windows, Linux, or macOS)
2. Links the appropriate static library
3. Adds the `include/rtc/` headers to the include path
4. Defines `RTC_STATIC=1` for static library mode

**No manual configuration required** - the build-plugin-core GitHub Action automatically downloads the appropriate artifacts for your platform.

## Where Libraries Come From

The pre-built libraries are maintained in the [lifelike-and-believable/libdatachannel](https://github.com/lifelike-and-believable/libdatachannel) repository and published as GitHub releases. The workflow automatically:

1. Fetches the latest release from the libdatachannel repository
2. Downloads the appropriate platform-specific asset (Windows, Linux, or macOS)
3. Extracts the libraries and headers to this directory
4. The Unreal Build.cs then links against these downloaded libraries

## Local Development

For local development, the GitHub Actions workflow (`.github/actions/build-plugin-core/action.yml`) will automatically download the pre-built libraries. However, if you need to use custom builds:

### Option 1: Use Pre-built Releases (Recommended)

Download the latest release artifacts from [lifelike-and-believable/libdatachannel releases](https://github.com/lifelike-and-believable/libdatachannel/releases) and extract them to this directory.

### Option 2: Build Locally (Advanced)

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

## Downloaded Artifacts

The workflow automatically downloads platform-specific artifacts:

- **Windows**: `datachannel.lib`, `usrsctp.lib`, `mbedtls.lib`, etc. + headers
- **Linux**: `libdatachannel.a`, `libusrsctp.a`, `libmbedtls.a`, etc. + headers
- **macOS**: `libdatachannel.a`, `libusrsctp.a`, `libmbedtls.a`, etc. + headers

These are extracted from the latest release zip files and placed in this directory during the build process.

## Troubleshooting

### "datachannel.lib not found" or "libdatachannel.a not found"

**Solution**: The build workflow should automatically download these. If building locally outside of the workflow, download the latest release from [lifelike-and-believable/libdatachannel releases](https://github.com/lifelike-and-believable/libdatachannel/releases) and extract to this directory.

### "Undefined reference to RTC symbols"

**Solution**: Ensure `RTC_STATIC=1` is defined in `Build.cs` - this should be automatic

### "OpenSSL required" error during build

**Solution**: Verify that `USE_MBEDTLS=ON` is set and MbedTLS is built first

### Plugin compiles but WebRTC features don't work

**Solution**: Ensure the libraries were downloaded correctly. Check that library files exist in this directory. Re-run the build workflow if needed.

## Version Information

The version of libdatachannel used is determined by the latest release in the [lifelike-and-believable/libdatachannel](https://github.com/lifelike-and-believable/libdatachannel) repository.

- **TLS Backend**: MbedTLS (with DTLS-SRTP)
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

- **Download Action**: [.github/actions/build-plugin-core/action.yml](../../../../.github/actions/build-plugin-core/action.yml) - Downloads releases automatically
- **libdatachannel Releases**: https://github.com/lifelike-and-believable/libdatachannel/releases
- **libdatachannel**: https://github.com/paullouisageneau/libdatachannel
- **MbedTLS**: https://github.com/Mbed-TLS/mbedtls
