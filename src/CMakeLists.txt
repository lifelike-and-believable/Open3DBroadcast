cmake_minimum_required(VERSION  3.13)

project(Open3DStreamLib)

find_package(Flatbuffers CONFIG REQUIRED flatbuffers flatc HINTS ${CMAKE_CURRENT_LIST_DIR}/../usr)
find_package(nng CONFIG REQUIRED HINTS ${CMAKE_CURRENT_LIST_DIR}/../usr)

# WebRTC support
# - O3DS_ENABLE_WEBRTC (default: ON) keeps backward compatibility with existing builds/docs
# - O3DS_DISABLE_WEBRTC (default: OFF) is an explicit opt-out that overrides the enable flag
# - O3DS_LIBDATACHANNEL_ROOT: Optional path to a directory containing prebuilt libdatachannel
#   (and its dependencies) plus headers. When provided, an imported target
#   LibDataChannel::LibDataChannel is created from these artifacts instead of using find_package.
option(O3DS_ENABLE_WEBRTC "Enable WebRTC support" ON)
option(O3DS_DISABLE_WEBRTC "Disable WebRTC support (overrides enable)" OFF)
set(O3DS_LIBDATACHANNEL_ROOT "" CACHE PATH "Root folder with prebuilt libdatachannel libs and include")

# Compute effective switch used by the build
set(O3DS_WITH_WEBRTC ${O3DS_ENABLE_WEBRTC})
if(O3DS_DISABLE_WEBRTC)
	set(O3DS_WITH_WEBRTC OFF)
endif()

if(O3DS_WITH_WEBRTC)
	# Prefer prebuilt artifacts when a root path is provided; otherwise use find_package
	if(O3DS_LIBDATACHANNEL_ROOT)
		# Expect headers under <root>/include (or <root> if include not present)
		set(LDC_INCLUDE_DIR "${O3DS_LIBDATACHANNEL_ROOT}/include")
		if(NOT EXISTS "${LDC_INCLUDE_DIR}")
			set(LDC_INCLUDE_DIR "${O3DS_LIBDATACHANNEL_ROOT}")
		endif()

		# Library file list must be present directly in the provided root
		if(WIN32)
			set(LDC_LIB_FILES
				"${O3DS_LIBDATACHANNEL_ROOT}/datachannel.lib"
				"${O3DS_LIBDATACHANNEL_ROOT}/usrsctp.lib"
				"${O3DS_LIBDATACHANNEL_ROOT}/juice.lib"
				"${O3DS_LIBDATACHANNEL_ROOT}/mbedtls.lib"
				"${O3DS_LIBDATACHANNEL_ROOT}/mbedx509.lib"
				"${O3DS_LIBDATACHANNEL_ROOT}/mbedcrypto.lib"
			)
			set(LDC_SYSTEM_LIBS bcrypt synchronization)
		elseif(APPLE)
			set(LDC_LIB_FILES
				"${O3DS_LIBDATACHANNEL_ROOT}/libdatachannel.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libusrsctp.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libjuice.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedtls.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedx509.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedcrypto.a"
			)
			set(LDC_SYSTEM_LIBS "")
		else()
			# Linux and other UNIX
			set(LDC_LIB_FILES
				"${O3DS_LIBDATACHANNEL_ROOT}/libdatachannel.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libusrsctp.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libjuice.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedtls.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedx509.a"
				"${O3DS_LIBDATACHANNEL_ROOT}/libmbedcrypto.a"
			)
			set(LDC_SYSTEM_LIBS "")
		endif()

		# Validate presence of expected libraries
		set(_missing_files "")
		foreach(_f IN LISTS LDC_LIB_FILES)
			if(NOT EXISTS "${_f}")
				list(APPEND _missing_files "${_f}")
			endif()
		endforeach()
		if(_missing_files)
			message(FATAL_ERROR "O3DS_LIBDATACHANNEL_ROOT is set but required libraries not found:\n  ${_missing_files}\nProvide a root containing the library files listed above.")
		endif()

		add_library(LibDataChannel::LibDataChannel INTERFACE IMPORTED)
		set_target_properties(LibDataChannel::LibDataChannel PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${LDC_INCLUDE_DIR}")
		# Link both third-party libs and any system libs required
		set_property(TARGET LibDataChannel::LibDataChannel APPEND PROPERTY
			INTERFACE_LINK_LIBRARIES ${LDC_LIB_FILES} ${LDC_SYSTEM_LIBS})
		message(STATUS "WebRTC: using prebuilt libdatachannel from ${O3DS_LIBDATACHANNEL_ROOT}")
	else()
		find_package(LibDataChannel REQUIRED HINTS ${CMAKE_CURRENT_LIST_DIR}/../usr)
		message(STATUS "WebRTC: using LibDataChannel CMake package")
	endif()

	add_compile_definitions(O3DS_ENABLE_WEBRTC)
	message(STATUS "WebRTC support enabled")
else()
	message(STATUS "WebRTC support disabled")
endif()

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/_generated/o3ds_generated.h
    COMMAND flatbuffers::flatc -o ${CMAKE_CURRENT_BINARY_DIR}/_generated --cpp ${CMAKE_CURRENT_LIST_DIR}/o3ds.fbs
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/o3ds.fbs )

