if (NOT BUILD_WASM)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(glfw)

  # Vulkan is found at the root via find_package(Vulkan REQUIRED) so the
  # Vulkan::Vulkan imported target is visible to all subdirectories.
endif()
