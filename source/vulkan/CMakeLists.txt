set(LIB
  util
  math
  gpu
  spatial
  Vulkan::Vulkan
  glfw
)

set(SRC
  PUBLIC vk_context.h
  PUBLIC vk_backend.h
  PUBLIC vk_compute.h
  PUBLIC vk_normals.h
  PUBLIC vk_overlay.h
  PUBLIC vk_screenshot.h
  PUBLIC vk_swapchain.h
  vk_context.cc
  vk_backend.cc
  vk_compute.cc
  vk_normals.cc
  vk_overlay.cc
  vk_screenshot.cc
  vk_swapchain.cc
)

sc_add_library(vulkan "${SRC}" "${LIB}" STATIC)
target_include_directories(vulkan PUBLIC "${CMAKE_SOURCE_DIR}/extern")

# GPU-resident stroke path (debug app): compile the standalone compute shaders
# used by GpuNormalPass (vertex-normal recompute + scatter into render VBOs)
# with glslc into ${CMAKE_BINARY_DIR}/vk_shaders, and point VK_COMPUTE_SPV_DIR
# at them. Gated the same way as debug_core's SBRUSH_GPU_DISPATCH so the .spv
# only exist when the SPIR-V backend is on. FindVulkan exposes glslc as
# Vulkan_GLSLC_EXECUTABLE; fall back to a plain PATH lookup.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_SPIRV)
  if (NOT Vulkan_GLSLC_EXECUTABLE)
    find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc
      DOC "glslc GLSL->SPIR-V compiler (Vulkan SDK)")
  endif()
  if (NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR
      "SBRUSH_BACKEND_SPIRV=ON but 'glslc' was not found (needed for the "
      "GpuNormalPass compute shaders). Install the Vulkan SDK or unset the option.")
  endif()
  message(STATUS "vulkan: glslc = ${Vulkan_GLSLC_EXECUTABLE}")

  set(VK_SPV_DIR "${CMAKE_BINARY_DIR}/vk_shaders")
  file(MAKE_DIRECTORY "${VK_SPV_DIR}")
  set(_vk_spv_outputs)
  foreach(_shader normal_face normal_vert scatter)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}.comp")
    set(_out "${VK_SPV_DIR}/${_shader}.spv")
    add_custom_command(
      OUTPUT "${_out}"
      COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O "${_src}" -o "${_out}"
      DEPENDS "${_src}"
      COMMENT "glslc ${_shader}.comp -> ${_shader}.spv"
      VERBATIM)
    list(APPEND _vk_spv_outputs "${_out}")
  endforeach()
  add_custom_target(vk_compute_shaders ALL DEPENDS ${_vk_spv_outputs})
  add_dependencies(vulkan vk_compute_shaders)

  target_compile_definitions(vulkan PRIVATE VK_COMPUTE_SPV_DIR="${VK_SPV_DIR}")
endif()
