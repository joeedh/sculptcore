set(LIB
    eigen
    util
    math
    props
    displace
    spatial
    meshlog
    platform
    subdiv
)

set(SRC
  PUBLIC brush.h
  PUBLIC test.h
  PUBLIC exec.h
  PUBLIC props.h
  PUBLIC brush_iterators.h
  PUBLIC brush_command.h
  PUBLIC brush_concepts.h
  PUBLIC compute_layout.h
  PUBLIC compute_dispatch.h
  PUBLIC gpu_marshal.h
  gpu_marshal.cc
  PUBLIC gpu_brush_session.h
  c-api/gpu_brush_c_api.cc
  PUBLIC grid_executor.h
  PUBLIC grid_gpu_session.h
  c-api/grid_stroke_c_api.cc
  c-api/mesh_stroke_batch_c_api.cc
  PUBLIC stroke_spacing.h
  PUBLIC stroke_curve.h
  PUBLIC stroke_driver.h
  stroke_driver.cc
  PUBLIC brushes/all.h
  PUBLIC brushes/extra.h
  PUBLIC feature_field.h
  feature_field.cc
  PUBLIC texture_eval.h
  PUBLIC texture_registry.h
  texture_registry.cc
  PUBLIC texture_jit.h
  texture_jit.cc
  PUBLIC texture_program.h
  texture_program.cc
  PUBLIC host_sampler.h
  host_sampler.cc
  brush_command.cc
  brush_executor.h
  brush_executor.cc
  brush.cc
  PUBLIC bindings.h
  bindings.cc
)

sc_add_library(brush "${SRC}" "${LIB}" STATIC)

# Runtime texture-script JIT (embedded tinycc, extern/tinycc): native builds
# only. WASM has no tcc — textureScriptCpuAvailable() compiles to false there
# and hosts fall back to the precompiled texture registry.
if (NOT BUILD_WASM)
  target_compile_definitions(brush PRIVATE SCULPTCORE_HAVE_TCC=1)
  # Plain signature — sc_add_library already links this target plain-form,
  # and CMake forbids mixing it with the PRIVATE/PUBLIC keyword form.
  target_link_libraries(brush tinycc)
endif()

# Runtime texture-script compilation (compileTextureScript): the sbrush
# compiler objects link into the engine lib natively. WASM (and a build
# without the cpp backend) compiles the stub that reports unavailability.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_CPP)
  target_compile_definitions(brush PRIVATE SCULPTCORE_HAVE_TEXTURE_COMPILER=1)
  target_link_libraries(brush sbrushc_core)
endif()

