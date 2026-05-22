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
  PUBLIC vk_overlay.h
  PUBLIC vk_screenshot.h
  PUBLIC vk_swapchain.h
  vk_context.cc
  vk_backend.cc
  vk_overlay.cc
  vk_screenshot.cc
  vk_swapchain.cc
)

sc_add_library(vulkan "${SRC}" "${LIB}" STATIC)
target_include_directories(vulkan PUBLIC "${CMAKE_SOURCE_DIR}/extern")
