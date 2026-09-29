cmake_minimum_required(VERSION  3.13)

# Don't put /W3 into CMAKE_<LANG>_FLAGS by default on MSVC (CMake 3.15+), so
# o3ds_enable_warnings()'s /W4 below doesn't trigger a "D9025: overriding
# /W3 with /W4" command-line warning on every file.
if(POLICY CMP0092)
	cmake_policy(SET CMP0092 NEW)
endif()

project(Open3DStream VERSION)

#if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
#  set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_LIST_DIR}/usr )
#endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_DEBUG_POSTFIX d)

# Version 1.0.4  Remove unused subjects, add all matrixes
set(O3DS_VERSION_TAG "1.0.4")

option(O3DS_BUILD_TESTS "Build the core library unit test suite (CTest)" ON)
option(O3DS_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang only)" OFF)
option(O3DS_BUILD_FUZZERS "Build the libFuzzer targets under test/fuzz (Clang only; instruments the whole build)" OFF)

# CORE-20: warnings for O3DS's own code only - the core library's non-legacy
# sources (src/CMakeLists.txt), the tests and the fuzz targets. Not applied
# globally, so vendored third-party code and the legacy connectors/apps are
# unaffected. No -Werror yet: the warning count is ratcheted in CI instead
# (test/check_warning_baseline.sh, test/warning-baseline.txt).
if(MSVC)
	set(O3DS_WARNING_FLAGS /W4)
else()
	set(O3DS_WARNING_FLAGS -Wall -Wextra)
endif()

function(o3ds_enable_warnings target)
	target_compile_options(${target} PRIVATE ${O3DS_WARNING_FLAGS})
endfunction()

if(O3DS_ENABLE_SANITIZERS)
	if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
		message(FATAL_ERROR "O3DS_ENABLE_SANITIZERS requires GCC or Clang")
	endif()
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
	add_link_options(-fsanitize=address,undefined)
endif()

if(O3DS_BUILD_FUZZERS)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "O3DS_BUILD_FUZZERS requires Clang (libFuzzer)")
	endif()
	# Coverage instrumentation for everything (fuzzer-no-link), so libFuzzer
	# sees inside the core library, not just the fuzz target sources; only
	# the o3ds_fuzz_<target> executables link libFuzzer itself (see
	# test/fuzz/CMakeLists.txt). No sanitizer recovery: a UBSan report must
	# stop the run as a crash, like an ASan one.
	add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
	add_link_options(-fsanitize=address,undefined)
endif()

#add_subdirectory(apps/FbxStream)
add_subdirectory(src)
add_subdirectory(apps/Repeater)
add_subdirectory(apps/SubscribeTest)
add_subdirectory(plugins/mobu)
#add_subdirectory(plugins/maya)

add_subdirectory(apps/Test1)
add_subdirectory(apps/DeterminismProbe)
add_subdirectory(apps/PredictorEval)

if(O3DS_BUILD_TESTS OR O3DS_BUILD_FUZZERS)
	enable_testing()
	if(O3DS_BUILD_TESTS)
		add_subdirectory(test)
	endif()
	add_subdirectory(test/fuzz)
endif()

