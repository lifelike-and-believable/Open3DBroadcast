cmake_minimum_required(VERSION  3.13)
project(O3DS3rdParty)

set(CMAKE_DEBUG_POSTFIX d)

add_subdirectory(flatbuffers)
add_subdirectory(nng)
add_subdirectory(crccpp)

# WebRTC Support - libdatachannel
option(O3DS_BUILD_WEBRTC "Build libdatachannel for WebRTC support" ON)
if(O3DS_BUILD_WEBRTC)
    message(STATUS "Building libdatachannel for WebRTC support")
    
    # Configure libdatachannel options
    set(NO_WEBSOCKET OFF CACHE BOOL "Disable WebSocket support in libdatachannel")
    set(NO_EXAMPLES ON CACHE BOOL "Disable libdatachannel examples")
    set(NO_TESTS ON CACHE BOOL "Disable libdatachannel tests")
    set(USE_NICE OFF CACHE BOOL "Disable libnice ICE support")
    
    # Use system crypto instead of OpenSSL (Windows will use schannel, others will use platform defaults)
    set(USE_GNUTLS OFF CACHE BOOL "Don't use GnuTLS")
    set(USE_MBEDTLS OFF CACHE BOOL "Don't use mbedTLS for now - use system crypto")
    
    add_subdirectory(libdatachannel)
else()
    message(STATUS "Skipping libdatachannel build (O3DS_BUILD_WEBRTC=OFF)")
endif()
