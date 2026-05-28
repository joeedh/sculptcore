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

add_compile_options(-Wno-parentheses-equality)


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
option(SBRUSH_BACKEND_CUDA   "Emit CUDA kernels + clang device-only gate"        OFF)
option(SBRUSH_BACKEND_HIP    "Emit HIP kernels + clang device-only gate"         OFF)
option(SBRUSH_BACKEND_OPENCL "Emit OpenCL C kernels + clspv/poclcc"              OFF)
option(SBRUSH_VALIDATE_ALL   "Run external validators on every emit"             OFF)
option(SBRUSH_REGEN_ON_BUILD "Re-run sbrushc during build (vs. checked-in outputs)" ON)
option(SBRUSH_WEBGPU_COMPUTE "Build the native WebGPU (wgpu-native) compute brush backend into debug_app" OFF)

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
      webgpu
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

	# --- Node / Electron N-API addon ----------------------------------------
	# Built only when CMake is configured by cmake-js, which injects CMAKE_JS_*
	# (headers, node.lib import lib, win delay-load hook) for the chosen runtime
	# (--runtime electron). A plain `make.mjs configure native` never defines
	# CMAKE_JS_VERSION, so this target is invisible to the normal native build;
	# it's driven by `make.mjs node`. See documentation/plans/native-electron.md
	# (Workstream A) and sculptcore/spike/napi/RESULTS.md.
	if (DEFINED CMAKE_JS_VERSION)
	  # cmake-js puts "/DELAYLOAD:NODE.EXE" (link.exe syntax) in the global
	  # CMAKE_SHARED_LINKER_FLAGS; the clang++ driver reads a bare "/DELAYLOAD:.."
	  # as an input filename. Strip it and re-add it on the addon in clang
	  # passthrough form below. (Spike A.5 finding.)
	  string(REPLACE "/DELAYLOAD:NODE.EXE" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")

	  # sculptcore_core pulls in initBindings() + getBindingManager() and links
	  # every module lib transitively; ${LIB} is listed too to be explicit. The
	  # raw C N-API entry is in source/napi/. ${CMAKE_JS_SRC} is the Windows
	  # delay-load hook that rebinds node imports to the host (electron.exe).
	  add_library(sculptcore_node MODULE
	    source/napi/napi_entry.cc
	    source/napi/napi_runtime.cc
	    ${CMAKE_JS_SRC})
	  set_target_properties(sculptcore_node PROPERTIES PREFIX "" SUFFIX ".node")
	  target_include_directories(sculptcore_node PRIVATE ${CMAKE_JS_INC})
	  target_link_libraries(sculptcore_node sculptcore_core ${LIB} ${CMAKE_JS_LIB})
	  target_compile_definitions(sculptcore_node PRIVATE NAPI_VERSION=8)
	  if (WIN32)
	    target_link_options(sculptcore_node PRIVATE "LINKER:/DELAYLOAD:node.exe")
	    target_link_libraries(sculptcore_node delayimp.lib)
	  endif()
	endif()
endif()

