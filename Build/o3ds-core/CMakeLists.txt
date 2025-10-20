cmake_minimum_required(VERSION 3.13)
project(O3DSCore VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Version tag (can be overridden via -DO3DS_VERSION_TAG=...)
if(NOT DEFINED O3DS_VERSION_TAG)
    # Try to get git tag or short SHA
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE O3DS_VERSION_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
    if(NOT O3DS_VERSION_TAG)
        set(O3DS_VERSION_TAG "1.0.4")
    endif()
endif()
message(STATUS "O3DS version: ${O3DS_VERSION_TAG}")

# Locate source files relative to repository root
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SRC_DIR "${REPO_ROOT}/src")
set(THIRDPARTY_DIR "${REPO_ROOT}/thirdparty")

# FlatBuffers is header-only - we just need the generated header
# Ensure o3ds_generated.h exists in src/
if(NOT EXISTS "${SRC_DIR}/o3ds_generated.h")
    message(FATAL_ERROR "o3ds_generated.h not found at ${SRC_DIR}/o3ds_generated.h. Run flatc to generate it first.")
endif()

# Build minimal flatbuffers (header-only mode)
message(STATUS "Building minimal FlatBuffers (header-only)")
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/flatbuffers ${CMAKE_CURRENT_BINARY_DIR}/flatbuffers EXCLUDE_FROM_ALL)

# Build nng as static library
message(STATUS "Building NNG static library")
set(NNG_TESTS OFF CACHE BOOL "" FORCE)
set(NNG_TOOLS OFF CACHE BOOL "" FORCE)
set(NNG_ENABLE_STATS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY_DIR}/nng ${CMAKE_CURRENT_BINARY_DIR}/nng EXCLUDE_FROM_ALL)

# Source files for O3DS core library
# Include all core sources; NNG connector is always included (it's part of core)
set(O3DS_SOURCES
    ${SRC_DIR}/o3ds/o3ds.cpp
    ${SRC_DIR}/o3ds/getTime.cpp
    ${SRC_DIR}/o3ds/model.cpp
    ${SRC_DIR}/o3ds/base_connector.cpp
    ${SRC_DIR}/o3ds/nng_connector.cpp
    ${SRC_DIR}/o3ds/math.cpp
    ${SRC_DIR}/o3ds/tcp.cpp
    ${SRC_DIR}/o3ds/udp.cpp
    ${SRC_DIR}/o3ds/udp_fragment.cpp
    ${SRC_DIR}/o3ds/socket_exception.cpp
    ${SRC_DIR}/o3ds/socket_util.cpp
    ${SRC_DIR}/o3ds/socket_address.cpp
    ${SRC_DIR}/o3ds/binary_stream.cpp
    ${SRC_DIR}/o3ds/o3ds_version.cpp
    ${SRC_DIR}/o3ds/context.cpp
    ${SRC_DIR}/o3ds/xsens_parser.cpp
    ${SRC_DIR}/o3ds/subscriber.cpp
    ${SRC_DIR}/o3ds/publisher.cpp
    ${SRC_DIR}/o3ds/pair.cpp
    ${SRC_DIR}/o3ds/request.cpp
    ${SRC_DIR}/o3ds/pipeline.cpp
    ${SRC_DIR}/o3ds/async_subscriber.cpp
    ${SRC_DIR}/o3ds/async_publisher.cpp
    ${SRC_DIR}/o3ds/async_pair.cpp
    ${SRC_DIR}/o3ds/async_request.cpp
    ${SRC_DIR}/o3ds/async_pipeline.cpp
)

# Optional WebSocket support (not needed for Unreal)
option(O3DS_WITH_WEBSOCKET "Include WebSocket support" OFF)
if(O3DS_WITH_WEBSOCKET)
    list(APPEND O3DS_SOURCES ${SRC_DIR}/o3ds/websocket.cpp)
    add_compile_definitions(O3DS_WITH_WEBSOCKET)
endif()

