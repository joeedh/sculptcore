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
  add_compile_options( -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20 -Wno-invalid-offsetof ) #Zc:twoPhase- )
  if (WIN32)
    add_compile_options( -DWIN32 )
  endif()
endif()


set(CMAKE_OPTIMIZE_DEPENDENCIES true)

# Native build links the per-module static libs into libsculptcore.so;
# without -fPIC the static archives can't be relocated into the shared
# object. (Harmless on WASM, where everything is a static archive.)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# sbrush DSL — brush compute compiler. See documentation/brush_compute_dsl.md
# and the Wave 1 plan in .claude/plans/.
option(SBRUSH_BACKEND_CPP    "Emit C++ brush kernels (reference)"                ON)
option(SBRUSH_BACKEND_WGSL   "Emit WGSL kernels + tint validate"                 OFF)
option(SBRUSH_BACKEND_SPIRV  "Emit SPIR-V kernels + spirv-val"                   OFF)
option(SBRUSH_BACKEND_CUDA   "Emit CUDA kernels + nvcc --cuda --dryrun"          OFF)
option(SBRUSH_BACKEND_HIP    "Emit HIP kernels + hipcc syntax-only"              OFF)
option(SBRUSH_BACKEND_OPENCL "Emit OpenCL C kernels + clspv/poclcc"              OFF)
option(SBRUSH_VALIDATE_ALL   "Run external validators on every emit"             OFF)
option(SBRUSH_REGEN_ON_BUILD "Re-run sbrushc during build (vs. checked-in outputs)" ON)

include("build_files/macros.cmake")
include("source/litestl/build_files/macros.cmake")

# Backend toolchain probes — gated per-backend inside the include so
# disabling all GPU backends keeps the configure short and offline.
if (SBRUSH_BACKEND_WGSL OR SBRUSH_BACKEND_SPIRV OR SBRUSH_BACKEND_CUDA
    OR SBRUSH_BACKEND_HIP OR SBRUSH_BACKEND_OPENCL)
  include("build_files/sbrush_toolchains.cmake")
endif()

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

