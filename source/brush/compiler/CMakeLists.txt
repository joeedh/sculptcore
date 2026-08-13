# sbrushc — host tool that compiles .sbrush kernels to per-backend
# source. Built only in native configurations; cross-compiled WASM
# builds consume the pre-generated outputs in kernels/generated/.

if (BUILD_WASM)
  return()
endif()

# The compiler proper is a static lib so tests can drive the parser and
# emitters directly (tests/test_sbrush_textures.cc); sbrushc is a thin
# CLI wrapper over it.
set(SBRUSHC_CORE_SRC
  ir.cc
  lexer.cc
  parser.cc
  emit_cpp.cc
  emit_registry.cc
  emit_wgsl.cc
  emit_cuda.cc
  emit_opencl.cc
  ../kernels/ir/intrinsics.cc
)

add_library(sbrushc_core STATIC ${SBRUSHC_CORE_SRC})

target_link_libraries(sbrushc_core PUBLIC util math)
target_include_directories(sbrushc_core PUBLIC
  ${CMAKE_SOURCE_DIR}/source
)

add_executable(sbrushc sbrushc_main.cc)
target_link_libraries(sbrushc PRIVATE sbrushc_core)

if (MSVC)
  target_compile_options(sbrushc_core PRIVATE /std:c++20)
  target_compile_options(sbrushc PRIVATE /std:c++20)
endif()
