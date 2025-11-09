# Building WebRTC Libraries

This document explains how to build the pre-built WebRTC libraries for the Open3DStream Unreal plugin.

## Quick Start

### Windows

```bat
build_webrtc_lib.bat
```

Or with a custom OpenSSL path:
```bat
build_webrtc_lib.bat "C:\path\to\OpenSSL"
```

### Linux / macOS

```bash
./build_webrtc_lib.sh
```

## Prerequisites

### All Platforms

- **CMake 3.13+** - Build system
- **Git** - For submodules
- **OpenSSL** - Crypto library for libdatachannel

### Windows Specific

- **Visual Studio 2022** - For C++ compilation
- **MSBuild** - Comes with Visual Studio

### Installing OpenSSL

#### Windows

**Option 1: vcpkg (Recommended)**
```bat
vcpkg install openssl:x64-windows
set CMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake
```

**Option 2: Chocolatey**
```bat
choco install openssl
```

**Option 3: Manual Install**
- Download from: https://slproweb.com/products/Win32OpenSSL.html
- Install "Win64 OpenSSL v3.x"
- Script will auto-detect in common locations

#### Linux

**Ubuntu/Debian:**
```bash
sudo apt-get install libssl-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install openssl-devel
```

#### macOS

**Homebrew:**
```bash
brew install openssl
export OPENSSL_ROOT_DIR=/usr/local/opt/openssl
```

## What the Build Script Does

1. **Checks for OpenSSL** - Verifies OpenSSL is available
2. **Creates build directory** - Sets up isolated build environment
3. **Configures CMake** - Configures libdatachannel with:
   - OpenSSL for crypto (DTLS/SRTP)
   - Static library output
   - No examples/tests (faster build)
   - No media support (avoids libsrtp)
4. **Builds Debug & Release** - Compiles both configurations
5. **Copies to plugin** - Deploys to `plugins/unreal/Open3DStream/ThirdParty/webrtc/`
6. **Generates build info** - Creates metadata file

## Output Files

After successful build:

```
plugins/unreal/Open3DStream/ThirdParty/webrtc/
├── datachannel.lib           # Windows release library
├── datachanneld.lib          # Windows debug library (optional)
├── libdatachannel.a          # Linux/macOS library
├── include/                  # libdatachannel headers
│   └── rtc/
│       ├── rtc.hpp
│       ├── ...
├── BUILD_INFO.txt            # Build metadata
└── README.md                 # Usage documentation
```

## Build Configurations

The script builds libdatachannel with these settings:

| Option | Value | Reason |
|--------|-------|--------|
| `NO_EXAMPLES` | ON | Faster build, not needed |
| `NO_TESTS` | ON | Faster build, not needed |
| `USE_NICE` | OFF | Don't need libnice ICE |
| `NO_WEBSOCKET` | OFF | Keep WebSocket support |
| `NO_MEDIA` | ON | Avoid libsrtp dependency |
| `USE_GNUTLS` | OFF | Use OpenSSL instead |
| `USE_MBEDTLS` | OFF | Use OpenSSL instead |

## Troubleshooting

### OpenSSL Not Found

**Windows:**
- Ensure OpenSSL is installed in a standard location
- Try providing explicit path: `build_webrtc_lib.bat "C:\OpenSSL-Win64"`
- Check that `OPENSSL_ROOT_DIR` environment variable is set

**Linux/macOS:**
- Install development headers (`-dev` or `-devel` package)
- Check `pkg-config --exists openssl`
- Set `OPENSSL_ROOT_DIR` if in non-standard location

### CMake Configuration Failed

- Verify CMake version: `cmake --version` (need 3.13+)
- Check that libdatachannel submodule is initialized:
  ```bash
  git submodule update --init --recursive thirdparty/libdatachannel
  ```
- Check CMake output for specific errors

### Build Failed

**Windows:**
- Ensure Visual Studio 2022 is installed
- Check that MSBuild is in PATH
- Try opening Visual Studio solution manually:
  ```
  start thirdparty\build_webrtc\datachannel.sln
  ```

**Linux/macOS:**
- Check compiler is installed: `gcc --version` or `clang --version`
- Verify all dependencies: `cmake --system-information`

### Library Not Copied

- Check permissions on `plugins/unreal/Open3DStream/ThirdParty/webrtc/`
- Verify build completed successfully (look for `.lib` or `.a` files in build directory)
- Check disk space

## Using Custom CMake Arguments

You can modify the build scripts to add custom CMake arguments:

**Windows** - Edit `build_webrtc_lib.bat`:
```bat
SET CMAKE_ARGS=%CMAKE_ARGS% -DYOUR_OPTION=ON
```

**Linux/macOS** - Edit `build_webrtc_lib.sh`:
```bash
CMAKE_ARGS+=(
    -DYOUR_OPTION=ON
)
```

## Committing the Libraries

After building, commit the libraries to Git:

```bash
# Consider using Git LFS for large binaries
git lfs track "*.lib"
git lfs track "*.a"
git add .gitattributes

# Add the libraries
git add plugins/unreal/Open3DStream/ThirdParty/webrtc/

# Commit
git commit -m "Add pre-built WebRTC libraries"
```

## Clean Build

To start fresh:

**Windows:**
```bat
rmdir /s /q thirdparty\build_webrtc
rmdir /s /q usr_webrtc
del /q plugins\unreal\Open3DStream\ThirdParty\webrtc\*.lib
```

**Linux/macOS:**
```bash
rm -rf thirdparty/build_webrtc
rm -rf usr_webrtc
rm -f plugins/unreal/Open3DStream/ThirdParty/webrtc/*.a
```

Then run the build script again.

## Advanced: Building Specific Configuration

If you only need one configuration:

**Windows:**
```bat
cd thirdparty
mkdir build_webrtc
cd build_webrtc
cmake -G "Visual Studio 17 2022" -A x64 -DNO_EXAMPLES=ON -DNO_TESTS=ON -DNO_MEDIA=ON ../libdatachannel
cmake --build . --config RelWithDebInfo --target datachannel-static
copy RelWithDebInfo\datachannel-static.lib ..\..\plugins\unreal\Open3DStream\ThirdParty\webrtc\datachannel.lib
```

**Linux/macOS:**
```bash
cd thirdparty
mkdir build_webrtc
cd build_webrtc
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNO_EXAMPLES=ON -DNO_TESTS=ON -DNO_MEDIA=ON ../libdatachannel
cmake --build . --target datachannel-static
cp libdatachannel-static.a ../../plugins/unreal/Open3DStream/ThirdParty/webrtc/libdatachannel.a
```

## Version Information

The build script automatically captures:
- Build date and time
- libdatachannel version (Git tag)
- OpenSSL version
- Platform and configuration
- File sizes

This information is saved to `BUILD_INFO.txt` in the library directory.

## Next Steps

After building the libraries:

1. **Build the Unreal plugin** - The plugin will automatically detect and use the libraries
2. **Test in Unreal Editor** - Verify WebRTC options appear in LiveLink Source dialog
3. **Test WebRTC connectivity** - Create WebRTC Client/Server and verify data flow

## Related Documentation

- [libdatachannel GitHub](https://github.com/paullouisageneau/libdatachannel)
- [OpenSSL Website](https://www.openssl.org/)
- [Issue #10](https://github.com/lifelike-and-believable/Open3DStream/issues/10) - Future: Build mbedTLS from source
