# sbrushc — host tool that compiles .sbrush kernels to per-backend
# source. Built only in native configurations; cross-compiled WASM
# builds consume the pre-generated outputs in kernels/generated/.

if (BUILD_WASM)
  return()
endif()

set(SBRUSHC_SRC
  ir.cc
  lexer.cc
  parser.cc
  emit_cpp.cc
  emit_registry.cc
  emit_wgsl.cc
  emit_cuda.cc
  emit_opencl.cc
  sbrushc_main.cc
  ../kernels/ir/intrinsics.cc
)

add_executable(sbrushc ${SBRUSHC_SRC})

target_link_libraries(sbrushc PRIVATE util math)
target_include_directories(sbrushc PRIVATE
  ${CMAKE_SOURCE_DIR}/source
)

if (MSVC)
  target_compile_options(sbrushc PRIVATE /std:c++20)
endif()
