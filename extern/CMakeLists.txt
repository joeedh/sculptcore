if (NOT BUILD_WASM)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(glfw)
  # GLFW is used Vulkan-only in this project; tell glfw3.h not to pull
  # in <GL/gl.h> for every consumer that links glfw. INTERFACE so the
  # define doesn't leak into GLFW's own TUs (its internal.h forbids it).
  target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

  # Vulkan is found at the root via find_package(Vulkan REQUIRED) so the
  # Vulkan::Vulkan imported target is visible to all subdirectories.

  include("${CMAKE_CURRENT_SOURCE_DIR}/imgui_build.cmake")
endif()