add_custom_target(protocol DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/_generated/o3ds_generated.h)

set(pub_headers			
	o3ds/o3ds.h
	o3ds/getTime.h
	o3ds/model.h
	o3ds/base_connector.h
	o3ds/nng_connector.h
	o3ds/subscriber.h
	o3ds/async_subscriber.h
	o3ds/async_publisher.h
	o3ds/pair.h
	o3ds/async_pair.h
	o3ds/publisher.h
	o3ds/async_request.h
	o3ds/transform_component.h
	o3ds/request.h
	o3ds/pipeline.h
	o3ds/async_pipeline.h
	o3ds/context.h
	o3ds/math.h
	o3ds/tcp.h
	o3ds/udp_fragment.h
	o3ds/socket_exception.h
	o3ds/socket_util.h
	o3ds/udp.h
	o3ds/socket_address.h
	o3ds/binary_stream.h
	o3ds/xsens_parser.h
	o3ds/o3ds_version.h
	o3ds/o3ds_version.h
)

if(O3DS_WITH_WEBRTC)
    list(APPEND pub_headers o3ds/webrtc_connector.h)
endif()	

set(SOURCES 
	o3ds/o3ds.cpp
	o3ds/getTime.cpp
	o3ds/model.cpp	
	o3ds/base_connector.cpp
	o3ds/nng_connector.cpp
	o3ds/subscriber.cpp
	o3ds/async_subscriber.cpp
	o3ds/async_publisher.cpp
	o3ds/pair.cpp
	o3ds/async_pair.cpp
	o3ds/publisher.cpp
	o3ds/async_request.cpp
	o3ds/request.cpp
	o3ds/pipeline.cpp
	o3ds/async_pipeline.cpp
	o3ds/context.cpp
	o3ds/math.cpp
	o3ds/tcp.cpp
	o3ds/udp_fragment.cpp
	o3ds/socket_exception.cpp
	o3ds/socket_util.cpp
	o3ds/udp.cpp
	o3ds/socket_address.cpp
	o3ds/binary_stream.cpp
	o3ds/xsens_parser.cpp	
	o3ds/o3ds_version.cpp	
)

if(O3DS_WITH_WEBRTC)
    list(APPEND SOURCES o3ds/webrtc_connector.cpp)
endif()
	
if(WIN32)
set(xtralibs wsock32 ws2_32)
endif()

# Shared library
# add_library(open3dstream SHARED ${SOURCES})
# target_link_libraries(open3dstream PRIVATE ${libs}  )
# add_dependencies(open3dstream protocol)

# Static linkable library
add_library(open3dstreamstatic STATIC ${SOURCES} ${pub_headers})

target_include_directories(open3dstreamstatic PUBLIC 
	${CMAKE_CURRENT_BINARY_DIR}/_generated ${CMAKE_CURRENT_LIST_DIR}
	${FLATBUFFERS_INCLUDE_DIRS} 
	${NNG_INCLUDE_DIRS}/..)

if(O3DS_WITH_WEBRTC)
    target_link_libraries(open3dstreamstatic PUBLIC LibDataChannel::LibDataChannel)
endif()

# target_link_libraries(open3dstreamstatic PRIVATE ${NNG_LIBRARIES} ${FLATBUFFERS_LIBRARIES} ${xtralibs})        
add_dependencies(open3dstreamstatic protocol)

list(JOIN pub_headers ";" pub_headers_str)
set_target_properties(open3dstreamstatic PROPERTIES PUBLIC_HEADER "${pub_headers_str}")

if(NOT DEFINED O3DS_VERSION_TAG)
	message(FATAL "No version defined")
endif()
target_compile_definitions(open3dstreamstatic PUBLIC NNG_STATIC_LIB CRCPP_USE_NAMESPACE O3DS_VERSION_TAG=${O3DS_VERSION_TAG})

install(TARGETS open3dstreamstatic 
	LIBRARY DESTINATION lib
	PUBLIC_HEADER DESTINATION include/o3ds
	CONFIGURATIONS RelWithDebInfo)

install(FILES 	${CMAKE_CURRENT_BINARY_DIR}/_generated/o3ds_generated.h
	DESTINATION include)
