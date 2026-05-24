# Wave 3 — Real WGSL GPU compute dispatch

Make `set_backend wgsl` in the debug app actually execute brush kernels on a
GPU compute pipeline (lavapipe software Vulkan in CI/dev containers, real GPU
elsewhere) instead of silently running the C++ path. Validate the GPU result
against the C++ reference, bit-modulo-fp, via the existing A/B harness.

Scope for this slice: **DRAW brush only**, single dab + stroke_path, headless.
Other brushes already emit valid WGSL/SPIR-V; wiring them is a follow-up.

## Prerequisite — RESOLVED

`Scene::ensureGPU()` previously created a `window::Window` and called
`glfwInit()` even when headless, which `abort()`s in a displayless container.
Fixed in `source/debug/scene.cc`: headless skips window creation entirely and
brings up `VkContext::init(nullptr)` + `OffscreenTarget`. Confirmed: headless
`screenshot` now renders via lavapipe (PNG produced, exit 0). The same
`VkContext` is reused for compute.

## Binding contract (group 0) — from emit_wgsl.cc

| bind | resource | type | usage |
|------|----------|------|-------|
| 0 | co_buf | storage RW `array<vec3<f32>>` | vert positions (in/out) |
| 1 | no_buf | storage RW `array<vec3<f32>>` | vert normals (in/out) |
| 2 | mask_buf | storage RW `array<f32>` | per-vert mask |
| 3 | unique_verts | storage read `array<u32>` | flattened node→global vert |
| 4 | nodes | storage read `array<NodeMeta{vert_offset,vert_count:u32}>` | one per workgroup |
| 5 | brush_u | uniform `BrushUniforms` | strength/radius/falloff/coord_space/... |
| 6 | ctx_u | uniform `CtxUniforms{surfacePos,surfaceNo:vec3; render_matrix:mat4x4}` | per-dab |
| 7 | falloff_lut | storage read `array<f32,256>` | only read when falloff_kind==3 |
| 8 | brush_tex | `texture_2d<f32>` | 1×1 white when no tex |
| 9 | brush_samp | `sampler` | |
| 10 | stroke_path | storage read `array<StrokeSample{pos,normal:vec3; arclen:f32}>` | STROKE_CURVED |

Entry: `@compute @workgroup_size(64) fn main(@builtin(local_invocation_index)
lid, @builtin(workgroup_id) gid)`. One workgroup per node;
`vert = unique_verts[nodes[gid.x].vert_offset + lid]`, early-return if
`lid >= vert_count`.

`vec3` storage elements are 16-byte-aligned (std430). `BrushUniforms` /
`CtxUniforms` follow std140 (vec3→16B, mat4x4→64B). For DRAW the brush_u tail
is just the 10 builtin fields (no spilled DSL uniforms); ctx_u is
surfacePos/surfaceNo/render_matrix only.

## Steps

1. **vk_compute.{h,cc}** in `source/vulkan/` — a self-contained compute helper,
   independent of `VulkanBackend` (which is graphics-only):
   - load `.spv` bytes (runtime-read `build/native/sbrush_out/spirv/<brush>.spv`)
     → `VkShaderModule`.
   - descriptor-set layout for the 11 bindings; pipeline layout; compute
     pipeline (`main`).
   - `Buffer` create/upload/readback helper using `VkContext::findMemoryType`
     (host-visible|coherent for upload/readback; storage + uniform usage flags).
   - 1×1 white `R8G8B8A8` image + sampler for binding 8/9.
   - `dispatch(nodeCount)` records `vkCmdBindPipeline/DescriptorSets/Dispatch`
     inside `VkContext::runOneShot`.

2. **Marshaling** (host side, in the debug app GPU stroke path):
   - co/no from `mesh.v.co` / `mesh.v.no`; mask from `treeMesh.mask`. Pack into
     16-byte-strided vec3 arrays for co/no/mask buffers (mask is plain f32).
   - flatten filtered nodes' `unique_verts` (OrderedSet<int>) into binding 3,
     build NodeMeta (offset,count) into binding 4.
   - fill BrushUniforms from `scene.brush` (pad falloff bytes→u32), CtxUniforms
     per dab (surfacePos=origin, surfaceNo=normal, render_matrix=identity for
     headless / GLOBAL).
   - falloff_lut from the brush curve; stroke_path from `brush.strokePath`.

3. **Hook the debug app** — when `scene.currentBackend == BrushBackend::Wgsl`,
   the `stroke` / `stroke_path` verbs route to the GPU dispatcher instead of
   `CommandExecutor`. Keep Vulkan out of libbrush: the dispatcher lives in
   debug + vulkan only. Readback binding 0 → write back into `mesh.v.co`
   (and no_buf→`mesh.v.no` if the kernel touches normals; DRAW does not).
   - meshlog: log the affected verts for undo so A/B `undo` works. Simplest:
     log all marshaled verts' co before dispatch (matches what the C++ path
     logs per touched node).

4. **A/B validation** — extend `tests/scripts/brush_backends/` with a DRAW
   script that strokes once under cpp, undoes, strokes under wgsl, and the
   harness compares co_sum/co_sqsum/aabb bit-modulo-fp (existing golden-stat
   mechanism). Add a native test asserting cpp vs wgsl agree within fp epsilon.

## Open decisions to confirm with user

- **Marshal-all-verts vs per-node**: simplest correct path marshals the full
  mesh vert arrays once and indexes via unique_verts. Fine for DRAW; revisit if
  perf matters.
- **SPIR-V source**: runtime-load the `.spv` from the build tree (zero codegen
  churn) vs bake into a generated header. Plan assumes runtime-load for the
  slice.
- **Normals**: DRAW doesn't recompute normals on GPU; leave no_buf as a
  passthrough binding for now.
