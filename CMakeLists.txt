cmake_minimum_required(VERSION 3.15)
project(sculptcore C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED true)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SOURCE_FILE_EXTENSIONS .cc .cpp)

if (NOT BUILD_WASM) 
	include(CTest)
endif()

#enable_testing()
if (BUILD_WASM)
	add_compile_options(-DWASM -Wno-invalid-offsetof)
endif()

set(CMAKE_OPTIMIZE_DEPENDENCIES true)

include("build_files/macros.cmake")
include("source/litestl/build_files/macros.cmake")

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
    binding_generators
	  sculptcore_wasm
	  sculptcore_core
    gpu
	)

	# merge litestl wasm symbols
	get_property(existing GLOBAL PROPERTY WASM_SYMBOLS)
	get_property(lt_existing GLOBAL PROPERTY LT_WASM_SYMBOLS)
	set_property(GLOBAL PROPERTY WASM_SYMBOLS "${existing}\n${lt_existing}")

	# build final symbol list
	get_property(wasmsym GLOBAL PROPERTY "WASM_SYMBOLS")

	
  build_wasm_browser(sculptcore-browser "" "${LIB}" "${wasmsym}")
	build_wasm(sculptcore "" "${LIB}" "${wasmsym}")
  
else()
 	set(LIB
 	  util
 	  mesh
	  spatial
	  brush
    gpu
	)

	add_library(sculptcore SHARED)
	target_link_libraries(sculptcore ${LIB})
endif()

