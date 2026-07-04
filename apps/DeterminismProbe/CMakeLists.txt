SET(CMAKE_MIN_REQ_VERSION 3.13)
cmake_minimum_required(VERSION ${CMAKE_MIN_REQ_VERSION})

project(DeterminismProbe)

add_executable(DeterminismProbe main.cpp)
target_link_libraries(DeterminismProbe PRIVATE open3dstreamstatic)

install(TARGETS DeterminismProbe DESTINATION bin)
