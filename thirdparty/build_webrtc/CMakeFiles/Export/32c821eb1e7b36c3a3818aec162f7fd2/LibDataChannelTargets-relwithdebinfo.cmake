#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "LibDataChannel::LibDataChannel" for configuration "RelWithDebInfo"
set_property(TARGET LibDataChannel::LibDataChannel APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(LibDataChannel::LibDataChannel PROPERTIES
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/libdatachannel.so.0.23.2"
  IMPORTED_SONAME_RELWITHDEBINFO "libdatachannel.so.0.23"
  )

list(APPEND _cmake_import_check_targets LibDataChannel::LibDataChannel )
list(APPEND _cmake_import_check_files_for_LibDataChannel::LibDataChannel "${_IMPORT_PREFIX}/lib/libdatachannel.so.0.23.2" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
