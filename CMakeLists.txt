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
	add_compile_options(-DWASM -Wno-invalid-offsetof -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20 )

	if (CMAKE_BUILD_TYPE STREQUAL "Debug")
		add_compile_options(-fsanitize=address)
    # do not use debug allocator together with asan 
    add_compile_options(-DNO_DEBUG_ALLOC)
		add_link_options(-fsanitize=address)
		add_link_options(-sINITIAL_MEMORY=943718400)
  else()
    #add_link_options(-sINITIAL_MEMORY=943718400)
	endif()

  add_link_options(-sALLOW_MEMORY_GROWTH=1)
  add_link_options(-sSHARED_MEMORY=1)
  add_link_options(-pthread)
  add_compile_options(-pthread)
  add_link_options(-sEXPORTED_RUNTIME_METHODS=wasmMemory)
  add_link_options(-sIMPORTED_MEMORY=1)
else()
  add_compile_options( -DWIN32 -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20 -Wno-invalid-offsetof ) #Zc:twoPhase- )
endif()


set(CMAKE_OPTIMIZE_DEPENDENCIES true)

include("build_files/macros.cmake")
include("source/litestl/build_files/macros.cmake")

if (NOT BUILD_WASM)
  # Vulkan headers + loader from the system Vulkan SDK. Found here at the
  # root so the Vulkan::Vulkan imported target is visible to every
  # subdirectory that links it (source/vulkan/, source/debug/).
  find_package(Vulkan REQUIRED)
endif()

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
      spatial
	  brush
	  props
      pthread
	  meshlog
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
	  math
 	  mesh
	  spatial
	  brush
	  props
      gpu
      binding
	  binding_generators
	)

	add_library(sculptcore SHARED)
	target_link_libraries(sculptcore ${LIB})
endif()

