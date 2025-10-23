SET(CMAKE_MIN_REQ_VERSION 3.13)
cmake_minimum_required(VERSION ${CMAKE_MIN_REQ_VERSION})

project(RepeaterSmokePush)

find_package(nng CONFIG REQUIRED)

add_executable(RepeaterSmokePush main.cpp)
target_link_libraries(RepeaterSmokePush open3dstreamstatic ${NNG_LIBRARY})

install(TARGETS RepeaterSmokePush DESTINATION bin)
