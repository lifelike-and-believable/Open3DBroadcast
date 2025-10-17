#!/bin/bash
# ============================================================================
# Build datachannel.lib/libdatachannel.a for WebRTC support
# This script builds libdatachannel with OpenSSL and copies the result
# to the Unreal plugin directory for pre-built library usage.
# ============================================================================

set -e  # Exit on error

echo ""
echo "========================================================================"
echo "Building libdatachannel for WebRTC Support"
echo "========================================================================"
echo ""

# Set paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THIRDPARTY_DIR="${SCRIPT_DIR}/thirdparty"
BUILD_DIR="${THIRDPARTY_DIR}/build_webrtc"
PLUGIN_LIB_DIR="${SCRIPT_DIR}/plugins/unreal/Open3DStream/lib/webrtc"
INSTALL_DIR="${SCRIPT_DIR}/usr_webrtc"

# Check for CMake
if ! command -v cmake &> /dev/null; then
    echo "[ERROR] CMake not found in PATH!"
    echo "Please install CMake."
    exit 1
fi

echo "[1/7] Checking for OpenSSL..."
echo ""

# Check for OpenSSL using pkg-config
OPENSSL_FOUND=0
if command -v pkg-config &> /dev/null; then
    if pkg-config --exists openssl; then
        OPENSSL_VERSION=$(pkg-config --modversion openssl)
        echo "[✓] OpenSSL found: ${OPENSSL_VERSION}"
        OPENSSL_FOUND=1
    fi
fi

# Try CMake's find_package
if [ $OPENSSL_FOUND -eq 0 ]; then
    if cmake -P check_openssl.cmake &> /dev/null; then
        echo "[✓] OpenSSL found via CMake"
        OPENSSL_FOUND=1
    fi
fi

if [ $OPENSSL_FOUND -eq 0 ]; then
    echo "[ERROR] OpenSSL not found!"
    echo ""
    echo "Please install OpenSSL using your package manager:"
    echo ""
    echo "  Ubuntu/Debian:"
    echo "    sudo apt-get install libssl-dev"
    echo ""
    echo "  Fedora/RHEL:"
    echo "    sudo dnf install openssl-devel"
    echo ""
    echo "  macOS (Homebrew):"
    echo "    brew install openssl"
    echo "    export OPENSSL_ROOT_DIR=/usr/local/opt/openssl"
    echo ""
    exit 1
fi
echo ""

echo "[2/7] Initializing libdatachannel submodules..."
cd "$THIRDPARTY_DIR/libdatachannel"
if [ ! -f "deps/plog/CMakeLists.txt" ]; then
    echo "    Initializing dependencies..."
    git submodule update --init --recursive
    echo "    [✓] Submodules initialized"
else
    echo "    [✓] Submodules already initialized"
fi
cd "$SCRIPT_DIR"
echo ""

echo "[3/7] Creating build directory..."
if [ -d "$BUILD_DIR" ]; then
    echo "    Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"
echo "[✓] Build directory ready"
echo ""

echo "[4/7] Configuring CMake for libdatachannel..."
echo ""
cd "$BUILD_DIR"

CMAKE_ARGS=(
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DNO_EXAMPLES=ON
    -DNO_TESTS=ON
    -DUSE_NICE=OFF
    -DNO_WEBSOCKET=OFF
    -DNO_MEDIA=ON
    -DUSE_GNUTLS=OFF
    -DUSE_MBEDTLS=OFF
)

# Add OpenSSL root if provided
if [ -n "${OPENSSL_ROOT_DIR}" ]; then
    CMAKE_ARGS+=(-DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}")
fi

# Add toolchain file if provided (for vcpkg)
if [ -n "${CMAKE_TOOLCHAIN_FILE}" ]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE}")
fi

cmake "${CMAKE_ARGS[@]}" ../libdatachannel
echo "[✓] CMake configuration complete"
echo ""

echo "[5/7] Building libdatachannel (Debug)..."
cmake --build . --config Debug --target datachannel-static
echo "[✓] Debug build complete"
echo ""

echo "[6/7] Building libdatachannel (RelWithDebInfo)..."
cmake --build . --config RelWithDebInfo --target datachannel-static
echo "[✓] RelWithDebInfo build complete"
echo ""

echo "[7/7] Copying libraries to plugin directory..."
mkdir -p "$PLUGIN_LIB_DIR"
mkdir -p "$PLUGIN_LIB_DIR/include"

# Find and copy the library files
if [ -f "libdatachannel-static.a" ]; then
    cp -f "libdatachannel-static.a" "$PLUGIN_LIB_DIR/libdatachannel.a"
    echo "    [✓] Copied libdatachannel.a"
elif [ -f "RelWithDebInfo/libdatachannel-static.a" ]; then
    cp -f "RelWithDebInfo/libdatachannel-static.a" "$PLUGIN_LIB_DIR/libdatachannel.a"
    echo "    [✓] Copied libdatachannel.a"
else
    echo "    [ERROR] Library file not found!"
    exit 1
fi

# Copy headers
echo "    Copying headers..."
cp -rf ../libdatachannel/include/rtc "$PLUGIN_LIB_DIR/include/"
echo "    [✓] Copied headers"

cd "$SCRIPT_DIR"
echo "[✓] Files copied to plugin directory"
echo ""

echo "[8/8] Creating build info file..."
cat > "$PLUGIN_LIB_DIR/BUILD_INFO.txt" << EOF
libdatachannel Build Information
=======================================

Build Date: $(date)
Built by: $(whoami)
Build Host: $(hostname)
EOF

# Get libdatachannel version
cd "$THIRDPARTY_DIR/libdatachannel"
DC_VERSION=$(git describe --tags --always 2>/dev/null || echo "unknown")
cd "$SCRIPT_DIR"

cat >> "$PLUGIN_LIB_DIR/BUILD_INFO.txt" << EOF
libdatachannel Version: ${DC_VERSION}
OpenSSL: $(pkg-config --modversion openssl 2>/dev/null || echo "system")
Configuration: RelWithDebInfo
Platform: $(uname -m)

Files:
EOF

# List library files
find "$PLUGIN_LIB_DIR" -maxdepth 1 -type f \( -name "*.a" -o -name "*.so" \) 2>/dev/null | while read -r lib; do
    SIZE=$(du -h "$lib" | cut -f1)
    echo "  - $(basename "$lib") ($SIZE)" >> "$PLUGIN_LIB_DIR/BUILD_INFO.txt"
done

echo "[✓] Build info created"
echo ""

echo "========================================================================"
echo "SUCCESS! WebRTC library built and deployed"
echo "========================================================================"
echo ""
echo "Library location: $PLUGIN_LIB_DIR/"
echo "Headers location: $PLUGIN_LIB_DIR/include/rtc/"
echo ""
echo "Next steps:"
echo "  1. Commit the libraries to Git: git add plugins/unreal/Open3DStream/lib/webrtc/"
echo "  2. Build the Unreal plugin"
echo "  3. Test WebRTC in Unreal Editor"
echo ""
echo "For Git LFS support (recommended for large binaries):"
echo "  git lfs track \"*.a\""
echo "  git lfs track \"*.lib\""
echo "  git add .gitattributes"
echo ""
