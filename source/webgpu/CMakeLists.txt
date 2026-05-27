set(LIB
  util
  math
  gpu
  mesh
  spatial
)

set(SRC
  PUBLIC wgpu_context.h
  PUBLIC wgpu_backend.h
  PUBLIC wgpu_compute.h
  wgpu_context.cc
  wgpu_backend.cc
  wgpu_compute.cc
  wgpu_demo.cc
  wgpu_screenshot.cc
)

sc_add_library(webgpu "${SRC}" "${LIB}" STATIC)

if (BUILD_WASM)
  # emdawnwebgpu supplies <webgpu/webgpu.h> via an emscripten port. The flag is
  # needed at compile time (include path) here, and at link time on the final
  # executable (added in build_wasm_post). PUBLIC so consumers that include
  # wgpu_backend.h pick up the header path too.
  target_compile_options(webgpu PUBLIC "--use-port=emdawnwebgpu")
else()
  # Native: the prebuilt wgpu-native carries <webgpu/webgpu.h> + the import
  # lib. Plain signature to match sc_add_library; the imported target's
  # INTERFACE include dir + implib propagate to consumers transitively.
  target_link_libraries(webgpu wgpu_native)
  # stb_image_write.h for wgpu_screenshot.cc (same as source/vulkan).
  target_include_directories(webgpu PRIVATE "${CMAKE_SOURCE_DIR}/extern")
endif()

wasm_add_symbols("
  webgpuRenderScene
")
