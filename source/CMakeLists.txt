add_subdirectory(litestl)
add_subdirectory(mesh)
add_subdirectory(displace)
add_subdirectory(subdiv)
add_subdirectory(vdm)
add_subdirectory(spatial)
add_subdirectory(wasm)
add_subdirectory(props)
add_subdirectory(brush)
add_subdirectory(dyntopo)
add_subdirectory(remesh)
add_subdirectory(core)
add_subdirectory(gpu)
add_subdirectory(meshlog)
# WebGPU backend builds for both targets: emdawnwebgpu under WASM, the
# prebuilt wgpu-native impl natively (extern/wgpu_native).
add_subdirectory(webgpu)
if (NOT BUILD_WASM)
  # window/app (GLFW) and the Vulkan backend + debug app are the interactive
  # path; the shipped libs render via source/webgpu. window/app are the only
  # consumers of glfw, so gate them together — this also drops glfw's X11
  # requirement on headless Linux CI (WITH_VULKAN=OFF).
  if (WITH_VULKAN)
    add_subdirectory(window)
    add_subdirectory(app)
    add_subdirectory(vulkan)
    add_subdirectory(debug)
  endif()
endif()
