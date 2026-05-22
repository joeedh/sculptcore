# Dear ImGui (docking branch) — vendored under extern/imgui/.
# Native-only: linked from source/debug/. The WASM target uses the WebGPU
# backend later; for now we only need the GLFW + Vulkan backends.

if (BUILD_WASM)
  return()
endif()

set(IMGUI_DIR "${CMAKE_CURRENT_LIST_DIR}/imgui")

add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC
  ${IMGUI_DIR}
  ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
# imgui_impl_vulkan emits VK_NULL_HANDLE / VkPipelineCreateInfo defaults
# that aren't all members of every struct version — accept the warning
# rather than patching vendor sources.
if (MSVC)
  target_compile_options(imgui PRIVATE /wd4267 /wd4244)
endif()
