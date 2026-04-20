cmake_minimum_required(VERSION 3.15)
project(sculptcore C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SOURCE_FILE_EXTENSIONS .cc .cpp)

message("BUILD_WASM = " ${BUILD_WASM})

if (NOT BUILD_WASM) 
	include(CTest)
endif()

#enable_testing()
if (BUILD_WASM)
	add_compile_options(-DWASM -Wno-invalid-offsetof)
endif()

set(CMAKE_OPTIMIZE_DEPENDENCIES true)

include("build_files/macros.cmake")

add_subdirectory(extern)
add_subdirectory(source)

if (NOT BUILD_WASM)
	add_subdirectory(tests)
endif()

if (BUILD_WASM)
	set(LIB
	  util
	  math
	  mesh
	  platform
	  binding
	  sculptcore_wasm
	  sculptcore_core
	)

	add_executable(sculptcore)
	target_link_libraries(sculptcore ${LIB})
	get_property(wasmsym GLOBAL PROPERTY "WASM_SYMBOLS")
	#string(REPLACE "\n" "," wasmsym "${wasmsym}")
	string(REPLACE " " "" wasmsym "${wasmsym}")
	string(REGEX REPLACE "\n\n+" "\n" wasmsym "${wasmsym}")
	string(STRIP "${wasmsym}" wasmsym)
	string(REPLACE "\n" ",_"  wasmsym "${wasmsym}")
	set(wasmsym "_${wasmsym}")
	message("SYMBOLS = ${wasmsym}")

	target_link_options(sculptcore PRIVATE "-sMODULARIZE=1")
	target_link_options(sculptcore PRIVATE "-sEXPORTED_RUNTIME_METHODS=HEAPU8")
	
	target_link_options(sculptcore PRIVATE "-sEXPORTED_FUNCTIONS=${wasmsym}")
	target_link_options(sculptcore PRIVATE "--bind")
else()
 	set(LIB
 	  util
 	  mesh
	  spatial
	  brush
	)

	add_library(sculptcore SHARED)
	target_link_libraries(sculptcore ${LIB})
endif()

