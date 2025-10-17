cmake_minimum_required(VERSION  3.13)
project(O3DS3rdParty)

set(CMAKE_DEBUG_POSTFIX d)

add_subdirectory(flatbuffers)
add_subdirectory(nng)
add_subdirectory(crccpp)

# Build MbedTLS for libdatachannel
# Note: Using development branch (4.0+) for DTLS-SRTP support
# Stable 3.x releases lack the DTLS-SRTP APIs required by libdatachannel
option(O3DS_BUILD_MBEDTLS "Build MbedTLS for libdatachannel" ON)
if(O3DS_BUILD_MBEDTLS)
    message(STATUS "Building MbedTLS for libdatachannel")
    
    # Configure MbedTLS options
    set(ENABLE_TESTING OFF CACHE BOOL "Disable MbedTLS tests")
    set(ENABLE_PROGRAMS OFF CACHE BOOL "Disable MbedTLS programs")
    set(GEN_FILES ON CACHE BOOL "Enable file generation for MbedTLS")
    
    add_subdirectory(mbedtls)
else()
    message(STATUS "Skipping MbedTLS build (O3DS_BUILD_MBEDTLS=OFF)")
endif()

# WebRTC Support - libdatachannel
option(O3DS_BUILD_WEBRTC "Build libdatachannel for WebRTC support" ON)
if(O3DS_BUILD_WEBRTC)
    message(STATUS "Building libdatachannel for WebRTC support with MbedTLS")
    
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
    
    # Use MbedTLS instead of OpenSSL or GnuTLS
    set(USE_GNUTLS OFF CACHE BOOL "Don't use GnuTLS")
    set(USE_MBEDTLS ON CACHE BOOL "Use MbedTLS for crypto")
    
    # Point to our MbedTLS installation
    if(NOT MBEDTLS_ROOT_DIR)
        set(MBEDTLS_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls" CACHE PATH "MbedTLS root directory")
    endif()
    
    add_subdirectory(libdatachannel)
else()
    message(STATUS "Skipping libdatachannel build (O3DS_BUILD_WEBRTC=OFF)")
endif()