# Optional WebRTC support (not needed for core - Unreal uses libdatachannel directly)
option(O3DS_WITH_WEBRTC "Include WebRTC connector support" OFF)
if(O3DS_WITH_WEBRTC)
    list(APPEND O3DS_SOURCES ${SRC_DIR}/o3ds/webrtc_connector.cpp)
    add_compile_definitions(O3DS_ENABLE_WEBRTC)
endif()

# Public headers
set(O3DS_HEADERS
    ${SRC_DIR}/o3ds/o3ds.h
    ${SRC_DIR}/o3ds/getTime.h
    ${SRC_DIR}/o3ds/model.h
    ${SRC_DIR}/o3ds/base_connector.h
    ${SRC_DIR}/o3ds/nng_connector.h
    ${SRC_DIR}/o3ds/subscriber.h
    ${SRC_DIR}/o3ds/async_subscriber.h
    ${SRC_DIR}/o3ds/async_publisher.h
    ${SRC_DIR}/o3ds/pair.h
    ${SRC_DIR}/o3ds/async_pair.h
    ${SRC_DIR}/o3ds/publisher.h
    ${SRC_DIR}/o3ds/async_request.h
    ${SRC_DIR}/o3ds/transform_component.h
    ${SRC_DIR}/o3ds/request.h
    ${SRC_DIR}/o3ds/pipeline.h
    ${SRC_DIR}/o3ds/async_pipeline.h
    ${SRC_DIR}/o3ds/context.h
    ${SRC_DIR}/o3ds/math.h
    ${SRC_DIR}/o3ds/tcp.h
    ${SRC_DIR}/o3ds/udp_fragment.h
    ${SRC_DIR}/o3ds/socket_exception.h
    ${SRC_DIR}/o3ds/socket_util.h
    ${SRC_DIR}/o3ds/udp.h
    ${SRC_DIR}/o3ds/socket_address.h
    ${SRC_DIR}/o3ds/binary_stream.h
    ${SRC_DIR}/o3ds/xsens_parser.h
    ${SRC_DIR}/o3ds/o3ds_version.h
)

if(O3DS_WITH_WEBSOCKET)
    list(APPEND O3DS_HEADERS ${SRC_DIR}/o3ds/websocket.h)
endif()

if(O3DS_WITH_WEBRTC)
    list(APPEND O3DS_HEADERS ${SRC_DIR}/o3ds/webrtc_connector.h)
endif()

# Create static library
add_library(o3ds STATIC ${O3DS_SOURCES} ${O3DS_HEADERS})

# Link to nng (static)
target_link_libraries(o3ds PUBLIC nng)

# Include directories
target_include_directories(o3ds PUBLIC
    $<BUILD_INTERFACE:${SRC_DIR}>
    $<BUILD_INTERFACE:${SRC_DIR}/o3ds>
    $<BUILD_INTERFACE:${THIRDPARTY_DIR}/flatbuffers/include>
    $<BUILD_INTERFACE:${THIRDPARTY_DIR}/crccpp>
    $<INSTALL_INTERFACE:include>
)

# Compile definitions
target_compile_definitions(o3ds PUBLIC
    O3DS_VERSION_TAG=${O3DS_VERSION_TAG}
    CRCPP_USE_NAMESPACE
    NNG_STATIC_LIB
)

# Platform-specific settings
if(WIN32)
    target_link_libraries(o3ds PUBLIC wsock32 ws2_32)
    target_compile_definitions(o3ds PUBLIC _WIN32_WINNT=0x0601)
elseif(APPLE)
    # macOS specific settings
    target_compile_options(o3ds PRIVATE -Wall)
else()
    # Linux specific settings
    target_compile_options(o3ds PRIVATE -Wall)
    target_link_libraries(o3ds PUBLIC pthread)
endif()

# Installation rules
install(TARGETS o3ds
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Install headers maintaining directory structure
install(FILES ${O3DS_HEADERS}
    DESTINATION include/o3ds
)

install(FILES ${SRC_DIR}/o3ds_generated.h
    DESTINATION include
)
