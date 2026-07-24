cmake_minimum_required(VERSION  3.13)

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

if(O3DS_ENABLE_SANITIZERS)
	if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
		message(FATAL_ERROR "O3DS_ENABLE_SANITIZERS requires GCC or Clang")
	endif()
	add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
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

if(O3DS_BUILD_TESTS)
	enable_testing()
	add_subdirectory(test)
endif()