wasm_add_symbols("
  GpuBrush_beginStroke
  GpuBrush_free
  GpuBrush_kernelName
  GpuBrush_info
  GpuBrush_marshalDab
  GpuBrush_dataSize
  GpuBrush_dataPtr
  GpuBrush_applyCo
  GpuBrush_endStroke
  GridStroke_new
  GridStroke_free
  GridStroke_supported
  GridStroke_setMirror
  GridStroke_setNonAccum
  GridStroke_setAnchoredGrab
  GridStroke_setDeferNormals
  GridStroke_flushNormals
  GridStroke_sync
  GridStroke_syncMask
  Multires_hasGridDomain
  GridStroke_begin
  GridStroke_dab
  GridStroke_dabProgram
  GridStroke_castBatch
  GridStroke_dabBatch
  GridStroke_dabBatchProgram
  MeshStroke_castBatch
  MeshStroke_dabBatch
  MeshStroke_dabBatchProgram
  GridStroke_end
  GridStroke_canUndo
  GridStroke_canRedo
  GridStroke_undo
  GridStroke_redo
  GridStroke_dropOldest
  GridStroke_undoBytes
  GridTree_castRay
  sc_host_sampler_register
  sc_host_sampler_unregister
")

# Generated sbrush kernels (created by `node make.mjs codegen`).
# The directory may not exist yet at configure time; that's fine —
# it becomes populated before the first build that needs it.
target_include_directories(brush PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/kernels/generated
)

# sbrush compiler — host-only tool. Skipped on WASM (cross-compile);
# WASM builds consume the .gen.h files produced by a prior native
# `node make.mjs codegen` run.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_CPP)
  add_subdirectory(compiler)
endif()

# Standalone texture units (.stex) — precompiled procedural textures that
# brushes import with `use texture <Name>;`. The checked-in
# kernels/generated/*.tex.gen.h + sculptcore_textures.gen.h come from
# `node make.mjs codegen`; here the units are passed (--texture=) to every
# in-build sbrushc kernel compile so imports resolve.
file(GLOB SB_TEX_UNITS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.stex")
list(SORT SB_TEX_UNITS)
set(SB_TEX_UNIT_FLAGS)
foreach(SRC ${SB_TEX_UNITS})
  list(APPEND SB_TEX_UNIT_FLAGS "--texture=${SRC}")
endforeach()

# Extra (out-of-repo) kernels: compile each configured dir's .sbrush files
# in-build and generate the registry (enum items + factory dispatch) consumed
# via brushes/extra.h. Gen files land in <build>/sbrush_extra/gen, which the
# root CMakeLists put on the include path alongside SCULPTCORE_EXTRA_BRUSHES.
# Ids are positional: dirs in option order, stems sorted bytewise per dir.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_CPP AND SCULPTCORE_EXTRA_KERNEL_DIRS)
  set(SB_EXTRA_GEN_DIR "${CMAKE_BINARY_DIR}/sbrush_extra/gen")
  file(MAKE_DIRECTORY "${SB_EXTRA_GEN_DIR}")

  # Built-in kernels feed the registry's collision checks (stems + @brush names).
  file(GLOB SB_EXTRA_BUILTINS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  list(SORT SB_EXTRA_BUILTINS)

  # Built-in SculptBrushes enum item names — NOT derivable from @brush names
  # (FEATURE_ALIGN vs "featurealign"). Keep in sync with brushes/types.h.
  set(SB_EXTRA_RESERVED "DRAW,INFLATE,CLAY,PINCH,SHARP,MASK,SMOOTH,KELVINLET,POSE,TEXDRAW,SCRAPE,FILL,WINGSCRAPE,COLOR,POLYGROUP,BSMOOTH,GRAB,SNAKEHOOK,COLORSMOOTH,FEATURE_ALIGN,LAYERDRAW,ENHANCE,TEXGRAD")

  set(SB_EXTRA_INPUTS)
  set(SB_EXTRA_TEX_UNITS)
  foreach(DIR ${SCULPTCORE_EXTRA_KERNEL_DIRS})
    file(GLOB SB_EXTRA_DIR_INPUTS CONFIGURE_DEPENDS "${DIR}/*.sbrush")
    list(SORT SB_EXTRA_DIR_INPUTS)
    list(APPEND SB_EXTRA_INPUTS ${SB_EXTRA_DIR_INPUTS})
    file(GLOB SB_EXTRA_DIR_TEX CONFIGURE_DEPENDS "${DIR}/*.stex")
    list(SORT SB_EXTRA_DIR_TEX)
    list(APPEND SB_EXTRA_TEX_UNITS ${SB_EXTRA_DIR_TEX})
  endforeach()

  # Extra kernels can import both in-repo and extra-dir texture units.
  set(SB_EXTRA_TEX_FLAGS ${SB_TEX_UNIT_FLAGS})
  foreach(SRC ${SB_EXTRA_TEX_UNITS})
    list(APPEND SB_EXTRA_TEX_FLAGS "--texture=${SRC}")
  endforeach()

  set(SB_EXTRA_OUTPUTS)
  set(SB_EXTRA_IN_FLAGS)
  foreach(SRC ${SB_EXTRA_INPUTS})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(OUT "${SB_EXTRA_GEN_DIR}/${STEM}.brush.gen.h")
    add_custom_command(
      OUTPUT "${OUT}"
      COMMAND $<TARGET_FILE:sbrushc> --backend=cpp --extras --in=${SRC} --out=${OUT}
              ${SB_EXTRA_TEX_FLAGS}
      DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS} ${SB_EXTRA_TEX_UNITS}
      COMMENT "sbrush extra: ${STEM}.brush.gen.h"
      VERBATIM)
    list(APPEND SB_EXTRA_OUTPUTS "${OUT}")
    list(APPEND SB_EXTRA_IN_FLAGS "--in=${SRC}")
  endforeach()

  # Extra-dir texture units are precompiled in-build, and the texture registry
  # is regenerated over in-repo + extra units into SB_EXTRA_GEN_DIR — which
  # precedes kernels/generated on the include path, so it shadows the
  # checked-in (in-repo-only) sculptcore_textures.gen.h.
  if (SB_EXTRA_TEX_UNITS)
    # The shadow registry lands *earlier* on the include path than the header
    # texture_registry.cc's depfile recorded — invisible to ninja. Flip a
    # define on that TU so none->some recompiles it (some->none self-dirties).
    set_source_files_properties(texture_registry.cc PROPERTIES
        COMPILE_DEFINITIONS SCULPTCORE_EXTRA_TEXTURES=1)
    set(SB_EXTRA_TEX_REG_IN_FLAGS)
    foreach(SRC ${SB_TEX_UNITS} ${SB_EXTRA_TEX_UNITS})
      list(APPEND SB_EXTRA_TEX_REG_IN_FLAGS "--in=${SRC}")
    endforeach()
    foreach(SRC ${SB_EXTRA_TEX_UNITS})
      get_filename_component(STEM "${SRC}" NAME_WE)
      set(OUT "${SB_EXTRA_GEN_DIR}/${STEM}.tex.gen.h")
      add_custom_command(
        OUTPUT "${OUT}"
        COMMAND $<TARGET_FILE:sbrushc> --texture-unit --in=${SRC} --out=${OUT}
        DEPENDS sbrushc ${SRC}
        COMMENT "sbrush texture unit: ${STEM}.tex.gen.h"
        VERBATIM)
      list(APPEND SB_EXTRA_OUTPUTS "${OUT}")
    endforeach()
    add_custom_command(
      OUTPUT "${SB_EXTRA_GEN_DIR}/sculptcore_textures.gen.h"
      COMMAND $<TARGET_FILE:sbrushc> --texture-registry --out-dir=${SB_EXTRA_GEN_DIR}
              ${SB_EXTRA_TEX_REG_IN_FLAGS}
      DEPENDS sbrushc ${SB_TEX_UNITS} ${SB_EXTRA_TEX_UNITS}
      COMMENT "sbrush texture registry (with extras)"
      VERBATIM)
    list(APPEND SB_EXTRA_OUTPUTS "${SB_EXTRA_GEN_DIR}/sculptcore_textures.gen.h")
  else()
    # A shadow registry left by a since-removed extra .stex would keep
    # overriding the checked-in one (this dir precedes kernels/generated on
    # the include path) — drop it at configure time.
    file(REMOVE "${SB_EXTRA_GEN_DIR}/sculptcore_textures.gen.h")
  endif()

  set(SB_EXTRA_BUILTIN_FLAGS)
  foreach(SRC ${SB_EXTRA_BUILTINS})
    list(APPEND SB_EXTRA_BUILTIN_FLAGS "--builtin=${SRC}")
  endforeach()

  set(SB_EXTRA_REG_OUT
      "${SB_EXTRA_GEN_DIR}/sculptcore_extra_brushes.gen.h"
      "${SB_EXTRA_GEN_DIR}/sculptcore_extra_brushes_enum.inc")
  add_custom_command(
    OUTPUT ${SB_EXTRA_REG_OUT}
    COMMAND $<TARGET_FILE:sbrushc> --registry --out-dir=${SB_EXTRA_GEN_DIR}
            ${SB_EXTRA_IN_FLAGS} ${SB_EXTRA_BUILTIN_FLAGS}
            --reserved=${SB_EXTRA_RESERVED}
    DEPENDS sbrushc ${SB_EXTRA_INPUTS} ${SB_EXTRA_BUILTINS}
    COMMENT "sbrush extra registry"
    VERBATIM)

  add_custom_target(sbrush-extra ALL DEPENDS ${SB_EXTRA_OUTPUTS} ${SB_EXTRA_REG_OUT})
  add_dependencies(brush sbrush-extra)
endif()

# WGSL backend: emit one .wgsl per .sbrush into the build dir, and
# (when SBRUSH_VALIDATE_ALL=ON) gate the build on `tint` accepting it.
# The .wgsl artifacts are not linked into libbrush — they exist for
# host-side WebGPU dispatch + CI validation only.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_WGSL)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_WGSL=1)
  file(GLOB SBRUSH_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(WGSL_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/wgsl")
  file(MAKE_DIRECTORY "${WGSL_OUT_DIR}")
  set(WGSL_OUTPUTS)
  foreach(SRC ${SBRUSH_INPUTS})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(OUT "${WGSL_OUT_DIR}/${STEM}.wgsl")
    # Emit pass; the optional validator pass writes a stamp file so
    # downstream targets can depend on "WGSL produced AND tint-validated"
    # rather than re-running validation every build.
    set(STAMP "${OUT}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=wgsl --in=${SRC} --out=${OUT}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${OUT} --format=wgsl -o ${OUT}.tint.wgsl
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush wgsl + tint: ${STEM}.wgsl"
        VERBATIM)
      list(APPEND WGSL_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${OUT}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=wgsl --in=${SRC} --out=${OUT}
                ${SB_TEX_UNIT_FLAGS}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush wgsl: ${STEM}.wgsl"
        VERBATIM)
      list(APPEND WGSL_OUTPUTS "${OUT}")
    endif()
  endforeach()
  add_custom_target(sbrush-wgsl ALL DEPENDS ${WGSL_OUTPUTS})
  sbrush_backend(wgsl tint)
endif()

# SPIR-V backend: chain sbrushc(--backend=spirv) -> tint(--format=spirv)
# -> spirv-val. sbrushc's spirv backend emits WGSL (the tint input) per the
# design doc's "WGSL+Tint" lowering option; a future slice can swap the tint
# step for a direct emit_spirv.cc backend without touching this wiring. The
# .spv artifact is what a future Vulkan dispatcher would consume.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_SPIRV)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_SPIRV=1)
  file(GLOB SBRUSH_INPUTS_SPV "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(SPV_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/spirv")
  file(MAKE_DIRECTORY "${SPV_OUT_DIR}")
  set(SPV_OUTPUTS)
  foreach(SRC ${SBRUSH_INPUTS_SPV})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(WGSL "${SPV_OUT_DIR}/${STEM}.wgsl")
    set(SPV  "${SPV_OUT_DIR}/${STEM}.spv")
    set(STAMP "${SPV}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=spirv --in=${SRC} --out=${WGSL}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${WGSL} --format=spirv -o ${SPV}
        COMMAND ${SBRUSH_SPIRV_VALIDATOR} ${SPV}
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush spirv (via tint) + spirv-val: ${STEM}.spv"
        VERBATIM)
      list(APPEND SPV_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${SPV}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=spirv --in=${SRC} --out=${WGSL}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_WGSL_VALIDATOR} ${WGSL} --format=spirv -o ${SPV}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush spirv (via tint): ${STEM}.spv"
        VERBATIM)
      list(APPEND SPV_OUTPUTS "${SPV}")
    endif()
  endforeach()
  add_custom_target(sbrush-spirv ALL DEPENDS ${SPV_OUTPUTS})
  sbrush_backend(spirv spirv-val)
endif()

# CUDA / HIP backends: chain sbrushc(--backend=cuda|hip) -> clang device-only
# syntactic gate. Both share emit_cuda.cc; only the clang offload flags and
# output extension differ. The gate emits PTX/GCN assembly (`-S`) from the
# self-contained device source — no GPU, CUDA headers, or device libs needed
# (-nogpuinc -nogpulib), and nothing is linked. Like the WGSL/SPIR-V backends,
# the artifacts aren't linked into libbrush; they exist for CI validation (and
# a future native dispatcher) only.
#
# sb_gpu_backend(<name> <clang-lang> <arch-flag> <out-ext>)
macro(sb_gpu_backend name lang archflag ext)
  string(TOUPPER "${name}" NAME_UC)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_${NAME_UC}=1)
  file(GLOB SB_${NAME_UC}_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(SB_${NAME_UC}_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/${name}")
  file(MAKE_DIRECTORY "${SB_${NAME_UC}_OUT_DIR}")
  set(SB_${NAME_UC}_OUTPUTS)
  foreach(SRC ${SB_${NAME_UC}_INPUTS})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(DEV "${SB_${NAME_UC}_OUT_DIR}/${STEM}.${ext}")
    set(ASM "${DEV}.s")
    set(STAMP "${DEV}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=${name} --in=${SRC} --out=${DEV}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_OFFLOAD_COMPILER} -x ${lang} ${archflag}
                -nogpuinc -nogpulib --cuda-device-only
                -Wno-parentheses-equality -Wno-unknown-cuda-version
                -S -o ${ASM} ${DEV}
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush ${name} + clang device-only: ${STEM}.${ext}"
        VERBATIM)
      list(APPEND SB_${NAME_UC}_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${DEV}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=${name} --in=${SRC} --out=${DEV}
                ${SB_TEX_UNIT_FLAGS}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush ${name}: ${STEM}.${ext}"
        VERBATIM)
      list(APPEND SB_${NAME_UC}_OUTPUTS "${DEV}")
    endif()
  endforeach()
  add_custom_target(sbrush-${name} ALL DEPENDS ${SB_${NAME_UC}_OUTPUTS})
  sbrush_backend(${name} clang)
endmacro()

if (NOT BUILD_WASM AND SBRUSH_BACKEND_CUDA)
  sb_gpu_backend(cuda cuda "--cuda-gpu-arch=sm_60" cu)
endif()

if (NOT BUILD_WASM AND SBRUSH_BACKEND_HIP)
  sb_gpu_backend(hip hip "--offload-arch=gfx900" hip)
endif()

# OpenCL backend: chain sbrushc(--backend=opencl) -> clspv (OpenCL C -> SPIR-V)
# -> spirv-val. OpenCL 1.2 has no program-scope global pointers, so the .cl is
# all kernel-args; emit_opencl.cc threads uniforms/buffers through and shares
# the brush_* helper bodies with the other backends. The .spv is what a future
# OpenCL/Vulkan dispatcher would consume; not linked into libbrush.
if (NOT BUILD_WASM AND SBRUSH_BACKEND_OPENCL)
  target_compile_definitions(brush PUBLIC SBRUSH_BACKEND_OPENCL=1)
  file(GLOB SBRUSH_INPUTS_CL "${CMAKE_CURRENT_SOURCE_DIR}/kernels/*.sbrush")
  set(CL_OUT_DIR "${CMAKE_BINARY_DIR}/sbrush_out/opencl")
  file(MAKE_DIRECTORY "${CL_OUT_DIR}")
  set(CL_OUTPUTS)
  foreach(SRC ${SBRUSH_INPUTS_CL})
    get_filename_component(STEM "${SRC}" NAME_WE)
    set(CL  "${CL_OUT_DIR}/${STEM}.cl")
    set(SPV "${CL_OUT_DIR}/${STEM}.spv")
    set(STAMP "${SPV}.validated")
    if (SBRUSH_VALIDATE_ALL)
      add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=opencl --in=${SRC} --out=${CL}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_OPENCL_VALIDATOR} --cl-std=CL1.2 -inline-entry-points ${CL} -o ${SPV}
        COMMAND ${SBRUSH_SPIRV_VALIDATOR} ${SPV}
        COMMAND ${CMAKE_COMMAND} -E touch ${STAMP}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush opencl (via clspv) + spirv-val: ${STEM}.spv"
        VERBATIM)
      list(APPEND CL_OUTPUTS "${STAMP}")
    else()
      add_custom_command(
        OUTPUT "${SPV}"
        COMMAND $<TARGET_FILE:sbrushc> --backend=opencl --in=${SRC} --out=${CL}
                ${SB_TEX_UNIT_FLAGS}
        COMMAND ${SBRUSH_OPENCL_VALIDATOR} --cl-std=CL1.2 -inline-entry-points ${CL} -o ${SPV}
        DEPENDS sbrushc ${SRC} ${SB_TEX_UNITS}
        COMMENT "sbrush opencl (via clspv): ${STEM}.spv"
        VERBATIM)
      list(APPEND CL_OUTPUTS "${SPV}")
    endif()
  endforeach()
  add_custom_target(sbrush-opencl ALL DEPENDS ${CL_OUTPUTS})
  sbrush_backend(opencl clspv)
endif()

