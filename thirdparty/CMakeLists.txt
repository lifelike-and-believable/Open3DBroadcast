cmake_minimum_required(VERSION  3.13)
project(O3DS3rdParty)

set(CMAKE_DEBUG_POSTFIX d)

add_subdirectory(flatbuffers)
add_subdirectory(nng)
add_subdirectory(crccpp)

# WebRTC Support - libdatachannel
# Note: Pre-built libraries are available in plugins/unreal/Open3DStream/ThirdParty/webrtc/
# Only build from source if explicitly needed (e.g., updating libdatachannel version)
option(O3DS_BUILD_WEBRTC "Build libdatachannel for WebRTC support" ON)

if(O3DS_BUILD_WEBRTC)
    message(STATUS "Building libdatachannel for WebRTC support with OpenSSL")
    
    # OpenSSL is the default TLS backend for libdatachannel
    # Ensure OpenSSL is available on the system
    find_package(OpenSSL QUIET)
    if(NOT OPENSSL_FOUND)
        message(WARNING "OpenSSL not found. Install OpenSSL development libraries.")
        message(WARNING "  Ubuntu/Debian: sudo apt-get install libssl-dev")
        message(WARNING "  Fedora/RHEL: sudo dnf install openssl-devel")
        message(WARNING "  macOS: brew install openssl")
        message(WARNING "  Windows: choco install openssl")
    else()
        message(STATUS "Found OpenSSL ${OPENSSL_VERSION}")
    endif()
    
    # Configure libdatachannel options for CI compatibility
    # Build static libraries only
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static library only")
    
    # Disable WebSocket and media support to minimize dependencies
    set(NO_WEBSOCKET ON CACHE BOOL "Disable WebSocket support in libdatachannel")
    set(NO_MEDIA ON CACHE BOOL "Disable media transport support in libdatachannel")
    
    # Disable examples and tests
    set(NO_EXAMPLES ON CACHE BOOL "Disable libdatachannel examples")
    set(NO_TESTS ON CACHE BOOL "Disable libdatachannel tests")
    set(USE_NICE OFF CACHE BOOL "Disable libnice ICE support")
    
    # Use OpenSSL (default) by explicitly disabling other TLS backends
    set(USE_GNUTLS OFF CACHE BOOL "Don't use GnuTLS")
    set(USE_MBEDTLS OFF CACHE BOOL "Don't use MbedTLS - use OpenSSL instead")
    
    add_subdirectory(libdatachannel)
else()
    message(STATUS "Skipping libdatachannel build (O3DS_BUILD_WEBRTC=OFF)")
endif()
