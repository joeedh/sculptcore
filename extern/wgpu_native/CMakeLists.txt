# Imported target for the prebuilt wgpu-native (the native webgpu.h impl;
# emdawnwebgpu supplies it under WASM). Binaries are not checked in — run
# `node extern/wgpu_native/fetch.mjs` (or `node make.mjs fetch-wgpu-native`)
# to populate include/ and lib/. Native build only.

set(WGPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if (NOT EXISTS "${WGPU_DIR}/include/webgpu/webgpu.h")
  message(FATAL_ERROR
    "wgpu-native prebuilt missing. Run: node extern/wgpu_native/fetch.mjs")
endif()

add_library(wgpu_native SHARED IMPORTED GLOBAL)

if (WIN32)
  set_target_properties(wgpu_native PROPERTIES
    IMPORTED_LOCATION "${WGPU_DIR}/lib/wgpu_native.dll"
    IMPORTED_IMPLIB   "${WGPU_DIR}/lib/wgpu_native.dll.lib")
else()
  message(FATAL_ERROR "wgpu_native: only the Windows prebuilt is wired up")
endif()

target_include_directories(wgpu_native INTERFACE "${WGPU_DIR}/include")
