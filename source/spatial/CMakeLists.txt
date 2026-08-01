set(LIB
	PUBLIC mesh
	util
	math
	gpu
	binding
)

set(SRC
  PUBLIC node.h
  PUBLIC spatial.h
  PUBLIC spatial_enums.h
  PUBLIC spatial_attrs.h
  PUBLIC bindings.h
  PUBLIC shaders/spatial_shaders.h
  shaders/spatial_shaders.cc
  node.cc
  spatial.cc
  spatial_gpu.cc
  bindings.cc
  c-api/spatial_c_api.cc
  c-api/external_draw.cc
)

# Codegen: WGSL -> {.wgsl.h, .spirv.h}
#
# We always emit the .wgsl.h text header (zero deps). SPIR-V emission needs the
# `naga` CLI and is skipped under WASM (the native Vulkan backend is the only
# consumer).
set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/generated")
file(MAKE_DIRECTORY "${SHADER_GEN_DIR}")
find_program(NODE_EXECUTABLE NAMES node nodejs REQUIRED)
set(WGSL_TOOL "${CMAKE_SOURCE_DIR}/tools/wgsl-to-spirv.mjs")

if (BUILD_WASM)
  set(SHADER_EMIT_MODE text)
else()
  set(SHADER_EMIT_MODE both)
endif()

set(SHADER_GEN_OUTPUTS)
function(_sc_add_shader name)
  set(_wgsl "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${name}.wgsl")
  set(_text "${SHADER_GEN_DIR}/${name}.wgsl.h")
  set(_spv "${SHADER_GEN_DIR}/${name}.spirv.h")
  if (BUILD_WASM)
    set(_outs ${_text})
  else()
    set(_outs ${_text} ${_spv})
  endif()
  add_custom_command(
    OUTPUT ${_outs}
    COMMAND ${NODE_EXECUTABLE} ${WGSL_TOOL}
            --input ${_wgsl}
            --out-dir ${SHADER_GEN_DIR}
            --name ${name}
            --emit ${SHADER_EMIT_MODE}
    DEPENDS ${_wgsl} ${WGSL_TOOL}
    COMMENT "WGSL codegen (${SHADER_EMIT_MODE}): ${name}"
    VERBATIM
  )
  set(SHADER_GEN_OUTPUTS ${SHADER_GEN_OUTPUTS} ${_outs} PARENT_SCOPE)
endfunction()

_sc_add_shader(basic_line)
_sc_add_shader(basic_mesh)
_sc_add_shader(point_sprite)

add_custom_target(spatial_shaders_gen DEPENDS ${SHADER_GEN_OUTPUTS})

sc_add_library(spatial "${SRC}" "${LIB}" OBJECT)
add_dependencies(spatial spatial_shaders_gen)
target_include_directories(spatial PRIVATE "${SHADER_GEN_DIR}")
wasm_add_symbols("
  Mesh_buildSpatialTree
  SpatialTree_free
  getSpatialShaders
  setTreeRequestedAttrs
  setTreeDrawShader
  getTreeMissingAttrSlots
  setTreeFaceDisplacementBounds
  sc_external_draw_register
  sc_external_draw_unregister
  sc_external_draw_update
  sc_external_draw_enable_dynamic
  sc_external_draw_set_default_group
  sc_external_draw_provider
")
