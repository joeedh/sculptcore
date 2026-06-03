set(LIB_SRC
  PUBLIC scene.h
  PUBLIC script.h
  PUBLIC state_dump.h
  PUBLIC camera.h
  PUBLIC input.h
  PUBLIC interactive.h
  PUBLIC ui.h
  PUBLIC gpu_stroke.h
  scene.cc
  script.cc
  gpu_stroke.cc
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
    SBRUSH_SPV_DIR=${CMAKE_BINARY_DIR}/sbrush_out/spirv)

  # Native WebGPU (wgpu-native) compute backend: adds the WgpuNative brush
  # backend that runs the .wgsl kernels through webgpu.h. Layers on top of the
  # spirv dispatch path (whole GPU path lives in gpu_stroke.cc); requires the
  # WGSL kernels too. The wgpu_native DLL is copied next to debug_app below.
  if (SBRUSH_WEBGPU_COMPUTE AND SBRUSH_BACKEND_WGSL)
    # PUBLIC: debug_app.cc (separate executable target) gates its --backend
    # webgpu parser on SBRUSH_WEBGPU_COMPUTE, so the define must propagate
    # transitively through the link, not stay private to debug_core.
    
    # Escape backslashes so the path survives as a C string literal (Windows
    # paths can contain '\', which would otherwise be read as escape sequences).
    string(REPLACE "\\" "/" _sbrush_wgsl_dir "${CMAKE_BINARY_DIR}/sbrush_out/wgsl")

    target_compile_definitions(debug_core PUBLIC
      SBRUSH_WEBGPU_COMPUTE=1
      SBRUSH_WGSL_DIR=${_sbrush_wgsl_dir})
    target_link_libraries(debug_core webgpu)
  endif()
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

# wgpu-native ships as a DLL — copy it next to debug_app so the WgpuNative
# backend loads it at run (same as tests/test_webgpu_render).
if (NOT BUILD_WASM AND SBRUSH_WEBGPU_COMPUTE AND SBRUSH_BACKEND_WGSL AND SBRUSH_BACKEND_SPIRV)
  add_custom_command(TARGET debug_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:wgpu_native> $<TARGET_FILE_DIR:debug_app>
    VERBATIM)
endif()
