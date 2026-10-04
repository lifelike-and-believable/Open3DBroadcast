SET(CMAKE_MIN_REQ_VERSION 3.13)
cmake_minimum_required(VERSION ${CMAKE_MIN_REQ_VERSION})

project(Repeater)

find_package(nng CONFIG REQUIRED)
find_package(Threads REQUIRED)

# The relay is a library so the core tests can run it (test/repeater_tests.cpp).
add_library(repeater_relay STATIC relay.cpp relay.h)
target_include_directories(repeater_relay PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(repeater_relay PUBLIC open3dstreamstatic nng::nng Threads::Threads)
o3ds_enable_warnings(repeater_relay)

add_executable(Repeater main.cpp)
target_link_libraries(Repeater PRIVATE repeater_relay)
o3ds_enable_warnings(Repeater)

install(TARGETS Repeater DESTINATION bin)
