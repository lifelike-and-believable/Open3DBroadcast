# Simple CMake script to check if OpenSSL can be found
cmake_minimum_required(VERSION 3.13)
project(CheckOpenSSL)

find_package(OpenSSL QUIET)

if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "  Include: ${OPENSSL_INCLUDE_DIR}")
    message(STATUS "  Libraries: ${OPENSSL_LIBRARIES}")
else()
    message(FATAL_ERROR "OpenSSL not found")
endif()
