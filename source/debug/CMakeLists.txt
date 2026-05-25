set(LIB_SRC
  PUBLIC scene.h
  PUBLIC script.h
  PUBLIC state_dump.h
  PUBLIC camera.h
  PUBLIC input.h
  PUBLIC interactive.h
  PUBLIC ui.h
  scene.cc
  script.cc
  stb_image_impl.cc
  state_dump.cc
  input.cc
  interactive.cc
  ui.cc
)

set(LIB
  util math platform
  mesh meshlog brush spatial gpu
  window vulkan imgui
)

sc_add_library(debug_core "${LIB_SRC}" "${LIB}" STATIC)

# Real GPU compute dispatch for the WGSL backend needs the SPIR-V kernels
# (built by the spirv backend) and a Vulkan device. Gate the dispatch path on
# both; without it, `set_backend wgsl` falls back to the C++ executor.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_SPIRV)
  target_compile_definitions(debug_core PRIVATE
    SBRUSH_GPU_DISPATCH=1
    SBRUSH_SPV_DIR="${CMAKE_BINARY_DIR}/sbrush_out/spirv")
endif()

target_link_libraries(debug_core glfw Vulkan::Vulkan)
target_include_directories(debug_core PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)

add_executable(debug_app debug_app.cc)
target_link_libraries(debug_app debug_core)
target_include_directories(debug_app PUBLIC
  ${CMAKE_SOURCE_DIR}/source
  ${CMAKE_SOURCE_DIR}/extern
)
if(MSVC)
  target_compile_options(debug_app PUBLIC /std:c++20)
endif()
