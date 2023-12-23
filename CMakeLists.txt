cmake_minimum_required(VERSION 3.15)
project(sculptcore C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SOURCE_FILE_EXTENSIONS .cc)

option(BUILD_WASM "Build wasm library" OFF)
set(EMSDK_PATH "" CACHE PATH "Path to emsdk")
set(EMCC_PATH "" CACHE PATH "Path to emcc")

message("BUILD_WASM" ${BUILD_WASM})

enable_testing()

set(CMAKE_OPTIMIZE_DEPENDENCIES true)

#if (BUILD_WASM)
#	include("build_files/WASM.cmake")
#endif()

include("build_files/macros.cmake")

add_subdirectory(extern)
add_subdirectory(source)

if (NOT BUILD_WASM)
	add_subdirectory(tests)
endif()

if (NOT BUILD_WASM)
	set(LIB
	  util
	  mesh
	  platform
	)

	add_library(sculptcore SHARED)
	target_link_libraries(sculptcore ${LIB})
else()
	set(LIB
	  util
	  mesh
	)

	add_library(sculptcore STATIC)

	add_custom_command(
		TARGET sculptcore
		POST_BUILD
		COMMAND python ${CMAKE_SOURCE_DIR}/build_files/link_wasm.py ${EMCC_PATH} $<TARGET_FILE:math> $<TARGET_FILE:util> $<TARGET_FILE:mesh> 
	)

	target_link_libraries(sculptcore ${LIB})
endif()
